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
#include "WasmDebugServer.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "CallFrame.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyModule.h"
#include "NativeCallee.h"
#include "Options.h"
#include "VM.h"
#include "WasmBreakpointManager.h"
#include "WasmDebugServerExecutionHandler.h"
#include "WasmDebugServerMemoryHandler.h"
#include "WasmDebugServerQueryHandler.h"
#include "WasmDebugServerRegisterHandler.h"
#include "WasmIPIntSlowPaths.h"
#include "WasmModuleInformation.h"
#include "WasmModuleManager.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#if OS(WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <wtf/Assertions.h>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {
namespace Wasm {

// Helper function to send error reply with proper error code
static inline StringView getErrorReply(ProtocolError error)
{
    switch (error) {
    case ProtocolError::InvalidPacket:
        return "E01"_s;
    case ProtocolError::InvalidAddress:
        return "E02"_s;
    case ProtocolError::InvalidRegister:
        return "E03"_s;
    case ProtocolError::MemoryError:
        return "E04"_s;
    case ProtocolError::UnknownCommand:
        return "E05"_s;
    default:
        return "E00"_s;
    }
}

bool getWasmReturnPC(CallFrame* currentFrame, uint8_t*& returnPC, uint64_t& virtualReturnPC)
{
    CallFrame* callerFrame = currentFrame->callerFrame();
    auto* caller = callerFrame->callee().asNativeCallee();
    if (caller->category() != NativeCallee::Category::Wasm)
        return false;

    auto* wasmCaller = static_cast<const Wasm::Callee*>(caller);
    if (wasmCaller->compilationMode() != Wasm::CompilationMode::IPIntMode)
        return false;

    // Read the WebAssembly return PC from IPInt's saved PC location (cfr-8)
    // This contains the WebAssembly bytecode address where execution should continue in the caller
    uint8_t* pcLocation = reinterpret_cast<uint8_t*>(currentFrame) - 8;
    memcpy(&returnPC, pcLocation, sizeof(returnPC));

    JSWebAssemblyInstance* callerInstance = callerFrame->wasmInstance();
    auto* ipintCaller = static_cast<const Wasm::IPIntCallee*>(wasmCaller);
    virtualReturnPC = WASMModuleManager::physicalToVirtual(callerInstance->jsModule(), ipintCaller->functionIndex(), returnPC);
    return virtualReturnPC != 0;
}

DebugServer& DebugServer::singleton()
{
    static NeverDestroyed<DebugServer> instance;
    return instance.get();
}

DebugServer::DebugServer()
    : m_queryHandler(WTF::makeUnique<DebugServerQueryHandler>(*this))
    , m_memoryHandler(WTF::makeUnique<DebugServerMemoryHandler>(*this))
    , m_registerHandler(WTF::makeUnique<DebugServerRegisterHandler>(*this))
{
}

bool DebugServer::startServer(VM* vm)
{
    if (isSocketValid(m_serverSocket)) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Server already running");
        return false;
    }
    if (!createServerSocket())
        return false;
    if (!bindAndListenOnPort(m_port)) {
        cleanup();
        return false;
    }

    m_vm = vm;

    // Initialize cached mutator thread ID to avoid VM lock during debugging
    auto ownerThread = vm->ownerThread();
    RELEASE_ASSERT(ownerThread && *ownerThread);
    m_mutatorThreadId = (*ownerThread)->uid();

    m_moduleManager = WTF::makeUnique<WASMModuleManager>(*vm);
    m_breakpointManager = WTF::makeUnique<WASMBreakpointManager>();
    m_executionHandler = WTF::makeUnique<DebugServerExecutionHandler>(*this, *m_moduleManager, *m_breakpointManager);

    startAcceptThread();

    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] WASM Debug Server listening on port %d\n", m_port);
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Using ARM64 register mapping\n");
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Connect with: lldb -o 'gdb-remote localhost:%d'\n", m_port);
    return true;
}

bool DebugServer::createServerSocket()
{
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (!isSocketValid(m_serverSocket)) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Failed to create socket");
        return false;
    }

    // Set socket options for better reusability
    int opt = 1;
#if OS(WINDOWS)
    const char* optPtr = reinterpret_cast<const char*>(&opt);
#else
    const void* optPtr = &opt;
#endif
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, optPtr, sizeof(opt)) < 0) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Warning: Failed to set SO_REUSEADDR");
        // Continue anyway, this is not critical
    }

    return true;
}

