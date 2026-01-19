"""
Entry point for running robot_controller as a module.

Usage:
    python -m robot_controller
"""

import sys
from .relay_service import main

if __name__ == "__main__":
    sys.exit(main())
