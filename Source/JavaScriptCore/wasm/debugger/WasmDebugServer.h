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

#pragma once

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "WasmDebugServerUtilities.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#if OS(WINDOWS)
#include <winsock2.h>
using SocketType = SOCKET;
static constexpr SocketType INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
using SocketType = int;
static constexpr SocketType INVALID_SOCKET_VALUE = -1;
#endif

namespace JSC {

class VM;
class CallFrame;
class JSWebAssemblyInstance;
class JSWebAssemblyModule;

namespace IPInt {
struct IPIntLocal;
struct IPIntStackEntry;
}

namespace Wasm {

// ----

bool getWasmReturnPC(CallFrame*, uint8_t*& returnPC, uint64_t& virtualReturnPC);

class DebugServerQueryHandler;
class DebugServerMemoryHandler;
class DebugServerRegisterHandler;
class DebugServerExecutionHandler;
class IPIntCallee;
class WASMModuleManager;
class WASMBreakpointManager;

class DebugServer {
    WTF_MAKE_TZONE_ALLOCATED(DebugServer);
public:
    JS_EXPORT_PRIVATE static DebugServer& singleton();
    DebugServer();
    ~DebugServer() = default;
    VM* vm() const { return m_vm; }
    uint64_t mutatorThreadId() const { return m_mutatorThreadId; }
    uint64_t debugServerThreadId() const { return m_debugServerThreadId; }
    WASMModuleManager& moduleManager()
    {
        RELEASE_ASSERT(m_moduleManager);
        return *m_moduleManager;
    }

    JS_EXPORT_PRIVATE bool startServer(VM*);
    JS_EXPORT_PRIVATE void stopServer();

    void trackModule(JSWebAssemblyModule*);

    void updateRegisterState(uint64_t pc, uint64_t sp, uint64_t fp);

    bool interruptRequested() const;

    void setInterruptBreakpoint(JSWebAssemblyModule*, IPIntCallee*);
    bool stopCode(CallFrame*, JSWebAssemblyInstance*, IPIntCallee*, uint8_t* pc, uint8_t* mc, IPInt::IPIntLocal* locals, IPInt::IPIntStackEntry* stack);
    void clearAllBreakpoints();

    void handleThreadStopInfo(StringView packet);

    void setPort(uint64_t port) { m_port = port; }

public:
    // Server lifecycle management
    bool createServerSocket();
    bool bindAndListenOnPort(uint16_t port);
    void startAcceptThread();
    void acceptClientConnections();
    void cleanup();
    void closeSocket(SocketType& socket);

    void handleClient();
    void handlePacket(StringView packet);

    void sendAck();
    void sendReplyOK();
    void sendReply(StringView reply);

    // Enhanced protocol handling
    void sendErrorReply(ProtocolError);

    // Packet handlers
    void handleThreadInfo(StringView packet);

    bool isSocketValid(SocketType clientSocket) const {
#if OS(WINDOWS)
        return clientSocket != INVALID_SOCKET_VALUE;
#else
        return clientSocket >= 0;
#endif
    }
    bool isActive() const { return isSocketValid(m_clientSocket); }

    friend class DebugServerQueryHandler;
    friend class DebugServerMemoryHandler;
    friend class DebugServerRegisterHandler;
    friend class DebugServerExecutionHandler;

    uint16_t m_port { 1234 };

    std::unique_ptr<DebugServerQueryHandler> m_queryHandler;
    std::unique_ptr<DebugServerMemoryHandler> m_memoryHandler;
    std::unique_ptr<DebugServerRegisterHandler> m_registerHandler;
    std::unique_ptr<DebugServerExecutionHandler> m_executionHandler;
    std::unique_ptr<WASMModuleManager> m_moduleManager;
    std::unique_ptr<WASMBreakpointManager> m_breakpointManager;

    SocketType m_serverSocket { INVALID_SOCKET_VALUE };
    SocketType m_clientSocket { INVALID_SOCKET_VALUE };
    bool m_noAckMode { false };

    VM* m_vm { nullptr };
    uint64_t m_mutatorThreadId { 0 };
    uint64_t m_debugServerThreadId { 0 };
};
} // namespace JSC
} // namespace Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