bool DebugServer::bindAndListenOnPort(uint16_t port)
{
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(m_serverSocket, (sockaddr*)&address, sizeof(address)) < 0) {
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Failed to bind socket to port %d\n", port);
        return false;
    }

    if (listen(m_serverSocket, 1) < 0) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Failed to listen on socket");
        return false;
    }

    return true;
}

void DebugServer::startAcceptThread()
{
    WTF::Thread::create("WasmDebugServer", [this]() {
        m_debugServerThreadId = Thread::currentSingleton().uid();

        while (isSocketValid(m_serverSocket)) {
            dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Server socket is valid");
            sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            SocketType clientSocket = accept(m_serverSocket, (sockaddr*)&clientAddr, &clientLen);
            if (isSocketValid(clientSocket)) {
                m_clientSocket = clientSocket;
                handleClient();
            } else {
                if (!isSocketValid(m_serverSocket)) {
                    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Server socket was intentionally stopped");
                    break;
                }
                dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Accept failed, continuing...");
            }
        }
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Server socket is invalid");
    })->detach();
}

void DebugServer::closeSocket(SocketType& socket)
{
    ASSERT(&socket == &m_serverSocket || &socket == &m_clientSocket);
    if (isSocketValid(socket)) {
#if OS(WINDOWS)
        ::closesocket(socket);
#else
        ::close(socket);
#endif
        socket = INVALID_SOCKET_VALUE;
    }
}

void DebugServer::cleanup()
{
    closeSocket(m_serverSocket);
    closeSocket(m_clientSocket);
    m_noAckMode = false;
    m_vm = nullptr;
    m_mutatorThreadId = 0;
    m_debugServerThreadId = 0;
}

void DebugServer::stopServer()
{
    cleanup();
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] LLDB Server is stopped");
}

void DebugServer::handleClient()
{
    ASSERT(isSocketValid(m_clientSocket));

    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] LLDB connected, starting client handler - process continues running normally");

    auto cleanup = makeScopeExit([&] {
        clearAllBreakpoints();
    });

    // Send initial acknowledgment - LLDB expects this immediately
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html#Packet-Acknowledgment
    sendAck();

    // Use dynamic buffer for receiving data - start with reasonable size and can grow if needed
    constexpr size_t INITIAL_RECV_BUFFER_SIZE = 4096;
    auto receiveBuffer = makeUniqueArray<char>(INITIAL_RECV_BUFFER_SIZE);

    while (true) {
        int bytesRead = static_cast<int>(recv(m_clientSocket, receiveBuffer.get(), INITIAL_RECV_BUFFER_SIZE - 1, 0));
        if (bytesRead <= 0) {
            dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Client disconnected (bytesRead=%d)\n", bytesRead);
            break;
        }

        receiveBuffer[bytesRead] = '\0';
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Received raw: '%s' (%d bytes)\n", receiveBuffer.get(), bytesRead);

        // Process each character in the buffer
        // GDB Remote Protocol packet format: $<data>#<checksum>
        // Where <checksum> is 2-digit hex checksum of <data>
        for (int i = 0; i < bytesRead; i++) {
            char character = receiveBuffer[i];

            // Handle interrupt character (0x03 = Ctrl+C)
            // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Interrupts.html
            if (character == 0x03) {
                dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Received Ctrl+C interrupt - triggering stack overflow");
                m_executionHandler->interrupt();
                continue;
            }

            // Handle ACK/NACK characters ('+' = ACK, '-' = NACK)
            // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packet-Acknowledgment.html
            if (character == '+' || character == '-')
                continue;

            if (character == '$') {
                char* packetStart = &receiveBuffer[i];
                char* packetEnd = strchr(packetStart, '#');
                if (packetEnd && (packetEnd - receiveBuffer.get() + 2) < bytesRead) {
                    // Extract packet data between '$' and '#'
                    size_t packetLen = packetEnd - packetStart - 1;
                    // Use dynamic allocation for packet data too
                    auto packetData = makeUniqueArray<char>(packetLen + 1);
                    strncpy(packetData.get(), packetStart + 1, packetLen);
                    packetData[packetLen] = '\0';
                    handlePacket(StringView::fromLatin1(packetData.get()));
                    // Skip past '#XX' (checksum is 2 hex digits)
                    i = packetEnd - receiveBuffer.get() + 2;
                }
            }
        }
    }

    closeSocket(m_clientSocket);
    m_noAckMode = false;
    dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] LLDB disconnected");
}

