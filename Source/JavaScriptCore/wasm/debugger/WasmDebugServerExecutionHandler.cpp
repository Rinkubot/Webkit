/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmDebugServerExecutionHandler.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "IPIntNextInstruction.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyModule.h"
#include "Options.h"
#include "SubspaceInlines.h"
#include "VM.h"
#include "WasmBreakpointManager.h"
#include "WasmCallee.h"
#include "WasmDebugServer.h"
#include "WasmIPIntGenerator.h"
#include "WasmIPIntSlowPaths.h"
#include "WasmModuleManager.h"
#include "WasmOps.h"
#include <cstdlib>
#include <cstring>
#if OS(WINDOWS)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <wtf/Assertions.h>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringParsingBuffer.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DebugServerExecutionHandler);

class StopWorld {
public:
    explicit StopWorld(VM& vm)
        : m_vm(vm)
    {
    }

    ~StopWorld()
    {
        if (m_vm.isStopWorldActive())
            deactivateStopWorld();
    }

    void activateStopWorld()
    {
        m_vm.traps().requestStop();
        m_vm.setIsStopWorldActive(true);
    }

    void deactivateStopWorld()
    {
        m_vm.traps().cancelStop();
        m_vm.setIsStopWorldActive(false);
    }

private:
    VM& m_vm;
    Vector<std::pair<JSWebAssemblyInstance*, void*>> m_savedLimits;
};

struct StopReasonInfo {
    String reasonString;
    StringView reasonSuffix;
};

static inline StopReasonInfo stopReasonCodeToInfo(DebugServerExecutionHandler::StopReason::Code code)
{
    switch (code) {
    case DebugServerExecutionHandler::StopReason::Code::Signal:
        return { "T02"_s, "signal"_s }; // SIGINT - Interrupt signal
    case DebugServerExecutionHandler::StopReason::Code::Trace:
        return { "T05"_s, "trace"_s }; // SIGTRAP - Trace/single step
    case DebugServerExecutionHandler::StopReason::Code::Breakpoint:
        return { "T05"_s, "breakpoint"_s }; // SIGTRAP - Breakpoint hit
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return { String(), "trace"_s };
    }
}

// Helper function to parse GDB Remote Protocol breakpoint packets
//
// Packet Format: [Z|z]<type>,<address>,<length>
// - Z = Insert breakpoint/watchpoint
// - z = Remove breakpoint/watchpoint
// - type: Breakpoint type (see BreakpointPacket::Type enum)
// - address: Memory address in hexadecimal (e.g., "4000000000001ad")
// - length: Length of breakpoint in bytes, typically 1 for instruction breakpoints
//
// Examples:
// - "Z0,4000000000001ad,1" = Insert software breakpoint at address 0x4000000000001ad, length 1
// - "z0,4000000000001ad,1" = Remove software breakpoint at address 0x4000000000001ad, length 1
//
// Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#insert-breakpoint-or-watchpoint-packet
struct BreakpointPacket {
    // GDB Remote Protocol breakpoint/watchpoint types
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#insert-breakpoint-or-watchpoint-packet
    enum class Type : uint8_t {
        Software = 0, // Software breakpoint (instruction patching)
        Hardware = 1, // Hardware breakpoint (CPU debug registers)
        WriteWatchpoint = 2, // Write watchpoint (memory write detection)
        ReadWatchpoint = 3, // Read watchpoint (memory read detection)
        AccessWatchpoint = 4 // Access watchpoint (memory read/write detection)
    };

    Type type { Type::Software }; // Breakpoint type (see Type enum)
    uint64_t address { 0 }; // Memory address where breakpoint should be set (parsed from hex string)
    uint32_t length { 0 }; // Length of breakpoint in bytes (typically 1 for instruction breakpoints)
    bool isValid { false }; // True if packet was successfully parsed, false if malformed
};

static BreakpointPacket parseBreakpointPacket(StringView packet)
{
    BreakpointPacket result;

    size_t firstComma = packet.find(',');
    size_t secondComma = packet.find(',', firstComma + 1);

    if (firstComma == notFound || secondComma == notFound)
        return result;

    auto typeStr = packet.substring(1, firstComma - 1);
    auto addressStr = packet.substring(firstComma + 1, secondComma - firstComma - 1);
    auto lengthStr = packet.substring(secondComma + 1);

    CString typeCStr = typeStr.utf8();
    CString addressCStr = addressStr.utf8();
    CString lengthCStr = lengthStr.utf8();

    // Parse packet components using C library functions
    int typeValue = static_cast<int>(strtol(typeCStr.data(), nullptr, 10)); // Parse type as decimal integer
    result.type = static_cast<BreakpointPacket::Type>(typeValue); // Convert to enum
    result.address = strtoull(addressCStr.data(), nullptr, 16); // Parse address as hexadecimal
    result.length = static_cast<uint32_t>(strtoul(lengthCStr.data(), nullptr, 10)); // Parse length as decimal integer
    result.isValid = true; // Mark as successfully parsed
    return result;
}

