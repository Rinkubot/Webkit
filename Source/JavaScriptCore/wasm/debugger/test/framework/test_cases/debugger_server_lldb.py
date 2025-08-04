"""
Debugger Server and LLDB Test Case

Test case for starting debugger server and connecting LLDB
"""

from ..base import BaseTestCase, TestResult
from ..utils import Logger


class DebuggerServerAndLLDBTestCase(BaseTestCase):
    """Combined test case for starting debugger server and connecting LLDB"""

    def __init__(self, build_config: str = None):
        super().__init__(build_config)
        self.description = f"Start WebAssembly debugger server with add/main.js and connect LLDB"
        self.stop_detected = False
        self.interrupt_sent = False

    def execute(self):
        """Execute the test case"""
        Logger.step("1", "Starting WebAssembly Debugger Server and Connecting LLDB")

        self.setup_debugging_session_or_raise("add/main.js")

        Logger.success("Debugger server started and LLDB connected successfully")