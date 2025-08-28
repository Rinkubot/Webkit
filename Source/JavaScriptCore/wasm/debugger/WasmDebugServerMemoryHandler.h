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
#include <wtf/Vector.h>

namespace JSC {
namespace Wasm {

class DebugServer;

struct MemoryRegion {
    uint64_t startAddress;
    uint64_t size;
    bool readable;
    bool writable;
    bool executable;
    String name;

    MemoryRegion(uint64_t start, uint64_t sz, bool r, bool w, bool x, StringView n)
        : startAddress(start), size(sz), readable(r), writable(w), executable(x), name(n.toString()) { }

    bool contains(uint64_t address) const {
        return address >= startAddress && address < (startAddress + size);
    }
};

class DebugServerMemoryHandler {
    WTF_MAKE_TZONE_ALLOCATED(DebugServerMemoryHandler);
public:
    DebugServerMemoryHandler(DebugServer& debugServer)
        : m_debugServer(debugServer)
    {
    }

    // Memory operations
    void handleReadMemory(StringView packet);
    void handleWriteMemory(StringView packet);
    void handleMemoryMap(StringView packet);
    void handleMemoryInfo(StringView packet);

    // Memory region management
    void addMemoryRegion(uint64_t startAddress, uint64_t size, bool readable, bool writable, bool executable, StringView name);
    void removeMemoryRegion(uint64_t startAddress);
    void clearMemoryRegions();

    // Memory access validation
    bool isValidAddress(uint64_t address) const;
    bool isReadableAddress(uint64_t address) const;
    bool isWritableAddress(uint64_t address) const;
    const MemoryRegion* findMemoryRegion(uint64_t address) const;

    // WebAssembly-specific memory operations
    void setWasmMemoryBase(uint64_t baseAddress, uint64_t size);
    void setWasmStackBase(uint64_t baseAddress, uint64_t size);
    void setWasmGlobalsBase(uint64_t baseAddress, uint64_t size);

    // Memory content simulation (for testing)
    void setMemoryContent(uint64_t address, const Vector<uint8_t>& data);
    Vector<uint8_t> getMemoryContent(uint64_t address, uint32_t size) const;

private:
    // Helper methods
    bool parseMemoryPacket(StringView packet, uint64_t& address, uint32_t& size);
    bool parseWriteMemoryPacket(StringView packet, uint64_t& address, Vector<uint8_t>& data);
    void sendMemoryData(const Vector<uint8_t>& data);
    void sendMemoryMapData();
    void sendErrorReply(ProtocolError);

    // Utility functions
    uint8_t hexCharToValue(char) const;
    char valueToHexChar(uint8_t value) const;
    Vector<uint8_t> hexStringToBytes(StringView hexString, size_t length) const;
    String bytesToHexString(const Vector<uint8_t>& bytes) const;

    // Reference to debug server for communication
    DebugServer& m_debugServer;

    // Memory regions
    Vector<MemoryRegion> m_memoryRegions;

    // Simulated memory content (for testing)
    HashMap<uint64_t, uint8_t> m_memoryContent;

    // WebAssembly memory layout
    uint64_t m_wasmMemoryBase { 0 };
    uint64_t m_wasmMemorySize { 0 };
    uint64_t m_wasmStackBase { 0 };
    uint64_t m_wasmStackSize { 0 };
    uint64_t m_wasmGlobalsBase { 0 };
    uint64_t m_wasmGlobalsSize { 0 };
};

} // namespace Wasm
} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
