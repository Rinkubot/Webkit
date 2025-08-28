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
#include "WasmDebugServerRegisterHandler.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "Options.h"
#include "WasmDebugServer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DebugServerRegisterHandler);

void DebugServerRegisterHandler::handleGetAllRegisters(StringView)
{
    // For WASM32 architecture, provide minimal register set
    // WASM32 register layout: PC=0, SP=1
    StringBuilder registers;

    // PC (Program Counter) - register 0 for WASM32
    RegisterValue pcValue = getRegisterValue(static_cast<WasmRegister>(0));
    if (pcValue.valid && pcValue.value) {
        char buffer[16];
        SAFE_SPRINTF(std::span(buffer), "%08x", static_cast<uint32_t>(pcValue.value));
        registers.append(String::fromLatin1(buffer));
        dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> All registers: Using actual PC: 0x%x\n",
            static_cast<uint32_t>(pcValue.value));
    } else {
        // Fallback to base address if no PC is set
        registers.append("10000000"_s); // 0x10000000 in little-endian
        dataLogLnIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> All registers: No PC set, using base address");
    }

    // SP (Stack Pointer) - register 1 for WASM32
    RegisterValue spValue = getRegisterValue(static_cast<WasmRegister>(1));
    if (spValue.valid && spValue.value) {
        char buffer[16];
        SAFE_SPRINTF(std::span(buffer), "%08x", static_cast<uint32_t>(spValue.value));
        registers.append(String::fromLatin1(buffer));
    } else {
        registers.append("00000000"_s);
    }

    m_debugServer.sendReply(registers.toString());
}

void DebugServerRegisterHandler::handleGetRegister(StringView packet)
{
    // Format: p<register_number>
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#read-registers-packet
    // LLDB is asking: Read single register value
    // WebAssembly Context: Provide register values for WASM32 debugging
    // Reply Decision: Register value in little-endian hex format

    // Parse packet format: p<register_number>
    StringView regNumStr = packet.substring(1);
    CString regNumCStr = regNumStr.utf8();
    int regNum = static_cast<int>(strtol(regNumCStr.data(), nullptr, 16));
    dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Reading register %d (0x%x)\n", regNum, regNum);

    // WASM32 register layout: PC=0, SP=1
    // For WebAssembly debugging, we primarily care about PC (Program Counter)

    if (regNum == 0) { // PC (Program Counter) - now 64-bit like WAMR
        RegisterValue pcValue = getRegisterValue(static_cast<WasmRegister>(0));
        dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> WASM PC query (reg 0): valid=%d, value=%p\n", pcValue.valid, (void*)pcValue.value);

        if (pcValue.valid && pcValue.value) {
            // Use WAMR-compatible 64-bit addressing with high base address
            constexpr uint64_t WAMR_ADDRESS_BASE = 0x4000000000000000ULL;

            uint64_t wasmAddress;
            if (pcValue.value >= WAMR_ADDRESS_BASE) {
                // Already in WAMR format
                wasmAddress = pcValue.value;
            } else {
                // Convert to WAMR format: high base + offset
                wasmAddress = WAMR_ADDRESS_BASE + pcValue.value;
            }

            // Send 64-bit address in little-endian format (like WAMR)
            char buffer[20];
            SAFE_SPRINTF(std::span(buffer), "%02x%02x%02x%02x%02x%02x%02x%02x",
                static_cast<uint8_t>(wasmAddress & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 8) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 16) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 24) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 32) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 40) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 48) & 0xFF),
                static_cast<uint8_t>((wasmAddress >> 56) & 0xFF));
            m_debugServer.sendReply(StringView::fromLatin1(buffer));
            dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> WASM PC register 0: returning %p as %s (64-bit WASM address)\n", (void*)wasmAddress, buffer);
        } else {
            // Fallback to zero if no PC is set
            m_debugServer.sendReply("0000000000000000"_s); // 64-bit zero
            dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> WASM PC register 0: No PC set, returning 0x0\n");
        }
    } else if (regNum == 1) { // SP (Stack Pointer) for WASM32
        RegisterValue spValue = getRegisterValue(static_cast<WasmRegister>(1));
        if (spValue.valid && spValue.value) {
            char buffer[16];
            uint32_t sp = static_cast<uint32_t>(spValue.value);
            SAFE_SPRINTF(std::span(buffer), "%02x%02x%02x%02x",
                static_cast<uint8_t>(sp & 0xFF),
                static_cast<uint8_t>((sp >> 8) & 0xFF),
                static_cast<uint8_t>((sp >> 16) & 0xFF),
                static_cast<uint8_t>((sp >> 24) & 0xFF));
            m_debugServer.sendReply(StringView::fromLatin1(buffer));
        } else
            m_debugServer.sendReply("00000000"_s);
    } else {
        // Return zero for all other registers in little-endian format (32-bit)
        m_debugServer.sendReply("00000000"_s);
    }
}

