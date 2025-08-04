#!/usr/bin/env python3
"""
WebAssembly Debugger Test Framework - Modular Version

A modular test framework for WebAssembly debugging functionality using the new framework structure.
"""

import sys
import argparse
from pathlib import Path

# Import the modular framework
from framework import (
    WebAssemblyDebuggerTestRunner,
    TestRegistry,
    Logger,
    WebKitEnvironment,
)

# Import test cases from the modular structure
from framework.test_cases import (
    # Core test cases
    JavaScriptCoreTestCase,
    DebuggerServerAndLLDBTestCase,
    ContinueInterruptTestCase,
    # Breakpoint tests
    BreakpointTestCase,
    BreakpointManagementTestCase,
    # Stepping tests
    StepOverTestCase,
    StepIntoTestCase,
    StepOutTestCase,
    StepInstructionTestCase,
    # Inspection tests
    InspectionTestCase,
    # Advanced tests
)


def create_test_runner(
    verbose: bool = False, build_config: str = None
) -> WebAssemblyDebuggerTestRunner:
    """Create and configure the test runner with registered test cases"""
    runner = WebAssemblyDebuggerTestRunner(build_config=build_config)

    # Register core test cases
    runner.registry.register_test(JavaScriptCoreTestCase)
    runner.registry.register_test(DebuggerServerAndLLDBTestCase)
    runner.registry.register_test(ContinueInterruptTestCase)

    # Register breakpoint tests
    runner.registry.register_test(BreakpointTestCase)
    runner.registry.register_test(BreakpointManagementTestCase)

    # Register stepping tests
    runner.registry.register_test(StepOverTestCase)
    runner.registry.register_test(StepIntoTestCase)
    runner.registry.register_test(StepOutTestCase)
    runner.registry.register_test(StepInstructionTestCase)

    # Register inspection tests
    runner.registry.register_test(InspectionTestCase)

    return runner


def main():
    """Main entry point for the modular test framework"""
    parser = argparse.ArgumentParser(
        description="WebAssembly Debugger Test Framework - Modular Version",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 test-wasm-debugger.py                    # Use auto-detected JSC
  python3 test-wasm-debugger.py --debug            # Use WebKitBuild/Debug/jsc
  python3 test-wasm-debugger.py --release          # Use WebKitBuild/Release/jsc
  python3 test-wasm-debugger.py --list             # List available tests
  python3 test-wasm-debugger.py --test DebuggerServerAndLLDBTestCase  # Run specific test
        """,
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Enable verbose logging"
    )
    parser.add_argument(
        "--test", "-t", action="append", help="Run specific test case(s)"
    )
    parser.add_argument(
        "--list", "-l", action="store_true", help="List available test cases"
    )

    # Build configuration options (mutually exclusive)
    build_group = parser.add_mutually_exclusive_group()
    build_group.add_argument(
        "--debug",
        action="store_true",
        help="Use WebKitBuild/Debug/jsc (forces Debug build)",
    )
    build_group.add_argument(
        "--release",
        action="store_true",
        help="Use WebKitBuild/Release/jsc (forces Release build)",
    )

    args = parser.parse_args()

    # Determine build configuration
    build_config = None
    if args.debug:
        build_config = "Debug"
    elif args.release:
        build_config = "Release"

    # Set verbose mode on Logger
    if args.verbose:
        Logger.set_verbose(True)
        Logger.verbose("Verbose mode enabled")

    # Create test runner
    runner = create_test_runner(verbose=args.verbose, build_config=build_config)

    if args.list:
        Logger.info("Available test cases:")
        for name in runner.registry.get_all_test_names():
            test_class = runner.registry.get_test_class(name)
            # Create a temporary instance to get description
            temp_instance = test_class()
            print(f"  {name}: {temp_instance.description}")
        return

    # Run tests
    try:
        if args.test:
            # Run specific tests
            results = runner.run_specific_tests(args.test)
        else:
            # Run all tests
            results = runner.run_all_tests()

        # Check results
        failed_count = runner.get_failed_count()
        if failed_count > 0:
            Logger.error(f"Test suite completed with {failed_count} failures")
            sys.exit(1)
        else:
            Logger.success("All tests passed!")
            sys.exit(0)

    except KeyboardInterrupt:
        Logger.info("Test interrupted by user")
        sys.exit(1)
    except Exception as e:
        Logger.error(f"Test suite failed: {e}")
        sys.exit(1)
    finally:
        # Cleanup
        runner.cleanup()


if __name__ == "__main__":
    main()
