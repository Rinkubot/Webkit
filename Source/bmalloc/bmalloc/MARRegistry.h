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

// MAR: Malloc Audit Records
//
// MAR provides a new way to audit bmalloc/libpas memory allocations
// without resorting to PGM's guard pages. MAR maintains the address
// of each allocation, but instead tracks what allocations were made
// within pages of interest through the stack trace when `malloc` is
// invoked.

#ifndef MAR_REGISTRY_H
#define MAR_REGISTRY_H

#include <assert.h>
#include <execinfo.h>
#include <string.h>
#include <stdint.h>

#include "MARCrashReporterReport.h"

const unsigned MARProbability = 8192;
const unsigned MARTrackedBacktraces = 16384;
const unsigned MARTrackedAllocations = 16384;

// We'll use an approach similar to hardware FIFO; the queue is empty if head == tail
// and full if head ^ tail == size
const unsigned MARAllocationRecordTableFIFOModulus = 2 * MARTrackedAllocations;

const unsigned PageSize = 16384;
const unsigned PageShift = 14;

struct BacktraceRecord {
    unsigned m_numFrames;
    uint32_t m_hash;
    void* m_backtraceBuffer[BacktraceMaxSize];
};

struct MemoryActionRecord {
    void* address;
    size_t m_allocationSize;
    unsigned m_backtraceRegistryIndex;
    uint32_t m_backtraceHash;
    bool isAllocation;
};

typedef struct MARRegistry MARRegistry;
struct MARRegistry {
    struct BacktraceRecord m_backtraceRegistry[MARTrackedBacktraces];
    struct MemoryActionRecord m_allocationRecordTable[MARTrackedAllocations];
    // push to the tail of the FIFO, evict from head
    unsigned m_allocationRecordTableHead;
    unsigned m_allocationRecordTableTail;
};

struct ExportedAllocationRecord {
    MARBacktrace m_allocationTrace;
    MARBacktrace m_deallocationTrace;
    unsigned m_allocationSize;
    bool m_isValid;
};

void MAR_didAllocate(MARRegistry* registry, void* address, size_t size);
void MAR_didDeallocate(MARRegistry* registry, void* address);

void MAR_recordAllocation(MARRegistry* registry, void* address, size_t size, unsigned numStackFrames, void** backtrace);
void MAR_recordDeallocation(MARRegistry* registry, void* address, unsigned numStackFrames, void** backtrace);
struct ExportedAllocationRecord MAR_getAllocationRecord(MARRegistry* registry, void* address);

// Paging helpers

inline void* canonicalizeAddress(void* address)
{
    return (void*)((uintptr_t)(address) & ((1ull << 48) - 1));
}

inline uintptr_t addressToVirtualPageNumber(void* address)
{
    return (uintptr_t)(canonicalizeAddress(address)) >> PageShift;
}

// FIFO helpers

inline unsigned MAR_allocationTableHeadIndex(MARRegistry* registry)
{
    return registry->m_allocationRecordTableHead % MARAllocationRecordTableFIFOModulus;
}

inline unsigned MAR_allocationTableTailIndex(MARRegistry* registry)
{
    return registry->m_allocationRecordTableTail % MARAllocationRecordTableFIFOModulus;
}

inline bool MAR_isAllocationTableFull(MARRegistry* registry)
{
    return (registry->m_allocationRecordTableHead ^ registry->m_allocationRecordTableTail) == MARTrackedAllocations;
}

inline void MAR_incrementAllocationRecordTableHead(MARRegistry* registry)
{
    registry->m_allocationRecordTableHead = registry->m_allocationRecordTableHead + 1;
    registry->m_allocationRecordTableHead %= MARAllocationRecordTableFIFOModulus;
}

inline void MAR_incrementAllocationRecordTableTail(MARRegistry* registry)
{
    registry->m_allocationRecordTableTail = registry->m_allocationRecordTableTail + 1;
    registry->m_allocationRecordTableHead %= MARAllocationRecordTableFIFOModulus;
}

#ifdef __cplusplus
extern "C" {
#endif

// If the page number % MARProbability == marQualifyingPageIndex, then it qualifies
extern int marQualifyingPageIndex;
extern struct MARRegistry marRegistry;
extern struct MARRegistry* marRegistryForCrashReporterEnumeration;

#ifdef __cplusplus
};
#endif

inline bool MAR_isAddressInQualifyingPage(void* address)
{
    return addressToVirtualPageNumber(address) % MARProbability == marQualifyingPageIndex;
}

#endif
