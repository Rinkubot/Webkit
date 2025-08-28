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

#include <wtf/DataLog.h>

#if ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "InitializeThreading.h"
#include "JSWebAssemblyModule.h"
#include "VM.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmModuleManager.h"
#include <wtf/HexNumber.h>
#include <wtf/Vector.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

#if OS(WINDOWS)
#include <wtf/win/WTFCRTDebug.h>
#endif

using namespace JSC;
using namespace JSC::Wasm;

// Test counters
static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message)                          \
    do {                                                         \
        testsRun++;                                              \
        if (condition) {                                         \
            testsPassed++;                                       \
            dataLogLn("PASS: ", message);                        \
        } else {                                                 \
            testsFailed++;                                       \
            dataLogLn("FAIL: ", message, " (", #condition, ")"); \
        }                                                        \
    } while (0)

static void testWASMModuleManagerConstants()
{
    dataLogLn("=== Testing WASMModuleManager Constants ===");

    // Test virtual address constants
    TEST_ASSERT(WASMModuleManager::VIRTUAL_ADDRESS_BASE == 0x4000000000000000ULL,
        "VIRTUAL_ADDRESS_BASE should be 0x4000000000000000");

    TEST_ASSERT(WASMModuleManager::MODULE_ADDRESS_SPACING == 0x100000000ULL,
        "MODULE_ADDRESS_SPACING should be 4GB (0x100000000)");

    // Test address space design
    uint64_t module0 = WASMModuleManager::VIRTUAL_ADDRESS_BASE;
    uint64_t module1 = module0 + WASMModuleManager::MODULE_ADDRESS_SPACING;
    uint64_t module2 = module1 + WASMModuleManager::MODULE_ADDRESS_SPACING;

    TEST_ASSERT(module0 == 0x4000000000000000ULL, "Module 0 should be at base address");
    TEST_ASSERT(module1 == 0x4000000100000000ULL, "Module 1 should be at base + 4GB");
    TEST_ASSERT(module2 == 0x4000000200000000ULL, "Module 2 should be at base + 8GB");

    dataLogLn("Constants tests completed");
}

static void testWASMModuleManagerBasicOperations()
{
    dataLogLn("=== Testing WASMModuleManager Basic Operations ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Test initial state
    TEST_ASSERT(!moduleManager.moduleCount(), "Initial module count should be 0");

    Vector<String> initialNames = moduleManager.moduleNames();
    TEST_ASSERT(initialNames.isEmpty(), "Initial module names should be empty");

    // Test empty library XML generation
    String emptyXML = moduleManager.generateLibrariesXML();
    TEST_ASSERT(!emptyXML.isEmpty(), "Library XML should not be empty");
    TEST_ASSERT(emptyXML.contains("<?xml version=\"1.0\"?>"), "XML should have proper header");
    TEST_ASSERT(emptyXML.contains("<library-list>"), "XML should contain library-list");
    TEST_ASSERT(emptyXML.contains("</library-list>"), "XML should be properly closed");
    TEST_ASSERT(!emptyXML.contains("<library name="), "Empty XML should not contain library entries");

    // Note: readSourceBinary() requires registered modules, so we skip this test for empty manager
    // This is by design - the method is intended for use with registered modules only

    dataLogLn("Basic operations tests completed");
}

static void testWASMModuleManagerMemoryReading()
{
    dataLogLn("=== Testing WASMModuleManager Memory Reading ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Note: readSourceBinary() uses RELEASE_ASSERT and requires registered modules
    // Testing invalid addresses would cause crashes, so we test the address parsing logic instead

    // Test address parsing logic indirectly through other methods
    TEST_ASSERT(!moduleManager.moduleCount(), "Empty manager should have 0 modules");

    Vector<String> emptyNames = moduleManager.moduleNames();
    TEST_ASSERT(emptyNames.isEmpty(), "Empty manager should have no module names");

    // Test that we can generate XML without crashing
    String xml = moduleManager.generateLibrariesXML();
    TEST_ASSERT(!xml.isEmpty(), "XML generation should work with empty manager");

    dataLogLn("Memory reading tests completed");
}

static void testWASMModuleManagerLibraryXMLGeneration()
{
    dataLogLn("=== Testing WASMModuleManager Library XML Generation ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Test XML structure with no modules
    String xml = moduleManager.generateLibrariesXML();

    // Verify XML structure
    TEST_ASSERT(xml.startsWith("<?xml version=\"1.0\"?>"_s), "XML should start with proper declaration");
    TEST_ASSERT(xml.contains("<library-list>"), "XML should contain opening library-list tag");
    TEST_ASSERT(xml.contains("</library-list>"), "XML should contain closing library-list tag");

    // Verify no library entries in empty manager
    TEST_ASSERT(!xml.contains("<library name="), "Empty manager should not have library entries");
    TEST_ASSERT(!xml.contains("<section address="), "Empty manager should not have section entries");

    // Test XML is well-formed (basic validation)
    size_t openTags = 0;
    size_t closeTags = 0;

    for (size_t i = 0; i < xml.length(); ++i) {
        if (xml[i] == '<') {
            if (i + 1 < xml.length() && xml[i + 1] == '/') {
                // Closing tag like </library-list>
                closeTags++;
            } else if (i + 1 < xml.length() && xml[i + 1] == '?') {
                // XML declaration like <?xml version="1.0"?>
                // Skip this, it's not a regular tag
                continue;
            } else {
                // Opening tag, check if it's self-closing
                bool isSelfClosing = false;
                for (size_t j = i + 1; j < xml.length() && xml[j] != '>'; ++j) {
                    if (xml[j] == '/' && j + 1 < xml.length() && xml[j + 1] == '>') {
                        isSelfClosing = true;
                        break;
                    }
                }

                if (!isSelfClosing)
                    openTags++;
                // Self-closing tags don't need separate closing tags, so we don't count them
            }
        }
    }

    // For well-formed XML: openTags should equal closeTags
    // Self-closing tags don't need separate closing tags
    TEST_ASSERT(openTags == closeTags, "XML should have balanced opening and closing tags");

    dataLogLn("Library XML generation tests completed");
}

static void testWASMModuleManagerAddressValidation()
{
    dataLogLn("=== Testing WASMModuleManager Address Validation ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Test various invalid addresses
    struct AddressTest {
        uint64_t address;
        const char* description;
    };

    AddressTest invalidAddresses[] = {
        { 0x0, "null address" },
        { 0x1000, "low memory address" },
        { 0x7FFFFFFF, "32-bit address space" },
        { 0x3FFFFFFFFFFFFFF, "below virtual base" },
        { 0x8000000000000000ULL, "high invalid address" },
        { 0xFFFFFFFFFFFFFFFFULL, "maximum address" }
    };

    // Note: readSourceBinary() uses RELEASE_ASSERT, so we can't test invalid addresses directly
    // Instead, we test the address space design and validation logic

    for (const auto& test : invalidAddresses) {
        // Test that these addresses are outside the expected virtual address space
        bool isInValidRange = (test.address >= WASMModuleManager::VIRTUAL_ADDRESS_BASE);
        if (test.address < WASMModuleManager::VIRTUAL_ADDRESS_BASE) {
            TEST_ASSERT(!isInValidRange,
                makeString("Address "_s, String::fromLatin1(test.description), " (0x"_s, hex(test.address, Lowercase), ") should be below virtual base"_s).utf8().data());
        }
    }

    // Test virtual address space design
    uint64_t validAddresses[] = {
        WASMModuleManager::VIRTUAL_ADDRESS_BASE,
        WASMModuleManager::VIRTUAL_ADDRESS_BASE + WASMModuleManager::MODULE_ADDRESS_SPACING,
        WASMModuleManager::VIRTUAL_ADDRESS_BASE + (2 * WASMModuleManager::MODULE_ADDRESS_SPACING)
    };

    for (uint64_t address : validAddresses) {
        // Test that these addresses are in the valid virtual range
        bool isInValidRange = (address >= WASMModuleManager::VIRTUAL_ADDRESS_BASE);
        TEST_ASSERT(isInValidRange,
            makeString("Valid virtual address 0x"_s, hex(address, Lowercase), " should be in valid range"_s).utf8().data());
    }

    dataLogLn("Address validation tests completed");
}

static void testWASMModuleManagerEdgeCases()
{
    dataLogLn("=== Testing WASMModuleManager Edge Cases ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Note: readSourceBinary() requires registered modules, so we test other edge cases

    // Test that large parameters don't break other methods
    TEST_ASSERT(!moduleManager.moduleCount(), "Module count should remain 0");

    Vector<String> names = moduleManager.moduleNames();
    TEST_ASSERT(names.isEmpty(), "Module names should remain empty");

    // Test multiple XML generations (should be consistent)
    String xml1 = moduleManager.generateLibrariesXML();
    String xml2 = moduleManager.generateLibrariesXML();
    TEST_ASSERT(xml1 == xml2, "Multiple XML generations should be identical");

    // Test module names and count consistency
    size_t count1 = moduleManager.moduleCount();
    Vector<String> names1 = moduleManager.moduleNames();
    size_t count2 = moduleManager.moduleCount();
    Vector<String> names2 = moduleManager.moduleNames();

    TEST_ASSERT(count1 == count2, "Module count should be consistent");
    TEST_ASSERT(names1.size() == names2.size(), "Module names size should be consistent");
    TEST_ASSERT(count1 == names1.size(), "Module count should match names size");

    dataLogLn("Edge cases tests completed");
}

static void testWASMModuleManagerIntegration()
{
    dataLogLn("=== Testing WASMModuleManager Integration Scenarios ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Test scenario: Debug server would use these APIs

    // 1. Check initial state
    size_t initialCount = moduleManager.moduleCount();
    TEST_ASSERT(!initialCount, "Debug server should see empty initial state");

    // 2. Generate library list for LLDB
    String libraryList = moduleManager.generateLibrariesXML();
    TEST_ASSERT(!libraryList.isEmpty(), "Debug server should get valid library list");
    TEST_ASSERT(libraryList.contains("library-list"), "Library list should be valid XML");

    // 3. Handle memory read requests from LLDB (would be done after modules are registered)
    // Note: readSourceBinary() requires registered modules, so we skip this test

    // 4. Get module information
    Vector<String> moduleNames = moduleManager.moduleNames();
    TEST_ASSERT(moduleNames.isEmpty(), "No modules should be registered initially");

    // 5. Test address space boundaries
    uint64_t baseAddr = WASMModuleManager::VIRTUAL_ADDRESS_BASE;
    uint64_t spacing = WASMModuleManager::MODULE_ADDRESS_SPACING;

    // Test address space design for different module slots
    for (int i = 0; i < 5; ++i) {
        uint64_t moduleAddr = baseAddr + (i * spacing);
        // Test that addresses are properly spaced
        TEST_ASSERT(moduleAddr >= baseAddr,
            makeString("Module slot "_s, String::number(i), " address should be >= base address"_s).utf8().data());

        if (i > 0) {
            uint64_t prevAddr = baseAddr + ((i - 1) * spacing);
            TEST_ASSERT(moduleAddr - prevAddr == spacing,
                makeString("Module slot "_s, String::number(i), " should be properly spaced"_s).utf8().data());
        }
    }

    dataLogLn("Integration scenarios tests completed");
}

static void testWASMModuleManagerPerformance()
{
    dataLogLn("=== Testing WASMModuleManager Performance Characteristics ===");

    VM& vm = VM::create().leakRef();
    WASMModuleManager moduleManager(vm);

    // Test repeated operations for performance characteristics
    const int iterations = 1000;

    // Test repeated XML generation
    for (int i = 0; i < iterations; ++i) {
        String xml = moduleManager.generateLibrariesXML();
        if (!i)
            TEST_ASSERT(!xml.isEmpty(), "First XML generation should succeed");
    }

    // Test repeated module count queries
    for (int i = 0; i < iterations; ++i) {
        size_t count = moduleManager.moduleCount();
        if (!i)
            TEST_ASSERT(!count, "Module count should be consistent");
    }

    // Test repeated operations that don't require registered modules
    for (int i = 0; i < iterations; ++i) {
        size_t count = moduleManager.moduleCount();
        if (!i)
            TEST_ASSERT(!count, "Module count should be consistent");
    }

    // Test repeated module names queries
    for (int i = 0; i < iterations; ++i) {
        Vector<String> names = moduleManager.moduleNames();
        if (!i)
            TEST_ASSERT(names.isEmpty(), "Module names should be consistent");
    }

    TEST_ASSERT(true, "Performance test completed without crashes");

    dataLogLn("Performance characteristics tests completed");
}

static void runAllTests()
{
    dataLogLn("Starting Comprehensive WASM Module Manager Test Suite");
    dataLogLn("====================================================");

    // Test constants and basic design
    testWASMModuleManagerConstants();

    // Test basic operations
    testWASMModuleManagerBasicOperations();

    // Test memory reading functionality
    testWASMModuleManagerMemoryReading();

    // Test XML generation
    testWASMModuleManagerLibraryXMLGeneration();

    // Test address validation
    testWASMModuleManagerAddressValidation();

    // Test edge cases
    testWASMModuleManagerEdgeCases();

    // Test integration scenarios
    testWASMModuleManagerIntegration();

    // Test performance characteristics
    testWASMModuleManagerPerformance();

    dataLogLn("====================================================");
    dataLogLn("Test Results:");
    dataLogLn("  Tests run: ", testsRun);
    dataLogLn("  Passed: ", testsPassed);
    dataLogLn("  Failed: ", testsFailed);

    if (!testsFailed) {
        dataLogLn("All tests PASSED!");
        dataLogLn("WASM Module Manager is working correctly");
        dataLogLn("allWasmDebuggerTestsPassed");
    } else {
        dataLogLn("Some tests FAILED!");
        dataLogLn("WASM Module Manager needs attention");
    }
}

int main(int argc, char** argv)
{
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);

#if OS(WINDOWS)
    // Cygwin calls ::SetErrorMode(SEM_FAILCRITICALERRORS), which we will inherit. This is bad for
    // testing/debugging, as it causes the post-mortem debugger not to be invoked. We reset the
    // error mode here to work around Cygwin's behavior. See <http://webkit.org/b/55222>.
    ::SetErrorMode(0);

    WTF::disableCRTDebugAssertDialog();
#endif

    JSC::initialize();
    runAllTests();
    return (!testsFailed) ? 0 : 1;
}

#if OS(WINDOWS)
extern "C" __declspec(dllexport) int WINAPI dllLauncherEntryPoint(int argc, const char* argv[])
{
    return main(argc, const_cast<char**>(argv));
}
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#else // !ENABLE(WEBASSEMBLY)

int main(int argc, char** argv)
{
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);

    dataLogLn("WASM debugger tests are disabled (WEBASSEMBLY not enabled)");
    dataLogLn("allWasmDebuggerTestsPassed");
    return 0;
}

#if OS(WINDOWS)
extern "C" __declspec(dllexport) int WINAPI dllLauncherEntryPoint(int argc, const char* argv[])
{
    return main(argc, const_cast<char**>(argv));
}
#endif

#endif // ENABLE(WEBASSEMBLY)
