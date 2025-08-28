"""
Inspection Test Cases
"""

import time
from ..base import BaseTestCase, PatternMatchMode


class InspectionTestCase(BaseTestCase):
    """Test disassembly of WebAssembly functions"""

    def __init__(self, build_config: str = None, function_name: str = "add"):
        super().__init__(build_config)
        self.description = (
            f"Disassemble {function_name} function and verify WASM instructions"
        )
        self.function_name = function_name

    def execute(self):
        self.logger.info(f"Disassembling {self.function_name} function...")

        self.setup_debugging_session_or_raise("add/main.js")

        try:
            self.send_lldb_command_or_raise("b add")
            self.send_lldb_command_or_raise(
                "c",
                patterns=[
                    "Process 1 stopped",
                    "Process 1 stopped",
                    "stop reason = breakpoint",
                ],
            )

            self.send_lldb_command_or_raise(
                "target modules list", patterns=["(0x4000000000000000)"]
            )

            self.send_lldb_command_or_raise(
                "list 1",
                patterns=[
                    "add(int a, int b)",
                    "main()",
                ],
            )

            self.send_lldb_command_or_raise(
                "dis", patterns=["->  0x400000000000018b <+28>: local.get 2"]
            )

            self.send_lldb_command_or_raise(
                "bt",
                patterns=[
                    "frame #0: 0x400000000000018b",
                    "frame #1: 0x40000000000001e0",
                    "frame #2: 0x4000000000000201",
                ],
            )

            self.send_lldb_command_or_raise(
                "var",
                patterns=[
                    "a =",
                    "b =",
                    "result =",
                ],
            )

            self.send_lldb_command_or_raise(
                "thread list",
                patterns=[
                    "0x400000000000018b",
                    "add.c:2:18",
                ],
            )

            # TODO: memory read

        except Exception as e:
            raise Exception(f"Disassembly test failed: {e}")
