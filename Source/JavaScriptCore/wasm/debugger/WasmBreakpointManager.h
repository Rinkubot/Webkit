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
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class CallFrame;
class JSWebAssemblyInstance;
class JSWebAssemblyModule;

namespace Wasm {

class IPIntCallee;
class WASMModuleManager;

/**
 * WASMBreakpointManager - Standalone WebAssembly Breakpoint Management
 *
 * Based on analysis of the actual interrupt breakpoint flow:
 *
 * INTERRUPT BREAKPOINT FLOW:
 * 1. Ctrl+C → interrupt() → vm.enableWasmEntryBreak()
 * 2. Next WASM function entry → debug_interrupt_handler() called
 * 4. Patches function entry with unreachable (0x00) instruction
 * 5. Execution continues → hits unreachable → unreachable_breakpoint_handler()
 * 6. unreachable_breakpoint_handler() → stop() → restores original bytecode
 *
 * REGULAR BREAKPOINT FLOW:
 * 1. LLDB Z0 command → setRegularBreakpoint(virtualAddress)
 * 2. Resolves virtual address to physical bytecode address via WASMModuleManager
 * 3. Patches target address with unreachable (0x00) instruction
 * 4. Execution hits unreachable → unreachable_breakpoint_handler()
 * 5. stop() → restores original bytecode
 *
 * KEY INSIGHTS:
 * - Both breakpoint types use the same unreachable patching mechanism
 * - Interrupt breakpoints are always at function entry (callee->bytecode())
 * - Regular breakpoints can be at any virtual address within a function
 * - Original bytecode must be stored for restoration
 * - Function name mapping uses "ipint-function[N]" format for interrupt breakpoints
 * - Regular breakpoints are tracked by virtual address
 * - WASMModuleManager provides virtual address resolution
 */
class WASMBreakpointManager {
    WTF_MAKE_TZONE_ALLOCATED(WASMBreakpointManager);

public:
    JS_EXPORT_PRIVATE WASMBreakpointManager() = default;
    JS_EXPORT_PRIVATE ~WASMBreakpointManager();

    JS_EXPORT_PRIVATE Breakpoint* breakpoint(uint64_t virtualAddress);
    JS_EXPORT_PRIVATE void setBreakpoint(uint64_t virtualAddress, Breakpoint&&);
    JS_EXPORT_PRIVATE bool removeRegularBreakpoint(uint64_t virtualAddress);
    JS_EXPORT_PRIVATE void clearAllTmpBreakpoints();
    JS_EXPORT_PRIVATE void clearAllBreakpoints();

private:
    UncheckedKeyHashMap<uint64_t, Breakpoint> m_breakpoints;
    UncheckedKeyHashSet<uint64_t> m_tmpBreakpoints;
    // TODO: need lock?
};

} // namespace Wasm
} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
