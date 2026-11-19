#!/usr/bin/env python3
"""Validate the pinned engine checkout, Bazel build inputs, and Pixi native pins.

Usage:
    python scripts/check_build_inputs.py [--check] [--engine-source PATH]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from engrave_testing.paths import pixi_env

NATIVE_PINNED_PACKAGES = ("cairo", "zlib", "libzlib")
_LOCKED_PACKAGE = re.compile(r"^- conda: (\S+/(win-64|linux-64)/([A-Za-z0-9_.-]+?)-\d\S*\.conda)$", re.MULTILINE)


def _find_native_root(project_root: Path) -> Path:
    env = pixi_env(project_root)
    library = env / "Library"
    return library if library.is_dir() else env


def _locked_native_packages(lock: Path) -> dict[tuple[str, str], str]:
    pins: dict[tuple[str, str], str] = {}
    for url, platform, name in _LOCKED_PACKAGE.findall(lock.read_text(encoding="utf-8")):
        if name in NATIVE_PINNED_PACKAGES:
            pins[platform, name] = url
    return pins


def _check_native_pins(host_lock: Path, engine_lock: Path) -> list[str]:
    for lock in (host_lock, engine_lock):
        if not lock.is_file():
            return [f"missing Pixi lock: {lock}"]
    host, engine = _locked_native_packages(host_lock), _locked_native_packages(engine_lock)
    errors = []
    for key in sorted(host.keys() | engine.keys()):
        if host.get(key) != engine.get(key):
            errors.append(
                f"pixi.lock pins differ for {key[1]} ({key[0]}): host {host.get(key)}, engine {engine.get(key)}"
            )
    return errors


def validate(project_root: Path, source_root: Path) -> list[str]:
    errors: list[str] = []
    native_root = _find_native_root(project_root)

    required_files = {
        "host MODULE.bazel": project_root / "MODULE.bazel",
        "host BUILD.bazel": project_root / "BUILD.bazel",
        "host .bazelversion": project_root / ".bazelversion",
        "tool schema source": project_root / "engrave_mcp" / "tools.json",
        "engine version file": source_root / "version.cmake",
        "engine MODULE.bazel": source_root / "MODULE.bazel",
        "engine BUILD.bazel": source_root / "BUILD.bazel",
        "engine .bazelversion": source_root / ".bazelversion",
        "engine bazel defs": source_root / "bazel" / "defs.bzl",
        "engine bazel deps": source_root / "bazel" / "deps.bzl",
        "native env cairo header": native_root / "include" / "cairo" / "cairo.h",
        "native env zlib header": native_root / "include" / "zlib.h",
    }
    for label, path in required_files.items():
        if not path.is_file():
            errors.append(f"missing {label}: {path}")

    host_ver = project_root / ".bazelversion"
    eng_ver = source_root / ".bazelversion"
    if host_ver.is_file() and eng_ver.is_file():
        hv = host_ver.read_text(encoding="utf-8").strip()
        ev = eng_ver.read_text(encoding="utf-8").strip()
        if hv != ev:
            errors.append(f".bazelversion mismatch: host {hv!r} != engine {ev!r}")

    library_dir = native_root / "lib"
    if not library_dir.is_dir():
        errors.append(f"missing native library directory: {library_dir}")
    else:
        expected = ("cairo.lib", "z.lib") if sys.platform == "win32" else ("libcairo.so", "libz.so")
        for name in expected:
            if not (library_dir / name).is_file():
                errors.append(f"missing native link library: {library_dir / name}")

    errors += _check_native_pins(project_root / "pixi.lock", source_root / "pixi.lock")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate checked-in Bazel and Pixi build inputs (default)",
    )
    parser.add_argument(
        "--engine-source",
        "--source-root",
        dest="source_root",
        type=Path,
        help="engine checkout to read (default: vendor/engrave)",
    )
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parents[1]
    source_root = (args.source_root or (project_root / "vendor" / "engrave")).resolve()

    errors = validate(project_root, source_root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("Bazel and Pixi build inputs verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
