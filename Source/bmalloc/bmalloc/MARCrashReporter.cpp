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

#include "MARCrashReporter.h"
#include "MARCrashReporterReport.h"

kern_return_t populateMARCrashReport(MARCrashReport* report, const char* errorType, const char* confidence,
        vm_address_t faultAddress, size_t allocationSize,
        MARBacktrace* allocationBacktrace, MARBacktrace* deallocationBacktrace)
{
    report->errorType = errorType;
    report->confidence = confidence;
    report->faultAddress = faultAddress;
    report->allocationSize = allocationSize;

    report->allocationBacktrace.m_numFrames = allocationBacktrace->m_numFrames;
    memcpy(&report->allocationBacktrace.m_backtraceBuffer, &allocationBacktrace->m_backtraceBuffer, sizeof(report->allocationBacktrace.m_backtraceBuffer));

    report->deallocationBacktrace.m_numFrames = deallocationBacktrace->m_numFrames;
    memcpy(&report->deallocationBacktrace.m_backtraceBuffer, &deallocationBacktrace->m_backtraceBuffer, sizeof(report->deallocationBacktrace.m_backtraceBuffer));
    return KERN_SUCCESS;
}

static crash_reporter_memory_reader_t memory_reader;
typedef kern_return_t memory_reader_t(task_t remote_task, vm_address_t remote_address, vm_size_t size, void * __sized_by(size) *local_memory);

static kern_return_t memory_reader_adapter(task_t task, vm_address_t address, vm_size_t size, void** local_memory)
{
    if (!local_memory)
        return KERN_FAILURE;

    void* ptr = memory_reader(task, address, size);
    *local_memory = ptr;
    return ptr ? KERN_SUCCESS : KERN_FAILURE;
}

static memory_reader_t* setup_memory_reader(crash_reporter_memory_reader_t crm_reader)
{
    memory_reader = crm_reader;
    return memory_reader_adapter;
}

kern_return_t extractMARCrashReport(vm_address_t faultAddress, mach_vm_address_t marGlobalRegistry, unsigned version, task_t task, MARCrashReport* report, crash_reporter_memory_reader_t crmReader)
{
    if (version != MARCrashReportVersion)
        return KERN_FAILURE;

    MARRegistry* deadMARRegistry = NULL;
    memory_reader_t* reader = setup_memory_reader(crmReader);
    kern_return_t kr = reader(task, marGlobalRegistry, sizeof(MARRegistry), (void**)&deadMARRegistry);
    if (kr != KERN_SUCCESS)
        return KERN_FAILURE;

    auto result = MAR_getAllocationRecord((MARRegistry*)deadMARRegistry, (void*)faultAddress);

    if (!result.m_isValid)
        return KERN_NOT_FOUND;

    if (result.m_deallocationTrace.m_numFrames != 0)
        return populateMARCrashReport(report, "UAF", "high", faultAddress, result.m_allocationSize, &result.m_allocationTrace, &result.m_deallocationTrace);
    else
        return populateMARCrashReport(report, "bad access", "high", faultAddress, result.m_allocationSize, &result.m_allocationTrace, &result.m_deallocationTrace);
}

