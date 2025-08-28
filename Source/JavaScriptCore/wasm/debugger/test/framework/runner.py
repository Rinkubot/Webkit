"""
Test runner for the WebAssembly Debugger Test Framework
"""

import time
from typing import List, Optional

from .base import BaseTestCase, TestResult
from .registry import TestRegistry
from .utils import Logger
from .process_manager import ProcessManager


class WebAssemblyDebuggerTestRunner:
    """Test runner for WebAssembly debugger tests"""

    def __init__(self, build_config: Optional[str] = None):
        self.logger = Logger()
        self.registry = TestRegistry()
        self.results: List[TestResult] = []
        self.build_config = build_config

    def run_all_tests(self) -> List[TestResult]:
        """Run all registered tests"""
        self.logger.header("🚀 Starting WebAssembly Debugger Test Suite")
        
        test_names = self.registry.get_all_test_names()
        if not test_names:
            self.logger.warning("No tests registered")
            return []

        self.logger.info(f"Found {len(test_names)} tests to run")
        
        # Clear previous results
        self.results.clear()
        
        # Run each test
        for test_name in test_names:
            self.logger.subheader(f"Running test: {test_name}")
            result = self._run_single_test(test_name)
            self.results.append(result)
            
            # Log result
            if result.success:
                self.logger.success(f"✅ {test_name} PASSED ({result.duration():.2f}s)")
            else:
                self.logger.error(f"❌ {test_name} FAILED ({result.duration():.2f}s): {result.error_message}")

        # Print summary
        self._print_summary()
        
        return self.results

    def run_specific_tests(self, test_names: List[str]) -> List[TestResult]:
        """Run specific tests by name"""
        self.logger.header(f"🚀 Running {len(test_names)} specific tests")
        
        # Clear previous results
        self.results.clear()
        
        # Run each specified test
        for test_name in test_names:
            if not self.registry.get_test_class(test_name):
                self.logger.error(f"Test '{test_name}' not found in registry")
                continue
                
            self.logger.subheader(f"Running test: {test_name}")
            result = self._run_single_test(test_name)
            self.results.append(result)
            
            # Log result
            if result.success:
                self.logger.success(f"✅ {test_name} PASSED ({result.duration():.2f}s)")
            else:
                self.logger.error(f"❌ {test_name} FAILED ({result.duration():.2f}s): {result.error_message}")

        # Print summary
        self._print_summary()
        
        return self.results

    def _run_single_test(self, test_name: str) -> TestResult:
        """Run a single test case"""
        try:
            # Create test instance with build configuration
            test_instance = self.registry.create_test_instance(test_name, self.build_config)
            
            # Run the test
            result = test_instance.run()
            
            return result
            
        except Exception as e:
            # Create failed result for exceptions during test creation/execution
            result = TestResult(test_name)
            result.mark_failure(f"Test execution error: {str(e)}")
            return result

    def _print_summary(self):
        """Print test run summary"""
        if not self.results:
            return
            
        total_tests = len(self.results)
        passed_tests = sum(1 for r in self.results if r.success)
        failed_tests = total_tests - passed_tests
        total_duration = sum(r.duration() for r in self.results)
        
        self.logger.header("📊 Test Summary")
        self.logger.info(f"Total tests: {total_tests}")
        self.logger.info(f"Passed: {passed_tests}")
        self.logger.info(f"Failed: {failed_tests}")
        self.logger.info(f"Total duration: {total_duration:.2f}s")
        
        if failed_tests > 0:
            self.logger.subheader("❌ Failed Tests:")
            for result in self.results:
                if not result.success:
                    self.logger.error(f"  • {result.name}: {result.error_message}")
        
        # Overall result
        if failed_tests == 0:
            self.logger.success("🎉 All tests passed!")
        else:
            self.logger.error(f"💥 {failed_tests} test(s) failed")

    def get_results(self) -> List[TestResult]:
        """Get test results"""
        return self.results.copy()

    def get_passed_count(self) -> int:
        """Get number of passed tests"""
        return sum(1 for r in self.results if r.success)

    def get_failed_count(self) -> int:
        """Get number of failed tests"""
        return sum(1 for r in self.results if not r.success)

    def get_total_duration(self) -> float:
        """Get total test duration"""
        return sum(r.duration() for r in self.results)

    def cleanup(self):
        """Cleanup any remaining resources"""
        self.logger.info("🧹 Cleaning up test runner resources")
        
        # Emergency cleanup of any remaining processes
        process_manager = ProcessManager()
        active_processes = process_manager.get_active_processes()
        
        if active_processes:
            self.logger.warning(f"Found {len(active_processes)} active processes, cleaning up")
            process_manager.cleanup_all_processes()
        
        self.logger.success("✅ Test runner cleanup complete")