"""
Base test case classes for the WebAssembly Debugger Test Framework
"""

import os
import subprocess
import time
import threading
from typing import Optional, Dict, Any, List, Union
from enum import Enum
import re

from .process_manager import ProcessManager
from .utils import Logger
from .environment import WebKitEnvironment


class PatternMatchMode(Enum):
    """Pattern matching modes for LLDB output monitoring"""

    ANY = "any"  # Complete when ANY pattern is matched
    ALL = "all"  # Complete when ALL patterns are matched


class PatternMatcher:
    """Enhanced pattern matcher for LLDB output with flexible matching modes"""

    def __init__(
        self,
        patterns: List[Union[str, re.Pattern]],
        mode: PatternMatchMode = PatternMatchMode.ANY,
    ):
        """
        Initialize pattern matcher

        Args:
            patterns: List of string patterns or compiled regex patterns
            mode: PatternMatchMode.ANY (any pattern matches) or PatternMatchMode.ALL (all patterns must match)
        """
        self.patterns = []
        self.mode = mode
        self.matched_patterns = set()

        # Convert string patterns to compiled regex patterns
        for pattern in patterns:
            if isinstance(pattern, str):
                # Escape special regex characters for literal string matching
                escaped_pattern = re.escape(pattern)
                self.patterns.append(re.compile(escaped_pattern))
            elif isinstance(pattern, re.Pattern):
                self.patterns.append(pattern)
            else:
                raise ValueError(
                    f"Pattern must be string or compiled regex, got {type(pattern)}"
                )

    def check_line(self, line: str) -> bool:
        """
        Check if a line matches the pattern criteria

        Args:
            line: Line of text to check

        Returns:
            True if completion criteria are met, False otherwise
        """
        line_matched_any = False

        for i, pattern in enumerate(self.patterns):
            if pattern.search(line):
                self.matched_patterns.add(i)
                line_matched_any = True

                # For ANY mode, return immediately on first match
                if self.mode == PatternMatchMode.ANY:
                    return True

        # For ALL mode, check if all patterns have been matched
        if self.mode == PatternMatchMode.ALL:
            return len(self.matched_patterns) == len(self.patterns)

        return False

    def reset(self):
        """Reset the matcher state"""
        self.matched_patterns.clear()

    def get_matched_patterns(self) -> List[int]:
        """Get indices of patterns that have been matched"""
        return sorted(list(self.matched_patterns))

    def is_complete(self) -> bool:
        """Check if matching is complete based on mode"""
        if self.mode == PatternMatchMode.ANY:
            return len(self.matched_patterns) > 0
        else:  # ALL mode
            return len(self.matched_patterns) == len(self.patterns)


class LLDBMonitoringResult:
    """Result container for LLDB monitoring operations"""

    def __init__(self):
        self.success = False
        self.matched_patterns = []
        self.output_lines = []
        self.completion_reason = ""
        self.timeout = False
        self.error = ""


class TestResult:
    """Test result container"""

    def __init__(self, name: str):
        self.name = name
        self.success = False
        self.error_message = ""
        self.start_time = time.time()
        self.end_time = None

    def mark_success(self):
        """Mark test as successful"""
        self.success = True
        self.end_time = time.time()

    def mark_failure(self, error_message: str):
        """Mark test as failed with error message"""
        self.success = False
        self.error_message = error_message
        self.end_time = time.time()

    def duration(self) -> float:
        """Get test duration in seconds"""
        if self.end_time:
            return self.end_time - self.start_time
        return time.time() - self.start_time


