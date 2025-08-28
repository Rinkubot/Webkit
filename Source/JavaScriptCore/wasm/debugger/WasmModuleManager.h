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

#include "WeakGCMap.h"
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSWebAssemblyModule;
class VM;

namespace Wasm {

class IPIntCallee;
class FunctionCodeIndex;

/**
 * WASMModuleManager - Maps WASM modules to unique virtual addresses for LLDB debugging
 *
 * The Problem:
 * - Multiple WASM modules loaded in JSC would all appear at the same memory addresses
 * - LLDB can't tell them apart - breakpoints set in one module affect all modules
 * - Debugging becomes impossible when you have multiple WASM modules
 *
 * The Solution:
 * - Give each WASM module its own unique 4GB virtual address space
 * - LLDB sees each module as a separate "binary" at different addresses
 * - Now you can debug each module independently with proper breakpoints
 *
 * Usage: Call registerModule() when loading, then debug normally with LLDB.
 *
 * Virtual Address Assignment:
 * - Module 0: 0x4000000000000000 (base address)
 * - Module 1: 0x4000000100000000 (base + 4GB)
 * - Module 2: 0x4000000200000000 (base + 8GB)
 * - Module N: 0x4000000000000000 + (N * 4GB)
 *
 * Note: These are fake addresses for LLDB - not real memory locations
 */
class WASMModuleManager {
    WTF_MAKE_TZONE_ALLOCATED(WASMModuleManager);

public:
    static constexpr uint64_t VIRTUAL_ADDRESS_BASE = 0x4000000000000000ULL;
    static constexpr uint64_t MODULE_ADDRESS_SPACING = 0x100000000ULL;

    JS_EXPORT_PRIVATE WASMModuleManager(VM&);
    JS_EXPORT_PRIVATE ~WASMModuleManager();

    JS_EXPORT_PRIVATE uint64_t registerModule(JSWebAssemblyModule*);

    JS_EXPORT_PRIVATE static uint64_t physicalToVirtual(JSWebAssemblyModule*, FunctionCodeIndex, const uint8_t* pc);
    JS_EXPORT_PRIVATE uint8_t* virtualToPhysical(uint64_t virtualAddress) const;

    JS_EXPORT_PRIVATE Vector<uint8_t> readSourceBinary(uint64_t virtualAddress, size_t) const;
    JS_EXPORT_PRIVATE String generateLibrariesXML() const;

    JS_EXPORT_PRIVATE Vector<String> moduleNames() const;
    JS_EXPORT_PRIVATE size_t moduleCount() const;

private:
    JS_EXPORT_PRIVATE static std::pair<uint64_t, uint64_t> parseVirtualAddress(uint64_t virtualAddress);
    String generateModuleName(uint64_t virtualAddress, JSWebAssemblyModule* jsModule = nullptr) const;

    VM& m_vm;
    WeakGCMap<uint64_t, JSWebAssemblyModule> m_addressToModule;
    uint64_t m_nextVirtualAddress { VIRTUAL_ADDRESS_BASE };
};

} // namespace Wasm
} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
