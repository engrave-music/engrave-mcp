#!/usr/bin/env python3
"""Run linters (ruff for Python, clice or clang-tidy for C++) on project files.

The C++ linter runs one process per translation unit, on as many threads as
there are jobs: clang-tidy parses one file at a time, so a single process over
a whole tree leaves every core but one idle. Translation units the database
lists for several targets are checked once: references are resolved against a
temporary database carrying the selected entries only, because clang-tidy
re-parses a file once per entry of the database it is given.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from engine_style import ENGINE_ROOT

CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx"}
PYTHON_EXTENSIONS = {".py", ".pyi"}

IGNORED_PREFIXES = (
    "vendor/",
    ".upstream/",
    "tests/data/",
    ".pixi/",
    ".xmake/",
    "build/",
    ".runtime/",
)

BACKENDS = ("auto", "clice", "clang-tidy")

# Flags that only name the object file's place. Two database entries for one
# file that differ in these compile the same source the same way.
SEPARATE_OUTPUT_FLAGS = {"-o", "-MF", "-MT", "-MQ", "-Fd", "/Fo", "/Fd", "--output"}
ATTACHED_OUTPUT_PREFIXES = ("/Fo", "/Fd", "-MF", "-MT", "-MQ", "-Fd")


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
    parser.add_argument(
        "--backend",
        choices=BACKENDS,
        default="auto",
        help=(
            "'clice' when its binary, clice.toml and a compilation database are usable, 'clang-tidy' always: "
            "clice checks every unit of the database (the engine included), clang-tidy only the requested files "
            "(default: auto)"
        ),
    )
    parser.add_argument("--engine-root", type=Path, default=ENGINE_ROOT, help="engine checkout (with --engine)")
    parser.add_argument("--engine", action="store_true", help="check compiled owned engine translation units")
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="lint processes to run at once (default: CPU count)",
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


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def clice_project_root(path: Path) -> Path | None:
    """The nearest directory at or above a project path that holds clice's config."""
    start = Path(path).resolve()
    for directory in (start, *start.parents):
        if (directory / "clice.toml").is_file() or (directory / ".clice" / "config.toml").is_file():
            return directory
    return None


def extension_roots() -> list[Path]:
    """Directories holding editor extensions, in the order to search them."""
    roots = []
    configured = os.environ.get("VSCODE_EXTENSIONS")
    if configured:
        roots.append(Path(configured))
    home = Path.home()
    for edition in (
        ".vscode",
        ".vscode-insiders",
        ".vscode-server",
        ".antigravity",
        ".antigravity-ide",
        ".cursor",
        ".windsurf",
    ):
        roots.append(home / edition / "extensions")
    # Portable and Scoop-style installs keep the extension tree inside the
    # install's data directory; the editor's own executable says where that
    # install is, which also covers an install on another drive.
    for editor in ("code", "code-insiders", "codium", "antigravity", "antigravity-ide", "windsurf"):
        executable = shutil.which(editor)
        if not executable:
            continue
        for ancestor in Path(executable).resolve().parents:
            roots.append(ancestor / "data" / "extensions")
            roots += sorted((ancestor / "persist").glob("*/data/extensions"))
    for variable in ("SCOOP", "SCOOP_GLOBAL"):
        scoop = os.environ.get(variable)
        if scoop:
            for edition in ("vscode", "vscode-insiders", "antigravity", "cursor"):
                roots.append(Path(scoop) / "persist" / edition / "data" / "extensions")
    for variable in ("LOCALAPPDATA", "PROGRAMFILES", "PROGRAMFILES(X86)"):
        base = os.environ.get(variable)
        if base:
            roots += sorted((Path(base) / "Programs").glob("*/data/extensions"))
    return list(dict.fromkeys(roots))


def clice_extension_binaries() -> list[Path]:
    """The clice servers shipped inside editor extensions, newest first."""
    if sys.platform != "win32":
        return []
    found: list[Path] = []
    for root in extension_roots():
        if not root.is_dir():
            continue
        for pattern in ("clice-io.clice-*/clice/bin/clice.exe", "clice-io.clice-*/clice/bin/clice"):
            found += [path for path in root.glob(pattern) if path.is_file()]
    return sorted(found, key=lambda path: path.parent.parent.parent.name, reverse=True)


