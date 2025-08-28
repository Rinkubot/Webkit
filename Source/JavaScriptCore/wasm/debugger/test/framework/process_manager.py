"""
Process management for the WebAssembly Debugger Test Framework
"""

import subprocess
import threading
from typing import List, Dict, Any


class ProcessManager:
    """Centralized synchronized manager for debugger+LLDB process pairs"""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        """Singleton pattern for centralized process management"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if not self._initialized:
            # Direct process tracking: List of {'debugger': process, 'lldb': process, 'port': int, 'test_name': str}
            self.active_processes: List[Dict[str, Any]] = []
            # Port allocation - incremental starting from 12340
            self._next_port = 12340
            self._max_port = 12399
            # Thread synchronization
            self._allocation_lock = threading.Lock()
            self._process_lock = threading.Lock()
            self._initialized = True

    def allocate_port(self) -> Dict[str, Any]:
        """
        Allocate a unique incremental port

        Returns:
            Dict with 'success', 'port', 'error' keys
        """
        with self._allocation_lock:
            # Allocate next available port
            if self._next_port > self._max_port:
                return {
                    "success": False,
                    "error": f"No more ports available (max: {self._max_port})",
                    "port": None,
                }

            allocated_port = self._next_port
            self._next_port += 1

            return {
                "success": True,
                "port": allocated_port,
                "error": None,
            }

    def register_processes(
        self,
        debugger_process: subprocess.Popen,
        lldb_process: subprocess.Popen,
        port: int,
        test_name: str
    ):
        """Register processes for tracking and cleanup"""
        with self._process_lock:
            process_entry = {
                "debugger": debugger_process,
                "lldb": lldb_process,
                "port": port,
                "test_name": test_name,
            }
            self.active_processes.append(process_entry)
            print(f"🔧 Registered processes for {test_name} on port {port}")

    def cleanup_processes(
        self,
        debugger_process: subprocess.Popen = None,
        lldb_process: subprocess.Popen = None,
        port: int = None
    ):
        """Clean up specific processes"""
        with self._process_lock:
            # Find and remove the process entry
            entry_to_remove = None
            for entry in self.active_processes:
                if (debugger_process and entry["debugger"] == debugger_process) or \
                   (lldb_process and entry["lldb"] == lldb_process) or \
                   (port and entry["port"] == port):
                    entry_to_remove = entry
                    break

            if entry_to_remove:
                self.active_processes.remove(entry_to_remove)
                port = entry_to_remove["port"]
                test_name = entry_to_remove["test_name"]
                
                print(f"🧹 Cleaning up processes for {test_name} on port {port}")

                # Stop processes
                for process_type in ["debugger", "lldb"]:
                    process = entry_to_remove.get(process_type)
                    if process:
                        print(
                            f"🧹 Stopping {process_type} process (PID: {process.pid if process.poll() is None else 'terminated'})"
                        )
                        try:
                            if process.poll() is None:
                                if process_type == "lldb" and hasattr(process, "stdin"):
                                    try:
                                        process.stdin.write("quit\n")
                                        process.stdin.flush()
                                        process.wait(timeout=2)
                                    except Exception:
                                        process.terminate()
                                        process.wait(timeout=1)
                                else:
                                    process.terminate()
                                    process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                        except Exception:
                            pass

                # Clean up port
                print(f"🧹 Cleaning up port {port}")
                self._cleanup_port(port)
                print(f"🧹 Process cleanup complete for {test_name}")

    def _cleanup_port(self, port: int):
        """Kill any processes using the specified port"""
        try:
            # Kill processes using the port
            result = subprocess.run(
                f"lsof -ti:{port}", shell=True, capture_output=True, text=True
            )
            if result.stdout.strip():
                for pid in result.stdout.strip().split("\n"):
                    if pid.strip():
                        subprocess.run(
                            f"kill -9 {pid}", shell=True, capture_output=True
                        )

            # Kill JSC processes with --wasm-debug
            result = subprocess.run(
                "pgrep -f 'jsc.*--wasm-debug'",
                shell=True,
                capture_output=True,
                text=True,
            )
            if result.stdout.strip():
                for pid in result.stdout.strip().split("\n"):
                    if pid.strip():
                        subprocess.run(
                            f"kill -9 {pid}", shell=True, capture_output=True
                        )
        except Exception:
            pass

    def get_active_processes(self) -> List[Dict[str, Any]]:
        """Get all active processes (for debugging)"""
        with self._process_lock:
            return self.active_processes.copy()

    def cleanup_all_processes(self):
        """Emergency cleanup of all processes"""
        with self._process_lock:
            processes_to_cleanup = self.active_processes.copy()
            for entry in processes_to_cleanup:
                self.cleanup_processes(port=entry["port"])