class BaseTestCase:
    """Base class for all WebAssembly debugger test cases"""

    def __init__(self, build_config: str = None):
        self.name = self.__class__.__name__
        self.logger = Logger()
        # Initialize environment with test directory path
        from pathlib import Path

        # Use the test directory path, not the framework base.py path
        test_dir = Path(__file__).parent.parent / "test-wasm-debugger.py"
        self.env = WebKitEnvironment(test_dir, build_config)
        self.process_manager = ProcessManager()

        # Direct process references
        self.debugger_process: Optional[subprocess.Popen] = None
        self.lldb_process: Optional[subprocess.Popen] = None
        self.current_port: Optional[int] = None

        # Synchronization and pattern-based monitoring
        self._lldb_output_lock = threading.Lock()
        self._lldb_output = []
        self._pattern_matcher = None
        self._pattern_completion_event = threading.Event()
        self._monitoring_result = LLDBMonitoringResult()

    def setup(self):
        """Setup method called before test execution"""
        self.logger.header(f"Setting up test: {self.name}")

    def teardown(self):
        """Cleanup method called after test execution"""
        self.logger.header(f"Tearing down test: {self.name}")
        if self.debugger_process or self.lldb_process:
            self.process_manager.cleanup_processes(
                debugger_process=self.debugger_process,
                lldb_process=self.lldb_process,
                port=self.current_port,
            )
        self.debugger_process = None
        self.lldb_process = None
        self.current_port = None

    def run(self) -> TestResult:
        """Run the test case"""
        result = TestResult(self.name)
        try:
            self.setup()
            self.execute()
            result.mark_success()
            self.logger.success(f"Test {self.name} passed")
        except Exception as e:
            result.mark_failure(str(e))
            self.logger.error(f"Test {self.name} failed: {e}")
        finally:
            self.teardown()
        return result

    def execute(self):
        """Override this method in subclasses to implement test logic"""
        raise NotImplementedError("Subclasses must implement execute() method")

    def start_debugger(self, test_file: str) -> bool:
        """
        Start the WebAssembly debugger server

        Args:
            test_file: Path to the test file (JavaScript or WebAssembly) to debug

        Returns:
            True if debugger started successfully, False otherwise
        """
        # Allocate port
        port_result = self.process_manager.allocate_port()
        if not port_result["success"]:
            raise Exception(f"Failed to allocate port: {port_result['error']}")

        self.current_port = port_result["port"]

        # Build command
        jsc_path = self.env.get_jsc_path()
        if not jsc_path:
            raise Exception("JSC executable not found")

        cmd = [jsc_path, f"--wasm-debug={self.current_port}", test_file]

        self.logger.verbose(f"Starting debugger on port {self.current_port}")
        self.logger.verbose(f"Command: {' '.join(cmd)}")

        try:
            # Start debugger process with proper environment and working directory
            test_dir = os.path.dirname(
                os.path.dirname(os.path.abspath(__file__))
            )  # Go up to test directory

            # If test_file contains a directory (like "add/main.js"), set working directory to that subdirectory
            if "/" in test_file:
                test_subdir = os.path.dirname(test_file)
                working_dir = os.path.join(test_dir, test_subdir)
                # Update the command to use just the filename
                test_filename = os.path.basename(test_file)
                cmd = [jsc_path, f"--wasm-debug={self.current_port}", test_filename]
            else:
                working_dir = test_dir

            self.logger.verbose(f"Working directory: {working_dir}")
            self.logger.verbose(
                f"Environment variables: VM={self.env.env.get('VM')}, DYLD_FRAMEWORK_PATH={self.env.env.get('DYLD_FRAMEWORK_PATH')}"
            )
            self.logger.verbose("Starting debugger subprocess...")

            self.debugger_process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                universal_newlines=True,
                cwd=working_dir,  # Set working directory appropriately
                env=self.env.env,  # Use the WebKit environment with DYLD_FRAMEWORK_PATH
            )

            self.logger.verbose(
                f"Debugger process started with PID: {self.debugger_process.pid}"
            )

            # Start debugger output monitoring thread
            self.logger.verbose("Starting debugger output monitoring thread...")
            self._start_debugger_monitoring()
            self.logger.verbose(
                f"Debugger started successfully on port {self.current_port}"
            )
            return True

        except Exception as e:
            self.logger.error(f"Failed to start debugger: {e}")
            if self.debugger_process:
                self.debugger_process.terminate()
                self.debugger_process = None
            return False

    def start_lldb(self, connection_timeout: float = 10.0) -> bool:
        """
        Start LLDB and connect to the debugger using WebAssembly plugin

        Args:
            connection_timeout: Maximum time to wait for connection and stop detection

        Returns:
            True if LLDB started and connected successfully, False otherwise
        """
        if not self.current_port:
            raise Exception("No debugger port allocated")

        # Use environment detection for LLDB path
        lldb_path = self.env.get_lldb_path()
        if not lldb_path:
            raise Exception("LLDB executable not found")

        self.logger.verbose(f"Starting LLDB and connecting to port {self.current_port}")
        self.logger.verbose(f"Using LLDB at: {lldb_path}")

        try:
            # Use LLDB with direct WebAssembly connection command
            connect_cmd = (
                f"process connect --plugin wasm connect://localhost:{self.current_port}"
            )

            self.logger.verbose("Starting LLDB with WebAssembly connection...")
            self.logger.verbose(f"Connection command: {connect_cmd}")

            # Start LLDB process with the connection command
            self.lldb_process = subprocess.Popen(
                [lldb_path, "-o", connect_cmd],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True,
            )
            self.logger.verbose(
                f"LLDB process started with PID: {self.lldb_process.pid}"
            )

            # Start output monitoring thread
            self.logger.verbose("Starting LLDB output monitoring thread...")
            self._start_lldb_monitoring()

            # Wait for stop detection with timeout
            self.logger.verbose(
                f"Waiting for LLDB stop detection (timeout: {connection_timeout}s)..."
            )
            if self.wait_for_lldb_stop(connection_timeout):
                # Register processes with manager
                self.process_manager.register_processes(
                    self.debugger_process,
                    self.lldb_process,
                    self.current_port,
                    self.name,
                )

                self.logger.success(
                    f"LLDB connected to debugger on port {self.current_port} using WebAssembly plugin"
                )
                return True
            else:
                self.logger.error(
                    f"❌ LLDB connection failed - no stop detected within {connection_timeout}s"
                )
                if self.lldb_process:
                    self.lldb_process.terminate()
                    self.lldb_process = None
                return False

        except Exception as e:
            self.logger.error(f"Failed to start LLDB: {e}")
            if self.lldb_process:
                self.lldb_process.terminate()
                self.lldb_process = None
            return False

    def setup_debugging_session(self, test_file: str) -> bool:
        """
        Setup complete debugging session (debugger + LLDB)

        Args:
            test_file: Path to the test file (JavaScript or WebAssembly) to debug

        Returns:
            True if both debugger and LLDB started successfully, False otherwise
        """
        self.logger.header(f"Setting up debugging session for {self.name}")

        # Start debugger
        if not self.start_debugger(test_file):
            return False

        # Start LLDB
        if not self.start_lldb():
            return False

        self.logger.success(f"Debugging session ready for {self.name}")
        return True

    def send_lldb_command_with_patterns(
        self,
        command: str,
        patterns: List[Union[str, re.Pattern]] = None,
        mode: PatternMatchMode = PatternMatchMode.ALL,
        timeout: float = 5.0,
    ) -> LLDBMonitoringResult:
        """
        Send command to LLDB and wait for specific patterns in the output

        Args:
            command: LLDB command to send
            patterns: List of patterns to wait for (if None, uses command-specific patterns)
            mode: PatternMatchMode.ANY or PatternMatchMode.ALL
            timeout: Maximum time to wait for completion

        Returns:
            LLDBMonitoringResult with command execution status
        """
        if not self.lldb_process:
            result = LLDBMonitoringResult()
            result.error = "No LLDB process available"
            return result

        # Use command-specific patterns if none provided
        if patterns is None:
            patterns = self._get_command_completion_patterns(command)

        try:
            # Send the command
            self.logger.verbose(f"Sending LLDB command with pattern monitoring: {command}")

            self.lldb_process.stdin.write(f"{command}\n")
            self.lldb_process.stdin.flush()

            # Wait for completion using pattern matching
            result = self.wait_for_lldb_patterns(patterns, mode, timeout)
            result.output_lines = [
                line for line in result.output_lines if line.strip()
            ]  # Filter empty lines

            return result

        except Exception as e:
            result = LLDBMonitoringResult()
            result.error = f"Failed to send LLDB command: {e}"
            self.logger.error(result.error)
            return result

    def send_lldb_command_or_raise(self, command: str, patterns=None, mode=PatternMatchMode.ALL, timeout=5.0):
        """
        Send LLDB command with patterns and raise exception on failure
        
        Args:
            command: LLDB command to send
            patterns: List of patterns to wait for (optional)
            mode: PatternMatchMode.ANY or PatternMatchMode.ALL
            timeout: Maximum time to wait
            
        Raises:
            Exception: If command fails
            
        Returns:
            LLDBMonitoringResult: The result if successful
        """
        result = self.send_lldb_command_with_patterns(
            command,
            patterns=patterns,
            mode=mode,
            timeout=timeout,
        )
        if not result.success:
            raise Exception(f"Command '{command}' failed: {result.error}")
        return result

    def setup_debugging_session_or_raise(self, test_file: str) -> bool:
        """
        Setup complete debugging session (debugger + LLDB) and raise exception on failure
        
        Args:
            test_file: Path to the test file (JavaScript or WebAssembly) to debug
            
        Raises:
            Exception: If session setup fails
            
        Returns:
            True if successful
        """
        if not self.setup_debugging_session(test_file):
            raise Exception("Session setup failed")
        return True

    def wait_for_lldb_stop(self, timeout: float = 10.0) -> bool:
        """
        Wait for LLDB to detect a stop (breakpoint hit, etc.)

        This is now a wrapper around the enhanced pattern-based monitoring system.

        Args:
            timeout: Maximum time to wait in seconds

        Returns:
            True if stop was detected, False if timeout
        """
        self.logger.verbose(f"Waiting for LLDB stop (timeout: {timeout}s)")

        # Use the enhanced pattern-based system
        result = self.wait_for_lldb_patterns(
            ["Process 1 stopped"], PatternMatchMode.ANY, timeout
        )

        if result.success:
            self.logger.success("LLDB stop detected")
            return True
        else:
            self.logger.warning(f"⏰ Timeout waiting for LLDB stop after {timeout}s")
            return False

    def wait_for_lldb_patterns(
        self,
        patterns: List[Union[str, re.Pattern]],
        mode: PatternMatchMode = PatternMatchMode.ANY,
        timeout: float = 10.0,
    ) -> LLDBMonitoringResult:
        """
        Wait for LLDB output to match specific patterns

        Args:
            patterns: List of patterns to match (strings or compiled regex)
            mode: PatternMatchMode.ANY (any pattern) or PatternMatchMode.ALL (all patterns)
            timeout: Maximum time to wait in seconds

        Returns:
            LLDBMonitoringResult with success status and matched patterns
        """
        self.logger.verbose(f"Waiting for LLDB Patterns: {[str(p) for p in patterns]} (mode: {mode.value}, timeout: {timeout}s)")

        # Set up pattern matcher
        with self._lldb_output_lock:
            self._pattern_matcher = PatternMatcher(patterns, mode)
            self._pattern_completion_event.clear()
            self._monitoring_result = LLDBMonitoringResult()

        # Wait for pattern completion or timeout
        pattern_matched = self._pattern_completion_event.wait(timeout)

        # Get final result
        with self._lldb_output_lock:
            result = self._monitoring_result
            if pattern_matched:
                result.success = True
                result.matched_patterns = self._pattern_matcher.get_matched_patterns()
                result.completion_reason = f"Patterns matched in {mode.value} mode"
                self.logger.verbose(f"LLDB patterns matched: {result.matched_patterns}")
            else:
                result.success = False
                result.timeout = True
                result.completion_reason = f"Timeout after {timeout}s"
                self.logger.warning(
                    f"⏰ Timeout waiting for LLDB patterns after {timeout}s"
                )

            # Clean up
            self._pattern_matcher = None

        return result

    def wait_for_lldb_complete(
        self, command: str, timeout: float = 5.0
    ) -> LLDBMonitoringResult:
        """
        Wait for LLDB command completion using command-specific patterns

        Args:
            command: The LLDB command that was sent
            timeout: Maximum time to wait in seconds

        Returns:
            LLDBMonitoringResult with completion status
        """
        patterns = self._get_command_completion_patterns(command)
        return self.wait_for_lldb_patterns(patterns, PatternMatchMode.ANY, timeout)

    def _get_command_completion_patterns(self, command: str) -> List[str]:
        """
        Get completion patterns for specific LLDB commands

        Args:
            command: The LLDB command

        Returns:
            List of patterns that indicate command completion
        """
        cmd = command.strip().lower()

        # Command-specific completion patterns
        if cmd.startswith("b "):
            return ["Breakpoint"]
        elif cmd.startswith("process interrupt"):
            return ["Process 1 stopped"]
        elif cmd in ["c"]:
            return ["Process 1 resuming"]
        elif cmd in ["n"]:
            return ["Process 1 stopped"]
        elif cmd in ["s"]:
            return ["Process 1 stopped"]
        elif cmd in ["si"]:
            return ["Process 1 stopped"]
        elif cmd in ["fin"]:
            return ["Process 1 stopped"]
        elif cmd.startswith("disass"):
            return ["->", "(lldb)"]
        elif cmd.startswith("frame variable") or cmd.startswith("p "):
            return ["=", "(lldb)"]
        else:
            # Crash here to identify commands without specific patterns
            raise ValueError(
                f"No completion patterns defined for command: '{command}'. Please add specific patterns for this command."
            )

    def get_lldb_output(self) -> list:
        """Get current LLDB output lines"""
        with self._lldb_output_lock:
            return self._lldb_output.copy()

    def clear_lldb_output(self):
        """Clear LLDB output buffer"""
        with self._lldb_output_lock:
            self._lldb_output.clear()

    def _start_debugger_monitoring(self):
        """Start monitoring debugger output in a separate thread"""

        def monitor_debugger_output():
            try:
                while self.debugger_process and self.debugger_process.poll() is None:
                    # Monitor both stdout and stderr
                    stdout_line = self.debugger_process.stdout.readline()
                    stderr_line = self.debugger_process.stderr.readline()

                    if stdout_line:
                        stdout_line = stdout_line.strip()
                        if stdout_line:
                            self.logger.verbose(f"[Debugger] {stdout_line}")

                    if stderr_line:
                        stderr_line = stderr_line.strip()
                        if stderr_line:
                            # Always show stderr as it's usually important
                            self.logger.verbose(f"Debugger Error: {stderr_line}")

            except Exception as e:
                self.logger.verbose(f"Debugger monitoring error: {e}")

        monitor_thread = threading.Thread(target=monitor_debugger_output, daemon=True)
        monitor_thread.start()

    def _start_lldb_monitoring(self):
        """Start monitoring LLDB output in a separate thread with enhanced pattern detection"""

        def monitor_output():
            try:
                while self.lldb_process and self.lldb_process.poll() is None:
                    line = self.lldb_process.stdout.readline()
                    if line:
                        line = line.strip()
                        with self._lldb_output_lock:
                            self._lldb_output.append(line)
                            self._monitoring_result.output_lines.append(line)

                            # Enhanced pattern-based detection
                            if self._pattern_matcher:
                                if self._pattern_matcher.check_line(line):
                                    # Pattern matching completed
                                    self._monitoring_result.success = True
                                    self._monitoring_result.matched_patterns = (
                                        self._pattern_matcher.get_matched_patterns()
                                    )
                                    self._pattern_completion_event.set()

                        self.logger.verbose(f"[LLDB] {line}")

            except Exception as e:
                self.logger.error(f"LLDB monitoring error: {e}")
                with self._lldb_output_lock:
                    self._monitoring_result.error = str(e)
                    self._pattern_completion_event.set()

        monitor_thread = threading.Thread(target=monitor_output, daemon=True)
        monitor_thread.start()
