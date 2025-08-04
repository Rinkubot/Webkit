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
#include "WasmModuleManager.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "DeferGC.h"
#include "JSWebAssemblyModule.h"
#include "VM.h"
#include "WasmCallee.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmFormat.h"
#include "WeakGCMap.h"
#include "WeakGCMapInlines.h"
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/IterationStatus.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WASMModuleManager);

WASMModuleManager::WASMModuleManager(VM& vm)
    : m_vm(vm)
    , m_addressToModule(vm)
{
}

WASMModuleManager::~WASMModuleManager() = default;

uint64_t WASMModuleManager::registerModule(JSWebAssemblyModule* jsModule)
{
    const auto& moduleInfo = jsModule->moduleInformation();
    if (moduleInfo.debugBinary.isEmpty()) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][track] - no debug binary available");
        return 0;
    }

    uint64_t virtualBaseAddress = m_nextVirtualAddress;
    m_nextVirtualAddress += MODULE_ADDRESS_SPACING;
    m_addressToModule.set(virtualBaseAddress, jsModule);
    jsModule->module().setVirtualBaseAddress(virtualBaseAddress);
    dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][track] - tracked module at virtual address: 0x", hex(virtualBaseAddress, Lowercase), " size: ", moduleInfo.debugBinary.size(), " bytes");
    return virtualBaseAddress;
}

Vector<uint8_t> WASMModuleManager::readSourceBinary(uint64_t virtualAddress, size_t size) const
{
    auto [virtualBaseAddress, offset] = parseVirtualAddress(virtualAddress);
    JSWebAssemblyModule* jsModule = m_addressToModule.get(virtualBaseAddress);
    RELEASE_ASSERT(jsModule && offset < jsModule->moduleInformation().debugBinary.size());

    const auto& debugBinary = jsModule->moduleInformation().debugBinary;
    if (offset + size > debugBinary.size()) {
        dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][read] - read beyond module boundary. Address: 0x", hex(virtualAddress, Lowercase), " offset: ", offset, " size: ", size, " module size: ", debugBinary.size());
        return { };
    }

    Vector<uint8_t> result;
    result.reserveInitialCapacity(size);
    for (size_t i = 0; i < size; ++i)
        result.append(debugBinary[offset + i]);

    dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][read] - read ", size, " bytes at offset: ", offset);
    return result;
}

std::pair<uint64_t, uint64_t> WASMModuleManager::parseVirtualAddress(uint64_t virtualAddress)
{
    uint64_t virtualBaseAddress = (virtualAddress - VIRTUAL_ADDRESS_BASE) & ~(MODULE_ADDRESS_SPACING - 1);
    virtualBaseAddress += VIRTUAL_ADDRESS_BASE;
    uint64_t offset = virtualAddress - virtualBaseAddress;
    return { virtualBaseAddress, offset };
}

uint64_t WASMModuleManager::physicalToVirtual(JSWebAssemblyModule* jsModule, FunctionCodeIndex index, const uint8_t* pc)
{
    uint64_t virtualBaseAddress = jsModule->module().virtualBaseAddress();
    const Wasm::FunctionData& functionData = jsModule->moduleInformation().functions[index];
    uint64_t virtualOffset = pc - &functionData.data[0] + functionData.start;
    return virtualBaseAddress + virtualOffset;
}

uint8_t* WASMModuleManager::virtualToPhysical(uint64_t virtualAddress) const
{
    auto [virtualBaseAddress, offset] = parseVirtualAddress(virtualAddress);
    JSWebAssemblyModule* jsModule = m_addressToModule.get(virtualBaseAddress);
    RELEASE_ASSERT(jsModule && offset < jsModule->moduleInformation().debugBinary.size());

    const ModuleInformation& moduleInfo = jsModule->moduleInformation();
    auto metaInfo = moduleInfo.metadataDebugInfo(offset);
    const FunctionData& functionData = moduleInfo.functions[metaInfo.functionIndex];
    uint32_t offsetInFunction = offset - functionData.start;
    uint8_t* pc = const_cast<uint8_t*>(&functionData.data[0]) + offsetInFunction;
    dataLogFIf(Options::verboseWasmDebugger(), "[ModuleManager] Resolved virtual address: %p -> physical PC: %p\n", (void*)virtualAddress, pc);
    return pc;
}

