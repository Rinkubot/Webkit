"""
Breakpoint Test Cases
"""

import time
from ..base import BaseTestCase, PatternMatchMode


class BreakpointTestCase(BaseTestCase):
    """Test setting breakpoints and verifying they are hit"""

    def __init__(self, build_config: str = None, function_name: str = "add"):
        super().__init__(build_config)
        self.description = (
            f"Set breakpoint in {function_name} function and verify it's hit"
        )
        self.function_name = function_name
        self.breakpoint_hits = 0
        self.expected_hits = 3

    def execute(self):
        self.logger.info(f"Setting breakpoint in {self.function_name} function...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            # Set breakpoint
            self.send_lldb_command_or_raise(f"b {self.function_name}", timeout=3.0)

            self.logger.verbose(f"Breakpoint set in {self.function_name}")

            # Continue and wait for breakpoint hits
            for i in range(self.expected_hits):
                patterns = [
                    "Process 1 stopped",
                    "Process 1 stopped",
                    "stop reason = breakpoint",
                ]
                self.send_lldb_command_or_raise(
                    "c", patterns=patterns, mode=PatternMatchMode.ALL, timeout=5.0
                )

                self.logger.verbose(f"Continue {i + 1} completed")
                self.breakpoint_hits += 1

            self.logger.success(
                f"Breakpoint hit {self.breakpoint_hits} times as expected"
            )

        except Exception as e:
            raise Exception(f"Breakpoint test failed: {e}")


class BreakpointManagementTestCase(BaseTestCase):
    """Test breakpoint management based on breakpoint-set.txt log"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = "Test setting, listing, and deleting breakpoints"
        self.breakpoints_set = 0
        self.breakpoints_deleted = 0

    def execute(self):
        self.logger.info("Testing breakpoint management...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            # Set multiple breakpoints
            self.send_lldb_command_or_raise("b main", timeout=3.0)
            self.breakpoints_set += 1

            self.send_lldb_command_or_raise("b add", timeout=3.0)
            self.breakpoints_set += 1

            # List breakpoints
            self.send_lldb_command_or_raise(
                "br list",
                patterns=[
                    "Current breakpoints:",
                    "1: name = 'main'",
                    "2: name = 'add'",
                ],
                mode=PatternMatchMode.ALL,
                timeout=3.0,
            )

            # Test breakpoint hits
            for _ in range(0, 10):
                self.send_lldb_command_or_raise(
                    "c",
                    patterns=[
                        "add.c:7:9",
                        "add.c:2:18",
                    ],
                    mode=PatternMatchMode.ANY,
                    timeout=5.0,
                )

            # Delete all breakpoints
            self.send_lldb_command_or_raise(
                "br del 1",
                patterns=[
                    "deleted",
                ],
                mode=PatternMatchMode.ALL,
                timeout=3.0,
            )
            self.breakpoints_deleted += 1

            self.send_lldb_command_or_raise(
                "br del 2",
                patterns=[
                    "deleted",
                ],
                mode=PatternMatchMode.ALL,
                timeout=3.0,
            )
            self.breakpoints_deleted += 1

            if self.breakpoints_deleted != self.breakpoints_set:
                raise Exception(
                    f"Failed! breakpoints_deleted: {self.breakpoints_deleted} but breakpoints_set: {self.breakpoints_set}"
                )

            self.logger.success(
                f"Breakpoint management completed: {self.breakpoints_set} set, {self.breakpoints_deleted} deleted"
            )

        except Exception as e:
            raise Exception(f"Breakpoint management test failed: {e}")
