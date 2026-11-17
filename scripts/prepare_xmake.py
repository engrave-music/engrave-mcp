#!/usr/bin/env python3
"""Validate the pinned engine checkout and the checked-in xmake inputs.

The file intentionally does not invoke CMake, Ninja or xmake.  It checks that
the engine checkout has the layout the build expects, that the
engine carries its own xmake build (``xmake/engrave.lua`` and the pinned package
recipes), that the local native package environment provides what engrave_mcp/ needs,
and that the host's own package recipe is present.

Usage:
    python scripts/prepare_xmake.py --check [--engine-source PATH]
"""

from __future__ import annotations

import argparse
import re
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


# Packages whose builds the render goldens and archive bytes depend on; the
# host and the engine checkout must lock the same ones.
NATIVE_PINNED_PACKAGES = ("cairo", "zlib", "libzlib")
_LOCKED_PACKAGE = re.compile(r"^- conda: (\S+/(win-64|linux-64)/([A-Za-z0-9_.-]+?)-\d\S*\.conda)$", re.MULTILINE)


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

    # The fork's xmake build generates its version header directly; it no
    # longer consumes the removed Qt-era framework config template.
    required_files = {
        "engine version file": source_root / "version.cmake",
        "engine xmake build": source_root / "xmake" / "engrave.lua",
        "tool schema source": project_root / "engrave_mcp" / "tools.json",
        "host asio recipe": project_root / "xmake" / "packages" / "a" / "asio" / "xmake.lua",
        "native env cairo header": native_root / "include" / "cairo" / "cairo.h",
        "native env zlib header": native_root / "include" / "zlib.h",
    }
    for relative in (
        "packages.lua",
        "generated.lua",
        "environment.lua",
        "targets/modules.lua",
        "targets/engrave.lua",
        "rules/mode.asan.lua",
        "rules/build.ccache.lua",
        "rules/toolchain.lld.lua",
    ):
        required_files[f"engine build module {relative}"] = source_root / "xmake" / relative
    for name in ("mode.asan", "build.ccache", "toolchain.lld"):
        required_files[f"host rule {name}"] = project_root / "xmake/rules" / f"{name}.lua"
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

    errors += _check_native_pins(project_root / "pixi.lock", source_root / "pixi.lock")

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
