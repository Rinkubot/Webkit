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
#include "WasmDebugServerQueryHandler.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "CallFrame.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyModule.h"
#include "NativeCallee.h"
#include "Options.h"
#include "StackVisitor.h"
#include "VM.h"
#include "WasmCallee.h"
#include "WasmDebugServer.h"
#include "WasmDebugServerExecutionHandler.h"
#include "WasmDebugServerUtilities.h"
#include "WasmIPIntGenerator.h"
#include "WasmIPIntSlowPaths.h"
#include "WasmModuleInformation.h"
#include "WasmModuleManager.h"
#include <cstring>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DebugServerQueryHandler);

void DebugServerQueryHandler::handleGeneralQuery(StringView packet)
{
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Handling query: '%s'\n", packet.utf8().data());

    // Handle Q* packets (configuration/setup packets)
    if (packetStartsWith(packet, "QStartNoAckMode"_s)) {
        // Format: QStartNoAckMode
        // LLDB wants to disable ACK mode - acknowledge this
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packet-Acknowledgment.html
        // WebAssembly Context: ACK mode adds overhead to WASM debugging, so disabling improves performance
        // This is especially beneficial for WASM step-through debugging with many small packets

        m_debugServer.sendReplyOK(); // OK - WASM debugger supports no-ACK mode for better performance
        m_debugServer.m_noAckMode = true;
    } else if (packetStartsWith(packet, "qSupported"_s)) {
        // Format: qSupported[:feature[;feature]...]
        // Query supported features and packet size
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qSupported
        // WebAssembly Context: We support qXfer:libraries:read+ to let LLDB discover WASM modules
        // This allows LLDB to see loaded WebAssembly modules as "libraries" for debugging

        // Match WAMR exactly: PacketSize + basic features
        m_debugServer.sendReply("qXfer:libraries:read+;PacketSize=1000;"_s);
    } else if (packetStartsWith(packet, "QThreadSuffixSupported"_s)) {
        // Format: QThreadSuffixSupported
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Do we support thread ID suffixes like "Hg1.2" instead of "Hg1"
        // WebAssembly Context: WASM execution is typically single-threaded in our implementation, we use a simplified threading model, so complex thread suffixes aren't needed
        // Reply Decision: Empty reply means not supported - WASM debugger uses basic thread IDs only

        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "QListThreadsInStopReply"_s)) {
        // Format: QListThreadsInStopReply
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Can we include thread list in stop replies
        // WebAssembly Context: WASM typically runs in single thread, so this is simple to support, we can easily include our single main thread in stop replies
        // Reply Decision: OK - WASM debugger will include thread info (just main thread) in stop replies

        m_debugServer.sendReplyOK();
    } else if (packetStartsWith(packet, "QEnableErrorStrings"_s)) {
        // Format: QEnableErrorStrings
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Enable error strings in replies for better debugging
        // WebAssembly Context: Error strings help debug WASM execution issues, useful for reporting WASM trap conditions and runtime errors
        // Reply Decision: OK - WASM debugger will include error strings in replies

        m_debugServer.sendReplyOK();
    } else if (packetStartsWith(packet, "jThreadsInfo"_s)) {
        // Format: jThreadsInfo
        // Reference: https://lldb.llvm.org/resources/lldbgdbremote.html#jthreadsinfo
        // LLDB is asking: JSON thread info - provide basic thread info with PC address
        // WebAssembly Context: WASM runs in single main thread within JSC, we report this as thread ID 1 with name "main" for LLDB's thread view
        // Reply Decision: Include PC address to fix frame display issue
        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "jThreadExtendedInfo:"_s)) {
        // Format: jThreadExtendedInfo:<thread-id-in-hex>
        // LLDB expects this to work - no fallback to qThreadExtraInfo in modern versions
        StringView threadIdStr = packet.substring(strlen("jThreadExtendedInfo:"));
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] jThreadExtendedInfo thread ID: '%s'\n", threadIdStr.utf8().data());

        if (threadIdStr.isEmpty()) {
            // LLDB is sending malformed packet - this suggests our jThreadsInfo format is wrong
            // According to GDB spec, this shouldn't happen if jThreadsInfo is correct
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Empty thread ID - this indicates jThreadsInfo format issue\n");
            m_debugServer.sendReply(""_s);
        } else {
            // Parse thread ID - according to GDB spec, this is hexadecimal
            CString threadIdCStr = threadIdStr.utf8();
            unsigned long threadId = strtoul(threadIdCStr.data(), nullptr, 16);
            uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();
            dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Parsed thread ID: ", threadId, " (from hex: ", threadIdStr, "), mutator ID: ", mutatorThreadId);

            if (threadId == mutatorThreadId) {
                // Return JSON object with thread name and queue info
                m_debugServer.sendReply("{\"name\":\"main\",\"queue\":\"com.apple.main-thread\"}"_s);
            } else {
                dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unknown thread ID: %lu\n", threadId);
                m_debugServer.sendReply(""_s);
            }
        }
    } else if (packetEquals(packet, "qC"_s)) {
        // Format: qC
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qC-packet
        // LLDB is asking: Query current thread ID
        // WebAssembly Context: Use actual JSC mutator thread ID for consistency
        // This must match the thread ID used in stop replies and vCont commands
        uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();
        String response = makeString("QC"_s, hex(mutatorThreadId, Lowercase));
        m_debugServer.sendReply(response);
    } else if (packetStartsWith(packet, "qfThreadInfo"_s)) {
        // Format: qfThreadInfo
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qfThreadInfo
        // LLDB is asking: First thread info query - start thread enumeration
        // WebAssembly Context: WASM has single main thread to report
        // Reply Decision: Use actual mutator thread ID for consistency with qC

        uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();
        String reply = makeString("m"_s, hex(mutatorThreadId, Lowercase));
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qfThreadInfo - returning thread %p\n", (void*)mutatorThreadId);
        m_debugServer.sendReply(reply);
    } else if (packetStartsWith(packet, "qsThreadInfo"_s)) {
        // Format: qsThreadInfo
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qsThreadInfo
        // LLDB is asking: Subsequent thread info query - continue thread enumeration
        // WebAssembly Context: No more threads after main thread
        // Reply Decision: l - End of thread list

        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qsThreadInfo - end of thread list\n");
        m_debugServer.sendReply("l"_s);
    } else if (packetStartsWith(packet, "qThreadExtraInfo"_s)) {
        // Format: qThreadExtraInfo,<thread-id>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qThreadExtraInfo
        // LLDB is asking: Get extra thread information (human-readable description)
        // WebAssembly Context: Provide descriptive name for WASM thread
        // Reply Decision: Parse thread ID and return "main" in hex for our mutator thread

        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qThreadExtraInfo called: %s\n", packet.utf8().data());
        StringView threadIdStr = packet.substring(strlen("qThreadExtraInfo,"));
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qThreadExtraInfo thread ID: '%s'\n", threadIdStr.utf8().data());

        if (!threadIdStr.isEmpty()) {
            CString threadIdCStr = threadIdStr.utf8();
            unsigned long threadId = strtoul(threadIdCStr.data(), nullptr, 16);
            uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Parsed qThreadExtraInfo thread ID: %lu, mutator ID: %p\n", threadId, (void*)mutatorThreadId);
            if (threadId == mutatorThreadId) {
                dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Sending thread name 'main' in hex\n");
                m_debugServer.sendReply("6d61696e"_s); // "main" in hex
            } else {
                dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unknown thread ID in qThreadExtraInfo: %lu\n", threadId);
                m_debugServer.sendReply(""_s); // Unknown thread
            }
        } else {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Malformed qThreadExtraInfo packet\n");
            m_debugServer.sendReply(""_s); // Malformed packet
        }
    } else if (packetStartsWith(packet, "qThreadStopInfo"_s)) {
        // Format: qThreadStopInfo<thread-id>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qThreadStopInfo
        // LLDB is asking: Get stop info for specific thread (needed for frame variable)
        // WebAssembly Context: Provide stop reason for WASM thread
        // Reply Decision: Handled by execution handler
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling qThreadStopInfo for frame variable support");
        m_debugServer.handleThreadStopInfo(packet);
    } else if (packetStartsWith(packet, "qHostInfo"_s)) {
        // Format: qHostInfo
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Query host system information (architecture, pointer size, etc.)
        // WebAssembly Context: Report host architecture for proper WASM debugging setup
        // Reply Decision: Architecture-specific triple and pointer size

        handleHostInfo();
    } else if (packetStartsWith(packet, "qProcessInfo"_s)) {
        // Format: qProcessInfo
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Query process information (PID, parent PID, user/group IDs)
        // WebAssembly Context: Provide process info for JSC process running WASM
        // Reply Decision: Simulated process info for WASM debugging context

        handleProcessInfo();
    } else if (packetStartsWith(packet, "qAttached"_s)) {
        // Format: qAttached[:<pid>]
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qAttached
        // LLDB is asking: Query whether we attached to existing process (1) or created new one (0)
        // WebAssembly Context: We're attached to existing JSC process running WASM
        // Reply Decision: 1 - We are attached to existing process

        m_debugServer.sendReply("1"_s);
    } else if (packetStartsWith(packet, "qOffsets"_s)) {
        // Format: qOffsets
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qOffsets
        // LLDB is asking: Query section offsets for relocated executables
        // WebAssembly Context: WASM modules use fixed virtual addresses, no relocation needed
        // Reply Decision: Text=0;Data=0;Bss=0 - No relocation offsets

        m_debugServer.sendReply("Text=0;Data=0;Bss=0"_s);
    } else if (packetStartsWith(packet, "qXfer:features:read:target.xml"_s)) {
        // Format: qXfer:features:read:target.xml:<offset>,<length>
        // LLDB is asking: Transfer target description XML
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qXfer-target-description-read

        handleTargetXML();
    } else if (packetStartsWith(packet, "qRegisterInfo"_s)) {
        // Format: qRegisterInfo<hex-register-id>
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Query register information for specific register
        // WebAssembly Context: Provide register descriptions for ARM64 or x86_64 depending on configuration
        // Reply Decision: Architecture-specific register info or empty for unsupported registers

        handleRegisterInfo(packet);
    } else if (packetStartsWith(packet, "qVAttachOrWaitSupported"_s)) {
        // Format: qVAttachOrWaitSupported
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Query support for vAttachOrWait packet
        // WebAssembly Context: We don't support launching/waiting for WASM processes
        // Reply Decision: Empty - Not supported

        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "qStructuredDataPlugins"_s)) {
        // Format: qStructuredDataPlugins
        // Reference: https://github.com/llvm/llvm-project/blob/main/lldb/docs/lldb-gdb-remote.txt
        // LLDB is asking: Query available structured data plugins
        // WebAssembly Context: We don't provide structured data plugins for WASM
        // Reply Decision: Empty - Not supported

        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "qShlibInfoAddr"_s)) {
        // Format: qShlibInfoAddr
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qShlibInfoAddr
        // LLDB is asking: Query shared library info address
        // WebAssembly Context: WASM modules don't use traditional shared library mechanisms
        // Reply Decision: Empty - Not supported

        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "qSymbol::"_s)) {
        // Format: qSymbol[::<symbol-name>]
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qSymbol
        // LLDB is asking: "Do you need the address of any symbols?"
        // WebAssembly Context: qSymbol is designed for traditional debugging where the stub has no symbol
        // information and needs to ask LLDB for symbol addresses. This doesn't apply to WASM debugging:
        //   1. WebKit already knows all WASM module addresses and function locations
        //   2. WASM modules are loaded at known virtual addresses (e.g., 0x4000000000000000)
        //   3. LLDB discovers WASM symbols through other mechanisms (memory reading, DWARF info, etc.)
        //   4. Using qSymbol for WASM would be redundant and inappropriate
        // Reply Decision: Empty response means "not supported" - matches WAMR's approach
        // This prevents protocol violations that previously caused LLDB confusion and infinite qC loops
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qSymbol query - not supported for WASM debugging\n");
        m_debugServer.sendReply(""_s);
    } else if (packetStartsWith(packet, "qXfer:libraries:read::"_s)) {
        // Format: qXfer:libraries:read::<offset>,<length>
        // Transfer library list XML using simplified module manager
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qXfer-library-list-read

        size_t offset, maxSize;
        if (!parseLibrariesReadPacket(packet, offset, maxSize)) {
            m_debugServer.sendReply("E01"_s); // Invalid format
            return;
        }

        String response;
        bool success = handleChunkedLibrariesResponse(offset, maxSize, response);

        if (success) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Sending library list chunk: offset=%zu, maxSize=%zu\n", offset, maxSize);
            m_debugServer.sendReply(response);
        } else {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Failed to generate library list chunk\n");
            m_debugServer.sendReply("E02"_s); // Error generating response
        }
    } else if (packetStartsWith(packet, "qWasmCallStack:"_s)) {
        // Format: qWasmCallStack:<thread-id-in-hex>
        // Reference: LLDB WebAssembly debugging extension
        // LLDB is asking: Get WebAssembly call stack information for disassembly display
        // WebAssembly Context: This packet is essential for LLDB to show proper WASM disassembly
        // with source lines, instruction details, and frame information
        // Reply Decision: Return full call stack with all WebAssembly frame addresses to match WAMR

        StringView threadIdStr = packet.substring(strlen("qWasmCallStack:"));
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmCallStack thread ID: '%s'\n", threadIdStr.utf8().data());

        if (!threadIdStr.isEmpty()) {
            CString threadIdCStr = threadIdStr.utf8();
            unsigned long threadId = strtoul(threadIdCStr.data(), nullptr, 16);
            uint64_t mutatorThreadId = m_debugServer.mutatorThreadId();
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Parsed qWasmCallStack thread ID: %lu, mutator ID: %p\n", threadId, (void*)mutatorThreadId);

            if (threadId == mutatorThreadId) {
                String response = buildWasmCallStackResponse();
                dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmCallStack response: %s\n", response.utf8().data());
                m_debugServer.sendReply(response);
            } else {
                dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unknown thread ID in qWasmCallStack: %lu\n", threadId);
                m_debugServer.sendReply(""_s); // Unknown thread
            }
        } else {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Malformed qWasmCallStack packet\n");
            m_debugServer.sendReply(""_s); // Malformed packet
        }
    } else if (packetStartsWith(packet, "qWasmGlobal:"_s)) {
        // Format: qWasmGlobal:<frame-index>:<variable-index>
        // Reference: LLDB WebAssembly debugging extension
        // LLDB is asking: Get value of WebAssembly global variable
        // WebAssembly Context: Access global variables by index for debugging
        // Reply Decision: Return global value in little-endian hex format

        StringView params = packet.substring(strlen("qWasmGlobal:"));
        size_t separatorPos = params.find(';');

        if (separatorPos == notFound) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Malformed qWasmGlobal packet\n");
            m_debugServer.sendReply(""_s);
            return;
        }

        StringView frameIndexStr = params.substring(0, separatorPos);
        StringView variableIndexStr = params.substring(separatorPos + 1);

        CString frameIndexCStr = frameIndexStr.utf8();
        CString variableIndexCStr = variableIndexStr.utf8();
        unsigned long frameIndex = strtoul(frameIndexCStr.data(), nullptr, 10);
        unsigned long variableIndex = strtoul(variableIndexCStr.data(), nullptr, 10);

        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmGlobal frame=%lu, variable=%lu\n", frameIndex, variableIndex);

        // For now, only support frame 0 (current frame)
        if (frameIndex != 0) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmGlobal: frame index %lu not supported\n", frameIndex);
            m_debugServer.sendReply(""_s);
            return;
        }

        // FIXME: Access global variables from current WebAssembly instance
        // This requires extending the stop reason to include instance context
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmGlobal: global access not yet implemented\n");
        m_debugServer.sendReply(""_s);

    } else if (packetStartsWith(packet, "qWasmLocal:"_s)) {
        // Format: qWasmLocal:<frame-index>:<variable-index>
        // Reference: LLDB WebAssembly debugging extension
        // LLDB is asking: Get value of WebAssembly local variable (function argument or local)
        // WebAssembly Context: Access function locals and parameters for debugging
        // Reply Decision: Return local value or address based on variable type

        StringView params = packet.substring(strlen("qWasmLocal:"));
        size_t separatorPos = params.find(';');

        if (separatorPos == notFound) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Malformed qWasmLocal packet\n");
            m_debugServer.sendReply(""_s);
            return;
        }

        StringView frameIndexStr = params.substring(0, separatorPos);
        StringView variableIndexStr = params.substring(separatorPos + 1);

        CString frameIndexCStr = frameIndexStr.utf8();
        CString variableIndexCStr = variableIndexStr.utf8();
        unsigned long frameIndex = strtoul(frameIndexCStr.data(), nullptr, 10);
        unsigned long localIndex = strtoul(variableIndexCStr.data(), nullptr, 10);

        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmLocal frame=%lu, variable=%lu\n", frameIndex, localIndex);

        // For now, only support frame 0 (current frame)
        if (frameIndex != 0) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmLocal: frame index %lu not supported\n", frameIndex);
            m_debugServer.sendReply(""_s);
            return;
        }

        auto stopReason = m_debugServer.m_executionHandler->stopReason();
        auto functionIndex = stopReason.callee->functionIndex();
        const auto& moduleInfo = stopReason.instance->module().moduleInformation();
        const Vector<Type>& localTypes = moduleInfo.functions[functionIndex].localTypes;

        IPInt::IPIntLocal& local = stopReason.locals[localIndex];
        Type localType = localTypes[localIndex];
        logWasmLocalValue(localIndex, local, localType);

        void* memoryBase = stopReason.instance->cachedMemory();

        uint64_t value = 0;
        String response;
        switch (localType.kind) {
        case TypeKind::I32: {
            void* address = (uint8_t*)memoryBase + local.i32;
            response = toLittleEndianHex(address);
            break;
        }
        default:
            RELEASE_ASSERT(false, "not supported");
            break;
        }

        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] qWasmLocal response: ", response, " value: ", RawPointer((void*)value));
        m_debugServer.sendReply(response);
    } else if (packetStartsWith(packet, "qWasmStackValue:"_s)) {
        // Format: qWasmStackValue:<frame-index>:<variable-index>
        // Reference: LLDB WebAssembly debugging extension
        // LLDB is asking: Get value from WebAssembly operand stack
        // WebAssembly Context: Access values on the WebAssembly execution stack for debugging
        // Reply Decision: Return stack value in little-endian hex format

        StringView params = packet.substring(strlen("qWasmStackValue:"));
        size_t separatorPos = params.find(';');

        if (separatorPos == notFound) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Malformed qWasmStackValue packet\n");
            m_debugServer.sendReply(""_s);
            return;
        }

        StringView frameIndexStr = params.substring(0, separatorPos);
        StringView variableIndexStr = params.substring(separatorPos + 1);

        CString frameIndexCStr = frameIndexStr.utf8();
        CString variableIndexCStr = variableIndexStr.utf8();
        unsigned long frameIndex = strtoul(frameIndexCStr.data(), nullptr, 10);
        unsigned long variableIndex = strtoul(variableIndexCStr.data(), nullptr, 10);

        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue frame=%lu, variable=%lu\n", frameIndex, variableIndex);

        // Stack values are only available for the current frame (frame 0)
        // Non-current frames don't have accessible stack state in our implementation
        if (frameIndex != 0) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue: frame index %lu not supported (stack only available for current frame)\n", frameIndex);
            m_debugServer.sendReply(""_s);
            return;
        }

        // Access stack value from current execution state
        auto stopReason = m_debugServer.m_executionHandler->stopReason();

        if (!stopReason.isValid() || !stopReason.stack || !stopReason.locals) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue: no valid execution state\n");
            m_debugServer.sendReply(""_s);
            return;
        }

        // Calculate stack depth (stack grows downward toward locals)
        constexpr size_t STACK_ENTRY_SIZE = 16;
        if (stopReason.stack > reinterpret_cast<IPInt::IPIntStackEntry*>(stopReason.locals)) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue: invalid stack pointers\n");
            m_debugServer.sendReply(""_s);
            return;
        }

        size_t stackDepth = (reinterpret_cast<uint8_t*>(stopReason.locals) - reinterpret_cast<uint8_t*>(stopReason.stack)) / STACK_ENTRY_SIZE;

        if (variableIndex >= stackDepth) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue: variable index %lu out of range (stack depth %zu)\n", variableIndex, stackDepth);
            m_debugServer.sendReply(""_s);
            return;
        }

        // Get the address of the stack entry (not the value itself)
        IPInt::IPIntStackEntry& stackEntry = stopReason.stack[variableIndex];
        uint64_t address = reinterpret_cast<uint64_t>(&stackEntry);

        // Convert address to little-endian hex string (8 bytes = 16 hex chars)
        String response = toLittleEndianHex(address);
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] qWasmStackValue response: %s\n", response.utf8().data());
        m_debugServer.sendReply(response);

    } else if (packetStartsWith(packet, "qXfer:auxv:read::"_s))
        m_debugServer.sendReply("l"_s); // Empty auxiliary vector
    else {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unknown query: '%s'\n", packet.utf8().data());
        m_debugServer.sendReply(""_s);
    }
}

