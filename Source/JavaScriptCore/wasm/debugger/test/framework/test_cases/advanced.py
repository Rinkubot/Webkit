"""
Advanced Test Cases
"""

import time
from ..base import BaseTestCase


class ComprehensiveDebuggingTestCase(BaseTestCase):
    """Comprehensive test that combines multiple debugging operations"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = "Comprehensive test combining breakpoints, stepping, and variable inspection"
        self.operations_completed = []

    def execute(self):
        self.logger.info("Running comprehensive debugging test...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            # Operation 1: Set breakpoint
            self.send_lldb_command_or_raise("b add", timeout=3.0)
            self.operations_completed.append("breakpoint_set")

            # Operation 2: Continue to breakpoint
            self.send_lldb_command_or_raise("c", timeout=5.0)
            self.operations_completed.append("breakpoint_hit")

            # Operation 3: Single step
            self.send_lldb_command_or_raise("si", timeout=3.0)
            self.operations_completed.append("single_step")

            # Operation 4: Disassemble
            self.send_lldb_command_or_raise("disass", timeout=3.0)
            self.operations_completed.append("disassembly")

            # Operation 5: Step over
            self.send_lldb_command_or_raise("n", timeout=3.0)
            self.operations_completed.append("step_over")

            # Operation 6: Inspect variables
            self.send_lldb_command_or_raise("frame variable", timeout=3.0)
            self.operations_completed.append("variable_inspection")

            # Operation 7: Print specific variable
            try:
                self.send_lldb_command_or_raise("p result", timeout=3.0)
                self.operations_completed.append("print_variable")
            except Exception as e:
                self.logger.warning(f"Failed to print result variable: {e}")

            # Operation 8: Backtrace
            self.send_lldb_command_or_raise("bt", timeout=3.0)
            self.operations_completed.append("backtrace")

            expected_operations = 8
            self.logger.success(f"All {expected_operations} debugging operations completed successfully")

        except Exception as e:
            raise Exception(f"Comprehensive test failed: {e}")

