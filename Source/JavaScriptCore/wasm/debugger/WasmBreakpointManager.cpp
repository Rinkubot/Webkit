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
#include "WasmBreakpointManager.h"

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "Options.h"
#include <wtf/RawPointer.h>
#include <wtf/DataLog.h>

namespace JSC {
namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WASMBreakpointManager);

WASMBreakpointManager::~WASMBreakpointManager()
{
    clearAllBreakpoints();
}

void WASMBreakpointManager::setBreakpoint(uint64_t virtualAddress, Breakpoint&& breakpoint)
{
    breakpoint.patchBreakpoint();
    dataLogLnIf(Options::verboseWasmDebugger(), "[BreakpointManager] setBreakpoint ", breakpoint, " at virtualAddress:", RawPointer((void*)virtualAddress));
    if (breakpoint.isOneTimeBreakpoint())
        m_tmpBreakpoints.add(virtualAddress);
    m_breakpoints.set(virtualAddress, WTFMove(breakpoint));
}

Breakpoint* WASMBreakpointManager::breakpoint(uint64_t virtualAddress)
{
    if (auto it = m_breakpoints.find(virtualAddress); it != m_breakpoints.end())
        return &it->value;
    return nullptr;
}

bool WASMBreakpointManager::removeRegularBreakpoint(uint64_t virtualAddress)
{
    auto it = m_breakpoints.find(virtualAddress);
    RELEASE_ASSERT(it != m_breakpoints.end());
    Breakpoint& breakpoint = it->value;
    dataLogLnIf(Options::verboseWasmDebugger(), "[BreakpointManager] Removing ", breakpoint, " at ", RawPointer((void*)virtualAddress), " m_breakpoints.size()=", m_breakpoints.size());
    it->value.restorePatch();
    m_breakpoints.remove(it);
    return true;
}

void WASMBreakpointManager::clearAllTmpBreakpoints()
{
    for (uint64_t virtualAddress : m_tmpBreakpoints)
        removeRegularBreakpoint(virtualAddress);
    m_tmpBreakpoints.clear();
    dataLogLnIf(Options::verboseWasmDebugger(), "[BreakpointManager] Cleared all tmp breakpoints");
}

void WASMBreakpointManager::clearAllBreakpoints()
{
    for (auto& [_, breakpoint] : m_breakpoints)
        breakpoint.restorePatch();
    m_breakpoints.clear();
    RELEASE_ASSERT(m_tmpBreakpoints.isEmpty());
}

} // namespace Wasm
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY)