static uint64_t parseThreadId(StringView threadIdStr, uint64_t defaultValue)
{
    if (threadIdStr.isEmpty())
        return defaultValue;

    CString threadIdCStr = threadIdStr.utf8();
    return strtoull(threadIdCStr.data(), nullptr, 16);
}

DebugServerExecutionHandler::DebugServerExecutionHandler(DebugServer& debugServer, WASMModuleManager& moduleManager, WASMBreakpointManager& breakpointManager)
    : m_debugServer(debugServer)
    , m_moduleManager(moduleManager)
    , m_breakpointManager(breakpointManager)
{
}

template<typename LockType>
void DebugServerExecutionHandler::stopImpl(LockType& locker) WTF_REQUIRES_LOCK(m_lock)
{
    dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop][Breakpoint] Updated stop reason and waiting...");
    m_codeContinue.wait(locker);
    dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop][Breakpoint] Unblocked and running...");

    m_stopReason.reset();
    m_mutatorState = JSCState::Running;
    if (m_debuggerState == DebuggerState::ContinueRequested)
        m_debuggerContinue.notifyOne();
}

void DebugServerExecutionHandler::stopOneTimeBreakpoint(StopReason&& stopReason)
{
    Locker locker { m_lock };
    dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop][OneTimeBreakpoint] Start");

    m_stopReason = stopReason;
    m_mutatorState = JSCState::Stopped;
    m_breakpointManager.clearAllTmpBreakpoints();

    RELEASE_ASSERT(m_debuggerState == DebuggerState::StopRequested);
    m_debuggerContinue.notifyOne();

    stopImpl(locker);
}

void DebugServerExecutionHandler::stopRegularBreakpoint(StopReason&& stopReason)
{
    Locker locker { m_lock };
    dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop][RegularBreakpoint] Start");

    m_stopReason = stopReason;
    m_mutatorState = JSCState::Stopped;
    if (m_debuggerState == DebuggerState::ContinueRequested) {
        sendStopReply(locker);
        dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop][RegularBreakpoint] Currently in continue. Sent a stop reply and waiting...");
    } else {
        RELEASE_ASSERT(m_debuggerState == DebuggerState::StopRequested);
        m_debuggerContinue.notifyOne();
    }

    stopImpl(locker);
}

bool DebugServerExecutionHandler::stopCode(CallFrame* callFrame, JSWebAssemblyInstance* instance, IPIntCallee* callee, uint8_t* pc, uint8_t* mc, IPInt::IPIntLocal* locals, IPInt::IPIntStackEntry* stack)
{
    RELEASE_ASSERT(Thread::currentSingleton().uid() == m_debugServer.mutatorThreadId());

    uint64_t virtualAddress = WASMModuleManager::physicalToVirtual(instance->jsModule(), callee->functionIndex(), pc);
    if (auto* breakpoint = m_breakpointManager.breakpoint(virtualAddress)) {
        m_debugServer.updateRegisterState(reinterpret_cast<uint64_t>(pc), reinterpret_cast<uint64_t>(callFrame), reinterpret_cast<uint64_t>(callFrame));
        StopReason stopReason(breakpoint->type, virtualAddress, breakpoint->originalBytecode, pc, mc, locals, stack, callee, instance, callFrame);
        dataLogLnIf(Options::verboseWasmDebugger(), "[Code][Stop] Going to stop at ", *breakpoint, " with ", stopReason);
        if (breakpoint->isOneTimeBreakpoint())
            stopOneTimeBreakpoint(WTFMove(stopReason));
        else
            stopRegularBreakpoint(WTFMove(stopReason));
        return true;
    }
    return false;
}

void DebugServerExecutionHandler::resume()
{
    Locker locker { m_lock };

    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Continue] Start");

    RELEASE_ASSERT(Thread::currentSingleton().uid() == m_debugServer.debugServerThreadId());
    RELEASE_ASSERT(m_debuggerState == DebuggerState::Replied && m_mutatorState == JSCState::Stopped);
    m_codeContinue.notifyOne();

    // This is to simplify implementation. If we don't wait here, we may have a race condition that
    // after above notification, interrupt() may acquire the locker first.
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Continue] Notified code to continue and waiting...");
    m_debuggerState = DebuggerState::ContinueRequested;
    m_debuggerContinue.wait(m_lock);
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Continue] Confirmed that code is running...");
    // Note that the mutator may be stopped at this point due to hitting regular breakpoints.
}

