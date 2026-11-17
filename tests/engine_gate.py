#!/usr/bin/env python3
"""Compatibility entry point for the fork-owned standalone gate.

Unless --pixi-manifest is given, the gate builds against this project's Pixi
environment (pixi.lock), the cairo and zlib builds the goldens were made with.
Unless --ccachedir is given, the gate shares this project's build/.build_cache.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

if __name__ == "__main__":
    args = sys.argv[1:]
    project = Path(__file__).resolve().parents[1]
    root = project / "vendor/engrave"
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--engine-source", type=Path, default=root)
    parser.add_argument("--pixi-manifest", type=Path)
    parser.add_argument("--ccachedir", type=Path)
    parsed, _ = parser.parse_known_args(args)
    root = parsed.engine_source.resolve()
    prefix = []
    if parsed.pixi_manifest is None:
        prefix += ["--pixi-manifest", str(project / "pixi.toml")]
    if parsed.ccachedir is None:
        prefix += ["--ccachedir", str(project / "build/.build_cache")]
    args = [*prefix, *args]
    os.environ["PYTHONPATH"] = os.pathsep.join(
        (str(root / "tests"), str(root / "tools"), os.environ.get("PYTHONPATH", ""))
    )
    sys.exit(subprocess.call([sys.executable, str(root / "tests/gate.py"), *args]))
