"""Assemble the Windows dependency closure without requiring a MuseScore install."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

from engrave_testing.paths import pixi_env

ROOT = Path(__file__).resolve().parents[1]
CODEC_NOTICES = (
    ("fdk-aac", "codecs/fdk-aac/NOTICE"),
    ("flac", "codecs/flac/COPYING.FDL"),
    ("flac", "codecs/flac/COPYING.GPL"),
    ("flac", "codecs/flac/COPYING.LGPL"),
    ("flac", "codecs/flac/COPYING.Xiph"),
    ("lame", "codecs/lame/COPYING"),
    ("lame", "codecs/lame/LICENSE"),
    ("opus", "codecs/opus/COPYING"),
    ("opusenc", "codecs/opusenc/COPYING"),
)

ENGINE_THIRDPARTY_NOTICES = (
    (
        "fluidsynth",
        "muse/framework/audio/thirdparty/fluidsynth/fluidsynth-2.3.3/LICENSE",
        "thirdparty-fluidsynth-LICENSE",
    ),
    ("dtl", "src/engraving/thirdparty/dtl/COPYING", "thirdparty-dtl-COPYING"),
    (
        "beatroot",
        "src/importexport/midi/thirdparty/beatroot/COPYING",
        "thirdparty-beatroot-COPYING",
    ),
)

STAGED_FONT_DIRS = (
    "bravura",
    "campania",
    "edwin",
    "finalebroadway",
    "finalemaestro",
    "gootville",
    "leland",
    "mscore",
    "musejazz",
    "petaluma",
)


def resolve_engine_root(root: Path, override: Path | None = None) -> Path:
    return (override or root / "vendor/engrave").resolve()


def collect_package_notices(root: Path, engine_root: Path) -> list[tuple[str, Path, str]]:
    """Collect all required package notices.
    Returns list of (category, source_path, dest_relative_to_licenses).
    Fails if any expected notice is missing.
    """
    notices: list[tuple[str, Path, str]] = []

    # 1. Superproject LICENSE
    super_license = root / "LICENSE"
    if not super_license.is_file():
        raise RuntimeError(f"Missing superproject LICENSE: {super_license}")
    notices.append(("superproject", super_license, "LICENSE"))

    # 2. Entire licenses/ directory in superproject (including FluidR3Mono_License.md)
    licenses_dir = root / "licenses"
    if not licenses_dir.is_dir():
        raise RuntimeError(f"Missing licenses/ directory: {licenses_dir}")
    soundfont_license = licenses_dir / "FluidR3Mono_License.md"
    if not soundfont_license.is_file():
        raise RuntimeError(f"Missing SoundFont notice: {soundfont_license}")
    for p in sorted(licenses_dir.rglob("*")):
        if p.is_file():
            rel = p.relative_to(licenses_dir).as_posix()
            notices.append(("superproject-licenses", p, rel))

    # 3. Upstream engine license(s)
    upstream_licenses = sorted(engine_root.glob("LICENSE*"))
    if not upstream_licenses:
        raise RuntimeError(f"No upstream LICENSE file found under {engine_root}")
    for p in upstream_licenses:
        if p.is_file():
            notices.append(("upstream", p, p.name))

    # 4. Engine third-party notices
    for name, rel, dst_name in ENGINE_THIRDPARTY_NOTICES:
        src = engine_root / rel
        if not src.is_file():
            if name == "fluidsynth":
                raise RuntimeError(f"Missing bundled FluidSynth license: {src}")
            continue
        notices.append((f"thirdparty-{name}", src, dst_name))

    # 5. Codec notices
    for name, rel in CODEC_NOTICES:
        src = licenses_dir / rel
        if not src.is_file():
            raise RuntimeError(f"Missing bundled codec notice: {src}")
        notices.append((f"codec-{name}", src, f"thirdparty-{name}-{src.name}"))

    # 6. Font directory notices
    fonts_root = engine_root / "fonts"
    if not fonts_root.is_dir():
        raise RuntimeError(f"Missing fonts directory: {fonts_root}")
    fonts_readme = fonts_root / "README.md"
    if fonts_readme.is_file():
        notices.append(("font-root", fonts_readme, "font-README.md"))

    for font_name in STAGED_FONT_DIRS:
        fdir = fonts_root / font_name
        found = [
            p
            for p in sorted(fdir.glob("*"))
            if any(k in p.name.upper() for k in ["OFL", "LICENSE", "COPYING", "README"])
        ]
        if not found:
            if font_name == "mscore" and fonts_readme.is_file():
                pass
            else:
                raise RuntimeError(f"Missing font directory notice for staged font: {font_name} under {fdir}")
        for f in found:
            notices.append((f"font-{font_name}", f, f"font-{font_name}-{f.name}"))

    return notices


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/windows-package")
    parser.add_argument(
        "--no-qt",
        action="store_true",
        help="Package the Qt-free backend and external resources",
    )
    parser.add_argument("--binary", type=Path, help="Override the built executable")
    parser.add_argument(
        "--source-root",
        type=Path,
        help="Override engine root (default: vendor/engrave)",
    )
    parser.add_argument(
        "--check-notices",
        action="store_true",
        help="Validate all required package notices without packaging",
    )
    args = parser.parse_args()

    engine_root = resolve_engine_root(ROOT, args.source_root)
    notices = collect_package_notices(ROOT, engine_root)

    if args.check_notices:
        print(f"Validated {len(notices)} package notices across superproject, engine, codecs, and fonts:")
        for cat, src, dst in notices:
            print(f"  [{cat}] {src} -> licenses/{dst}")
        return
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit("Package output must be empty; choose a new --output directory")
    environment = pixi_env(ROOT)
    library = environment / "Library"
    redist = Path(os.environ["VCToolsRedistDir"]) / "x64"
    available = {p.name.lower(): p for p in redist.rglob("*.dll") if "debug" not in str(p).lower()}
    available.update({p.name.lower(): p for p in (library / "bin").glob("*.dll")})
    queue = []
    origins = {}

    def copy(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        origins[str(destination.relative_to(output))] = str(source)
        queue.append(destination)

    copy(
        args.binary or ROOT / "build/release/engrave-mcp.exe",
        output / "engrave-mcp.exe",
    )
    if not args.no_qt:
        copy(
            library / "lib/qt6/plugins/platforms/qoffscreen.dll",
            output / "plugins/platforms/qoffscreen.dll",
        )
    system = Path(os.environ["SystemRoot"]) / "System32"
    while queue:
        binary = queue.pop()
        result = subprocess.run(["dumpbin", "/dependents", str(binary)], capture_output=True, check=True)
        for name in re.findall(
            r"^\s+([A-Za-z0-9_.-]+\.dll)\s*$",
            result.stdout.decode("utf-8", errors="replace"),
            re.MULTILINE | re.IGNORECASE,
        ):
            key = name.lower()
            if args.no_qt and key.startswith(("qt5", "qt6")):
                raise RuntimeError(f"Qt dependency found in Qt-free package: {name}")
            if key.startswith(("api-ms-", "ext-ms-")):
                continue
            if key in available:
                destination = output / available[key].name
                if not destination.exists():
                    copy(available[key], destination)
            elif not (system / name).exists():
                raise RuntimeError(f"Unresolved dependency: {name} from {binary.name}")
    if args.no_qt:
        shutil.copytree(ROOT / ".runtime/assets", output / "assets")
    else:
        (output / "qt.conf").write_text("[Paths]\nPrefix=.\nLibraries=.\nPlugins=plugins\n", encoding="utf-8")
    soundfont = ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"
    if soundfont.exists():
        copy(soundfont, output / "soundfonts/FluidR3Mono_GM.sf3")
    licenses = output / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    for cat, src, dst in notices:
        target = licenses / dst
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
    for directory in (environment / "conda-meta",):
        # Package metadata contains license/provenance, not user environment credentials.
        if directory.is_dir():
            for source in directory.glob("*.json"):
                shutil.copy2(source, licenses / source.name)
    manifest = {
        str(p.relative_to(output)): {
            "bytes": p.stat().st_size,
            "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
        }
        for p in output.rglob("*")
        if p.is_file()
    }
    (output / "manifest.json").write_text(
        json.dumps(
            {
                "files": manifest,
                "total_bytes": sum(p["bytes"] for p in manifest.values()),
                "note": "Local validation bundle. Full third-party source/license distribution audit remains required before publishing.",
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "output": str(output),
                "files": len(manifest),
                "bytes": sum(p["bytes"] for p in manifest.values()),
            }
        )
    )


if __name__ == "__main__":
    main()