void DebugServerExecutionHandler::interrupt()
{
    RELEASE_ASSERT(Thread::currentSingleton().uid() == m_debugServer.debugServerThreadId());

    Locker locker { m_lock };
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Interrupt] Start");

    // LLDB implements a sophisticated interrupt queuing mechanism that prevents packet flooding while ensuring
    // all user interrupt requests are honored. When multiple Ctrl+C interrupts occur during process execution,
    // only the first interrupt sends the actual \x03 packet to the debugger, while subsequent interrupts are
    // queued and wait for the same stop reply. The m_async_count tracks queued interrupts, and m_is_running
    // prevents new interrupts from sending packets once the process is stopped. When the debugger sends a stop
    // reply, m_cv.notify_all() simultaneously resolves all queued interrupts, and m_is_running is set to false
    // to block further interrupt packets until the next continue operation.
    StopWorld stopWorld(*m_debugServer.vm());

    {
        RELEASE_ASSERT(m_mutatorState == JSCState::Running);
        m_debuggerState = DebuggerState::StopRequested;
        stopWorld.activateStopWorld();
    }

    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Interrupt] Notified code to continue and waiting...");
    m_debuggerContinue.wait(m_lock);
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Interrupt] Confirmed that code is running...");

    {
        stopWorld.deactivateStopWorld();
        sendStopReply(locker);
    }
}

void DebugServerExecutionHandler::stepImpl()
{
    RELEASE_ASSERT(Thread::currentSingleton().uid() == m_debugServer.debugServerThreadId());

    Locker locker { m_lock };
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Start with ", m_stopReason);

    uint8_t* currentPC = m_stopReason.pc;
    uint8_t* currentMC = m_stopReason.mc;

    auto setStepTmpBreakpoint = [&](const uint8_t* nextPC) WTF_REQUIRES_LOCK(m_lock) {
        uint64_t nextVirtualAddress = m_stopReason.virtualAddress + (nextPC - currentPC);
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step][SetTmpBreakpoint] current PC=%p(%p), next PC=%p(%p)\n", currentPC, (void*)m_stopReason.virtualAddress, nextPC, (void*)nextVirtualAddress);
        if (m_breakpointManager.breakpoint(nextVirtualAddress))
            return;
        m_breakpointManager.setBreakpoint(nextVirtualAddress, Breakpoint(const_cast<uint8_t*>(nextPC), Breakpoint::Type::Step));
    };

    auto setStepTmpBreakpointAtCaller = [&]() WTF_REQUIRES_LOCK(m_lock) {
        uint8_t* returnPC = nullptr;
        uint64_t virtualReturnPC = 0;
        if (getWasmReturnPC(m_stopReason.callFrame, returnPC, virtualReturnPC)) {
            m_breakpointManager.setBreakpoint(virtualReturnPC, Breakpoint(const_cast<uint8_t*>(returnPC), Breakpoint::Type::Step));
            return true;
        }
        return false;
    };

    auto setStepIntoBreakpointForDirectCall = [&]() WTF_REQUIRES_LOCK(m_lock) -> bool {
        const IPInt::CallMetadata* metadata = reinterpret_cast<const IPInt::CallMetadata*>(currentMC);
        Wasm::FunctionSpaceIndex functionSpaceIndex = metadata->functionIndex;

        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Call instruction metadata: function index %u", static_cast<uint32_t>(functionSpaceIndex));

        JSWebAssemblyInstance* instance = m_stopReason.instance;
        auto* calleeGroup = instance->calleeGroup();
        RefPtr<Wasm::IPIntCallee> callee = calleeGroup->wasmCalleeFromFunctionIndexSpace(functionSpaceIndex);
        if (callee->compilationMode() != Wasm::CompilationMode::IPIntMode) {
            dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Target function is not IPInt mode");
            return false;
        }

        const uint8_t* targetFunctionStart = callee->bytecode();
        uint64_t targetVirtualAddress = WASMModuleManager::physicalToVirtual(instance->jsModule(), callee->functionIndex(), callee->bytecode());
        m_breakpointManager.setBreakpoint(targetVirtualAddress, Breakpoint(const_cast<uint8_t*>(targetFunctionStart), Breakpoint::Type::Step));
        return true;
    };

    bool needToWaitForStop = true;
    switch (m_stopReason.originalBytecode) {
    case Return:
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Handling return instruction - setting breakpoint at caller");
        needToWaitForStop = setStepTmpBreakpointAtCaller();
        break;
    case Call:
    case TailCall:
        if (setStepIntoBreakpointForDirectCall())
            break;
        [[fallthrough]];
    case CallIndirect: // TODO:
    case TailCallIndirect: // TODO:
    case CallRef: // TODO:
    case TailCallRef: // TODO:
        [[fallthrough]];
    default:
        IPInt::NextInstructionResult result = IPInt::calculateNextInstruction(m_stopReason.originalBytecode, currentPC, currentMC);
        setStepTmpBreakpoint(result.nextPC);
        if (result.isConditionalBranch)
            setStepTmpBreakpoint(result.elsePC);
        break;
    }

    RELEASE_ASSERT(m_debuggerState == DebuggerState::Replied && m_mutatorState == JSCState::Stopped);
    m_codeContinue.notifyOne();

    if (needToWaitForStop) {
        m_debuggerState = DebuggerState::StopRequested;
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Notified code to continue and waiting...");
        m_debuggerContinue.wait(m_lock);
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] JSC is stoped");
        sendStopReply(locker);
    } else {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Notified code to continue and waiting...");
        m_debuggerState = DebuggerState::ContinueRequested;
        m_debuggerContinue.wait(m_lock);
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Confirmed that code is running...");
    }
}

