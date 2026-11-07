#!/usr/bin/env python3
"""Compatibility entry point for the fork-owned standalone gate."""

import argparse
import os
import subprocess
import sys
from pathlib import Path

if __name__ == "__main__":
    args = sys.argv[1:]
    root = Path(__file__).resolve().parents[1] / "vendor/engrave"
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--engine-source", type=Path, default=root)
    parsed, _ = parser.parse_known_args(args)
    root = parsed.engine_source.resolve()
    os.environ["PYTHONPATH"] = os.pathsep.join((str(root / "tests"), str(root / "tools")))
    sys.exit(subprocess.call([sys.executable, str(root / "tests/gate.py"), *args]))
