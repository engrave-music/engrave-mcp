#!/usr/bin/env python3
"""Unified acceptance test lane runner for musescore-mcp.

Replaces notes/a2/verify/run_all_a2.ps1 (Windows) and
notes/a2/linux/run_all_a2_linux.sh (Linux).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

LANE_NAMES = [
    "music_core",
    "score_edit",
    "score_audio",
    "score_positions",
    "noqt_support",
    "excerpts",
    "assets",
    "archives",
    "native_edit",
    "audio_facade",
    "async_session",
    "imports_midi",
    "open_data",
    "imports_gp",
    "tool_schema",
    "positions",
    "export_options",
    "transport",
    "golden",
    "semantic_check",
    "compat_corpus",
    "roundtrip",
    "oracle_harness",
    "rendered_page",
    "engine_audio",
    "upstream_gtest",
    "repeats",
    "version_info",
    "mcp_smoke",
]


def check_preflight_binaries(build_dir: Path, exe_suffix: str, lanes: list[str]) -> None:
    if not build_dir.is_dir():
        sys.stderr.write(f"Error: build dir does not exist or is not a directory: {build_dir}\n")
        sys.exit(2)
    missing = []
    requirements = {
        "music_core": ["music_core_test"], "score_edit": ["score_edit_test"],
        "score_audio": ["score_audio_test"], "score_positions": ["score_positions_test"],
        "noqt_support": ["noqt_support_test"], "excerpts": ["excerpts_test"],
        "assets": ["assets_test"], "archives": ["archive_test", "musescore-mcp"],
        "async_session": ["musescore-mcp-test"], "golden": ["musescore-mcp", "semantic_dump"],
        "semantic_check": [], "oracle_harness": [], "compat_corpus": ["semantic_dump"],
        "upstream_gtest": ["engraving_playback_tests", "mpe_tests"],
        "engine_audio": ["engine_audio_probe", "musescore-mcp"],
    }
    selected = {binary for lane in lanes for binary in requirements.get(lane, ["musescore-mcp"])}
    for binary_name in sorted(selected):
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


def setup_link(target: Path, link: Path) -> None:
    target = target.resolve()
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
) -> int:
    py = sys.executable

    retarget_code = (
        "import runpy, sys; from pathlib import Path; "
        "test_root = Path(sys.argv.pop(1)).resolve(); "
        "script = Path(sys.argv.pop(1)).resolve(); "
        "sys.argv[0] = str(script); "
        "module = runpy.run_path(str(script), run_name='__main__retargeted'); "
        "main = module['main']; "
        "main.__globals__['ROOT'] = test_root; "
        "res = main(); "
        "sys.exit(res if isinstance(res, int) else 0)"
    )

    if lane == "music_core":
        cmd = [str(build_dir / f"music_core_test{exe_suffix}"), str(assets)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_edit":
        cmd = [str(build_dir / f"score_edit_test{exe_suffix}"), str(assets)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_audio":
        cmd = [str(build_dir / f"score_audio_test{exe_suffix}"), str(assets), str(engine_source)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_positions":
        cmd = [str(build_dir / f"score_positions_test{exe_suffix}"), str(assets)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "noqt_support":
        noqt_dir = output / "noqt-support"
        if noqt_dir.exists():
            shutil.rmtree(noqt_dir)
        cmd = [str(build_dir / f"noqt_support_test{exe_suffix}"), str(noqt_dir)]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "excerpts":
        cmd = [
            py,
            str(repo_root / "scripts/test_excerpts.py"),
            "--binary", str(build_dir / f"excerpts_test{exe_suffix}"),
            "--assets", str(assets),
            "--output", str(output / "excerpts"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "assets":
        font = Path("C:/Windows/Fonts/arial.ttf") if sys.platform == "win32" else (repo_root / ".pixi/envs/native/fonts/DejaVuSans.ttf")
        cmd = [
            py,
            str(repo_root / "scripts/test_assets.py"),
            "--binary", str(build_dir / f"assets_test{exe_suffix}"),
            "--assets", str(assets),
            "--font", str(font),
            "--soundfont", str(soundfont),
            "--output", str(output / "assets"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "archives":
        cmd = [
            py, "-c", retarget_code,
            str(testcwd),
            str(repo_root / "scripts/test_archives.py"),
            "--binary", str(build_dir / f"archive_test{exe_suffix}"),
            "--server", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--output", str(output / "archives"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "native_edit":
        cmd = [
            py,
            str(repo_root / "scripts/test_native_edit.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--output", str(output / "native-edit"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "audio_facade":
        cmd = [
            py, "-c", retarget_code,
            str(testcwd),
            str(repo_root / "scripts/test_audio_facade.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--soundfont", str(soundfont),
            "--output", str(output / "audio-facade"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "async_session":
        cmd = [
            py,
            str(repo_root / "scripts/test_async_session.py"),
            "--binary", str(build_dir / f"musescore-mcp-test{exe_suffix}"),
            "--resources", str(assets),
            "--test-hooks",
            "--soundfont", str(soundfont),
            "--output", str(output / "async"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "imports_midi":
        cmd = [
            py,
            str(repo_root / "scripts/test_imports_midi.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--upstream", str(engine_source),
            "--output", str(output / "imports-midi"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "open_data":
        cmd = [
            py,
            str(repo_root / "scripts/test_open_data.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--upstream", str(engine_source),
            "--output", str(output / "open-data"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "imports_gp":
        gp_env = dict(base_env)
        gp_env["MUSIC_MCP_OUTPUT"] = str(output / "imports-guitarpro")
        gp_code = (
            "import asyncio, os, runpy, sys; from pathlib import Path; "
            "engine = Path(sys.argv.pop(1)).resolve(); "
            "script = Path(sys.argv.pop(1)).resolve(); "
            "sys.argv[0] = str(script); "
            "module = runpy.run_path(str(script), run_name='__main__gp'); "
            "main = module['main']; "
            "main.__globals__['FIXTURES'] = engine / 'src/importexport/guitarpro/tests/data'; "
            "res = asyncio.run(asyncio.wait_for(main(), timeout=600)); "
            "sys.exit(res if isinstance(res, int) else 0)"
        )
        cmd = [py, "-c", gp_code, str(engine_source), str(repo_root / "scripts/test_imports_guitarpro.py")]
        return subprocess.run(cmd, env=gp_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "tool_schema":
        cmd = [
            py,
            str(repo_root / "scripts/test_tool_schema.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--assets", str(assets),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "positions":
        cmd = [
            py,
            str(repo_root / "scripts/test_positions.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--upstream", str(engine_source),
            "--output", str(output / "positions"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "export_options":
        cmd = [
            py,
            str(repo_root / "scripts/test_export_options.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--soundfont", str(soundfont),
            "--output", str(output / "export-options"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "transport":
        cmd = [
            py,
            str(repo_root / "scripts/test_transport.py"),
            str(build_dir / f"musescore-mcp{exe_suffix}"),
            str(output / "transport.json"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "golden":
        golden_dir = repo_root / ("tests/golden/windows" if sys.platform == "win32" else "tests/golden/linux")
        cmd = [
            py,
            str(repo_root / "scripts/golden.py"),
            "compare",
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--soundfont", str(soundfont),
            "--golden", str(golden_dir),
            "--allow", str(repo_root / "tests/golden/allow.json"),
            "--output", str(output / "golden"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "semantic_check":
        actual_dir = output / "golden/actual"
        golden_dir = repo_root / ("tests/golden/windows" if sys.platform == "win32" else "tests/golden/linux")
        check_cmd = [
            py,
            str(repo_root / "scripts/check_semantic_dump.py"),
            str(actual_dir),
        ]
        check_proc = subprocess.run(check_cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT)
        check_code = check_proc.returncode

        expected_count = 0
        if golden_dir.is_dir():
            for p in golden_dir.iterdir():
                if p.is_dir() and (p / "score.mscx").is_file() and (p / "semantic.json").is_file():
                    expected_count += 1

        actual_count = 0
        if actual_dir.is_dir():
            for p in actual_dir.iterdir():
                if p.is_dir() and (p / "score.mscx").is_file() and (p / "semantic.json").is_file():
                    actual_count += 1

        msg = f"semantic coverage: {actual_count} of {expected_count} baseline fixtures\n"
        log_file.write(msg.encode("utf-8"))
        log_file.flush()
        if expected_count == 0 or actual_count != expected_count:
            return 1
        return check_code

    if lane == "compat_corpus":
        cmd = [
            py,
            str(repo_root / "scripts/test_compat_corpus.py"),
            "--binary", str(build_dir / f"semantic_dump{exe_suffix}"),
            "--resources", str(assets),
            "--output", str(output / "compat-corpus"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "roundtrip":
        cmd = [
            py,
            str(repo_root / "scripts/test_roundtrip.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--output", str(output / "roundtrip"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "oracle_harness":
        cmd = [py, str(repo_root / "scripts/test_oracle_harness.py")]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "rendered_page":
        cmd1 = [py, str(repo_root / "scripts/test_rendered_page.py")]
        proc1 = subprocess.run(cmd1, env=base_env, stdout=log_file, stderr=subprocess.STDOUT)
        if proc1.returncode != 0:
            return proc1.returncode
        cmd2 = [
            py,
            str(repo_root / "scripts/check_rendered_page.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--soundfont", str(soundfont),
            "--output", str(output / "rendered-page"),
        ]
        return subprocess.run(cmd2, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "engine_audio":
        audio_args = []
        if os.environ.get("MUSESCORE_A2_DEFER_DENSE_PERFORMANCE") == "1":
            audio_args.append("--defer-dense-performance")
        cmd = [
            py,
            str(repo_root / "scripts/test_engine_audio.py"),
            "--bin-dir", str(build_dir),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--soundfont", str(soundfont),
            "--engine-source", str(engine_source),
            "--output", str(output / "engine-audio"),
            *audio_args,
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "upstream_gtest":
        gtest_cwd = output / "gtest-cwd"
        gtest_cwd.mkdir(parents=True, exist_ok=True)
        cmd = [
            py,
            str(repo_root / "scripts/test_upstream_gtest.py"),
            "--bin-dir", str(build_dir),
            "--assets", str(assets),
            "--engraving-data", str(engine_source / "src/engraving/tests"),
            "--output", str(output / "gtest"),
        ]
        return subprocess.run(cmd, cwd=gtest_cwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "repeats":
        cmd = [
            py,
            str(repo_root / "scripts/test_repeats.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--upstream", str(engine_source),
            "--output", str(output / "repeats"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "version_info":
        cmd = [
            py,
            str(repo_root / "scripts/test_version_info.py"),
            "--binary", str(build_dir / f"musescore-mcp{exe_suffix}"),
            "--resources", str(assets),
            "--upstream", str(engine_source),
            "--output", str(output / "version-info"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "mcp_smoke":
        smoke_env = dict(base_env)
        smoke_env["MUSIC_MCP_OUTPUT"] = str(output / "smoke")
        cmd = [py, str(repo_root / "scripts/mcp_smoke.py")]
        return subprocess.run(cmd, env=smoke_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    sys.stderr.write(f"Unknown lane: {lane}\n")
    return 1


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run musescore-mcp acceptance test lanes.",
        allow_abbrev=False,
    )
    parser.add_argument("--build-dir", type=Path, required=True, help="Directory containing compiled binaries")
    parser.add_argument("--engine-source", type=Path, required=True, help="Path to engine source repository")
    parser.add_argument("--assets", type=Path, required=True, help="Path to assets-50 resources directory")
    parser.add_argument("--soundfont", type=Path, required=True, help="Path to FluidR3Mono_GM.sf3 soundfont")
    parser.add_argument("--output", type=Path, required=True, help="Output directory for logs and test artifacts")
    parser.add_argument("--only", type=str, default=None, help="Comma-separated list of lanes to run")
    parser.add_argument("--tag", type=str, default=None, help="Optional run tag")

    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    build_dir = args.build_dir.resolve()
    engine_source = args.engine_source.resolve()
    assets = args.assets.resolve()
    soundfont = args.soundfont.resolve()
    output = args.output.resolve()

    exe_suffix = ".exe" if sys.platform == "win32" else ""

    # Pre-flight check 1: expected binaries in build dir

    # Pre-flight check 2: git line endings and no UTF-8 BOM in both checkouts
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
    check_preflight_eol(engine_source)

    # Validate --only lanes if requested
    selected_lanes = LANE_NAMES
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

    check_preflight_binaries(build_dir, exe_suffix, selected_lanes)

    output.mkdir(parents=True, exist_ok=True)
    (output / "tmp").mkdir(parents=True, exist_ok=True)

    testcwd = output / "cwd"
    (testcwd / "vendor").mkdir(parents=True, exist_ok=True)
    setup_link(repo_root / "fixtures", testcwd / "fixtures")
    setup_link(engine_source, testcwd / "vendor/musescore")

    base_env = os.environ.copy()
    base_env["TEMP"] = str(output / "tmp")
    base_env["TMP"] = str(output / "tmp")
    base_env["TMPDIR"] = str(output / "tmp")
    base_env["MUSIC_MCP_BINARY"] = str(build_dir / f"musescore-mcp{exe_suffix}")
    base_env["MUSIC_MCP_SOUNDFONT"] = str(soundfont)
    base_env["MUSIC_MCP_RESOURCES"] = str(assets)
    base_env["MUSESCORE_ENGINE_SOURCE"] = str(engine_source)
    base_env["ENGRAVING_TESTS_DATA_ROOT"] = str(engine_source / "src/engraving/tests")
    base_env["PYTHONDONTWRITEBYTECODE"] = "1"

    native_env = repo_root / ".pixi/envs/native"
    for extra in (native_env / "Library/bin", native_env / "bin"):
        if extra.is_dir():
            base_env["PATH"] = str(extra.resolve()) + os.pathsep + base_env.get("PATH", "")
    for lib in (native_env / "Library/lib", native_env / "lib"):
        if lib.is_dir():
            base_env["LD_LIBRARY_PATH"] = str(lib.resolve()) + os.pathsep + base_env.get("LD_LIBRARY_PATH", "")

    failed_count = 0
    pending_count = 0
    selected_count = 0
    summary_lines = []

    for lane in selected_lanes:
        selected_count += 1
        log_path = output / f"{lane}.log"
        t0 = time.perf_counter()
        with open(log_path, "wb") as log_file:
            try:
                code = run_lane(
                    lane=lane,
                    repo_root=repo_root,
                    build_dir=build_dir,
                    engine_source=engine_source,
                    assets=assets,
                    soundfont=soundfont,
                    output=output,
                    testcwd=testcwd,
                    base_env=base_env,
                    exe_suffix=exe_suffix,
                    log_file=log_file,
                )
            except Exception as e:
                log_file.write(f"Lane execution failed with exception: {e}\n".encode("utf-8"))
                code = 1

        elapsed = time.perf_counter() - t0

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
