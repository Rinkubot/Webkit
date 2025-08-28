"""
Stepping Test Cases
"""

import time
from ..base import BaseTestCase, PatternMatchMode


class StepOverTestCase(BaseTestCase):
    """Test step-over functionality (next command)"""

    def __init__(self, build_config: str = None, steps: int = 3):
        super().__init__(build_config)
        self.description = f"Step over {steps} source lines using 'next' command"
        self.steps = steps
        self.steps_completed = 0

    def execute(self):
        self.logger.info(f"Step-over testing with {self.steps} steps...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            self.send_lldb_command_or_raise("b main", timeout=3.0)

            self.send_lldb_command_or_raise(
                "c",
                patterns=[
                    "Process 1 stopped",
                    "Process 1 stopped",
                    "stop reason = breakpoint",
                ],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            self.send_lldb_command_or_raise(
                "dis",
                patterns=["->  0x40000000000001c0 <+29>: local.get 0"],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            patterns = [
                ["->  0x40000000000001c7 <+36>: local.get 0"],
                ["->  0x40000000000001ce <+43>: local.get 0"],
                ["->  0x40000000000001e3 <+64>: local.get 0"],
                ["->  0x4000000000000201"],
                ["->  0x40000000000001c0 <+29>: local.get 0"],
            ]

            for _ in range(10):
                for pattern in patterns:
                    self.send_lldb_command_or_raise("n", timeout=3.0)

                    self.send_lldb_command_or_raise(
                        "dis",
                        patterns=pattern,
                        mode=PatternMatchMode.ALL,
                        timeout=5.0,
                    )
                    self.steps_completed += 1

            self.logger.success(
                f"Successfully stepped over {self.steps_completed} source lines"
            )

        except Exception as e:
            raise Exception(f"Step-over failed after {self.steps_completed} steps: {e}")


class StepIntoTestCase(BaseTestCase):
    """Test step-into functionality based on step-in.txt log"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = "Test step-into functionality to enter function calls"
        self.steps_completed = 0

    def execute(self):
        self.logger.info("Testing step-into functionality...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            self.send_lldb_command_or_raise("b main", timeout=3.0)

            self.send_lldb_command_or_raise(
                "c",
                patterns=[
                    "Process 1 stopped",
                    "Process 1 stopped",
                    "stop reason = breakpoint",
                ],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            self.send_lldb_command_or_raise(
                "dis",
                patterns=["->  0x40000000000001c0 <+29>: local.get 0"],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            patterns = [
                ["->  0x40000000000001c7 <+36>: local.get 0"],
                ["->  0x40000000000001ce <+43>: local.get 0"],
                ["->  0x4000000000000172 <+3>:  global.get 0"],
                ["->  0x400000000000018b <+28>: local.get 2"],
                ["->  0x400000000000019b <+44>: local.get 2"],
                ["->  0x40000000000001e0 <+61>: i32.store 0"],
                ["->  0x40000000000001e3 <+64>: local.get 0"],
                ["->  0x4000000000000201"],
                ["->  0x40000000000001c0 <+29>: local.get 0"],
            ]

            for _ in range(10):
                for pattern in patterns:
                    self.send_lldb_command_or_raise("s", timeout=3.0)

                    self.send_lldb_command_or_raise(
                        "dis",
                        patterns=pattern,
                        mode=PatternMatchMode.ALL,
                        timeout=5.0,
                    )
                    self.steps_completed += 1

            self.logger.success(
                f"Step-into test completed with {self.steps_completed} steps"
            )

        except Exception as e:
            raise Exception(f"Step-into test failed: {e}")


class StepOutTestCase(BaseTestCase):
    """Test step-out functionality based on step-out.txt log"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = "Test step-out functionality to exit current function"
        self.step_out_attempts = 0

    def execute(self):
        self.logger.info("Testing step-out functionality...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            self.send_lldb_command_or_raise("b add", timeout=3.0)

            for _ in range(10):
                self.send_lldb_command_or_raise(
                    "c",
                    patterns=[
                        "Process 1 stopped",
                        "Process 1 stopped",
                        "stop reason = breakpoint",
                    ],
                    mode=PatternMatchMode.ALL,
                    timeout=5.0,
                )

                self.send_lldb_command_or_raise(
                    "dis",
                    patterns=["->  0x400000000000018b <+28>: local.get 2"],
                    mode=PatternMatchMode.ALL,
                    timeout=5.0,
                )

                self.send_lldb_command_or_raise("fin", timeout=5.0)
                
                self.send_lldb_command_or_raise(
                    "dis",
                    patterns=["->  0x40000000000001e0 <+61>: i32.store 0"],
                    mode=PatternMatchMode.ALL,
                    timeout=5.0,
                )

                self.step_out_attempts += 1

            self.logger.success(
                f"Step-out test completed with {self.step_out_attempts} attempts"
            )

        except Exception as e:
            raise Exception(f"Step-out test failed: {e}")


class StepInstructionTestCase(BaseTestCase):
    """Test single-stepping through WebAssembly instructions"""

    def __init__(self, build_config: str = None, steps: int = 5):
        super().__init__(build_config)
        self.description = f"Single-step through {steps} WebAssembly instructions"
        self.steps = steps
        self.steps_completed = 0

    def execute(self):
        self.logger.info(f"Single-stepping through {self.steps} WASM instructions...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            self.send_lldb_command_or_raise("b add", timeout=3.0)

            self.send_lldb_command_or_raise(
                "c",
                patterns=[
                    "Process 1 stopped",
                    "Process 1 stopped",
                    "stop reason = breakpoint",
                ],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            self.send_lldb_command_or_raise(
                "dis",
                patterns=["->  0x400000000000018b <+28>: local.get 2"],
                mode=PatternMatchMode.ALL,
                timeout=5.0,
            )

            patterns = [
                ["->  0x400000000000018d <+30>: local.get 2"],
                ["->  0x400000000000018f <+32>: i32.load 12"],
                ["->  0x4000000000000192 <+35>: local.get 2"],
                ["->  0x4000000000000194 <+37>: i32.load 8"],
                ["->  0x4000000000000197 <+40>: i32.add"],
                ["->  0x4000000000000198 <+41>: i32.store 4"],
                ["->  0x400000000000019b <+44>: local.get 2"],
                ["->  0x400000000000019d <+46>: i32.load 4"],
                ["->  0x40000000000001a0 <+49>: return"],
            ]
            for pattern in patterns:
                self.send_lldb_command_or_raise("si", timeout=2.0)

                self.send_lldb_command_or_raise(
                    "dis",
                    patterns=pattern,
                    mode=PatternMatchMode.ALL,
                    timeout=5.0,
                )
                self.steps_completed += 1

            self.logger.success(
                f"Successfully stepped through {self.steps_completed} instructions"
            )

        except Exception as e:
            raise Exception(
                f"Single-step failed after {self.steps_completed} steps: {e}"
            )
