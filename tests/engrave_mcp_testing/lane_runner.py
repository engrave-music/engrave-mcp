#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import runpy
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import dataclass
from pathlib import Path

from engrave_testing.paths import pixi_env

from engrave_mcp_testing.paths import REPO_ROOT

BUILD_PROFILES = ("release", "debug", "releasedbg", "asan")


def execute_schedule(lanes, jobs, execute):
    results = {}
    for exclusive, workers in ((False, jobs), (True, 1)):
        pending = [lane for lane in lanes if (lane in EXCLUSIVE_LANES) == exclusive]
        with ThreadPoolExecutor(max_workers=workers) as pool:
            running = {}
            while pending or running:
                for lane in pending[:]:
                    if len(running) >= workers:
                        break
                    if any(
                        dependency in lanes and dependency not in results for dependency in LANES_BY_NAME[lane].after
                    ):
                        continue
                    running[pool.submit(execute, lane)] = lane
                    pending.remove(lane)
                done, _ = wait(running, return_when=FIRST_COMPLETED)
                for future in done:
                    results[running.pop(future)] = future.result()
    return results


def discover_build_dir(repo_root: Path, profile: str = "release") -> Path:
    return repo_root / "build" / profile


def build_profile(repo_root: Path, build_dir: Path, fallback: str) -> str:
    return next(
        (profile for profile in BUILD_PROFILES if build_dir == discover_build_dir(repo_root, profile).resolve()),
        fallback,
    )


def required_binaries(lanes: list[str]) -> set[str]:
    return {binary for lane in lanes for binary in LANES_BY_NAME[lane].binaries}


def build_required_binaries(repo_root: Path, build_dir: Path, profile: str, binaries: set[str]) -> None:
    if not binaries:
        return
    profile = build_profile(repo_root, build_dir, profile)
    build_options = (
        [] if build_dir == discover_build_dir(repo_root, profile).resolve() else ["--build-directory", str(build_dir)]
    )
    target_names = {"engrave-mcp": "engrave_mcp", "engrave-mcp-test": "engrave_mcp_test"}
    targets = sorted(target_names.get(binary, binary) for binary in binaries)
    print(f"Building {', '.join(targets)} in {build_dir} ({profile})", flush=True)
    result = subprocess.run(
        [
            sys.executable,
            str(repo_root / "scripts/xbuild.py"),
            "--profile",
            profile,
            *build_options,
            "build",
            *targets,
        ],
        cwd=repo_root,
    )
    if result.returncode:
        raise SystemExit(f"Error: target build failed with exit code {result.returncode}")


def check_preflight_binaries(build_dir: Path, exe_suffix: str, binaries: set[str]) -> None:
    if not build_dir.is_dir():
        sys.stderr.write(f"Error: build dir does not exist or is not a directory: {build_dir}\n")
        sys.exit(2)
    missing = []
    for binary_name in sorted(binaries):
        target = build_dir / f"{binary_name}{exe_suffix}"
        if not target.is_file():
            missing.append(target.name)
    if missing:
        sys.stderr.write(
            f"Error: build dir {build_dir} lacks expected binaries ({len(missing)} missing):\n"
            + "\n".join(f"  - {m}" for m in missing)
            + "\n"
        )
        sys.exit(2)


