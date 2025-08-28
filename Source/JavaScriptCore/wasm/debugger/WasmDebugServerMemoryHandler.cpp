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
#include "WasmDebugServerMemoryHandler.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "WasmDebugServer.h"
#include "WasmModuleManager.h"

#include "Options.h"
#include <cstdlib>
#include <cstring>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DebugServerMemoryHandler);

void DebugServerMemoryHandler::handleReadMemory(StringView packet)
{
    // Format: m<addr>,<length>
    // Reference: https://sourceware.org/gdb/onlinedocs/gdb/Packets.html#read-memory-packet
    // LLDB is asking: Read memory at specified address and length
    // WebAssembly Context: Read WASM bytecode/compiled code (virtual addresses) or variable data (real addresses)
    // Reply Decision: Hex-encoded memory data or E01 for invalid format

    size_t commaPos = packet.find(',', 1);
    if (commaPos != notFound) {
        StringView addressStr = packet.substring(1, commaPos - 1);
        StringView lengthStr = packet.substring(commaPos + 1);

        CString addressCStr = addressStr.utf8();
        CString lengthCStr = lengthStr.utf8();

        uint64_t address = strtoull(addressCStr.data(), nullptr, 16);
        size_t length = strtoul(lengthCStr.data(), nullptr, 16);
        StringBuilder data;

        // Check if this is a WebAssembly virtual address or a real physical address
        if (address >= WASMModuleManager::VIRTUAL_ADDRESS_BASE) {
            Vector<uint8_t> virtualData = m_debugServer.moduleManager().readSourceBinary(address, length);
            for (uint8_t byte : virtualData)
                data.append(hex(byte, 2, Lowercase));
        } else {
            const uint8_t* memPtr = reinterpret_cast<const uint8_t*>(address);
            for (size_t i = 0; i < length; i++)
                data.append(hex(memPtr[i], 2, Lowercase));
        }

        m_debugServer.sendReply(data.toString());
    } else
        m_debugServer.sendReply("E01"_s);
}

void DebugServerMemoryHandler::handleWriteMemory(StringView packet)
{
    if (packet.isEmpty())
        return;

    // Parse memory write packet (format: M<hex-address>,<hex-length>:<hex-data>)
    if (!packet.startsWith('M')) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    uint64_t address;
    Vector<uint8_t> data;
    if (!parseWriteMemoryPacket(packet.substring(1), address, data)) {
        sendErrorReply(ProtocolError::InvalidPacket);
        return;
    }

    if (!isWritableAddress(address)) {
        sendErrorReply(ProtocolError::MemoryError);
        return;
    }

    setMemoryContent(address, data);

    m_debugServer.sendReplyOK();
}

void DebugServerMemoryHandler::handleMemoryMap(StringView packet)
{
    UNUSED_PARAM(packet);
    sendMemoryMapData();
}

void DebugServerMemoryHandler::handleMemoryInfo(StringView packet)
{
    UNUSED_PARAM(packet);

    // Send basic memory information for LLDB debugging
    StringBuilder info;
    info.append("Memory regions: "_s);
    info.append(String::number(m_memoryRegions.size()));

    m_debugServer.sendReply(info.toString());
}

void DebugServerMemoryHandler::addMemoryRegion(uint64_t startAddress, uint64_t size, bool readable, bool writable, bool executable, StringView name)
{
    m_memoryRegions.append(MemoryRegion(startAddress, size, readable, writable, executable, name));
}

void DebugServerMemoryHandler::removeMemoryRegion(uint64_t startAddress)
{
    m_memoryRegions.removeAllMatching([startAddress](const MemoryRegion& region) {
        return region.startAddress == startAddress;
    });
}

void DebugServerMemoryHandler::clearMemoryRegions()
{
    m_memoryRegions.clear();
}

bool DebugServerMemoryHandler::isValidAddress(uint64_t address) const
{
    return findMemoryRegion(address) != nullptr;
}

bool DebugServerMemoryHandler::isReadableAddress(uint64_t address) const
{
    const MemoryRegion* region = findMemoryRegion(address);
    return region && region->readable;
}

bool DebugServerMemoryHandler::isWritableAddress(uint64_t address) const
{
    const MemoryRegion* region = findMemoryRegion(address);
    return region && region->writable;
}

const MemoryRegion* DebugServerMemoryHandler::findMemoryRegion(uint64_t address) const
{
    for (const auto& region : m_memoryRegions) {
        if (region.contains(address))
            return &region;
    }
    return nullptr;
}

void DebugServerMemoryHandler::setWasmMemoryBase(uint64_t baseAddress, uint64_t size)
{
    m_wasmMemoryBase = baseAddress;
    m_wasmMemorySize = size;
    addMemoryRegion(baseAddress, size, true, true, false, "wasm-memory"_s);
}

void DebugServerMemoryHandler::setWasmStackBase(uint64_t baseAddress, uint64_t size)
{
    m_wasmStackBase = baseAddress;
    m_wasmStackSize = size;
    addMemoryRegion(baseAddress, size, true, true, false, "wasm-stack"_s);
}

