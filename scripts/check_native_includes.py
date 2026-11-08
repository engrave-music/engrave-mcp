#!/usr/bin/env python3
"""Enforce public engine includes in every native host and test source."""

from __future__ import annotations

import argparse
from pathlib import Path

from engrave_mcp_testing.native_includes import audit

ROOT = Path(__file__).resolve().parents[1]

# Standard C/C++ and platform headers. Other dependencies have explicit
# prefixes below, so an unqualified engine header cannot slip through.


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-source", type=Path, default=ROOT / "vendor/engrave")
    args = parser.parse_args()
    native = ROOT / "engrave_mcp"
    public = args.engine_source / "engrave/include"
    if not public.is_dir():
        parser.error(f"engine public include directory does not exist: {public}")
    errors = audit(native, public)
    if errors:
        print("\n".join(errors))
        return 1
    print(f"native includes: public engine headers only ({native})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