void DebugServer::handlePacket(StringView packet)
{
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Processing packet: '%s'\n", packet.utf8().data());

    // Validate packet before processing
    if (packet.isEmpty()) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Empty packet received");
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    sendAck(); // Send ACK first if needed

    // Handle general queries
    if (packetStartsWith(packet, "Q"_s) || packetStartsWith(packet, "q"_s)) {
        // Format: q<query>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html
        // LLDB is asking: General query packets
        // WebAssembly Context: Various queries about WASM debugging capabilities
        // Reply Decision: Handled by dedicated function
        m_queryHandler->handleGeneralQuery(packet);
    } else if (packetStartsWith(packet, "j"_s)) {
        // JSON packets also go to query handler
        m_queryHandler->handleGeneralQuery(packet);
    }

    // Handle register queries
    else if (packetStartsWith(packet, "p"_s) && packet.length() > 1) {
        // Format: p<register_number>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#read-registers-packet
        // LLDB is asking: Read single register - handle basic register reads
        // WebAssembly Context: Register reads for WASM debugging, primarily PC/RIP for breakpoint handling
        // Reply Decision: Handled by dedicated function
        m_registerHandler->handleGetRegister(packet);
    } else if (packetStartsWith(packet, "P"_s)) {
        // Format: P<register>=<value>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#write-register-packet
        // LLDB is asking: Write single register
        // WebAssembly Context: Register writes for WASM debugging
        // Reply Decision: Handled by register handler
        m_registerHandler->handleSetRegister(packet);
    } else if (packetEquals(packet, "g"_s)) {
        // Format: g
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#read-registers-packet
        // LLDB is asking: Read general registers
        // WebAssembly Context: Get all register state for WASM debugging
        // Reply Decision: Handled by dedicated function
        m_registerHandler->handleGetAllRegisters(packet);
    }

    // TODO: Frame Variable Handler (LLDB frame variable support)
    else if (packetStartsWith(packet, "qfThreadInfo"_s)) {
        // Format: qfThreadInfo
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qfThreadInfo
        // LLDB is asking: Query first thread info for frame variable support
        // WebAssembly Context: WASM runs in single thread, use actual mutator thread ID for consistency
        // Reply Decision: Use actual JSC mutator thread ID
        uint64_t mutatorThreadId = m_mutatorThreadId;
        String reply = makeString("m"_s, hex(mutatorThreadId, Lowercase));
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Handling qfThreadInfo for frame variable support - thread %p\n", (void*)mutatorThreadId);
        sendReply(reply);
    } else if (packetStartsWith(packet, "qsThreadInfo"_s)) {
        // Format: qsThreadInfo
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#qsThreadInfo
        // LLDB is asking: Query subsequent thread info
        // WebAssembly Context: No more threads after main thread
        // Reply Decision: l - end of thread list
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling qsThreadInfo - end of thread list");
        sendReply("l"_s);
    }

    // Handle memory queries
    else if (packetStartsWith(packet, "m"_s)) {
        // Format: m<addr>,<length>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#read-memory-packet
        // LLDB is asking: Read memory
        // WebAssembly Context: Read WASM bytecode or compiled code from memory
        // Reply Decision: Handled by memory handler
        m_memoryHandler->handleReadMemory(packet);
    } else if (packetStartsWith(packet, "M"_s)) {
        // Format: M<addr>,<length>:<data>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#write-memory-packet
        // LLDB is asking: Write memory
        // WebAssembly Context: Write to WASM memory space
        // Reply Decision: Handled by memory handler
        m_memoryHandler->handleWriteMemory(packet);
    }

    // Handle execution queries
    else if (packetStartsWith(packet, "vCont?"_s)) {
        // Format: vCont?
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#vCont-packet
        // LLDB is asking: Query supported continue variants
        // WebAssembly Context: WASM execution supports basic continue operations, we support continue (c) and continue with signal (C) for WASM breakpoint handling
        // Reply Decision: vCont;c;C - WASM debugger supports continue and continue with signal
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling vCont query");
        sendReply("vCont;c;C;s;S;"_s); // Match WAMR exactly
    } else if (packetEquals(packet, "k"_s)) {
        // Format: k
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#kill-packet
        // LLDB is asking: Kill/detach request
        // WebAssembly Context: LLDB wants to disconnect from WASM debugging session
        // Reply Decision: Gracefully close client connection
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Received kill packet - closing client connection");
        closeSocket(m_clientSocket);
        return;
    } else if (packetStartsWith(packet, "Z"_s)) {
        // Format: Z<type>,<address>,<length>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#insert-breakpoint-or-watchpoint-packet
        // LLDB is asking: Insert breakpoint/watchpoint
        // WebAssembly Context: Set breakpoint in WASM function at specified address
        // Reply Decision: Handled by execution handler
        m_executionHandler->setBreakpoint(packet);
    } else if (packetStartsWith(packet, "z"_s)) {
        // Format: z<type>,<address>,<length>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#remove-breakpoint-or-watchpoint-packet
        // LLDB is asking: Remove breakpoint/watchpoint
        // WebAssembly Context: Remove breakpoint from WASM function
        // Reply Decision: Handled by execution handler
        m_executionHandler->removeBreakpoint(packet);
    } else if (packetEquals(packet, "c"_s) || packetStartsWith(packet, "c"_s)) {
        // Format: c[addr] or C<signal>[;addr]
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#continue-packet
        // LLDB is asking: Continue execution
        // WebAssembly Context: Resume WASM execution after breakpoint
        // Reply Decision: Handled by execution handler
        m_executionHandler->resume();
    } else if (packetStartsWith(packet, "vCont;c"_s)) {
        // Format: vCont;c or vCont;c:thread-id
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#vCont-packet
        // LLDB is asking: Continue execution using vCont packet
        // WebAssembly Context: Resume WASM execution after breakpoint
        // Reply Decision: Handled by execution handler
        m_executionHandler->resume();
    } else if (packetEquals(packet, "s"_s) || packetStartsWith(packet, "s"_s) || packetStartsWith(packet, "S"_s)) {
        // Format: s[addr] or S<signal>[;addr]
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#step-packet
        // LLDB is asking: Single step execution
        // WebAssembly Context: Step through WASM execution
        // Reply Decision: Handled by execution handler
        m_executionHandler->step(packet);
    } else if (packetStartsWith(packet, "vCont;s"_s)) {
        // Format: vCont;s or vCont;s:thread-id
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#vCont-packet
        // LLDB is asking: Single step execution using vCont packet (like WAMR)
        // WebAssembly Context: Step through WASM execution with thread ID
        // Reply Decision: Handled by execution handler
        m_executionHandler->step(packet);
    } else if (packetStartsWith(packet, "vCont;S"_s)) {
        // Format: vCont;S<signal> or vCont;S<signal>:thread-id
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#vCont-packet
        // LLDB is asking: Single step execution with signal using vCont packet
        // WebAssembly Context: Step through WASM execution with signal and thread ID
        // Reply Decision: Handled by execution handler
        m_executionHandler->step(packet);
    } else if (packetStartsWith(packet, "vCont;t"_s)) {
        // Format: vCont;t:thread-id
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#vCont-packet
        // LLDB is asking: Stop/interrupt execution
        // WebAssembly Context: Interrupt WASM execution (equivalent to Ctrl+C)
        // Reply Decision: Handle as Ctrl+C interrupt
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Received vCont interrupt packet - treating as Ctrl+C");
        m_executionHandler->interrupt();
    }

    // Handle other queries
    else if (packetStartsWith(packet, "vMustReplyEmpty"_s)) {
        // Format: vMustReplyEmpty
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html
        // LLDB is asking: Testing for features we don't implement
        // WebAssembly Context: LLDB probing for features we don't implement, empty reply prevents LLDB from trying unsupported WASM debugging features
        // Reply Decision: Empty - WASM debugger doesn't support this feature
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling vMustReplyEmpty");
        sendReply(""_s);
    } else if (packetStartsWith(packet, "Hg"_s)) {
        // Format: Hg<thread-id> or Hc<thread-id>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#thread-id-syntax
        // LLDB is asking: Set thread for subsequent operations (g=general, c=continue)
        // WebAssembly Context: WASM runs in single thread, so thread selection is simple, we acknowledge but always use the main thread for WASM execution
        // Reply Decision: OK - WASM debugger uses main thread (thread 1) for all operations
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling Hg (set thread)");
        sendReplyOK();
    } else if (packetStartsWith(packet, "H"_s)) {
        // Format: H<op><thread-id>
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#thread-id-syntax
        // LLDB is asking: Set thread for subsequent operations
        // WebAssembly Context: Thread selection for WASM operations
        // Reply Decision: Handled by dedicated function
        handleThreadInfo(packet);
    } else if (packetEquals(packet, "!"_s)) {
        // Format: !
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html#extended-mode
        // LLDB is asking: Enable extended mode for process control
        // WebAssembly Context: Extended mode allows LLDB to launch/attach to processes, for WASM debugging we're already attached to the JSC process running WASM
        // Reply Decision: OK - WASM debugger supports extended mode (already attached to JSC)
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling extended mode");
        sendReplyOK();
    } else if (packetEquals(packet, "?"_s)) {
        // Format: ?
        // Indicate the reason the target halted
        // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Stop-Reply-Packets.html
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] Handling halt reason query");
        m_executionHandler->interrupt();
    } else {
        // Unsupported packet - reply with empty response
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Unsupported packet: '%s'\n", packet.utf8().data());
        sendReply(""_s);
    }
}

