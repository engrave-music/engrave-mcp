#!/usr/bin/env python3
"""Compatibility entry point for the fork-owned standalone gate."""

import subprocess
import sys
from pathlib import Path

if __name__ == "__main__":
    args = sys.argv[1:]
    root = Path(__file__).resolve().parents[1] / "vendor/engrave"
    if "--engine-source" in args:
        root = Path(args[args.index("--engine-source") + 1]).resolve()
    sys.exit(subprocess.call([sys.executable, str(root / "tests/gate.py"), *args]))