void DebugServerMemoryHandler::setWasmGlobalsBase(uint64_t baseAddress, uint64_t size)
{
    m_wasmGlobalsBase = baseAddress;
    m_wasmGlobalsSize = size;
    addMemoryRegion(baseAddress, size, true, true, false, "wasm-globals"_s);
}

void DebugServerMemoryHandler::setMemoryContent(uint64_t address, const Vector<uint8_t>& data)
{
    for (size_t i = 0; i < data.size(); i++) {
        m_memoryContent.set(address + i, data[i]);
    }
}

Vector<uint8_t> DebugServerMemoryHandler::getMemoryContent(uint64_t address, uint32_t size) const
{
    Vector<uint8_t> result;
    result.reserveInitialCapacity(size);

    for (uint32_t i = 0; i < size; i++) {
        auto it = m_memoryContent.find(address + i);
        if (it != m_memoryContent.end())
            result.append(it->value);
        else
            result.append(0); // Default to zero for uninitialized memory
    }

    return result;
}

bool DebugServerMemoryHandler::parseMemoryPacket(StringView packet, uint64_t& address, uint32_t& size)
{
    if (packet.isEmpty())
        return false;

    // Format: <hex-address>,<hex-length>
    size_t commaPos = packet.find(',');
    if (commaPos == notFound)
        return false;

    // Parse address
    StringView addressStr = packet.substring(0, commaPos);
    CString addressCStr = addressStr.utf8();
    char* endPtr;
    address = strtoull(addressCStr.data(), &endPtr, 16);
    if (*endPtr != '\0')
        return false;

    // Parse size
    StringView sizeStr = packet.substring(commaPos + 1);
    CString sizeCStr = sizeStr.utf8();
    size = static_cast<uint32_t>(strtoul(sizeCStr.data(), &endPtr, 16));
    if (*endPtr != '\0')
        return false;

    return true;
}

bool DebugServerMemoryHandler::parseWriteMemoryPacket(StringView packet, uint64_t& address, Vector<uint8_t>& data)
{
    if (packet.isEmpty())
        return false;

    // Format: <hex-address>,<hex-length>:<hex-data>
    size_t commaPos = packet.find(',');
    if (commaPos == notFound)
        return false;

    size_t colonPos = packet.find(':', commaPos);
    if (colonPos == notFound)
        return false;

    // Parse address
    StringView addressStr = packet.substring(0, commaPos);
    CString addressCStr = addressStr.utf8();
    char* endPtr;
    address = strtoull(addressCStr.data(), &endPtr, 16);
    if (*endPtr != '\0')
        return false;

    // Parse length
    StringView lengthStr = packet.substring(commaPos + 1, colonPos - commaPos - 1);
    CString lengthCStr = lengthStr.utf8();
    uint32_t length = static_cast<uint32_t>(strtoul(lengthCStr.data(), &endPtr, 16));
    if (*endPtr != '\0')
        return false;

    // Parse hex data
    StringView hexDataStr = packet.substring(colonPos + 1);
    data = hexStringToBytes(hexDataStr, length * 2);
    return data.size() == length;
}

void DebugServerMemoryHandler::sendMemoryData(const Vector<uint8_t>& data)
{
    String hexData = bytesToHexString(data);
    m_debugServer.sendReply(hexData);
}

void DebugServerMemoryHandler::sendMemoryMapData()
{
    StringBuilder mapData;
    for (const auto& region : m_memoryRegions) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%016llx-%016llx %c%c%c %s\n",
            static_cast<unsigned long long>(region.startAddress),
            static_cast<unsigned long long>(region.startAddress + region.size - 1),
            region.readable ? 'r' : '-',
            region.writable ? 'w' : '-',
            region.executable ? 'x' : '-',
            region.name.utf8().data());
        mapData.append(String::fromLatin1(buffer));
    }

    m_debugServer.sendReply(mapData.toString());
}

void DebugServerMemoryHandler::sendErrorReply(ProtocolError error)
{
    m_debugServer.sendErrorReply(error);
}

uint8_t DebugServerMemoryHandler::hexCharToValue(char c) const
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

char DebugServerMemoryHandler::valueToHexChar(uint8_t value) const
{
    if (value < 10)
        return '0' + value;
    return 'a' + (value - 10);
}

Vector<uint8_t> DebugServerMemoryHandler::hexStringToBytes(StringView hexString, size_t length) const
{
    Vector<uint8_t> result;
    if (hexString.isEmpty() || length % 2 != 0 || hexString.length() < length)
        return result;

    result.reserveInitialCapacity(length / 2);

    for (size_t i = 0; i < length; i += 2) {
        uint8_t highNibble = hexCharToValue(hexString[i]);
        uint8_t lowNibble = hexCharToValue(hexString[i + 1]);
        result.append((highNibble << 4) | lowNibble);
    }

    return result;
}

String DebugServerMemoryHandler::bytesToHexString(const Vector<uint8_t>& bytes) const
{
    StringBuilder result;
    result.reserveCapacity(bytes.size() * 2);

    for (uint8_t byte : bytes) {
        result.append(valueToHexChar((byte >> 4) & 0xF));
        result.append(valueToHexChar(byte & 0xF));
    }

    return result.toString();
}

} // namespace Wasm
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