void DebugServerQueryHandler::handleTargetXML()
{
    // Target XML should describe the WebAssembly target architecture
    // This tells LLDB the target is WebAssembly, which configures:
    //     Register mappings: WASM-specific registers
    //     Instruction decoding: WebAssembly disassembly
    //     Debugging capabilities: 32-bit WASM debugging features
    //     Memory layout: WASM32 address space conventions
    StringView xml = "l"
                     "<?xml version=\"1.0\"?>"_s
                     "<target>"
                     "<architecture>wasm32</architecture>"
                     "</target>";
    m_debugServer.sendReply(xml);
}

void DebugServerQueryHandler::handleProcessInfo()
{
    // Process info describes the WebAssembly target being debugged
    // Match WAMR exactly for maximum compatibility
    StringView processInfo = "pid:1;"
                             "parent-pid:1;"_s
                             "vendor:wamr;"
                             "ostype:wasi;"
                             "arch:wasm32;"
                             "triple:7761736d33322d77616d722d776173692d7761736d;"
                             "endian:little;"
                             "ptrsize:4;";
    m_debugServer.sendReply(processInfo);
}

void DebugServerQueryHandler::handleHostInfo()
{
    // Match WAMR exactly - they report WebAssembly architecture for both host and process
    // This approach maximizes compatibility with LLDB's WebAssembly debugging expectations
    StringView hostInfo = "vendor:wamr;"
                          "ostype:wasi;"_s
                          "arch:wasm32;"
                          "triple:7761736d33322d77616d722d776173692d7761736d;"
                          "endian:little;"
                          "ptrsize:4;";
    m_debugServer.sendReply(hostInfo);
}

