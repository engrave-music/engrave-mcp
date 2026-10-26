#!/usr/bin/env python3
"""Prepare and validate the checked-in xmake migration inputs.

The file intentionally does not invoke CMake, Ninja or xmake.  It validates
the pinned source trees and the local native package environment, and can run
the one-time manifest capture against the existing CMake baseline build.

Usage:
    python scripts/prepare_xmake.py --capture
    python scripts/prepare_xmake.py --check
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_xmake_manifest as capture  # noqa: E402


def _harfbuzz_candidates(project_root: Path) -> list[Path]:
    return [
        project_root / ".upstream" / "harfbuzz",
        project_root / ".upstream" / "harfbuzz" / "harfbuzz",
        project_root / ".upstream" / "harfbuzz" / "harfbuzz-12.3.0",
    ]


def _find_harfbuzz(project_root: Path) -> Path | None:
    for candidate in _harfbuzz_candidates(project_root):
        if (candidate / "src" / "harfbuzz.cc").is_file():
            return candidate.resolve()
    return None


def _find_native_root(project_root: Path) -> Path:
    env = project_root / ".pixi" / "envs" / "native"
    library = env / "Library"
    return library if library.is_dir() else env


def validate(project_root: Path, require_harfbuzz: bool) -> list[str]:
    errors: list[str] = []
    pin = json.loads((project_root / "upstream.lock.json").read_text(encoding="utf-8-sig"))
    source_root = project_root / ".upstream" / "webmscore"
    native_root = _find_native_root(project_root)

    required_files = {
        "pinned MuseScore checkout": source_root / "version.cmake",
        "MuseScore module selection": source_root / "src" / "framework" / "global" / "globalmodule.cpp",
        "framework config template": source_root
        / "src"
        / "framework"
        / "cmake"
        / "muse_framework_config.h.in",
        "tool schema source": project_root / "native" / "tools.json",
        "native env cairo header": native_root / "include" / "cairo" / "cairo.h",
        "native env zlib header": native_root / "include" / "zlib.h",
        "native env nlohmann header": native_root / "include" / "nlohmann" / "json.hpp",
    }
    for label, path in required_files.items():
        if not path.is_file():
            errors.append(f"missing {label}: {path}")
    revision = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    if revision.returncode or revision.stdout.strip() != pin["revision"]:
        errors.append("MuseScore HEAD does not match upstream.lock.json")

    harfbuzz_root = _find_harfbuzz(project_root)
    if harfbuzz_root is None:
        message = (
            "missing pinned HarfBuzz source (expected one of: "
            + ", ".join(str(path) for path in _harfbuzz_candidates(project_root))
            + ")"
        )
        if require_harfbuzz:
            errors.append(message)
        else:
            print(f"warning: {message}", file=sys.stderr)
    else:
        print(f"harfbuzz: {harfbuzz_root}")
        marker = project_root / pin["harfbuzzDependency"]["sourceDirectory"] / ".archive-sha256"
        if not marker.is_file() or marker.read_text().strip() != pin["harfbuzzDependency"]["sha256"]:
            errors.append("HarfBuzz bootstrap marker does not match upstream.lock.json")

    library_dir = native_root / "lib"
    if not library_dir.is_dir():
        errors.append(f"missing native library directory: {library_dir}")
    else:
        expected = ("cairo.lib", "z.lib") if sys.platform == "win32" else ("libcairo.so", "libz.so")
        for name in expected:
            if not (library_dir / name).is_file():
                errors.append(f"missing native link library: {library_dir / name}")
    manifest = project_root / "xmake" / "manifest.lua"
    if not manifest.is_file():
        errors.append(
            f"missing xmake manifest: {manifest} (run with --capture once)"
        )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--capture",
        action="store_true",
        help="run the one-time compile_commands capture before validating",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only validate the checked-in inputs (default)",
    )
    parser.add_argument(
        "--compile-commands",
        type=Path,
        help="override the CMake baseline compile_commands.json",
    )
    parser.add_argument(
        "--allow-missing-harfbuzz",
        action="store_true",
        help="warn instead of failing when HarfBuzz has not been moved yet",
    )
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parents[1]
    if args.capture:
        compile_commands = (
            args.compile_commands
            or project_root / "build" / "native-windows" / "compile_commands.json"
        )
        output = project_root / "xmake" / "manifest.lua"
        try:
            harfbuzz_root = _find_harfbuzz(project_root)
            manifest = capture.capture(
                compile_commands.resolve(),
                project_root,
                (project_root / ".upstream" / "webmscore").resolve(),
                harfbuzz_root,
            )
            capture.write_manifest(manifest, output)
        except (OSError, ValueError, KeyError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        print(f"wrote {output}")

    errors = validate(
        project_root,
        require_harfbuzz=not args.allow_missing_harfbuzz and not args.capture,
    )
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("xmake inputs are present; parent runs `xmake` to configure/build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