void DebugServerExecutionHandler::step(StringView packet)
{
    if (packet.isEmpty())
        return;

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] Handling step command: %s\n", packet.utf8().data());

    // Parse different step packet formats
    if (packet[0] == 's' || packet[0] == 'S') {
        // Format: s[addr] or S[signal][;addr]
        // LLDB is asking: Single step execution
        // WebAssembly Context: Step through one WASM instruction
        // Reply Decision: Send stop reply with next PC after step
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Executing single step (s/S command)");
        stepImpl();
    } else if (packet.startsWith("vCont;s"_s)) {
        // Format: vCont;s or vCont;s:thread-id (like WAMR)
        // This is the primary step command used by LLDB for WebAssembly debugging

        size_t colonPos = packet.find(':');
        uint64_t threadId = m_debugServer.mutatorThreadId(); // Use actual JSC mutator thread ID

        if (colonPos != notFound) {
            auto threadIdStr = packet.substring(colonPos + 1);
            threadId = parseThreadId(threadIdStr, threadId);
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] vCont step with thread ID: %p\n", (void*)threadId);
        } else {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] vCont step without thread ID, using mutator thread: %p\n", (void*)threadId);
        }
        stepImpl();
    } else if (packet.startsWith("vCont;S"_s)) {
        // Format: vCont;S<signal> or vCont;S<signal>:thread-id
        // Step with signal handling
        size_t colonPos = packet.find(':');
        if (colonPos != notFound) {
            auto threadIdStr = packet.substring(colonPos + 1);
            uint64_t threadId = parseThreadId(threadIdStr, m_debugServer.mutatorThreadId());
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] vCont step with signal and thread ID: %p\n", (void*)threadId);
        } else {
            uint64_t threadId = m_debugServer.mutatorThreadId();
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] vCont step with signal, using mutator thread: %p\n", (void*)threadId);
        }
        // Execute single WebAssembly instruction step with signal
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger][Step] Executing single WASM instruction step with signal");
        stepImpl();
    } else {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][Step] Unknown step packet format: %s\n", packet.utf8().data());
        sendErrorReply(ProtocolError::InvalidPacket);
    }
}

void DebugServerExecutionHandler::setInterruptBreakpoint(JSWebAssemblyModule* module, IPIntCallee* callee)
{
    uint8_t* pc = const_cast<uint8_t*>(callee->bytecode());
    uint64_t virtualAddress = WASMModuleManager::physicalToVirtual(module, callee->functionIndex(), pc);
    if (m_breakpointManager.breakpoint(virtualAddress))
        return;
    m_breakpointManager.setBreakpoint(virtualAddress, Breakpoint(pc, Breakpoint::Type::Interrupt));
}

