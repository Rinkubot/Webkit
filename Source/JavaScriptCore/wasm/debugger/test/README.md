# WebAssembly Debugger Test Framework

A modular, thread-safe testing framework for WebAssembly debugging functionality in WebKit's JavaScriptCore.

## Architecture

### Framework Structure

```
framework/
├── __init__.py              # Main framework exports
├── base.py                  # BaseTestCase with pattern-based monitoring
├── environment.py           # WebKit environment management
├── process_manager.py       # Centralized process management
├── registry.py              # Test case registry
├── runner.py                # Test execution runner
├── utils.py                 # Utilities (Colors, Logger)
└── test_cases/              # Individual test case modules
    ├── __init__.py
    └── ...
```

### Key Components

#### ProcessManager (Singleton)
- Thread-safe process management with synchronized port allocation
- Incremental port assignment (12340, 12341, 12342...)
- Automatic cleanup of debugger and LLDB processes

#### BaseTestCase
- Abstract base class for all test cases
- Built-in debugging session management with `setup_debugging_session_or_raise()`
- Enhanced pattern-based LLDB monitoring with `send_lldb_command_or_raise()`
- Automatic cleanup in teardown

#### Pattern-Based Monitoring System
- **PatternMatchMode**: ANY (match any pattern) or ALL (match all patterns)
- **PatternMatcher**: Supports string patterns and regex patterns
- **LLDBMonitoringResult**: Comprehensive result tracking with matched patterns and output
