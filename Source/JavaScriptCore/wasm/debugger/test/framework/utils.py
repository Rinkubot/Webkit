"""
Utility classes for the WebAssembly Debugger Test Framework
"""


class Colors:
    """ANSI color codes for terminal output"""

    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    MAGENTA = "\033[95m"
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"


class Logger:
    """Centralized logging utility"""
    
    _verbose = False
    
    @classmethod
    def set_verbose(cls, verbose: bool):
        """Set verbose mode for all logging"""
        cls._verbose = verbose

    @staticmethod
    def success(msg: str):
        print(f"{Colors.GREEN}✅ {msg}{Colors.RESET}")

    @staticmethod
    def error(msg: str):
        print(f"{Colors.RED}❌ {msg}{Colors.RESET}")

    @staticmethod
    def warning(msg: str):
        print(f"{Colors.YELLOW}⚠️  {msg}{Colors.RESET}")

    @staticmethod
    def info(msg: str):
        print(f"{Colors.BLUE}ℹ️  {msg}{Colors.RESET}")

    @classmethod
    def verbose(cls, msg: str):
        """Print verbose message only if verbose mode is enabled"""
        if cls._verbose:
            print(f"{Colors.DIM}🔍 {msg}{Colors.RESET}")

    @classmethod
    def debug(cls, msg: str):
        """Print debug message only if verbose mode is enabled"""
        if cls._verbose:
            print(f"{Colors.DIM}🐛 DEBUG: {msg}{Colors.RESET}")

    @staticmethod
    def header(msg: str):
        print(f"\n{Colors.BOLD}{Colors.CYAN}=== {msg} ==={Colors.RESET}")

    @staticmethod
    def subheader(msg: str):
        print(f"\n{Colors.BOLD}{Colors.MAGENTA}--- {msg} ---{Colors.RESET}")

    @staticmethod
    def step(step_num: str, msg: str):
        print(f"\n{Colors.BOLD}{Colors.BLUE}Step {step_num}: {msg}{Colors.RESET}")