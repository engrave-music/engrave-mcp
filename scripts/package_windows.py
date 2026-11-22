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

from _package_common import (
    collect_package_notices,
    collect_pixi_runtime_notices,
    collect_vc_redist_notices,
    resolve_engine_root,
    stage_engine_assets,
    stage_package_notices,
    stage_soundfont,
    validate_soundfont,
)
from engrave_testing.paths import pixi_env

ROOT = Path(__file__).resolve().parents[1]


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


def build_windows_dll_map(library_bin: Path, vc_redist_dir: str | None) -> dict[str, Path]:
    available = {path.name.lower(): path for path in library_bin.glob("*.dll") if path.is_file()}
    if vc_redist_dir:
        redist = Path(vc_redist_dir).resolve() / "x64"
        for path in sorted(redist.rglob("*.dll")):
            if path.is_file() and not any("debug" in part.lower() for part in path.relative_to(redist).parts):
                available[path.name.lower()] = path
    return available


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
    parser.add_argument("--soundfont", type=Path, help="Bundle this SoundFont file")
    parser.add_argument(
        "--soundfont-license",
        type=Path,
        help="License notice for a custom SoundFont (optional for FluidR3Mono_GM.sf3)",
    )
    args = parser.parse_args()

    engine_root = resolve_engine_root(ROOT, args.source_root)
    notices = collect_package_notices(ROOT, engine_root)

    if args.check_notices:
        print(f"Validated {len(notices)} package notices across superproject, engine, codecs, and fonts:")
        for cat, src, dst in notices:
            print(f"  [{cat}] {src} -> licenses/{dst}")
        return
    notices.extend(validate_soundfont(args.soundfont, args.soundfont_license))
    output = args.output.resolve()
    if os.path.lexists(output) and (not output.is_dir() or any(output.iterdir())):
        raise SystemExit("Package output must be empty; choose a new --output directory")
    source_binary = (args.binary or ROOT / "build/release/engrave-mcp.exe").resolve()
    if not source_binary.is_file():
        raise RuntimeError(f"Missing Windows executable: {source_binary}")
    output.mkdir(parents=True, exist_ok=True)
    environment = pixi_env(ROOT)
    strip = None if args.no_strip else find_llvm_strip(environment)
    vc_redist_dir = os.environ.get("VCToolsRedistDir")
    available = build_windows_dll_map(environment / "Library/bin", vc_redist_dir)
    queue = []
    bundled = {}

    def copy_binary(source, destination):
        shutil.copy2(source, destination)
        queue.append(destination)

    executable = output / "engrave-mcp.exe"
    copy_binary(source_binary, executable)
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
                    copy_binary(available[key], destination)
                    bundled[destination.name] = available[key]
            elif not (system / name).exists():
                raise RuntimeError(f"Unresolved dependency: {name} from {binary.name}")
    env_resolved = environment.resolve()
    pixi_dlls = {
        name: path
        for name, path in bundled.items()
        if path.is_relative_to(environment) or path.resolve().is_relative_to(env_resolved)
    }
    notices.extend(collect_pixi_runtime_notices(environment, list(pixi_dlls.values())))
    redist_dlls = {name: path for name, path in bundled.items() if name not in pixi_dlls}
    redist_root = Path(vc_redist_dir).resolve() if vc_redist_dir else None
    if redist_dlls:
        if not vc_redist_dir:
            raise RuntimeError(f"Bundled DLLs outside Pixi without VCToolsRedistDir: {sorted(redist_dlls)}")
        notices.extend(collect_vc_redist_notices(vc_redist_dir, environment))
    stage_engine_assets(engine_root, output / "assets")
    stage_soundfont(args.soundfont, output / "soundfonts")
    stage_package_notices(output / "licenses", notices)

    def relative_origin(name: str, path: Path) -> str:
        resolved = path.resolve()
        if name in pixi_dlls:
            return resolved.relative_to(env_resolved).as_posix()
        if redist_root is not None and resolved.is_relative_to(redist_root):
            return resolved.relative_to(redist_root).as_posix()
        return path.name

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
                "dll_origins": {
                    name: {
                        "path": relative_origin(name, path),
                        "provider": "Pixi" if name in pixi_dlls else "VCToolsRedistDir",
                    }
                    for name, path in sorted(bundled.items())
                },
                "note": "Relocatable Windows package with third-party licenses and filtered runtime provenance.",
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