String WASMModuleManager::generateLibrariesXML() const
{
    // Generate XML library list for LLDB's GDB Remote Protocol
    //
    // <?xml version="1.0"?>
    // <library-list>
    //   <library name="wasm32_args.wasm">
    //     <section address="0x4000000000000000"/>
    //   </library>
    //   <library name="wasm32_args.wasm">
    //     <section address="0x4000000100000000"/>
    //   </library>
    // </library-list>
    //
    // Key design decisions:
    // - Use <section> - LLDB will parse WASM binary to find individual sections
    // - Use lowercase hex addresses to match LLDB expectations
    // - Escape XML entities in module names for security and correctness
    // - Each module gets its own <library> entry with unique virtual address
    StringBuilder xml;
    xml.append("<?xml version=\"1.0\"?>\n"_s);
    xml.append("<library-list>\n"_s);

    {
        DeferGC deferGC(m_vm);
        m_addressToModule.forEach([&](uint64_t addr, JSWebAssemblyModule* jsModule) {
            if (!jsModule)
                return IterationStatus::Continue;

            const auto& debugBinary = jsModule->moduleInformation().debugBinary;
            if (debugBinary.isEmpty())
                return IterationStatus::Continue;

            // Generate intelligent module name using available information
            // Priority: sourceMappingURL > meaningful fallback > address-based
            String moduleName = generateModuleName(addr, jsModule);
            xml.append("  <library name=\""_s);
            xml.append(moduleName);
            xml.append("\">\n"_s);
            xml.append("    <section address=\"0x"_s);
            xml.append(hex(addr, Lowercase));
            xml.append("\"/>\n"_s);
            xml.append("  </library>\n"_s);
            dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][XML] - added module '", moduleName, "' at 0x", hex(addr, Lowercase), " size: 0x", hex(debugBinary.size(), Lowercase));
            return IterationStatus::Continue;
        });
    }

    xml.append("</library-list>\n"_s);

    String result = xml.toString();
    dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager][XML] - generated library list XML: ", moduleCount(), " modules, ", result.length(), " characters");
    return result;
}

Vector<String> WASMModuleManager::moduleNames() const
{
    Vector<String> names;
    DeferGC deferGC(m_vm);
    m_addressToModule.forEach([&](uint64_t virtualBaseAddress, JSWebAssemblyModule* jsModule) {
        names.append(generateModuleName(virtualBaseAddress, jsModule));
        return IterationStatus::Continue;
    });
    return names;
}

size_t WASMModuleManager::moduleCount() const { return m_addressToModule.size(); }
String WASMModuleManager::generateModuleName(uint64_t virtualAddress, JSWebAssemblyModule* jsModule) const
{
    if (jsModule) {
        const auto& moduleInfo = jsModule->moduleInformation();
        // First priority: Use sourceMappingURL if available
        if (!moduleInfo.sourceMappingURL.isEmpty()) {
            // Convert Vector<char8_t> to String using WTF::makeString (same pattern as WebAssemblyModuleConstructor.cpp)
            String url = WTF::makeString(moduleInfo.sourceMappingURL);
            
            // Extract filename from URL (handle both file paths and URLs)
            size_t lastSlash = url.reverseFind('/');
            size_t lastBackslash = url.reverseFind('\\');
            size_t lastSeparator = std::max(lastSlash == WTF::notFound ? 0 : lastSlash + 1,
                                          lastBackslash == WTF::notFound ? 0 : lastBackslash + 1);
            
            if (lastSeparator < url.length()) {
                String filename = url.substring(lastSeparator);
                // Ensure it has .wasm extension
                if (!filename.endsWithIgnoringASCIICase(".wasm"_s)) {
                    filename = WTF::makeString(filename, ".wasm"_s);
                }
                dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager] Using sourceMappingURL filename: ", filename);
                return filename;
            }
        }
    }
    
    // Final fallback: Use address-based naming (original behavior)
    String fallbackName = WTF::makeString("wasm_module_0x"_s, hex(virtualAddress, Lowercase), ".wasm"_s);
    dataLogLnIf(Options::verboseWasmDebugger(), "[ModuleManager] Using fallback address-based name: ", fallbackName);
    return fallbackName;
}

}
} // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
