#!/usr/bin/env python3
"""Write or check the v2 tool contract from the standalone registry dump."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def dump_registry(executable: Path) -> bytes:
    raw = subprocess.check_output([str(executable.resolve())])
    again = subprocess.check_output([str(executable.resolve())])
    if raw != again:
        raise ValueError("registry dump is nondeterministic")
    if b"\r" in raw or raw.startswith(b"\xef\xbb\xbf") or not raw.endswith(b"\n"):
        raise ValueError("registry dump must be BOM-free UTF-8 with LF newlines")
    tools = json.loads(raw)
    if not isinstance(tools, list) or not tools:
        raise ValueError("registry dump must contain a nonempty array")
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path, required=True, help="Standalone registry_dump executable")
    parser.add_argument("--schema", type=Path, default=ROOT / "native/tools.json")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="Fail if schema bytes differ")
    mode.add_argument("--write", action="store_true", help="Update schema only when bytes differ")
    args = parser.parse_args()
    try:
        raw = dump_registry(args.dump)
        if args.check:
            if args.schema.read_bytes() != raw:
                raise ValueError("schema differs from registry dump; regenerate with --write")
            print(f"Registry schema matches: {args.schema}")
        else:
            if not args.schema.exists() or args.schema.read_bytes() != raw:
                args.schema.write_bytes(raw)
                print(f"Wrote registry schema: {args.schema}")
            else:
                print(f"Registry schema unchanged: {args.schema}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Schema generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
