#!/usr/bin/env python3
"""Unified acceptance test lane runner for engrave-mcp.

Builds an isolated working directory and runs the current acceptance lanes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from support.paths import REPO_ROOT

FOCUSED_DEFAULT_LANES = ["native_includes", "native_inputs"]
FOCUSED_OPT_IN_LANES = [
    "output_schema_boundaries",
    "document_stress",
    "live_dense_cancel",
    "live_queued_cancel",
    "native_470_conversion_cancel",
    "native_470_serialization_cancel",
    "transport_wakeup",
    "xmake_incremental",
]

LANE_NAMES = [
    *FOCUSED_DEFAULT_LANES,
    *FOCUSED_OPT_IN_LANES,
    "host_api",
    "live_export",
    "live_audio",
    "audio_budget",
    "runtime_assets",
    "score_edit",
    "score_audio",
    "score_positions",
    "import_export_probe",
    "excerpts",
    "assets",
    "archives",
    "native_edit",
    "linked_tie",
    "xml_text",
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
    "native_470_oracle",
    "roundtrip",
    "oracle_harness",
    "rendered_page",
    "engine_audio",
    "upstream_gtest",
    "repeats",
    "version_info",
    "mcp_smoke",
]

# The 4.7.6 oracle is a preserved Linux-only input and must be explicitly
# selected with its pinned archive, executable and assets.
DEFAULT_LANES = [name for name in LANE_NAMES if name not in {"native_470_oracle", *FOCUSED_OPT_IN_LANES}]


def check_preflight_binaries(build_dir: Path, exe_suffix: str, lanes: list[str]) -> None:
    if not build_dir.is_dir():
        sys.stderr.write(f"Error: build dir does not exist or is not a directory: {build_dir}\n")
        sys.exit(2)
    missing = []
    requirements = {
        "native_includes": [],
        "native_470_conversion_cancel": ["engrave-mcp-test"],
        "native_470_serialization_cancel": ["engrave-mcp-test"],
        "xmake_incremental": ["engrave-mcp", "registry_dump"],
        "host_api": ["host_api_test"],
        "live_export": ["live_export_test", "engrave-mcp-test"],
        "live_audio": ["engrave-mcp", "engrave-mcp-test"],
        "audio_budget": ["engrave-mcp-test"],
        "runtime_assets": ["engrave-mcp-test"],
        "score_edit": ["score_edit_test"],
        "score_audio": ["score_audio_test"],
        "score_positions": ["score_positions_test"],
        "import_export_probe": ["import_export_probe"],
        "excerpts": ["excerpts_test"],
        "assets": ["assets_test"],
        "archives": ["archive_test", "engrave-mcp"],
        "async_session": ["engrave-mcp-test"],
        "golden": ["engrave-mcp", "semantic_dump"],
        "semantic_check": [],
        "oracle_harness": [],
        "native_470_oracle": [],
        "upstream_gtest": ["engraving_playback_tests", "mpe_tests"],
        "engine_audio": ["engine_audio_probe", "engrave-mcp"],
        "tool_schema": ["registry_dump", "registry_test"],
    }
    selected = {binary for lane in lanes for binary in requirements.get(lane, ["engrave-mcp"])}
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
    py = sys.executable

    if lane in FOCUSED_DEFAULT_LANES + FOCUSED_OPT_IN_LANES:
        cmd = [py, str(repo_root / "tests/integration" / f"test_{lane}.py")]
        focused_env = dict(base_env)
        if lane == "document_stress":
            focused_env["ENGRAVE_MCP_STRESS_OUTPUT"] = str(output / lane)
        elif lane not in ("native_includes", "native_inputs"):
            test_binary = "engrave-mcp-test" if lane.startswith("native_470_") else "engrave-mcp"
            cmd += ["--binary", str(build_dir / f"{test_binary}{exe_suffix}"), "--output", str(output / lane)]
            if lane == "native_470_conversion_cancel":
                cmd += [
                    "--host-sha256",
                    hashlib.sha256((build_dir / f"{test_binary}{exe_suffix}").read_bytes()).hexdigest(),
                ]
            if lane != "xmake_incremental":
                cmd += ["--resources", str(assets)]
            if lane in ("live_dense_cancel", "live_queued_cancel", "output_schema_boundaries"):
                cmd += ["--soundfont", str(soundfont)]
            if lane == "live_dense_cancel":
                from integration.test_live_queued_cancel import generate_cancel_score

                score = output / "dense.musicxml"
                generate_cancel_score(score)
                cmd += ["--score", str(score), "--delay-ms", "10"]
        return subprocess.run(cmd, cwd=repo_root, env=focused_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    retarget_code = (
        "import runpy, sys; from pathlib import Path; "
        "test_root = Path(sys.argv.pop(1)).resolve(); "
        "script = Path(sys.argv.pop(1)).resolve(); "
        "sys.argv[0] = str(script); "
        "module = runpy.run_path(str(script), run_name='__main__retargeted'); "
        "main = module['main']; "
        "main.__globals__['REPO_ROOT'] = test_root; "
        "res = main(); "
        "sys.exit(res if isinstance(res, int) else 0)"
    )

    if lane == "host_api":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_host_api.py"),
            "--binary",
            str(build_dir / f"host_api_test{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--replacement-soundfont",
            str(repo_root / "vendor/engrave/tests/data/engine_audio/sine.sf2"),
            "--output",
            str(output / "host-api"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane in ("live_export", "live_audio"):
        cmd = [
            py,
            str(repo_root / ("tests/integration/test_" + lane + ".py")),
            "--binary",
            str(build_dir / f"{'engrave-mcp-test' if lane == 'live_export' else 'engrave-mcp'}{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / lane),
        ]
        if lane == "live_export":
            cmd += ["--direct-binary", str(build_dir / f"live_export_test{exe_suffix}"), "--soundfont", str(soundfont)]
        else:
            cmd += [
                "--test-binary",
                str(build_dir / f"engrave-mcp-test{exe_suffix}"),
                "--soundfont",
                str(soundfont),
            ]
        return subprocess.run(cmd, cwd=repo_root, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "audio_budget":
        for script in ("test_audio_output_budget.py", "test_audio_repeat_budget.py"):
            cmd = [
                py,
                str(repo_root / "tests/integration" / script),
                "--binary",
                str(build_dir / f"engrave-mcp-test{exe_suffix}"),
                "--resources",
                str(assets),
                "--soundfont",
                str(soundfont),
                "--output",
                str(output / script.removesuffix(".py")),
            ]
            result = subprocess.run(
                cmd,
                cwd=repo_root,
                env=base_env,
                stdout=log_file,
                stderr=subprocess.STDOUT,
            ).returncode
            if result:
                return result
        return 0

    if lane == "runtime_assets":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_runtime_assets_mcp.py"),
            "--binary",
            str(build_dir / f"engrave-mcp-test{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "runtime-assets"),
        ]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_edit":
        cmd = [str(build_dir / f"score_edit_test{exe_suffix}"), str(assets)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_audio":
        cmd = [
            str(build_dir / f"score_audio_test{exe_suffix}"),
            str(assets),
            str(engine_source),
        ]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "score_positions":
        cmd = [str(build_dir / f"score_positions_test{exe_suffix}"), str(assets)]
        return subprocess.run(cmd, cwd=testcwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "import_export_probe":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_import_export_probe.py"),
            "--binary",
            str(build_dir / f"import_export_probe{exe_suffix}"),
            "--assets",
            str(assets),
            "--input",
            str(repo_root / "vendor/engrave/tests/data/inputs/evening.musicxml"),
            "--output",
            str(output / "import-export-probe"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "excerpts":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_excerpts.py"),
            "--binary",
            str(build_dir / f"excerpts_test{exe_suffix}"),
            "--assets",
            str(assets),
            "--output",
            str(output / "excerpts"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "assets":
        font = (
            Path("C:/Windows/Fonts/arial.ttf")
            if sys.platform == "win32"
            else (repo_root / ".pixi/envs/native/fonts/DejaVuSans.ttf")
        )
        cmd = [
            py,
            str(repo_root / "tests/integration/test_assets.py"),
            "--binary",
            str(build_dir / f"assets_test{exe_suffix}"),
            "--assets",
            str(assets),
            "--font",
            str(font),
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "assets"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "archives":
        cmd = [
            py,
            "-c",
            retarget_code,
            str(testcwd),
            str(repo_root / "tests/integration/test_archives.py"),
            "--binary",
            str(build_dir / f"archive_test{exe_suffix}"),
            "--server",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / "archives"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "native_edit":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_native_edit.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / "native-edit"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "linked_tie":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_linked_tie.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / "linked-tie"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "xml_text":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_xml_text.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / "xml-text"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "audio_facade":
        cmd = [
            py,
            "-c",
            retarget_code,
            str(testcwd),
            str(repo_root / "tests/integration/test_audio_facade.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "audio-facade"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "async_session":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_async_session.py"),
            "--binary",
            str(build_dir / f"engrave-mcp-test{exe_suffix}"),
            "--resources",
            str(assets),
            "--test-hooks",
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "async"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "imports_midi":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_imports_midi.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--upstream",
            str(engine_source),
            "--output",
            str(output / "imports-midi"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "open_data":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_open_data.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--upstream",
            str(engine_source),
            "--output",
            str(output / "open-data"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "imports_gp":
        gp_env = dict(base_env)
        gp_env["ENGRAVE_MCP_OUTPUT"] = str(output / "imports-guitarpro")
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
        cmd = [
            py,
            "-c",
            gp_code,
            str(engine_source),
            str(repo_root / "tests/integration/test_imports_guitarpro.py"),
        ]
        return subprocess.run(cmd, env=gp_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "tool_schema":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_tool_schema.py"),
            "--dump",
            str(build_dir / f"registry_dump{exe_suffix}"),
            "--registry-test",
            str(build_dir / f"registry_test{exe_suffix}"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "positions":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_positions.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--upstream",
            str(engine_source),
            "--output",
            str(output / "positions"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "export_options":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_export_options.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "export-options"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "transport":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_transport.py"),
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            str(output / "transport.json"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "golden":
        golden_dir = repo_root / (
            "vendor/engrave/tests/data/golden/windows"
            if sys.platform == "win32"
            else "vendor/engrave/tests/data/golden/linux"
        )
        cmd = [
            py,
            str(repo_root / "tests/support/golden.py"),
            "compare",
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--golden",
            str(golden_dir),
            "--allow",
            str(repo_root / "vendor/engrave/tests/data/golden/allow.json"),
            "--output",
            str(output / "golden"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "semantic_check":
        actual_dir = output / "golden/actual"
        golden_dir = repo_root / (
            "vendor/engrave/tests/data/golden/windows"
            if sys.platform == "win32"
            else "vendor/engrave/tests/data/golden/linux"
        )
        check_cmd = [
            py,
            str(repo_root / "tests/support/check_semantic_dump.py"),
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

    if lane == "native_470_oracle":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_native_470_oracle.py"),
            "--output",
            str(output / "native-470-oracle"),
            *native_470_args,
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "roundtrip":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_roundtrip.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--output",
            str(output / "roundtrip"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "oracle_harness":
        for test in ("test_oracle_harness.py", "test_cleanup_regressions.py"):
            cmd = [py, str(repo_root / "tests/integration" / test)]
            result = subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT)
            if result.returncode:
                return result.returncode
        return 0

    if lane == "rendered_page":
        cmd1 = [py, str(repo_root / "tests/integration/test_rendered_page.py")]
        proc1 = subprocess.run(cmd1, env=base_env, stdout=log_file, stderr=subprocess.STDOUT)
        if proc1.returncode != 0:
            return proc1.returncode
        cmd2 = [
            py,
            str(repo_root / "tests/support/check_rendered_page.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--output",
            str(output / "rendered-page"),
        ]
        return subprocess.run(cmd2, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "engine_audio":
        audio_args = []
        if os.environ.get("ENGRAVE_DEFER_DENSE_PERFORMANCE") == "1":
            audio_args.append("--defer-dense-performance")
        cmd = [
            py,
            str(repo_root / "tests/integration/test_engine_audio.py"),
            "--bin-dir",
            str(build_dir),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--soundfont",
            str(soundfont),
            "--engine-source",
            str(engine_source),
            "--output",
            str(output / "engine-audio"),
            "--cross-platform-captures",
            str(output / "audio-cross-platform"),
            *audio_args,
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "upstream_gtest":
        gtest_cwd = output / "gtest-cwd"
        gtest_cwd.mkdir(parents=True, exist_ok=True)
        cmd = [
            py,
            str(repo_root / "tests/integration/test_upstream_gtest.py"),
            "--bin-dir",
            str(build_dir),
            "--assets",
            str(assets),
            "--engraving-data",
            str(engine_source / "src/engraving/tests"),
            "--output",
            str(output / "gtest"),
        ]
        return subprocess.run(cmd, cwd=gtest_cwd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "repeats":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_repeats.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--upstream",
            str(engine_source),
            "--output",
            str(output / "repeats"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "version_info":
        cmd = [
            py,
            str(repo_root / "tests/integration/test_version_info.py"),
            "--binary",
            str(build_dir / f"engrave-mcp{exe_suffix}"),
            "--resources",
            str(assets),
            "--upstream",
            str(engine_source),
            "--output",
            str(output / "version-info"),
        ]
        return subprocess.run(cmd, env=base_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    if lane == "mcp_smoke":
        smoke_env = dict(base_env)
        smoke_env["ENGRAVE_MCP_OUTPUT"] = str(output / "smoke")
        cmd = [py, str(repo_root / "tests/integration/test_mcp_smoke.py")]
        return subprocess.run(cmd, env=smoke_env, stdout=log_file, stderr=subprocess.STDOUT).returncode

    sys.stderr.write(f"Unknown lane: {lane}\n")
    return 1


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run engrave-mcp acceptance test lanes.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        required=True,
        help="Directory containing compiled binaries",
    )
    parser.add_argument(
        "--engine-source",
        type=Path,
        required=True,
        help="Path to engine source repository",
    )
    parser.add_argument(
        "--assets",
        type=Path,
        required=True,
        help="Path to assets-50 resources directory",
    )
    parser.add_argument(
        "--soundfont",
        type=Path,
        required=True,
        help="Path to FluidR3Mono_GM.sf3 soundfont",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output directory for logs and test artifacts",
    )
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
        help="Physical Linux oracle host (linux-host); oracle paths are remote",
    )
    parser.add_argument(
        "--native-470-baseline",
        action="store_true",
        help="Replay projections only; not writer acceptance",
    )

    args = parser.parse_args()

    repo_root = REPO_ROOT
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

    # Validate --only lanes if requested
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
            if not current_dump.is_file():
                parser.error(f"baseline/writer-results current-reader control is missing: {current_dump}")
            native_470_args += [
                "--current-binary",
                str(current_dump),
                "--current-assets",
                str(assets),
            ]

    check_preflight_binaries(build_dir, exe_suffix, selected_lanes)

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
    (output / "tmp").mkdir(parents=True, exist_ok=True)

    testcwd = output / "cwd"
    (testcwd / "vendor").mkdir(parents=True, exist_ok=True)
    (testcwd / "tests").mkdir(exist_ok=True)
    setup_link(repo_root / "vendor/engrave/tests/data/inputs", testcwd / "fixtures", output)
    setup_link(engine_source, testcwd / "vendor/engrave", output)

    base_env = os.environ.copy()
    base_env["PYTHONPATH"] = os.pathsep.join(
        (str(repo_root / "tests"), str(repo_root / "scripts"), base_env.get("PYTHONPATH", ""))
    )
    base_env["TEMP"] = str(output / "tmp")
    base_env["TMP"] = str(output / "tmp")
    base_env["TMPDIR"] = str(output / "tmp")
    base_env["ENGRAVE_MCP_BINARY"] = str(build_dir / f"engrave-mcp{exe_suffix}")
    base_env["ENGRAVE_MCP_SOUNDFONT"] = str(soundfont)
    base_env["ENGRAVE_MCP_RESOURCES"] = str(assets)
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

    if args.only is None:
        notice = (
            f"opt-in lanes NOT SELECTED: {', '.join(FOCUSED_OPT_IN_LANES)}; "
            "native_470_oracle NOT SELECTED: run it explicitly on physical Linux "
            "with the preserved oracle; native 4.70 interoperability is not verified by this run"
        )
        print(notice, flush=True)
        summary_lines.append(notice)

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
                    native_470_args=native_470_args,
                )
            except Exception as e:
                log_file.write(f"Lane execution failed with exception: {e}\n".encode())
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