void DebugServer::handleThreadInfo(StringView)
{
    // Format: H<op><thread-id>
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/General-Query-Packets.html#thread-id-syntax
    // LLDB is asking: Set thread for subsequent operations
    // WebAssembly Context: WASM runs in single main thread, so thread selection is straightforward
    // Reply Decision: OK - acknowledge thread selection (we only have one thread)
    sendReplyOK();
}

void DebugServer::sendReply(StringView reply) { m_executionHandler->sendReply(reply); }

void DebugServer::sendAck()
{
    // Sends a raw '+' character to LLDB to acknowledge receipt of a packet.
    // This is part of the GDB remote protocol flow control mechanism and indicates
    // that the packet was received without error (checksum passed).
    // Should only be used before replying to a packet, and skipped when NoAckMode is active.
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packet-Acknowledgment.html
    if (m_noAckMode)
        return;
    sendReply("+"_s);
}

void DebugServer::sendReplyOK()
{
    // Sends a full GDB remote protocol reply packet containing 'OK'.
    // This indicates successful completion of a request (e.g., QStartNoAckMode, Hg, etc.).
    // The reply is formatted as a full packet: $OK#checksum.
    // This is distinct from the '+' ACK, as it carries semantic meaning in the protocol.
    sendReply("OK"_s);
}