void DebugServerExecutionHandler::setBreakpoint(StringView packet)
{
    if (packet.isEmpty())
        return;

    // Parse packet format: Z0,<address>,<length>
    auto parsed = parseBreakpointPacket(packet);
    if (!parsed.isValid) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][SetBreakpoint] Setting breakpoint: type=%d, address=%p, length=%u\n", static_cast<int>(parsed.type), (void*)parsed.address, parsed.length);

    // Only support software breakpoints for now
    if (parsed.type != BreakpointPacket::Type::Software) {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][SetBreakpoint] Unsupported breakpoint type: %d\n", static_cast<int>(parsed.type));
        sendErrorReply(ProtocolError::UnknownCommand);
        return;
    }

    // Validate address is in WASM virtual address space
    if (parsed.address < WASMModuleManager::VIRTUAL_ADDRESS_BASE) {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][SetBreakpoint] Invalid breakpoint address: %p\n", (void*)parsed.address);
        sendErrorReply(ProtocolError::InvalidAddress);
        return;
    }

    RELEASE_ASSERT(!m_breakpointManager.breakpoint(parsed.address));
    uint8_t* pc = m_moduleManager.virtualToPhysical(parsed.address);
    RELEASE_ASSERT(pc);
    m_breakpointManager.setBreakpoint(parsed.address, Breakpoint(pc, Breakpoint::Type::Regular));
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger][SetBreakpoint] Successfully set breakpoint at %p (physical: %p, original: 0x%02x)\n", (void*)parsed.address, pc, *pc);
    sendReplyOK();
}

void DebugServerExecutionHandler::removeBreakpoint(StringView packet)
{
    if (packet.isEmpty())
        return;

    // Format: z0,<address>,<length>
    auto parsed = parseBreakpointPacket(packet);
    if (!parsed.isValid) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Removing breakpoint: type=%d, address=%p\n", static_cast<int>(parsed.type), (void*)parsed.address);

    // Only support software breakpoints for now
    if (parsed.type != BreakpointPacket::Type::Software) {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unsupported breakpoint type for removal: %d\n", static_cast<int>(parsed.type));
        sendErrorReply(ProtocolError::UnknownCommand);
        return;
    }

    // Delegate to breakpoint manager
    if (m_breakpointManager.removeRegularBreakpoint(parsed.address)) {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Breakpoint removed successfully from %p\n", (void*)parsed.address);
        sendReplyOK();
    } else {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Breakpoint not found at address: %p\n", (void*)parsed.address);
        sendErrorReply(ProtocolError::InvalidAddress);
    }
}

void DebugServerExecutionHandler::handleThreadStopInfo(StringView packet)
{
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Handling qThreadStopInfo: %s\n", packet.utf8().data());
    Locker locker { m_lock };
    sendStopReply(locker);
}

void DebugServerExecutionHandler::sendStopReply(AbstractLocker& locker) WTF_REQUIRES_LOCK(m_lock)
{
    RELEASE_ASSERT(m_mutatorState == JSCState::Stopped && m_stopReason.isValid());
    uint64_t pc = m_stopReason.virtualAddress;

    auto stopInfo = stopReasonCodeToInfo(m_stopReason.code);
    String reasonString = stopInfo.reasonString;
    StringView reasonSuffix = stopInfo.reasonSuffix;
    String pcBytes = toLittleEndianHex(pc);
    uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();

    String stopReplyStr = makeString(
        reasonString,
        "thread:"_s, hex(mutatorThreadId, Lowercase),
        ";name:JSC-mutator;threads:"_s, hex(mutatorThreadId, Lowercase),
        ";thread-pcs:"_s, hex(pc, 16, Lowercase),
        ";00:"_s,
        pcBytes,
        ";reason:"_s,
        reasonSuffix,
        ";"_s);

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Sending stop reply: %s\n", stopReplyStr.utf8().data());
    sendReply(locker, stopReplyStr);
}

void DebugServerExecutionHandler::sendReply(StringView reply)
{
    Locker locker { m_lock };
    sendReply(locker, reply);
}

void DebugServerExecutionHandler::sendReply(AbstractLocker&, StringView reply) WTF_REQUIRES_LOCK(m_lock)
{
    uint8_t checksum = 0;
    for (auto character : reply.codeUnits())
        checksum += character;

    String packet = makeString('$', reply, '#', hex(checksum, 2, Lowercase));
    CString packetData = packet.utf8();

    int sent = static_cast<int>(send(m_debugServer.m_clientSocket, packetData.data(), packetData.length(), 0));
    if (sent < 0) {
        m_debuggerState = DebugServerExecutionHandler::DebuggerState::ReplyFailed;
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Failed to send packet: %s sent: %d\n", packetData.data(), sent);
    } else {
        m_debuggerState = DebugServerExecutionHandler::DebuggerState::Replied;
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Sent reply: %s\n", packetData.data());
    }
}

void DebugServerExecutionHandler::sendReplyOK() { m_debugServer.sendReplyOK(); }
void DebugServerExecutionHandler::sendErrorReply(ProtocolError error) { m_debugServer.sendErrorReply(error); }

DebugServerExecutionHandler::StopReason DebugServerExecutionHandler::stopReason() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_stopReason.isValid());
    return m_stopReason;
}

} // namespace Wasm
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