def find_clice() -> Path | None:
    configured = os.environ.get("CLICE_BIN")
    if configured:
        binary = Path(configured).expanduser()
        if binary.is_file():
            return binary
        print(f"lint: CLICE_BIN={configured} is not a file", file=sys.stderr)
    on_path = shutil.which("clice")
    if on_path:
        return Path(on_path)
    for binary in clice_extension_binaries():
        return binary
    return None


def declared_databases(workspace: Path) -> list[Path] | None:
    """The compilation databases the active clice configuration declares.

    `None` means the configuration cannot be read here, leaving clice itself
    to decide whether it can run.
    """
    config = workspace / "clice.toml"
    if not config.is_file():
        config = workspace / ".clice" / "config.toml"
    try:
        import tomllib
    except ImportError:  # pragma: no cover - Python earlier than 3.11
        return None
    try:
        settings = tomllib.loads(config.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None

    default = settings.get("default_configuration") or ""
    rules = [rule for rule in settings.get("rules", []) if isinstance(rule, dict)]
    tags = [rule["configuration"] for rule in rules if rule.get("configuration")]
    if not default and tags:
        # clice falls back to the first declared tag.
        default = tags[0]
    entries: list[str] = []
    for rule in rules:
        tag = rule.get("configuration") or ""
        if tag and tag != default:
            continue
        entries += [entry for entry in rule.get("compile_commands", []) if isinstance(entry, str)]
    if not entries:
        entries = ["compile_commands.json"]

    resolved = []
    for entry in entries:
        path = Path(entry.replace("${workspace}", str(workspace)))
        if not path.is_absolute():
            path = workspace / path
        resolved.append(path if path.suffix.lower() == ".json" else path / "compile_commands.json")
    return resolved


def clice_database(workspace: Path) -> Path | None:
    """A compilation database clice loads for its active configuration."""
    databases = declared_databases(workspace)
    if databases is None:
        return None
    for database in databases:
        if database.is_file():
            return database
    for database in (workspace / "compile_commands.json", *workspace.glob("*/compile_commands.json")):
        if database.is_file():
            return database
    return None


def covers_project(paths: list[Path], repo_root: Path) -> bool:
    """Whether the request is the whole project, which is all clice lint checks."""
    for path in paths:
        try:
            if path.resolve() != repo_root.resolve():
                return False
        except OSError:
            return False
    return True


def resolve_backend(args: argparse.Namespace, repo_root: Path) -> tuple[str, Path | None, Path | None]:
    """Pick the C++ linter: the backend, its binary and clice's project root."""
    if args.fix:
        if args.backend == "clice":
            print("lint: --fix needs clang-tidy's fix mode; using clang-tidy", file=sys.stderr)
        return "clang-tidy", None, None
    if args.backend == "clang-tidy":
        return "clang-tidy", None, None

    binary = find_clice()
    workspace = clice_project_root(repo_root)
    if binary is None or workspace is None:
        if args.backend == "clice":
            print("lint: no clice binary and clice.toml above the project; using clang-tidy", file=sys.stderr)
        return "clang-tidy", None, None
    # clice lint checks every translation unit of the compilation database (both
    # host and engine), so auto keeps host lint runs fast and scoped to tracked
    # host files via parallel clang-tidy. Explicit --backend clice triggers the
    # full-database clice lint pass.
    if args.backend == "auto":
        return "clang-tidy", None, None
    return "clice", binary, workspace


def find_compile_commands(repo_root: Path) -> Path:
    """Return the freshest compilation database, generating one only if none exists."""
    candidates = [
        repo_root / "build/release/compile_commands.json",
        repo_root / "build/debug/compile_commands.json",
        repo_root / "build/releasedbg/compile_commands.json",
        repo_root / "build/asan/compile_commands.json",
        repo_root / "build/xmake-windows/compile_commands.json",
        repo_root / "build/xmake-linux/compile_commands.json",
        repo_root / "compile_commands.json",
    ]
    existing = [path for path in candidates if path.is_file()]
    if existing:
        return max(existing, key=lambda path: path.stat().st_mtime_ns)
    print("compile_commands.json not found; generating via xmake...", flush=True)
    fallback = repo_root / "compile_commands.json"
    try:
        subprocess.check_call(["xmake", "project", "-k", "compile_commands"], cwd=repo_root)
    except (OSError, subprocess.CalledProcessError) as error:
        raise ValueError(f"cannot generate compile_commands.json: {error}") from error
    return fallback


def compile_database(repo_root: Path) -> tuple[Path, list[dict]]:
    database_path = find_compile_commands(repo_root)
    return database_path, json.loads(database_path.read_text(encoding="utf-8"))


def entry_path(entry: dict) -> Path:
    path = Path(entry["file"])
    if not path.is_absolute():
        path = Path(entry["directory"]) / path
    return path.resolve()


def entry_directory(entry: dict) -> str:
    directory = entry.get("directory")
    if not directory:
        return ""
    return str(Path(directory).resolve()).replace("\\", "/")


def normalized_arguments(entry: dict) -> tuple[str, ...]:
    """The compile command with the flags that only name output files removed."""
    arguments = entry.get("arguments")
    if arguments is None:
        arguments = shlex.split(entry.get("command", ""), posix=False)
    source = str(entry["file"]).replace("\\", "/")
    normalized = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        cooked = argument.replace("\\", "/")
        if cooked in SEPARATE_OUTPUT_FLAGS:
            skip = True
            continue
        if cooked.startswith(ATTACHED_OUTPUT_PREFIXES):
            continue
        if cooked == source:
            continue
        normalized.append(cooked)
    return tuple(normalized)


def selected_units(database: list[dict], requested: set[Path]) -> tuple[list[dict], int, int]:
    """Keep one entry per distinct (source, directory, normalized command) tuple.

    Identical duplicates across targets are collapsed, while distinct
    compile configurations (e.g. test hooks enabled vs production or differing
    working directories) are retained so clang-tidy checks every variant. Header
    entries are dropped, as headers are checked through the translation units
    including them (`HeaderFilterRegex`).
    """
    entries = []
    seen_commands: dict[Path, set[tuple[str, tuple[str, ...]]]] = {}
    duplicates = 0
    variants = 0
    for entry in database:
        path = entry_path(entry)
        if path.suffix.lower() in HEADER_EXTENSIONS or path not in requested:
            continue
        command = (entry_directory(entry), normalized_arguments(entry))
        commands = seen_commands.setdefault(path, set())
        if command in commands:
            duplicates += 1
            continue
        if commands:
            variants += 1
        commands.add(command)
        entries.append(entry)
    return entries, duplicates, variants


def worker_count(jobs: int) -> int:
    """Processes to run at once; a non-positive count means the CPU count."""
    return jobs if jobs > 0 else max(1, os.cpu_count() or 1)


def run_clang_tidy(args: argparse.Namespace, repo_root: Path, cpp_files: list[Path]) -> int:
    try:
        database_path, database = compile_database(repo_root)
    except (OSError, ValueError) as error:
        print(f"lint: {error}", file=sys.stderr)
        return 1

    requested = {path.resolve() for path in cpp_files}
    explicit_files = {path.resolve() for path in args.paths if path.is_file()}
    units, duplicates, variants = selected_units(database, requested)
    known = {entry_path(entry) for entry in units}
    # Headers without a database entry are checked through the translation
    # units that include them (`HeaderFilterRegex`), unless named explicitly.
    extra = sorted(
        path
        for path in requested
        if path not in known and (path.suffix.lower() not in HEADER_EXTENSIONS or path in explicit_files)
    )
    jobs = sorted(known)
    jobs += extra

    workers = worker_count(args.jobs)
    if args.fix:
        # A header several units include would be edited from those units at
        # once; one process at a time keeps the fixes additive.
        workers = 1
        print("lint: --fix runs clang-tidy one file at a time", file=sys.stderr)

    print(
        f"Running clang-tidy on {len(jobs)} file(s) from {len(units)} translation unit entry/entries "
        f"({duplicates} duplicate collapsed, {variants} distinct variant(s) retained) with {workers} job(s)...",
        flush=True,
    )

    # clang-tidy re-parses a file once per database entry naming it, so the
    # selected entries are handed over as a database of their own. Every entry
    # carries an absolute working directory, which keeps it valid away from the
    # project root, and header lookups keep the units that hold them.
    package = database_path.parent
    temporary = None
    if units and len(units) != len(database):
        try:
            temporary = tempfile.TemporaryDirectory(prefix="engrave-lint-")
            (Path(temporary.name) / "compile_commands.json").write_text(json.dumps(units), encoding="utf-8")
            package = Path(temporary.name)
        except OSError as error:
            print(f"lint: cannot stage the collapsed database ({error}); linting {database_path}", file=sys.stderr)
            if temporary is not None:
                temporary.cleanup()
                temporary = None

    command = ["clang-tidy", "-p", str(package)]
    if args.fix:
        command += ["--fix-errors", "--format-style=file"]

    def check(path: Path):
        result = subprocess.run([*command, str(path)], capture_output=True, text=True, errors="replace")
        return path, result.returncode, result.stdout, result.stderr

    try:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            outcomes = list(pool.map(check, jobs))
    finally:
        if temporary is not None:
            temporary.cleanup()

    returncode = 0
    for path, code, stdout, stderr in outcomes:
        print(f"{path}: exit {code}", flush=True)
        if stdout:
            print(stdout, end="", flush=True)
        if stderr:
            print(stderr, end="", file=sys.stderr, flush=True)
        if returncode == 0:
            returncode = code
    return returncode


def lint_clice(binary: Path, workspace: Path, jobs: int) -> int:
    try:
        result = subprocess.run(
            [str(binary), "lint", "--workspace", str(workspace), "--jobs", str(worker_count(jobs))],
            capture_output=True,
            text=True,
            errors="replace",
        )
    except OSError as error:
        print(f"lint: failed to run {binary} ({error})", file=sys.stderr)
        return 2
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    return result.returncode


def lint_cpp(args: argparse.Namespace, repo_root: Path, cpp_files: list[Path]) -> int:
    backend, binary, workspace = resolve_backend(args, repo_root)
    if backend == "clice" and binary is not None and workspace is not None:
        if not covers_project(args.paths, repo_root):
            print(
                "lint: clice lint checks the project's whole compilation database; the path list does not narrow it",
                file=sys.stderr,
            )
        elif args.backend == "auto":
            # clice is the faster way to check every unit, but that is a wider
            # set than the tracked files clang-tidy is given.
            print(
                "lint: clice lint checks every unit of the compilation database, the engine's included; "
                "pass --backend clang-tidy for the tracked C++ files only",
                file=sys.stderr,
            )
        returncode = lint_clice(binary, workspace, args.jobs)
        if returncode != 2:
            return returncode
        print("lint: clice lint could not run here; using clang-tidy", file=sys.stderr)
    return run_clang_tidy(args, repo_root, cpp_files)


def main() -> int:
    args = parse_args()
    if args.engine:
        script = args.engine_root / "tools/lint.py"
        if not script.is_file():
            print(f"lint: {script} is missing; is the engine submodule initialized?", file=sys.stderr)
            return 1
        return subprocess.call([sys.executable, str(script), *sys.argv[1:]])
    repo_root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())

    try:
        files = project_files(args.paths)
    except (ValueError, FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"lint: {error}", file=sys.stderr)
        return 1

    py_files = [f for f in files if f.suffix.lower() in PYTHON_EXTENSIONS]
    cpp_files = [f for f in files if f.suffix.lower() in CPP_EXTENSIONS]

    from engrave_mcp_testing.layout import audit_test_layout

    layout_errors = audit_test_layout(repo_root)
    for error in layout_errors:
        print(error, file=sys.stderr)
    total_returncode = int(bool(layout_errors))

    if py_files:
        print(f"Running ruff check on {len(py_files)} Python file(s)...", flush=True)
        command = ["ruff", "check"]
        if args.fix:
            command.append("--fix")
        command.extend(map(str, py_files))
        returncode = subprocess.run(command, cwd=repo_root, check=False).returncode
        if returncode != 0:
            total_returncode = returncode

    if cpp_files:
        returncode = lint_cpp(args, repo_root, cpp_files)
        if returncode != 0:
            total_returncode = returncode

    return total_returncode


if __name__ == "__main__":
    sys.exit(main())
