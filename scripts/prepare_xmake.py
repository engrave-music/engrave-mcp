#!/usr/bin/env python3
"""Validate the pinned engine checkout and the checked-in xmake inputs.

The file intentionally does not invoke CMake, Ninja or xmake.  It checks that
the engine checkout has the MuseScore 5.0 layout the build expects, that the
local native package environment provides what native/ needs, and that
``xmake/manifest.lua`` is present.  ``--generate-manifest`` runs the generator
from ``scripts/capture_xmake_manifest.py`` when the engine moves.

Usage:
    python scripts/prepare_xmake.py --generate-manifest
    python scripts/prepare_xmake.py --check
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capture_xmake_manifest as capture  # noqa: E402


def _find_native_root(project_root: Path) -> Path:
    env = project_root / ".pixi" / "envs" / "native"
    library = env / "Library"
    return library if library.is_dir() else env


def validate(project_root: Path) -> list[str]:
    errors: list[str] = []
    source_root = project_root / "vendor" / "musescore"
    native_root = _find_native_root(project_root)

    # Engine layout: MuseScore 5.0 absorbed muse_framework at muse/, so the
    # framework paths are muse/framework/... and the module CMake files are the
    # manifest's input.
    required_files = {
        "MuseScore submodule": source_root / "version.cmake",
        "framework module list": source_root / "muse" / "framework" / "CMakeLists.txt",
        "global module": source_root / "muse" / "framework" / "global" / "CMakeLists.txt",
        "engraving module": source_root / "src" / "engraving" / "CMakeLists.txt",
        "framework config template": source_root
        / "muse"
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
            f"missing xmake manifest: {manifest} "
            "(run with --generate-manifest against the engine checkout)"
        )
    # Third-party dependencies are pinned packages, not trees in the engine.
    for recipe in ("harfbuzz", "flac", "opus", "opusenc", "pugixml", "picojson", "fdk-aac"):
        if not (project_root / "xmake" / "packages" / f"{recipe}.lua").is_file():
            errors.append(f"missing package recipe: xmake/packages/{recipe}.lua")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--generate-manifest",
        action="store_true",
        help="regenerate xmake/manifest.lua from the engine's CMake source lists",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only validate the checked-in inputs (default)",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        help="engine checkout to read (default: vendor/musescore)",
    )
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parents[1]
    source_root = (args.source_root or project_root / "vendor" / "musescore").resolve()

    if args.generate_manifest:
        try:
            manifest = capture.build_manifest(source_root, project_root, verbose=False)
            capture.write_manifest(manifest, project_root / "xmake" / "manifest.lua")
        except (OSError, capture.CmakeError, ValueError, KeyError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        print(f"wrote {project_root / 'xmake' / 'manifest.lua'}")

    errors = validate(project_root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("xmake inputs are present; parent runs `xmake` to configure/build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