def check_preflight_eol(repo_root: Path) -> None:
    try:
        res = subprocess.run(
            ["git", "ls-files", "--eol"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception as e:
        sys.stderr.write(f"Error running git ls-files --eol: {e}\n")
        sys.exit(2)

    violations = []
    boms = []
    for line in res.stdout.splitlines():
        if not line or "\t" not in line:
            continue
        meta, path = line.split("\t", 1)
        tokens = meta.split()
        if len(tokens) < 2:
            continue
        i_eol = tokens[0]
        w_eol = tokens[1]
        attrs = tokens[2:]
        binary = any("-text" in a for a in attrs)
        crlf_allowed = binary or any("eol=crlf" in a for a in attrs)
        if (w_eol == "w/mixed" and not binary) or (w_eol == "w/crlf" and not crlf_allowed):
            violations.append((path, w_eol, " ".join(attrs)))
        if not binary and i_eol != "i/-text":
            try:
                with open(repo_root / path, "rb") as f:
                    if f.read(3) == b"\xef\xbb\xbf":
                        boms.append(path)
            except OSError:
                pass

    if violations:
        sys.stderr.write(
            f"Error: git ls-files --eol in {repo_root} shows unexpected CRLF/mixed line endings in {len(violations)} file(s):\n"
            + "\n".join(f"  - {p}: {w} ({a})" for p, w, a in violations[:25])
            + "\n"
        )
        sys.exit(2)
    if boms:
        sys.stderr.write(
            f"Error: {len(boms)} text file(s) in {repo_root} start with a UTF-8 BOM:\n"
            + "\n".join(f"  - {p}" for p in boms[:25])
            + "\n"
        )
        sys.exit(2)


def setup_link(target: Path, link: Path, output: Path) -> None:
    target = target.resolve()
    # Resolve the parent, never the leaf: unlinking a link must not touch its target.
    boundary = output.resolve()
    if not link.parent.resolve().is_relative_to(boundary):
        raise ValueError(f"link parent escapes output directory: {link}")
    if link.exists() and not (link.is_symlink() or link.is_junction()):
        if not link.resolve().is_relative_to(boundary):
            raise ValueError(f"link replacement escapes output directory: {link}")
    link.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        import _winapi

        if link.is_symlink() or (hasattr(link, "is_junction") and link.is_junction()) or link.exists():
            try:
                if link.resolve() == target:
                    return
            except OSError:
                pass
            if hasattr(link, "is_junction") and link.is_junction():
                os.rmdir(link)
            elif link.is_symlink() or link.is_file():
                link.unlink()
            elif link.is_dir():
                shutil.rmtree(link)
        try:
            _winapi.CreateJunction(str(target), str(link))
        except Exception:
            os.symlink(str(target), str(link), target_is_directory=True)
    else:
        if link.is_symlink() or link.exists():
            try:
                if link.resolve() == target:
                    return
            except OSError:
                pass
            if link.is_symlink() or link.is_file():
                link.unlink()
            elif link.is_dir():
                shutil.rmtree(link)
        os.symlink(str(target), str(link), target_is_directory=True)


@dataclass(frozen=True)
class LaneContext:
    repo_root: Path
    build_dir: Path
    engine_source: Path
    assets: Path
    soundfont: Path
    output: Path
    testcwd: Path
    exe_suffix: str
    native_470_args: list[str]


@dataclass(frozen=True)
class Command:
    argv: list[str]
    cwd: Path
    env: dict[str, str]


type Argument = str | Callable[[LaneContext], str | list[str]]
type Builder = Callable[[LaneContext], list[Command]]
type Finish = Callable[[LaneContext, object, int], int]


@dataclass(frozen=True)
class Lane:
    name: str
    build: Builder
    binaries: tuple[str, ...] = ("engrave-mcp",)
    default: bool = True
    exclusive: bool = False
    after: tuple[str, ...] = ()
    finish: Finish | None = None


def _path(attribute: str, *parts: str) -> Argument:
    return lambda context: str(getattr(context, attribute).joinpath(*parts))


def _binary(name: str) -> Argument:
    return lambda context: str(context.build_dir / f"{name}{context.exe_suffix}")


def _arguments(context: LaneContext, arguments: tuple[Argument, ...]) -> list[str]:
    result = []
    for argument in arguments:
        value = argument(context) if callable(argument) else argument
        result.extend(value if isinstance(value, list) else [value])
    return result


def _command(
    *arguments: Argument,
    cwd: str = "testcwd",
    env: dict[str, Argument] | None = None,
) -> Builder:
    def build(context: LaneContext) -> list[Command]:
        extra_env = {key: _arguments(context, (value,))[0] for key, value in (env or {}).items()}
        return [Command(_arguments(context, arguments), getattr(context, cwd), extra_env)]

    return build


def _script(name: str, *arguments: Argument, **options) -> Builder:
    return _command(sys.executable, _path("repo_root", "tests/integration", f"test_{name}.py"), *arguments, **options)


def _host(
    name: str,
    output: str,
    *,
    binary: str = "engrave-mcp",
    soundfont: bool = False,
    upstream: bool = False,
    before_soundfont: tuple[Argument, ...] = (),
    extra: tuple[Argument, ...] = (),
) -> Builder:
    arguments: list[Argument] = ["--binary", _binary(binary), "--resources", _path("assets"), *before_soundfont]
    if soundfont:
        arguments += ["--soundfont", _path("soundfont")]
    if upstream:
        arguments += ["--upstream", _path("engine_source")]
    arguments += [*extra, "--output", _path("output", output)]
    return _script(name, *arguments)


def _sequence(*builders: Builder) -> Builder:
    return lambda context: [command for build in builders for command in build(context)]


def _cancel_score(context: LaneContext) -> str:
    from engrave_mcp_testing.cancel_score import generate_cancel_score

    score = context.output / "live_dense_cancel/dense.musicxml"
    score.parent.mkdir(parents=True, exist_ok=True)
    generate_cancel_score(score)
    return str(score)


def _test_binary_hash(context: LaneContext) -> str:
    return hashlib.sha256((context.build_dir / f"engrave-mcp-test{context.exe_suffix}").read_bytes()).hexdigest()


def _golden_dir(context: LaneContext) -> str:
    platform = "windows" if sys.platform == "win32" else "linux"
    return str(context.repo_root / "vendor/engrave/tests/data/golden" / platform)


def _semantic_result(context: LaneContext, log_file, check_code: int) -> int:
    def count(directory: Path) -> int:
        if not directory.is_dir():
            return 0
        return sum(
            child.is_dir() and (child / "score.mscx").is_file() and (child / "semantic.json").is_file()
            for child in directory.iterdir()
        )

    expected_count = count(Path(_golden_dir(context)))
    actual_count = count(context.output / "golden/actual")
    log_file.write(f"semantic coverage: {actual_count} of {expected_count} baseline fixtures\n".encode("utf-8"))
    log_file.flush()
    if expected_count == 0 or actual_count != expected_count:
        return 1
    return check_code


def _deferred_audio(context: LaneContext) -> list[str]:
    return ["--defer-dense-performance"] if os.environ.get("ENGRAVE_DEFER_DENSE_PERFORMANCE") == "1" else []


def _native_470_arguments(context: LaneContext) -> list[str]:
    return context.native_470_args


LANES = (
    Lane("native_includes", _script("native_includes"), binaries=()),
    Lane("native_inputs", _script("native_inputs", "--output", _path("output", "native_inputs"))),
    Lane(
        "windows_manifest",
        _script("windows_manifest", "--build-dir", _path("build_dir")),
        binaries=("engrave-mcp", "engrave-mcp-test", "engrave_output_test") if sys.platform == "win32" else (),
    ),
    Lane(
        "output_schema_boundaries",
        _script(
            "output_schema_boundaries",
            "--binary",
            _binary("engrave-mcp"),
            "--output",
            _path("output", "output_schema_boundaries"),
            "--resources",
            _path("assets"),
            "--soundfont",
            _path("soundfont"),
        ),
        default=False,
    ),
    Lane(
        "document_stress",
        _script("document_stress", env={"ENGRAVE_MCP_STRESS_OUTPUT": _path("output", "document_stress")}),
        default=False,
        exclusive=True,
    ),
    Lane(
        "live_dense_cancel",
        _script(
            "live_dense_cancel",
            "--binary",
            _binary("engrave-mcp"),
            "--output",
            _path("output", "live_dense_cancel"),
            "--resources",
            _path("assets"),
            "--soundfont",
            _path("soundfont"),
            "--score",
            _cancel_score,
            "--delay-ms",
            "10",
        ),
        default=False,
        exclusive=True,
    ),
    Lane(
        "live_queued_cancel",
        _script(
            "live_queued_cancel",
            "--binary",
            _binary("engrave-mcp"),
            "--output",
            _path("output", "live_queued_cancel"),
            "--resources",
            _path("assets"),
            "--soundfont",
            _path("soundfont"),
        ),
        default=False,
        exclusive=True,
    ),
    Lane(
        "native_470_conversion_cancel",
        _script(
            "native_470_conversion_cancel",
            "--binary",
            _binary("engrave-mcp-test"),
            "--output",
            _path("output", "native_470_conversion_cancel/run"),
            "--host-sha256",
            _test_binary_hash,
            "--resources",
            _path("assets"),
        ),
        binaries=("engrave-mcp-test",),
        default=False,
    ),
    Lane(
        "native_470_serialization_cancel",
        _script(
            "native_470_serialization_cancel",
            "--binary",
            _binary("engrave-mcp-test"),
            "--output",
            _path("output", "native_470_serialization_cancel/run"),
            "--resources",
            _path("assets"),
        ),
        binaries=("engrave-mcp-test",),
        default=False,
    ),
    Lane(
        "transport_wakeup",
        _script(
            "transport_wakeup",
            "--binary",
            _binary("engrave-mcp"),
            "--output",
            _path("output", "transport_wakeup.json"),
            "--resources",
            _path("assets"),
        ),
        default=False,
        exclusive=True,
    ),
    Lane(
        "xmake_incremental",
        _script(
            "xmake_incremental",
            "--binary",
            _binary("engrave-mcp"),
            "--output",
            _path("output", "xmake_incremental"),
            "--assets",
            _path("assets"),
            cwd="repo_root",
        ),
        binaries=("engrave-mcp", "registry_dump"),
        default=False,
        exclusive=True,
    ),
    Lane(
        "host_api",
        _host(
            "host_api",
            "host-api",
            binary="host_api_test",
            soundfont=True,
            extra=("--replacement-soundfont", _path("repo_root", "vendor/engrave/tests/data/engine_audio/sine.sf2")),
        ),
        binaries=("host_api_test",),
    ),
    # Cancellation and delivery deadlines require isolation from other lanes.
    Lane(
        "live_export",
        _script(
            "live_export",
            "--binary",
            _binary("engrave-mcp-test"),
            "--resources",
            _path("assets"),
            "--output",
            _path("output", "live_export"),
            "--direct-binary",
            _binary("live_export_test"),
            "--soundfont",
            _path("soundfont"),
        ),
        binaries=("live_export_test", "engrave-mcp-test"),
        exclusive=True,
    ),
    Lane(
        "live_audio",
        _script(
            "live_audio",
            "--binary",
            _binary("engrave-mcp"),
            "--resources",
            _path("assets"),
            "--output",
            _path("output", "live_audio"),
            "--test-binary",
            _binary("engrave-mcp-test"),
            "--soundfont",
            _path("soundfont"),
        ),
        binaries=("engrave-mcp", "engrave-mcp-test"),
        exclusive=True,
    ),
    Lane(
        "audio_budget",
        _sequence(
            _host("audio_output_budget", "test_audio_output_budget", binary="engrave-mcp-test", soundfont=True),
            _host("audio_repeat_budget", "test_audio_repeat_budget", binary="engrave-mcp-test", soundfont=True),
        ),
        binaries=("engrave-mcp-test",),
    ),
    Lane(
        "runtime_assets",
        _host("runtime_assets_mcp", "runtime-assets", binary="engrave-mcp-test", soundfont=True),
        binaries=("engrave-mcp-test",),
    ),
    Lane(
        "import_export_probe",
        _script(
            "import_export_probe",
            "--binary",
            _binary("import_export_probe"),
            "--assets",
            _path("assets"),
            "--input",
            _path("repo_root", "vendor/engrave/tests/data/inputs/evening.musicxml"),
            "--output",
            _path("output", "import-export-probe"),
        ),
        binaries=("import_export_probe",),
    ),
    Lane(
        "archives",
        _script(
            "archives",
            "--binary",
            _binary("archive_test"),
            "--server",
            _binary("engrave-mcp"),
            "--resources",
            _path("assets"),
            "--upstream",
            _path("engine_source"),
            "--output",
            _path("output", "archives"),
        ),
        binaries=("archive_test", "engrave-mcp"),
        exclusive=True,
    ),
    Lane("native_edit", _host("native_edit", "native-edit")),
    Lane("linked_tie", _host("linked_tie", "linked-tie")),
    Lane("xml_text", _host("xml_text", "xml-text")),
    Lane(
        "audio_facade",
        _host("audio_facade", "audio-facade", soundfont=True, upstream=True),
    ),
    Lane(
        "async_session",
        _host("async_session", "async", binary="engrave-mcp-test", soundfont=True, before_soundfont=("--test-hooks",)),
        binaries=("engrave-mcp-test",),
        exclusive=True,
    ),
    Lane("imports_midi", _host("imports_midi", "imports-midi", upstream=True)),
    Lane("open_data", _host("open_data", "open-data", upstream=True)),
    Lane(
        "imports_gp",
        _host("imports_guitarpro", "imports-guitarpro", upstream=True),
    ),
    Lane(
        "tool_schema",
        _script("tool_schema", "--dump", _binary("registry_dump"), "--registry-test", _binary("registry_test")),
        binaries=("registry_dump", "registry_test"),
    ),
    Lane("positions", _host("positions", "positions", upstream=True)),
    Lane("export_options", _host("export_options", "export-options", soundfont=True)),
    Lane("transport", _script("transport", _binary("engrave-mcp"), _path("output", "transport.json"))),
    Lane(
        "golden",
        _command(
            sys.executable,
            _path("repo_root", "tests/engrave_mcp_testing/golden.py"),
            "compare",
            "--binary",
            _binary("engrave-mcp"),
            "--resources",
            _path("assets"),
            "--soundfont",
            _path("soundfont"),
            "--golden",
            _golden_dir,
            "--allow",
            _path("repo_root", "vendor/engrave/tests/data/golden/allow.json"),
            "--output",
            _path("output", "golden"),
        ),
        binaries=("engrave-mcp", "semantic_dump"),
    ),
    Lane(
        "semantic_check",
        _command(
            sys.executable,
            _path("engine_source", "tests/engrave_testing/check_semantic_dump.py"),
            _path("output", "golden/actual"),
        ),
        binaries=(),
        after=("golden",),
        finish=_semantic_result,
    ),
    # The preserved 4.7.6 oracle requires pinned Linux-only inputs.
    Lane(
        "native_470_oracle",
        _script("native_470_oracle", "--output", _path("output", "native-470-oracle"), _native_470_arguments),
        binaries=(),
        default=False,
    ),
    Lane("roundtrip", _host("roundtrip", "roundtrip")),
    Lane(
        "oracle_harness",
        _sequence(_script("oracle_harness"), _script("cleanup_regressions")),
        binaries=(),
    ),
    Lane(
        "rendered_page",
        _sequence(
            _script("rendered_page"),
            _command(
                sys.executable,
                _path("repo_root", "tests/engrave_mcp_testing/check_rendered_page.py"),
                "--binary",
                _binary("engrave-mcp"),
                "--resources",
                _path("assets"),
                "--soundfont",
                _path("soundfont"),
                "--output",
                _path("output", "rendered-page"),
            ),
        ),
    ),
    Lane(
        "engine_audio",
        _script(
            "engine_audio",
            "--bin-dir",
            _path("build_dir"),
            "--binary",
            _binary("engrave-mcp"),
            "--resources",
            _path("assets"),
            "--soundfont",
            _path("soundfont"),
            "--engine-source",
            _path("engine_source"),
            "--output",
            _path("output", "engine-audio"),
            "--cross-platform-captures",
            _path("output", "audio-cross-platform"),
            _deferred_audio,
        ),
        binaries=("engine_audio_probe", "engrave-mcp"),
        exclusive=True,
    ),
    Lane("repeats", _host("repeats", "repeats", upstream=True)),
    Lane("version_info", _host("version_info", "version-info", upstream=True)),
    Lane("mcp_smoke", _script("mcp_smoke", env={"ENGRAVE_MCP_OUTPUT": _path("output", "smoke")})),
)

LANES_BY_NAME = {lane.name: lane for lane in LANES}
LANE_NAMES = list(LANES_BY_NAME)
DEFAULT_LANES = [lane.name for lane in LANES if lane.default]
FOCUSED_OPT_IN_LANES = [lane.name for lane in LANES if not lane.default and lane.name != "native_470_oracle"]
EXCLUSIVE_LANES = {lane.name for lane in LANES if lane.exclusive}


def run_lane(
    lane: str,
    repo_root: Path,
    build_dir: Path,
    engine_source: Path,
    assets: Path,
    soundfont: Path,
    output: Path,
    testcwd: Path,
    base_env: dict[str, str],
    exe_suffix: str,
    log_file,
    native_470_args: list[str],
) -> int:
    spec = LANES_BY_NAME.get(lane)
    if spec is None:
        sys.stderr.write(f"Unknown lane: {lane}\n")
        return 1
    context = LaneContext(
        repo_root, build_dir, engine_source, assets, soundfont, output, testcwd, exe_suffix, native_470_args
    )
    code = 0
    for command in spec.build(context):
        env = dict(base_env, **command.env) if command.env else base_env
        code = subprocess.run(
            command.argv, cwd=command.cwd, env=env, stdout=log_file, stderr=subprocess.STDOUT
        ).returncode
        if code:
            break
    return spec.finish(context, log_file, code) if spec.finish else code


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run engrave-mcp acceptance test lanes.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="Directory containing compiled binaries (default: build/<profile>)",
    )
    parser.add_argument(
        "--profile",
        choices=BUILD_PROFILES,
        default="release",
        help="Profile for the default or a custom build directory (default: release)",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="Skip the preflight build; some lanes build as part of their checks"
    )
    parser.add_argument(
        "--engine-source",
        type=Path,
        help="Path to engine source repository",
    )
    parser.add_argument(
        "--assets",
        type=Path,
        help="Path to assets-50 resources directory",
    )
    parser.add_argument(
        "--soundfont",
        type=Path,
        help="Path to FluidR3Mono_GM.sf3 soundfont",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output directory for logs and test artifacts",
    )
    parser.add_argument("-j", "--jobs", type=int, default=min(8, max(2, (os.cpu_count() or 4) // 2)))
    parser.add_argument("--list-lanes", action="store_true", help="Print the selected schedule without running lanes")
    parser.add_argument("--only", type=str, default=None, help="Comma-separated list of lanes to run")
    parser.add_argument("--tag", type=str, default=None, help="Optional run tag")
    parser.add_argument("--native-470-oracle-archive", type=Path, help="Pinned 4.7.6 oracle tar.gz")
    parser.add_argument(
        "--native-470-oracle-executable",
        type=Path,
        help="Extracted Linux 4.7.6 semantic_dump",
    )
    parser.add_argument("--native-470-oracle-assets", type=Path, help="Extracted 4.7.6 oracle assets")
    parser.add_argument("--native-470-writer-results", type=Path, help="Actual write470 result directories")
    parser.add_argument(
        "--native-470-live-edits",
        action="store_true",
        help="Include live tie-edit companion in producer lane",
    )
    parser.add_argument(
        "--native-470-producer",
        action="store_true",
        help="Real MCP source-to-oracle producer",
    )
    parser.add_argument("--host-binary", type=Path, help="Producer MCP executable")
    parser.add_argument("--host-sha256", help="Required expected producer binary SHA-256")
    parser.add_argument(
        "--native-470-oracle-ssh",
        help="Physical Linux oracle host (for example, user@<linux-host>); oracle paths are remote",
    )
    parser.add_argument(
        "--native-470-baseline",
        action="store_true",
        help="Replay projections only; not writer acceptance",
    )

    args = parser.parse_args()

    repo_root = REPO_ROOT
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    exe_suffix = ".exe" if sys.platform == "win32" else ""

    selected_lanes = DEFAULT_LANES
    if args.only is not None:
        requested = [item.strip() for item in args.only.split(",") if item.strip()]
        if not requested:
            sys.stderr.write("Error: --only selects no lanes\n")
            sys.exit(1)
        for name in requested:
            if name not in LANE_NAMES:
                sys.stderr.write(f"Error: Unknown lane: {name}\n")
                sys.exit(1)
        selected_lanes = [lane for lane in LANE_NAMES if lane in requested]

    if args.list_lanes:
        for exclusive in (False, True):
            for lane in selected_lanes:
                if (lane in EXCLUSIVE_LANES) != exclusive:
                    continue
                phase = "exclusive" if exclusive else "parallel"
                dependencies = [name for name in LANES_BY_NAME[lane].after if name in selected_lanes]
                dependency = f" (after {', '.join(dependencies)})" if dependencies else ""
                print(f"{phase}: {lane}{dependency}")
        return

    missing_args = [
        flag
        for flag, value in (
            ("--engine-source", args.engine_source),
            ("--assets", args.assets),
            ("--soundfont", args.soundfont),
            ("--output", args.output),
        )
        if value is None
    ]
    if missing_args:
        parser.error(f"the following arguments are required: {', '.join(missing_args)}")

    build_dir = (args.build_dir or discover_build_dir(repo_root, args.profile)).resolve()
    engine_source = args.engine_source.resolve()
    assets = args.assets.resolve()
    soundfont = args.soundfont.resolve()
    output = args.output.resolve()

    manifest = repo_root / "sha256.json"
    if manifest.is_file():
        # A frozen runner has no Git checkout. Verify its archived bytes instead.
        for name, expected in json.loads(manifest.read_text(encoding="utf-8")).items():
            path = (repo_root / name).resolve()
            if not path.is_relative_to(repo_root) or not path.is_file():
                raise SystemExit(f"Invalid archive member: {name}")
            if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise SystemExit(f"Archive hash mismatch: {name}")
    else:
        check_preflight_eol(repo_root)
        include_check = subprocess.run(
            [
                sys.executable,
                str(repo_root / "scripts/check_native_includes.py"),
                "--engine-source",
                str(engine_source),
            ],
            cwd=repo_root,
        )
        if include_check.returncode:
            raise SystemExit("Error: native include boundary check failed")
    check_preflight_eol(engine_source)

    native_470_args: list[str] = []
    native_470_requested = (
        args.native_470_oracle_archive,
        args.native_470_oracle_executable,
        args.native_470_oracle_assets,
        args.native_470_baseline,
        args.native_470_writer_results,
        args.native_470_producer,
        args.native_470_live_edits,
        args.native_470_oracle_ssh,
        args.host_binary,
        args.host_sha256,
    )
    if "native_470_oracle" not in selected_lanes and any(native_470_requested):
        parser.error("native oracle options require selecting --only native_470_oracle")
    if "native_470_oracle" in selected_lanes:
        if not all(
            (
                args.native_470_oracle_archive,
                args.native_470_oracle_executable,
                args.native_470_oracle_assets,
            )
        ):
            parser.error(
                "native_470_oracle requires --native-470-oracle-archive, --native-470-oracle-executable and --native-470-oracle-assets"
            )
        if (
            sum(
                (
                    args.native_470_baseline,
                    args.native_470_writer_results is not None,
                    args.native_470_producer,
                )
            )
            != 1
        ):
            parser.error(
                "native_470_oracle requires exactly one of --native-470-baseline, --native-470-writer-results, --native-470-producer"
            )
        native_470_args = [
            "--oracle-archive",
            args.native_470_oracle_archive.as_posix(),
            "--oracle-executable",
            args.native_470_oracle_executable.as_posix(),
            "--oracle-assets",
            args.native_470_oracle_assets.as_posix(),
        ]
        if args.native_470_oracle_ssh:
            native_470_args += ["--oracle-ssh", args.native_470_oracle_ssh]
        if args.native_470_live_edits and not args.native_470_producer:
            parser.error("--native-470-live-edits requires --native-470-producer")
        if args.native_470_live_edits:
            native_470_args += ["--live-edits"]
        if args.native_470_producer:
            if not args.host_binary or not args.host_sha256:
                parser.error("producer requires --host-binary and --host-sha256")
            native_470_args += [
                "--producer",
                "--host-binary",
                str(args.host_binary.resolve()),
                "--host-sha256",
                args.host_sha256,
                "--host-resources",
                str(assets),
            ]
        elif args.native_470_baseline:
            native_470_args += ["--baseline"]
        else:
            native_470_args += [
                "--writer-results",
                str(args.native_470_writer_results.resolve()),
            ]
        if not args.native_470_producer:
            current_dump = build_dir / f"semantic_dump{exe_suffix}"
            native_470_args += [
                "--current-binary",
                str(current_dump),
                "--current-assets",
                str(assets),
            ]

    binaries = required_binaries(selected_lanes)
    if "native_470_oracle" in selected_lanes:
        if not args.native_470_producer:
            binaries.add("semantic_dump")
        elif args.host_binary.resolve() == build_dir / f"engrave-mcp{exe_suffix}":
            binaries.update(("engrave-mcp", "semantic_dump"))
        elif not args.no_build:
            parser.error("an external oracle producer requires --no-build")
    if not args.no_build:
        build_required_binaries(repo_root, build_dir, args.profile, binaries)
    check_preflight_binaries(build_dir, exe_suffix, binaries)

    if (
        "semantic_check" in selected_lanes
        and "golden" not in selected_lanes
        and not (output / "golden/actual/index.json").is_file()
    ):
        sys.stderr.write(
            "Error: semantic_check needs a prior golden capture in "
            f"{output / 'golden/actual'}; select golden too or reuse its output directory\n"
        )
        sys.exit(2)

    output.mkdir(parents=True, exist_ok=True)
    base_env = os.environ.copy()
    base_env["ENGRAVE_MCP_BINARY"] = str(build_dir / f"engrave-mcp{exe_suffix}")
    base_env["ENGRAVE_MCP_SOUNDFONT"] = str(soundfont)
    base_env["ENGRAVE_MCP_RESOURCES"] = str(assets)
    base_env["MUSESCORE_ENGINE_SOURCE"] = str(engine_source)
    base_env["ENGRAVING_TESTS_DATA_ROOT"] = str(engine_source / "src/engraving/tests")
    base_env["PYTHONDONTWRITEBYTECODE"] = "1"
    if "xmake_incremental" in selected_lanes:
        # The lane invokes xmake directly after changing source, so it needs the
        # same configuration and package store as the preflight build.
        xbuild = runpy.run_path(str(repo_root / "scripts/xbuild.py"))
        profile = build_profile(repo_root, build_dir, args.profile)
        arch = "x64" if os.name == "nt" else xbuild["platform"].machine()
        config = xbuild["resolve_layout"](repo_root, profile, build_dir, arch)[1]
        base_env["XMAKE_CONFIGDIR"] = str(config)
        base_env["XMAKE_PKG_LOCALDIR"] = str(repo_root / "build/.packages")

    native_env = pixi_env(repo_root)
    for extra in (native_env / "Library/bin", native_env / "bin"):
        if extra.is_dir():
            base_env["PATH"] = str(extra.resolve()) + os.pathsep + base_env.get("PATH", "")
    for lib in (native_env / "Library/lib", native_env / "lib"):
        if lib.is_dir():
            base_env["LD_LIBRARY_PATH"] = str(lib.resolve()) + os.pathsep + base_env.get("LD_LIBRARY_PATH", "")

    failed_count = 0
    pending_count = 0
    selected_count = len(selected_lanes)
    summary_lines = []

    if args.only is None:
        notice = (
            f"opt-in lanes NOT SELECTED: {', '.join(FOCUSED_OPT_IN_LANES)}; "
            "native_470_oracle NOT SELECTED: run it explicitly on physical Linux "
            "with the preserved oracle; native 4.70 interoperability is not verified by this run"
        )
        print(notice, flush=True)
        summary_lines.append(notice)

    def execute(lane):
        log_path = output / f"{lane}.log"
        t0 = time.perf_counter()
        with open(log_path, "wb") as log_file:
            try:
                testcwd = output / lane / "cwd"
                (testcwd / "vendor").mkdir(parents=True, exist_ok=True)
                (testcwd / "tests").mkdir(exist_ok=True)
                setup_link(repo_root / "vendor/engrave/tests/data/inputs", testcwd / "fixtures", output)
                setup_link(engine_source, testcwd / "vendor/engrave", output)
                tmp = output / lane / "tmp"
                tmp.mkdir(parents=True, exist_ok=True)
                lane_env = dict(base_env, TEMP=str(tmp), TMP=str(tmp), TMPDIR=str(tmp))
                code = run_lane(
                    lane=lane,
                    repo_root=repo_root,
                    build_dir=build_dir,
                    engine_source=engine_source,
                    assets=assets,
                    soundfont=soundfont,
                    output=output,
                    testcwd=testcwd,
                    base_env=lane_env,
                    exe_suffix=exe_suffix,
                    log_file=log_file,
                    native_470_args=native_470_args,
                )
            except Exception as e:
                log_file.write(f"Lane execution failed with exception: {e}\n".encode())
                code = 1

        return code, time.perf_counter() - t0

    results = execute_schedule(selected_lanes, args.jobs, execute)
    for lane in selected_lanes:
        code, elapsed = results[lane]
        log_path = output / f"{lane}.log"
        if code == 0:
            status = "PASSED"
        elif lane == "engine_audio" and code == 2:
            status = "PENDING"
            pending_count += 1
        else:
            status = "FAILED"
            failed_count += 1

        line = f"{lane:<16} {status:<7} exit={code:<4} {elapsed:>5.1f}s  {log_path}"
        print(line, flush=True)
        summary_lines.append(line)

    if failed_count > 0:
        overall_status = "FAILED"
    elif pending_count > 0:
        overall_status = "PENDING"
    else:
        overall_status = "PASSED"

    overall_line = f"overall: {overall_status} ({selected_count} lanes; {failed_count} failed, {pending_count} pending)"
    print(overall_line, flush=True)
    summary_lines.append(overall_line)

    summary_file = output / "summary.txt"
    summary_file.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    if failed_count > 0 or pending_count > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
