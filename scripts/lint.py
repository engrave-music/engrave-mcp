#!/usr/bin/env python3
"""Run linters (ruff for Python, clang-tidy for C++) on project files."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
PYTHON_EXTENSIONS = {".py", ".pyi"}

IGNORED_PREFIXES = (
    "vendor/",
    ".upstream/",
    "tests/",
    "fixtures/",
    ".pixi/",
    ".xmake/",
    "build/",
    ".runtime/",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run linters on tracked project files.")
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        default=[Path(".")],
        help="files or directories to lint (defaults to current directory)",
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        help="automatically apply lint fixes where available",
    )
    return parser.parse_args()


def should_ignore(path: Path) -> bool:
    posix_path = path.as_posix()
    return any(posix_path.startswith(prefix) for prefix in IGNORED_PREFIXES)


def project_files(paths: list[Path]) -> list[Path]:
    for path in paths:
        if not path.exists():
            raise FileNotFoundError(path)
    command = [
        "git",
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
        "--",
        *map(str, paths),
    ]

    output = subprocess.check_output(command)
    files = [Path(path.decode()) for path in output.split(b"\0") if path]
    return [path for path in files if path.is_file() and not should_ignore(path)]


def ensure_compile_commands(repo_root: Path) -> None:
    compdb = repo_root / "compile_commands.json"
    if not compdb.exists():
        print("compile_commands.json not found; generating via xmake...", flush=True)
        subprocess.check_call(["xmake", "project", "-k", "compile_commands"], cwd=repo_root)


def main() -> int:
    args = parse_args()
    repo_root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())

    try:
        files = project_files(args.paths)
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"lint: {error}", file=sys.stderr)
        return 1

    py_files = [f for f in files if f.suffix.lower() in PYTHON_EXTENSIONS]
    cpp_files = [f for f in files if f.suffix.lower() in CPP_EXTENSIONS]

    total_returncode = 0

    # 1. Python linting with ruff
    if py_files:
        print(f"Running ruff check on {len(py_files)} Python file(s)...", flush=True)
        cmd = ["ruff", "check"]
        if args.fix:
            cmd.append("--fix")
        cmd.extend(map(str, py_files))
        res = subprocess.run(cmd, cwd=repo_root, check=False)
        if res.returncode != 0:
            total_returncode = res.returncode

    # 2. C++ linting with clang-tidy
    if cpp_files:
        ensure_compile_commands(repo_root)
        print(f"Running clang-tidy on {len(cpp_files)} C++ file(s)...", flush=True)
        cmd = ["clang-tidy", "-p", str(repo_root)]
        if args.fix:
            cmd.extend(["--fix-errors", "--format-style=file"])
        cmd.extend(map(str, cpp_files))
        res = subprocess.run(cmd, cwd=repo_root, check=False)
        if res.returncode != 0:
            total_returncode = res.returncode

    return total_returncode


if __name__ == "__main__":
    sys.exit(main())