void DebugServerRegisterHandler::handleSetRegister(StringView packet)
{
    if (packet.isEmpty())
        return;

    // Parse set register packet (format: P<hex-reg-num>=<hex-value>)
    if (!packet.startsWith('P')) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    size_t equalPos = packet.find('=');
    if (equalPos == notFound) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    // Parse register number
    StringView regNumStr = packet.substring(1, equalPos - 1);
    CString regNumCStr = regNumStr.utf8();
    uint32_t regNum = static_cast<uint32_t>(strtoul(regNumCStr.data(), nullptr, 16));

    if (!isValidRegister(regNum)) {
        sendErrorReply(ProtocolError::InvalidRegister);
        return;
    }

    // Parse register value
    StringView valueStr = packet.substring(equalPos + 1);
    CString valueCStr = valueStr.utf8();
    uint64_t value = strtoull(valueCStr.data(), nullptr, 16);
    uint32_t size = getRegisterSize(static_cast<WasmRegister>(regNum));

    setRegisterValue(static_cast<WasmRegister>(regNum), value, size);

    m_debugServer.sendReplyOK();
}

void DebugServerRegisterHandler::handleRegisterInfo(StringView packet)
{
    UNUSED_PARAM(packet);

    // LLDB requests register information in XML format
    // For WebAssembly debugging, we provide minimal register set information
    m_debugServer.sendReply(""_s);
}

void DebugServerRegisterHandler::setRegisterValue(WasmRegister reg, uint64_t value, uint32_t size)
{
    uint32_t regNum = static_cast<uint32_t>(reg);

    dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Setting register %u to value %p (size: %u)\n", regNum, (void*)value, size);

    // Validate register number
    if (regNum >= static_cast<uint32_t>(WasmRegister::MaxRegister)) {
        dataLogLnIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Invalid register number: ", regNum);
        return;
    }

    // Use UncheckedKeyHashMap which should handle uint32_t keys properly
    m_registers.set(regNum, RegisterValue(value, size));
}

RegisterValue DebugServerRegisterHandler::getRegisterValue(WasmRegister reg) const
{
    uint32_t regNum = static_cast<uint32_t>(reg);
    auto it = m_registers.find(regNum);
    if (it != m_registers.end())
        return it->value;

    // Return default value for unset registers
    return RegisterValue(0, getRegisterSize(reg));
}

void DebugServerRegisterHandler::clearAllRegisters()
{
    m_registers.clear();
    m_stackDepth = 0;
}

bool DebugServerRegisterHandler::isValidRegister(uint32_t regNum) const
{
    return regNum < static_cast<uint32_t>(WasmRegister::MaxRegister);
}

StringView DebugServerRegisterHandler::getRegisterName(WasmRegister reg) const
{
    switch (reg) {
    case WasmRegister::Pc:
        return "pc"_s;
    case WasmRegister::Sp:
        return "sp"_s;
    case WasmRegister::Fp:
        return "fp"_s;
    case WasmRegister::Stack0:
        return "stack0"_s;
    case WasmRegister::Stack1:
        return "stack1"_s;
    case WasmRegister::Stack2:
        return "stack2"_s;
    case WasmRegister::Stack3:
        return "stack3"_s;
    case WasmRegister::Stack4:
        return "stack4"_s;
    case WasmRegister::Stack5:
        return "stack5"_s;
    case WasmRegister::Stack6:
        return "stack6"_s;
    case WasmRegister::Stack7:
        return "stack7"_s;
    case WasmRegister::Local0:
        return "local0"_s;
    case WasmRegister::Local1:
        return "local1"_s;
    case WasmRegister::Local2:
        return "local2"_s;
    case WasmRegister::Local3:
        return "local3"_s;
    case WasmRegister::Local4:
        return "local4"_s;
    case WasmRegister::Local5:
        return "local5"_s;
    case WasmRegister::Local6:
        return "local6"_s;
    case WasmRegister::Local7:
        return "local7"_s;
    case WasmRegister::Global0:
        return "global0"_s;
    case WasmRegister::Global1:
        return "global1"_s;
    case WasmRegister::Global2:
        return "global2"_s;
    case WasmRegister::Global3:
        return "global3"_s;
    default:
        return "unknown"_s;
    }
}

uint32_t DebugServerRegisterHandler::getRegisterSize(WasmRegister reg) const
{
    // Updated for WAMR-compatible 64-bit PC register
    switch (reg) {
    case WasmRegister::Pc:
        return 8; // 64-bit for WAMR compatibility
    case WasmRegister::Sp:
    case WasmRegister::Fp:
        return 4; // 32-bit values for WASM32
    default:
        return 4; // 32-bit values by default
    }
}

void DebugServerRegisterHandler::pushStackValue(uint64_t value, uint32_t size)
{
    if (m_stackDepth < MAX_STACK_DEPTH) {
        WasmRegister stackReg = static_cast<WasmRegister>(static_cast<uint32_t>(WasmRegister::Stack0) + m_stackDepth);
        setRegisterValue(stackReg, value, size);
        m_stackDepth++;
    }
}

