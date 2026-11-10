#!/usr/bin/env python3
"""Validate the pinned engine checkout and the checked-in xmake inputs.

The file intentionally does not invoke CMake, Ninja or xmake.  It checks that
the engine checkout has the MuseScore 5.0 layout the build expects, that the
engine carries its own xmake build (``xmake/engrave.lua`` and the pinned package
recipes), that the local native package environment provides what engrave_mcp/ needs,
and that the host's own package recipe is present.

Usage:
    python scripts/prepare_xmake.py --check
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from engrave_testing.paths import pixi_env

ENGINE_PACKAGE_RECIPES = (
    "freetype",
    "harfbuzz",
    "libflac",
    "lame",
    "libopus",
    "libopusenc",
    "fdk-aac",
    "pugixml",
    "utfcpp",
    "picojson",
)


def _find_native_root(project_root: Path) -> Path:
    env = pixi_env(project_root)
    library = env / "Library"
    return library if library.is_dir() else env


def _engine_source_root(project_root: Path) -> Path:
    return project_root / "vendor" / "engrave"


def validate(project_root: Path, source_root: Path) -> list[str]:
    errors: list[str] = []
    native_root = _find_native_root(project_root)

    # The fork's xmake build generates its version header directly; it no
    # longer consumes the removed Qt-era framework config template.
    required_files = {
        "MuseScore submodule": source_root / "version.cmake",
        "engine xmake build": source_root / "xmake" / "engrave.lua",
        "tool schema source": project_root / "engrave_mcp" / "tools.json",
        "host asio recipe": project_root / "xmake" / "packages" / "a" / "asio" / "xmake.lua",
        "native env cairo header": native_root / "include" / "cairo" / "cairo.h",
        "native env zlib header": native_root / "include" / "zlib.h",
    }
    for label, path in required_files.items():
        if not path.is_file():
            errors.append(f"missing {label}: {path}")

    library_dir = native_root / "lib"
    if not library_dir.is_dir():
        errors.append(f"missing native library directory: {library_dir}")
    else:
        expected = ("cairo.lib", "z.lib") if sys.platform == "win32" else ("libcairo.so", "libz.so")
        for name in expected:
            if not (library_dir / name).is_file():
                errors.append(f"missing native link library: {library_dir / name}")

    # The engine's third-party dependencies are pinned recipes in the engine
    # checkout's xmake package repository, not trees in the superproject.
    for recipe in ENGINE_PACKAGE_RECIPES:
        path = source_root / "xmake" / "packages" / recipe[0] / recipe / "xmake.lua"
        if not path.is_file():
            errors.append(f"missing engine package recipe: {path}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="only validate the checked-in inputs (default)",
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
    source_root = (args.source_root or _engine_source_root(project_root)).resolve()

    errors = validate(project_root, source_root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("xmake inputs are present; parent runs `xmake` to configure/build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
