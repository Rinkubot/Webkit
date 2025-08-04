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

#include "WasmDebugServerUtilities.h"

#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Wasm {

class DebugServer;

// WebAssembly register definitions are now in WasmDebugServerUtilities.h

struct RegisterValue {
    uint64_t value;
    uint32_t size; // Size in bytes (4 for i32/f32, 8 for i64/f64)
    bool valid;

    RegisterValue()
        : value(0)
        , size(4)
        , valid(false)
    {
    }

    RegisterValue(uint64_t val, uint32_t sz)
        : value(val)
        , size(sz)
        , valid(true)
    {
    }

    // Add explicit copy constructor and assignment operator
    RegisterValue(const RegisterValue& other)
        : value(other.value)
        , size(other.size)
        , valid(other.valid)
    {
    }

    RegisterValue& operator=(const RegisterValue& other)
    {
        if (this != &other) {
            value = other.value;
            size = other.size;
            valid = other.valid;
        }
        return *this;
    }

    // Add move constructor and assignment operator
    RegisterValue(RegisterValue&& other)
        : value(other.value)
        , size(other.size)
        , valid(other.valid)
    {
        other.valid = false;
    }

    RegisterValue& operator=(RegisterValue&& other)
    {
        if (this != &other) {
            value = other.value;
            size = other.size;
            valid = other.valid;
            other.valid = false;
        }
        return *this;
    }
};

class DebugServerRegisterHandler {
    WTF_MAKE_TZONE_ALLOCATED(DebugServerRegisterHandler);
public:
    DebugServerRegisterHandler(DebugServer& debugServer)
        : m_debugServer(debugServer)
    {
    }

    // Register operations
    void handleGetAllRegisters(StringView packet);
    void handleGetRegister(StringView packet);
    void handleSetRegister(StringView packet);
    void handleRegisterInfo(StringView packet);

    // Register management
    void setRegisterValue(WasmRegister, uint64_t value, uint32_t size = 4);
    RegisterValue getRegisterValue(WasmRegister) const;
    void clearAllRegisters();

    // Register queries
    bool isValidRegister(uint32_t regNum) const;
    StringView getRegisterName(WasmRegister) const;
    uint32_t getRegisterSize(WasmRegister) const;

    // Stack simulation
    void pushStackValue(uint64_t value, uint32_t size = 4);
    uint64_t popStackValue();
    void clearStack();
    uint32_t getStackDepth() const { return m_stackDepth; }

    // Local variable simulation
    void setLocalValue(uint32_t index, uint64_t value, uint32_t size = 4);
    uint64_t getLocalValue(uint32_t index) const;

    // Frame variable support (LLDB frame variable feature)
    void handleFrameVariable(StringView packet);
    void setFrameVariables(const Vector<std::pair<String, uint64_t>>&);
    void clearFrameVariables();

private:
    // Frame variable storage for LLDB frame variable support
    HashMap<String, uint64_t> m_frameVariables;

private:
    // Helper methods
    void sendRegisterData(const RegisterValue& regValue);
    void sendAllRegistersData();
    void sendErrorReply(ProtocolError);
    uint32_t parseRegisterNumber(StringView packet);

    // Reference to debug server for communication
    DebugServer& m_debugServer;

    // Register storage - use UncheckedKeyHashMap with proper traits for uint32_t keys
    UncheckedKeyHashMap<uint32_t, RegisterValue, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_registers;

    // Stack simulation
    uint32_t m_stackDepth { 0 };
    static constexpr uint32_t MAX_STACK_DEPTH = 8;
};

} // namespace Wasm
} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
