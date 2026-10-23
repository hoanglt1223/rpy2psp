"""PyInstaller entry point for the Windows release."""

import sys

from rpy2psp.cli import main

if __name__ == "__main__":
    sys.exit(main())
