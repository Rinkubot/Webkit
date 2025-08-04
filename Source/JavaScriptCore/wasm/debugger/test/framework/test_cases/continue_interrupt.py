"""
Continue-Interrupt Test Case

Test case for continue-interrupt cycles using BaseTestCase architecture
"""

import time
from ..base import BaseTestCase
from ..utils import Logger


class ContinueInterruptTestCase(BaseTestCase):
    """Test case for continue-interrupt cycles"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = "Perform continue-interrupt debugging cycles"
        self.cycles = 10

    def execute(self):
        """Execute the test case"""
        self.logger.info(f"Continue-interrupt testing with {self.cycles} cycles...")

        self.setup_debugging_session_or_raise("add/main.js")

        for cycle in range(1, self.cycles + 1):
            try:
                self.send_lldb_command_or_raise("c")
                self.send_lldb_command_or_raise("process interrupt")
            except Exception as e:
                raise Exception(f"Cycle {cycle} failed: {e}")

        self.logger.success(f"All {self.cycles} cycles completed successfully")