void DebugServerQueryHandler::handleRegisterInfo(StringView packet)
{
    // For WASM32 architecture, provide WASM-specific register definitions
    // Field-by-field explanation:
    //     name:pc = Program Counter register for WASM32
    //     bitsize:32 = 32-bit register size for WASM32 architecture
    //     offset:0 = Located at byte offset 0 in register context block
    //     encoding:uint = Interpret contents as unsigned integer
    //     format:hex = Display in hexadecimal format by default
    //     set:General Purpose Registers = Belongs to GP register group
    //     gcc:0 = GCC compiler refers to this as register 0
    //     dwarf:0 = DWARF debug info refers to this as register 0

    StringView regNumStr = packet.substring(strlen("qRegisterInfo"));
    CString regNumCStr = regNumStr.utf8();
    int regNum = (int)strtol(regNumCStr.data(), nullptr, 16);

    if (regNum == 0) { // PC register - now 64-bit like WAMR
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Providing WASM PC register info for register 0 (64-bit like WAMR)\n");
        m_debugServer.sendReply("name:pc;alt-name:pc;bitsize:64;offset:0;encoding:uint;format:hex;set:General Purpose Registers;gcc:16;dwarf:16;generic:pc;"_s);
    } else {
        // Match WAMR exactly - return error for all other registers
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Returning error for register %d (matching WAMR)\n", regNum);
        m_debugServer.sendReply("E45"_s);
    }
}