void DebugServer::sendErrorReply(ProtocolError error)
{
    sendReply(getErrorReply(error));
}

void DebugServer::trackModule(JSWebAssemblyModule* module)
{
    if (!m_vm || !m_moduleManager)
        return;
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Tracking WebAssembly module: %p\n", module);
    uint64_t virtualAddress = m_moduleManager->registerModule(module);
    if (virtualAddress && isSocketValid(m_clientSocket)) {
        // Module notification to LLDB happens through memory map queries
        dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Module registered at virtual address %p\n", (void*)virtualAddress);
    }
}

void DebugServer::updateRegisterState(uint64_t pc, uint64_t sp, uint64_t fp)
{
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Updating register state: PC=%p, SP=%p, FP=%p\n", (void*)pc, (void*)sp, (void*)fp);

    if (!m_registerHandler) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[Debugger] ERROR: Register handler is null - cannot update register state");
        return;
    }

    m_registerHandler->setRegisterValue(static_cast<WasmRegister>(0), pc, 8); // 64-bit PC register (WAMR compatible)
    m_registerHandler->setRegisterValue(static_cast<WasmRegister>(1), sp, 4); // 32-bit SP register
    m_registerHandler->setRegisterValue(static_cast<WasmRegister>(2), fp, 4); // 32-bit FP register
    dataLogFIf(Options::verboseWasmDebugger(), "[Debugger] Register state update completed: PC=%p stored\n", (void*)pc);
}

bool DebugServer::interruptRequested() const { return m_vm && m_vm->isStopWorldActive(); }
void DebugServer::clearAllBreakpoints() { m_breakpointManager->clearAllBreakpoints(); }

void DebugServer::handleThreadStopInfo(StringView packet) { m_executionHandler->handleThreadStopInfo(packet); }

bool DebugServer::stopCode(CallFrame* callFrame, JSWebAssemblyInstance* instance, IPIntCallee* callee, uint8_t* pc, uint8_t* mc, IPInt::IPIntLocal* locals, IPInt::IPIntStackEntry* stack) { return m_executionHandler->stopCode(callFrame, instance, callee, pc, mc, locals, stack); }

void DebugServer::setInterruptBreakpoint(JSWebAssemblyModule* module, IPIntCallee* callee)
{
    return m_executionHandler->setInterruptBreakpoint(module, callee);
}

}
} // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
