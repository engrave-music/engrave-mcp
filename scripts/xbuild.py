#!/usr/bin/env python3
"""Run xmake with per-profile configurations and shared package and compile caches."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
from pathlib import Path

STAMP_VERSION = 3


def _find_msvc_bin_dir() -> Path | None:
    compiler = shutil.which("cl")
    if compiler:
        return Path(compiler).parent
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    try:
        install_path = subprocess.check_output(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property",
                "installationPath",
            ],
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    if not install_path:
        return None
    candidates = sorted((Path(install_path) / "VC/Tools/MSVC").glob("*/bin/Hostx64/x64/cl.exe"), reverse=True)
    return candidates[0].parent if candidates else None


def resolve_layout(
    root: Path,
    profile: str,
    build_directory: Path | None,
    arch: str,
) -> tuple[Path, Path, str, list[str], dict]:
    build = (root / (build_directory or Path("build") / profile)).resolve()
    default_build = (root / "build" / profile).resolve()
    plat = "windows" if os.name == "nt" else "linux"
    # Xmake appends .xmake/<host>/<arch> to XMAKE_CONFIGDIR itself.
    config = root / ".xmake" / "profiles" / f"{plat}-{arch}-{profile}"
    if build != default_build:
        config /= hashlib.sha256(str(build).encode()).hexdigest()[:12]
    prefix = root / ".pixi/envs/default"
    linux_tools: list[str] = []
    if os.name != "nt":
        cc = prefix / "bin/x86_64-conda-linux-gnu-cc"
        cxx = prefix / "bin/x86_64-conda-linux-gnu-c++"
        if cc.is_file() and cxx.is_file():
            linux_tools = [f"--cc=gcc@{cc}", f"--cxx=gxx@{cxx}", f"--ld=gxx@{cxx}"]
    stamp_payload = {
        "version": STAMP_VERSION,
        "profile": profile,
        "build": str(build),
        "plat": plat,
        "arch": arch,
        "compiler_options": linux_tools,
        "default_options": [f"--ccachedir={root / 'build/.build_cache'}", "--engrave_tests=y"],
    }
    return build, config, plat, linux_tools, stamp_payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("release", "debug", "releasedbg", "asan"), default="release")
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("--arch", default="x64" if os.name == "nt" else platform.machine())
    parser.add_argument("--reconfigure", action="store_true", help="Force re-running xmake f before the action")
    parser.add_argument("action", choices=("configure", "build", "run", "compdb"))
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    passthrough = []
    for item in args.arguments:
        if item == "--reconfigure":
            args.reconfigure = True
        else:
            passthrough.append(item)
    args.arguments = passthrough
    root = Path(__file__).resolve().parents[1]
    build, config, plat, linux_tools, stamp_payload = resolve_layout(
        root, args.profile, args.build_directory, args.arch
    )
    env = dict(os.environ, XMAKE_CONFIGDIR=str(config), XMAKE_PKG_LOCALDIR=str(root / "build/.packages"))
    prefix = root / ".pixi/envs/default"
    extra_paths = [prefix / "Library/bin", prefix / "bin"]
    if os.name == "nt":
        # cl's ASan import libraries select the matching runtime; execution needs
        # its DLL alongside the compiler, including outside a VS developer shell.
        msvc_bin = _find_msvc_bin_dir()
        if msvc_bin:
            extra_paths.append(msvc_bin)
    env["PATH"] = os.pathsep.join(map(str, extra_paths)) + os.pathsep + env.get("PATH", "")

    def run(*command: str) -> int:
        return subprocess.call(["xmake", *command], cwd=root, env=env)

    stamp_path = config / ".xbuild-stamp.json"

    def has_xmake_conf() -> bool:
        return any(config.rglob("xmake.conf"))

    def is_configured() -> bool:
        if not stamp_path.is_file() or not has_xmake_conf():
            return False
        try:
            return json.loads(stamp_path.read_text(encoding="utf-8")) == stamp_payload
        except (OSError, ValueError):
            return False

    def configure(extra: list[str]) -> int:
        supplied_options = {arg.partition("=")[0] for arg in extra if arg.startswith("--")}
        default_options = [
            option for option in stamp_payload["default_options"] if option.partition("=")[0] not in supplied_options
        ]
        options = [
            "f",
            "-P",
            str(root),
            "-y",
            "-p",
            plat,
            "-a",
            args.arch,
            "-m",
            args.profile,
            "-o",
            str(build),
            *linux_tools,
            *default_options,
        ]
        rc = run(*options, *extra)
        if rc == 0:
            config.mkdir(parents=True, exist_ok=True)
            stamp_path.write_text(json.dumps(stamp_payload), encoding="utf-8")
        else:
            stamp_path.unlink(missing_ok=True)
        return rc

    if args.action == "configure":
        return configure(args.arguments)
    configured = is_configured()
    if args.reconfigure or not configured:
        # Clear stale cached config (such as early g++@ entries) when migrating an existing config.
        extra = ["-c"] if not configured and has_xmake_conf() else []
        result = configure(extra)
        if result:
            return result
    if args.action == "compdb":
        build.mkdir(parents=True, exist_ok=True)
        return run("project", "-P", str(root), "-k", "compile_commands", str(build), *args.arguments)
    return run(args.action, "-P", str(root), *(["-y"] if args.action == "build" else []), *args.arguments)


if __name__ == "__main__":
    raise SystemExit(main())
