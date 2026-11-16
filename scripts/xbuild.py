#!/usr/bin/env python3
"""Run xmake with independent profile configuration and a shared package store.

Examples: xbuild.py --profile debug build -j 12 engrave_mcp
          xbuild.py --profile releasedbg compdb
          xbuild.py --profile asan configure --engrave_tests=y
Options before the action belong to this runner; subsequent arguments go to xmake.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
from pathlib import Path


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("release", "debug", "releasedbg", "asan"), default="release")
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("--arch", default="x64" if os.name == "nt" else platform.machine())
    parser.add_argument("--reconfigure", action="store_true", help="Force re-running xmake f before the action")
    parser.add_argument("action", choices=("configure", "build", "run", "compdb"))
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = (root / (args.build_directory or Path("build") / args.profile)).resolve()
    plat = "windows" if os.name == "nt" else "linux"
    # Xmake appends .xmake/<host>/<arch> to XMAKE_CONFIGDIR itself.
    config = root / ".xmake" / "profiles" / f"{plat}-{args.arch}-{args.profile}"
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
    stamp_payload = {"profile": args.profile, "build": str(build), "plat": plat, "arch": args.arch}

    def is_configured() -> bool:
        if not stamp_path.is_file() or not any(config.rglob("xmake.conf")):
            return False
        try:
            return json.loads(stamp_path.read_text(encoding="utf-8")) == stamp_payload
        except (OSError, ValueError):
            return False

    def configure(extra: list[str]) -> int:
        options = ["f", "-P", str(root), "-y", "-p", plat, "-a", args.arch, "-m", args.profile, "-o", str(build)]
        if os.name != "nt":
            cc = prefix / "bin/x86_64-conda-linux-gnu-cc"
            cxx = prefix / "bin/x86_64-conda-linux-gnu-c++"
            if cc.is_file() and cxx.is_file():
                options += [f"--cc=gcc@{cc}", f"--cxx=g++@{cxx}", f"--ld=g++@{cxx}"]
        rc = run(*options, *extra)
        if rc == 0:
            config.mkdir(parents=True, exist_ok=True)
            stamp_path.write_text(json.dumps(stamp_payload), encoding="utf-8")
        else:
            stamp_path.unlink(missing_ok=True)
        return rc

    if args.action == "configure":
        return configure(args.arguments)
    if args.reconfigure or not is_configured():
        # Reconfigure only when unconfigured, target build directory changed, or asked.
        result = configure([])
        if result:
            return result
    if args.action == "compdb":
        build.mkdir(parents=True, exist_ok=True)
        return run("project", "-P", str(root), "-k", "compile_commands", str(build), *args.arguments)
    return run(args.action, "-P", str(root), *(["-y"] if args.action == "build" else []), *args.arguments)


if __name__ == "__main__":
    raise SystemExit(main())
