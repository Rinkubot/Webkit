"""
WebAssembly Debugger Test Framework
A modular test framework for WebAssembly debugging functionality
"""

from .base import BaseTestCase, TestResult
from .environment import WebKitEnvironment
from .process_manager import ProcessManager
from .runner import WebAssemblyDebuggerTestRunner
from .registry import TestRegistry
from .utils import Colors, Logger

__all__ = [
    'BaseTestCase',
    'TestResult', 
    'WebKitEnvironment',
    'ProcessManager',
    'WebAssemblyDebuggerTestRunner',
    'TestRegistry',
    'Colors',
    'Logger'
]