uint64_t DebugServerRegisterHandler::popStackValue()
{
    if (m_stackDepth > 0) {
        m_stackDepth--;
        WasmRegister stackReg = static_cast<WasmRegister>(static_cast<uint32_t>(WasmRegister::Stack0) + m_stackDepth);
        RegisterValue regValue = getRegisterValue(stackReg);
        return regValue.value;
    }
    return 0;
}

void DebugServerRegisterHandler::clearStack()
{
    for (uint32_t i = 0; i < MAX_STACK_DEPTH; i++) {
        uint32_t regNum = static_cast<uint32_t>(WasmRegister::Stack0) + i;
        m_registers.remove(regNum);
    }
    m_stackDepth = 0;
}

void DebugServerRegisterHandler::setLocalValue(uint32_t index, uint64_t value, uint32_t size)
{
    if (index < 8) { // Support up to 8 locals
        WasmRegister localReg = static_cast<WasmRegister>(static_cast<uint32_t>(WasmRegister::Local0) + index);
        setRegisterValue(localReg, value, size);
    }
}

uint64_t DebugServerRegisterHandler::getLocalValue(uint32_t index) const
{
    if (index < 8) {
        WasmRegister localReg = static_cast<WasmRegister>(static_cast<uint32_t>(WasmRegister::Local0) + index);
        RegisterValue regValue = getRegisterValue(localReg);
        return regValue.value;
    }
    return 0;
}

void DebugServerRegisterHandler::handleFrameVariable(StringView packet)
{
    // Format: Various frame variable query packets from LLDB
    // Reference: LLDB frame variable implementation uses DWARF debug info and register queries
    // LLDB is asking: Frame variable information for current stack frame
    // WebAssembly Context: Provide WASM local variables as frame variables
    // Reply Decision: Variable information in DWARF-compatible format

    dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Handling frame variable query: %s\n", packet.utf8().data());

    // For now, provide basic frame variable support by exposing locals as pseudo-registers
    // This allows LLDB's "frame variable" command to work with WASM locals

    if (!m_frameVariables.isEmpty()) {
        // Build response with current frame variables
        StringBuilder response;
        bool first = true;

        for (const auto& pair : m_frameVariables) {
            if (!first)
                response.append(',');
            first = false;

            // Format: name=value (simplified for now)
            response.append(pair.key);
            response.append('=');
            response.append(String::number(pair.value));
        }

        m_debugServer.sendReply(response.toString());
    } else {
        // No frame variables available
        m_debugServer.sendReply(""_s);
    }
}

void DebugServerRegisterHandler::setFrameVariables(const Vector<std::pair<String, uint64_t>>& variables)
{
    m_frameVariables.clear();

    for (const auto& var : variables) {
        m_frameVariables.set(var.first, var.second);
        dataLogFIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Set frame variable: %s = %p\n", var.first.utf8().data(), (void*)var.second);
    }

    // Also update corresponding local registers for consistency
    uint32_t localIndex = 0;
    for (const auto& var : variables) {
        if (localIndex < 8) // Support up to 8 locals
            setLocalValue(localIndex, var.second);
        localIndex++;
    }
}

void DebugServerRegisterHandler::clearFrameVariables()
{
    m_frameVariables.clear();
    dataLogLnIf(Options::verboseWasmDebugger(), "<DebugServerRegisterHandler> Cleared all frame variables");
}

void DebugServerRegisterHandler::sendRegisterData(const RegisterValue& regValue)
{
    if (!regValue.valid) {
        m_debugServer.sendReply("xxxxxxxx"_s); // Invalid register data
        return;
    }

    char buffer[32];
    if (regValue.size == 8) {
        SAFE_SPRINTF(std::span(buffer), "%016llx", static_cast<unsigned long long>(regValue.value));
    } else
        SAFE_SPRINTF(std::span(buffer), "%08x", static_cast<uint32_t>(regValue.value));

    m_debugServer.sendReply(StringView::fromLatin1(buffer));
}

void DebugServerRegisterHandler::sendAllRegistersData()
{
    // Send all registers in order
    StringBuilder allRegs;
    for (uint32_t i = 0; i < static_cast<uint32_t>(WasmRegister::MaxRegister); i++) {
        RegisterValue regValue = getRegisterValue(static_cast<WasmRegister>(i));

        char buffer[32];
        if (regValue.size == 8) {
            SAFE_SPRINTF(std::span(buffer), "%016llx", static_cast<unsigned long long>(regValue.value));
        } else
            SAFE_SPRINTF(std::span(buffer), "%08x", static_cast<uint32_t>(regValue.value));
        allRegs.append(String::fromLatin1(buffer));
    }

    m_debugServer.sendReply(allRegs.toString());
}

void DebugServerRegisterHandler::sendErrorReply(ProtocolError error)
{
    m_debugServer.sendErrorReply(error);
}

uint32_t DebugServerRegisterHandler::parseRegisterNumber(StringView packet)
{
    if (packet.isEmpty())
        return 0;

    CString packetCStr = packet.utf8();
    return static_cast<uint32_t>(strtoul(packetCStr.data(), nullptr, 16));
}

} // namespace Wasm
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
