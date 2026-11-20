"""Assemble the Windows dependency closure without requiring a MuseScore install."""

import argparse
import hashlib
import json
import os
import shutil
import stat
import struct
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


def find_llvm_strip(environment: Path) -> str:
    candidates = [
        environment / "Library/bin/llvm-strip.exe",
        *sorted(
            (ROOT / "build").glob(
                f".bazel-symlinks-*/{ROOT.name}/external/*xclang_x86_64-w64-mingw32/bin/llvm-strip.exe"
            )
        ),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    tool = shutil.which("llvm-strip.exe")
    if tool is None:
        raise RuntimeError(
            "llvm-strip is required; use the xclang toolchain, put llvm-strip on PATH, or pass --no-strip"
        )
    return tool


def _pe_imported_dlls(binary: Path) -> list[str]:
    data = binary.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError(f"Not a valid PE binary: {binary}")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off : pe_off + 4] != b"PE\x00\x00":
        raise RuntimeError(f"Invalid PE signature: {binary}")
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    imp_dir_off = opt_off + (120 if magic == 0x20B else 104)
    imp_rva, imp_sz = struct.unpack_from("<II", data, imp_dir_off)
    if imp_rva == 0 or imp_sz == 0:
        return []
    sec_off = opt_off + opt_size
    secs = [struct.unpack_from("<8sIIIIIIHHI", data, sec_off + i * 40) for i in range(num_sec)]

    def rva_to_off(rva: int) -> int:
        for _, vsz, va, rsz, rptr, *_ in secs:
            span = max(vsz, rsz)
            if va <= rva < va + span:
                return rptr + (rva - va)
        return 0

    off = rva_to_off(imp_rva)
    names: list[str] = []
    while off and off + 20 <= len(data):
        _, _, _, name_rva, _ = struct.unpack_from("<IIIII", data, off)
        if not name_rva:
            break
        noff = rva_to_off(name_rva)
        if not noff:
            break
        end = data.index(0, noff)
        names.append(data[noff:end].decode("ascii", errors="replace"))
        off += 20
    return names


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
    parser.add_argument("--binary", type=Path, help="Override the built executable")
    parser.add_argument("--no-strip", action="store_true", help="Keep symbols in the packaged executable")
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
    strip = None if args.no_strip else find_llvm_strip(environment)
    available: dict[str, Path] = {}
    vc_redist = os.environ.get("VCToolsRedistDir")
    if vc_redist:
        redist = Path(vc_redist) / "x64"
        if redist.is_dir():
            available.update({p.name.lower(): p for p in redist.rglob("*.dll") if "debug" not in str(p).lower()})
    available.update({p.name.lower(): p for p in (library / "bin").glob("*.dll")})
    queue = []
    origins = {}

    def copy(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        origins[str(destination.relative_to(output))] = str(source)
        queue.append(destination)

    executable = output / "engrave-mcp.exe"
    copy(args.binary or ROOT / "build/release/engrave-mcp.exe", executable)
    unstripped_executable_sha256 = hashlib.sha256(executable.read_bytes()).hexdigest()
    if strip is not None:
        executable.chmod(executable.stat().st_mode | stat.S_IWUSR)
        subprocess.run([strip, "--strip-all", str(executable)], check=True)
    system = Path(os.environ["SystemRoot"]) / "System32"
    while queue:
        binary = queue.pop()
        for name in _pe_imported_dlls(binary):
            key = name.lower()
            if key.startswith(("api-ms-", "ext-ms-")):
                continue
            if key in available:
                destination = output / available[key].name
                if not destination.exists():
                    copy(available[key], destination)
            elif not (system / name).exists():
                raise RuntimeError(f"Unresolved dependency: {name} from {binary.name}")
    shutil.copytree(ROOT / ".runtime/assets", output / "assets")
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
                "unstripped_executable_sha256": unstripped_executable_sha256,
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