bool DebugServerQueryHandler::parseLibrariesReadPacket(StringView packet, size_t& offset, size_t& maxSize)
{
    // Parse offset and length from packet
    StringView offsetPart = packet.substring(strlen("qXfer:libraries:read::"));
    size_t commaPos = offsetPart.find(',');
    if (commaPos == notFound)
        return false;

    StringView offsetStr = offsetPart.substring(0, commaPos);
    StringView maxSizeStr = offsetPart.substring(commaPos + 1);

    CString offsetCStr = offsetStr.utf8();
    CString maxSizeCStr = maxSizeStr.utf8();
    offset = strtoull(offsetCStr.data(), nullptr, 16);
    maxSize = strtoull(maxSizeCStr.data(), nullptr, 16);

    return true;
}

bool DebugServerQueryHandler::handleChunkedLibrariesResponse(size_t offset, size_t maxSize, String& response)
{
    String xmlData = m_debugServer.moduleManager().generateLibrariesXML();

    // Handle chunked response according to GDB Remote Protocol
    // 'm' prefix = more data follows
    // 'l' prefix = last chunk
    if (offset >= xmlData.length()) {
        response = "l"_s;
        return true;
    }

    size_t availableData = xmlData.length() - offset;
    size_t chunkSize = std::min(maxSize, availableData);

    String chunk = xmlData.substring(offset, chunkSize);
    bool isLastChunk = (offset + chunkSize >= xmlData.length());

    StringBuilder result;
    result.append(isLastChunk ? 'l' : 'm');
    result.append(chunk);
    response = result.toString();
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Chunked library response: %c offset=%zu, chunk_size=%zu, total=%u\n",
        (isLastChunk ? 'l' : 'm'), offset, chunkSize, xmlData.length());
    return true;
}

String DebugServerQueryHandler::buildWasmCallStackResponse()
{
    auto stopReason = m_debugServer.m_executionHandler->stopReason();
    RELEASE_ASSERT(stopReason.isValid() && stopReason.callFrame);
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] buildWasmCallStackResponse: starting manual stack walk from CallFrame %p\n", stopReason.callFrame);

    Vector<uint64_t> frameAddresses;
    frameAddresses.append(stopReason.virtualAddress);
    CallFrame* currentFrame = stopReason.callFrame;
    uint8_t* returnPC = nullptr;
    uint64_t virtualReturnPC = 0;
    unsigned frameIndex = 0;

    while (getWasmReturnPC(currentFrame, returnPC, virtualReturnPC) && frameIndex < 100) {
        frameAddresses.append(virtualReturnPC);
        currentFrame = currentFrame->callerFrame();
        frameIndex++;
    }

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] CallStack: finished walking call stack, processed %u frames\n", frameIndex);

    StringBuilder result;
    for (uint64_t address : frameAddresses)
        result.append(toLittleEndianHex(address));
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] buildWasmCallStackResponse: collected %zu frames, response length: %u\n", frameAddresses.size(), result.length());
    return result.toString();
}

} // namespace Wasm
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
