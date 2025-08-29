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

#include "MARRegistry.h"

#include <stdio.h>

int marQualifyingPageIndex = 0;

struct MARRegistry marRegistry = {
    {},
    {},
    0,
    0,
};

struct MARRegistry* marRegistryForCrashReporterEnumeration = nullptr;

// Backtrace hashing

uint32_t hashBacktrace(int numStackFrames, void** backtrace);
uint32_t hashBacktrace(int numStackFrames, void** backtrace)
{
    // This implements Murmur hash on the low 32b of each backtrace
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    const uint32_t r1 = 15;
    const uint32_t r2 = 13;
    const uint32_t m = 5;
    const uint32_t n = 0xe6546b64;

    uint32_t result = 0;

    for (int i = 0; i < numStackFrames; ++i) {
        uint32_t k = ((uintptr_t)backtrace[i]) & ((1ull << 32) - 1);
        k *= c1;
        k = (k << r1) | (k >> (32 - r1));
        k *= c2;

        result ^= k;
        result = (result << r2) | (result >> (32 - r2));
        result = result * m + n;
    }

    result ^= (numStackFrames * 4);
    result = result ^ (result >> 16);
    result *= 0x85ebca6b;
    result = result ^ (result >> 13);
    result *= 0xc2b2ae35;
    result = result ^ (result >> 16);

    return result;
}

unsigned MAR_insertBacktrace(MARRegistry* registry, int numStackFrames, void** backtrace, uint32_t hash);
unsigned MAR_insertBacktrace(MARRegistry* registry, int numStackFrames, void** backtrace, uint32_t hash)
{
    unsigned index = hash % MARTrackedBacktraces;
    if (hash == registry->m_backtraceRegistry[index].m_hash)
        return index;

    registry->m_backtraceRegistry[index].m_numFrames = numStackFrames;
    registry->m_backtraceRegistry[index].m_hash = hash;
    memcpy(registry->m_backtraceRegistry[index].m_backtraceBuffer, backtrace, numStackFrames * sizeof(void*));
    return index;
}

// MAR Registry

void MAR_didAllocate(MARRegistry* registry, void* address, size_t size)
{
    if (!marRegistryForCrashReporterEnumeration)
        marRegistryForCrashReporterEnumeration = registry;
    void* stacktrace[BacktraceMaxSize];
    unsigned numStackFrames = backtrace(stacktrace, BacktraceMaxSize);

    MAR_recordAllocation(registry, address, size, numStackFrames, stacktrace);
}

void MAR_didDeallocate(MARRegistry* registry, void* address)
{
    void* stacktrace[BacktraceMaxSize];
    unsigned numStackFrames = backtrace(stacktrace, BacktraceMaxSize);

    MAR_recordDeallocation(registry, address, numStackFrames, stacktrace);
}

void MAR_recordAllocation(MARRegistry* registry, void* address, size_t size, unsigned numStackFrames, void** backtrace)
{
    assert(numStackFrames <= BacktraceMaxSize);

    if (MAR_isAllocationTableFull(registry)) [[likely]]
        MAR_incrementAllocationRecordTableHead(registry);

    unsigned allocationTableIndex = MAR_allocationTableTailIndex(registry);
    MAR_incrementAllocationRecordTableTail(registry);

    uint32_t backtraceHash = hashBacktrace(numStackFrames, backtrace);
    unsigned backtraceRegistryIndex = MAR_insertBacktrace(registry, numStackFrames, backtrace, backtraceHash);
    registry->m_allocationRecordTable[allocationTableIndex] = {
        address,
        size,
        backtraceRegistryIndex,
        backtraceHash,
        true
    };
}

void MAR_recordDeallocation(MARRegistry* registry, void* address, unsigned numStackFrames, void** backtrace)
{
    assert(numStackFrames <= BacktraceMaxSize);

    if (MAR_isAllocationTableFull(registry)) [[likely]]
        MAR_incrementAllocationRecordTableHead(registry);

    unsigned allocationTableIndex = MAR_allocationTableTailIndex(registry);
    MAR_incrementAllocationRecordTableTail(registry);

    uint32_t backtraceHash = hashBacktrace(numStackFrames, backtrace);
    unsigned backtraceRegistryIndex = MAR_insertBacktrace(registry, numStackFrames, backtrace, backtraceHash);
    registry->m_allocationRecordTable[allocationTableIndex] = {
        address,
        0,
        backtraceRegistryIndex,
        backtraceHash,
        false
    };
}

ExportedAllocationRecord MAR_getAllocationRecord(MARRegistry* registry, void* address)
{
    ExportedAllocationRecord result;
    result.m_isValid = false;

    address = canonicalizeAddress(address);

    void* baseObjectAddress = nullptr;
    for (unsigned i = 0; i < MARTrackedAllocations; ++i) {
        unsigned index = (MAR_allocationTableHeadIndex(registry) + i) % MARTrackedAllocations;
        if (index == MAR_allocationTableTailIndex(registry))
            break;
        MemoryActionRecord& ARTEntry = registry->m_allocationRecordTable[index];
        if (ARTEntry.isAllocation) {
            // Check if the allocation was within the range
            if ((uintptr_t) address >= (uintptr_t) canonicalizeAddress(ARTEntry.address) && (uintptr_t) address < ((uintptr_t) canonicalizeAddress(ARTEntry.address)) + ARTEntry.m_allocationSize) {
                // Check that we have a valid backtrace
                unsigned registryIndex = ARTEntry.m_backtraceRegistryIndex;
                if (registry->m_backtraceRegistry[registryIndex].m_hash != ARTEntry.m_backtraceHash)
                    continue;

                BacktraceRecord& backtrace = registry->m_backtraceRegistry[registryIndex];

                result.m_allocationSize = ARTEntry.m_allocationSize;
                result.m_isValid = true;
                result.m_allocationTrace.m_numFrames = backtrace.m_numFrames;
                memcpy(result.m_allocationTrace.m_backtraceBuffer, backtrace.m_backtraceBuffer, backtrace.m_numFrames * sizeof(void*));

                baseObjectAddress = ARTEntry.address;
            }
        }
        if (result.m_isValid && !ARTEntry.isAllocation && ARTEntry.address == baseObjectAddress) {
            // Check that we have a valid backtrace
            unsigned registryIndex = ARTEntry.m_backtraceRegistryIndex;
            if (registry->m_backtraceRegistry[registryIndex].m_hash != ARTEntry.m_backtraceHash)
                continue;

            BacktraceRecord& backtrace = registry->m_backtraceRegistry[registryIndex];

            result.m_deallocationTrace.m_numFrames = backtrace.m_numFrames;
            memcpy(result.m_deallocationTrace.m_backtraceBuffer, backtrace.m_backtraceBuffer, backtrace.m_numFrames * sizeof(void*));

            baseObjectAddress = nullptr;
        }
    }
    return result;
}

