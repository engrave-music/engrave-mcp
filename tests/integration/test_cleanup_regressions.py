"""Output-link isolation and authenticated golden provenance regressions."""

import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from engrave_mcp_testing import golden
from engrave_mcp_testing import lane_runner as run_lanes
from engrave_mcp_testing.paths import REPO_ROOT


class CleanupRegressions(unittest.TestCase):
    def test_reused_output_with_different_engine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "output"
            engines = [root / "engine-a", root / "engine-b"]
            for engine in engines:
                (engine / "tests/data").mkdir(parents=True)
                (engine / "tests/data/sentinel").write_text(engine.name)
            for engine in engines:
                argv = [
                    "run_lanes",
                    "--build-dir",
                    str(root),
                    "--engine-source",
                    str(engine),
                    "--assets",
                    str(root),
                    "--soundfont",
                    str(root / "font"),
                    "--output",
                    str(output),
                    "--only",
                    "host_api",
                ]
                with (
                    patch.object(sys, "argv", argv),
                    patch.object(run_lanes, "check_preflight_binaries"),
                    patch.object(run_lanes, "check_preflight_eol"),
                    patch.object(
                        run_lanes.subprocess, "run", return_value=__import__("subprocess").CompletedProcess([], 0)
                    ),
                    patch.object(run_lanes, "run_lane", return_value=0),
                ):
                    with self.assertRaises(SystemExit) as stopped:
                        run_lanes.main()
                    self.assertEqual(stopped.exception.code, 0)
                for checkout in engines:
                    self.assertEqual((checkout / "tests/data/sentinel").read_text(), checkout.name)
            # Reproduce the old nested operation explicitly: fail before touching data.
            with self.assertRaisesRegex(ValueError, "escapes output"):
                run_lanes.setup_link(
                    engines[0] / "tests/data",
                    output / "host_api/cwd/vendor/engrave/tests/data",
                    output,
                )
            self.assertEqual((engines[1] / "tests/data/sentinel").read_text(), "engine-b")

    def test_host_rejects_fixture_and_manifest_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "tests/data/golden/inputs.json"
            manifest.parent.mkdir(parents=True)
            shutil.copyfile(REPO_ROOT / "vendor/engrave/tests/data/golden/inputs.json", manifest)
            original = golden.fixture_files()[0]
            fixture = root / original.name
            fixture.write_bytes(original.read_bytes() + b"\n")
            pins = json.loads(manifest.read_text())
            name = original.name
            pins["fixtures"][name]["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
            manifest.write_text(json.dumps(pins))
            self.assertEqual(
                golden.verify_input_provenance(root=root, sources=[fixture], fixture_dir=root),
                ["input provenance manifest hash mismatch"],
            )
            # The comparison entry point must use the same authenticated host path.
            for folder in (root / "baseline", root / "actual"):
                folder.mkdir()
            (root / "baseline/index.json").write_text("{}")
            with patch.object(
                golden,
                "verify_input_provenance",
                side_effect=lambda: golden._engine_golden.verify_input_provenance(root=root),
            ):
                self.assertEqual(golden.compare_trees(root / "baseline", root / "actual", [], root, "windows"), 1)

    def test_parallel_lanes_precede_exclusive_and_semantic_follows_golden(self):
        barrier = threading.Barrier(2, timeout=2.0)
        lock = threading.Lock()
        active: set[str] = set()
        overlaps: dict[str, set[str]] = {}
        completion_order: list[str] = []

        def fake_execute(lane: str):
            with lock:
                active.add(lane)
                snapshot = set(active)
                overlaps.setdefault(lane, set()).update(snapshot)
                for other in snapshot:
                    overlaps.setdefault(other, set()).update(snapshot)
            try:
                if lane in ("golden", "native_includes"):
                    barrier.wait()
                return (7 if lane == "golden" else 0), 0.01
            finally:
                with lock:
                    active.remove(lane)
                    completion_order.append(lane)

        results = run_lanes.execute_schedule(
            ["golden", "native_includes", "semantic_check", "archives"], 2, fake_execute
        )
        self.assertEqual(
            {lane: code for lane, (code, _) in results.items()},
            {"golden": 7, "native_includes": 0, "semantic_check": 0, "archives": 0},
        )
        self.assertIn("native_includes", overlaps["golden"])
        self.assertNotIn("golden", overlaps["semantic_check"])
        self.assertEqual(overlaps["archives"], {"archives"})
        self.assertLess(completion_order.index("golden"), completion_order.index("semantic_check"))
        self.assertEqual(completion_order[-1], "archives")

    def test_selected_schedule_and_binary_requirements(self):
        argv = [
            "run_lanes",
            "--engine-source",
            "engine",
            "--assets",
            "assets",
            "--soundfont",
            "font",
            "--output",
            "output",
            "--list-lanes",
            "--only",
            "archives,semantic_check,golden,native_includes",
        ]
        output = io.StringIO()
        with patch.object(sys, "argv", argv), redirect_stdout(output):
            run_lanes.main()
        self.assertEqual(
            output.getvalue().splitlines(),
            [
                "parallel: native_includes",
                "parallel: golden",
                "parallel: semantic_check (after golden)",
                "exclusive: archives",
            ],
        )
        self.assertEqual(
            run_lanes.required_binaries(
                ["native_includes", "host_api", "tool_schema", "archives", "audio_budget", "golden", "semantic_check"]
            ),
            {
                "host_api_test",
                "registry_dump",
                "registry_test",
                "archive_test",
                "engrave-mcp",
                "engrave-mcp-test",
                "semantic_dump",
            },
        )

    def test_audio_budget_stops_after_first_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out"
            output.mkdir()
            recorded = []

            def fake_run(cmd, **kwargs):
                recorded.append(cmd)
                return subprocess.CompletedProcess(cmd, 13)

            with patch.object(run_lanes.subprocess, "run", side_effect=fake_run):
                code = run_lanes.run_lane(
                    lane="audio_budget",
                    repo_root=REPO_ROOT,
                    build_dir=root / "build",
                    engine_source=REPO_ROOT / "vendor/engrave",
                    assets=root / "assets",
                    soundfont=root / "soundfont.sf3",
                    output=output,
                    testcwd=root / "cwd",
                    base_env={"PATH": ""},
                    exe_suffix=".exe" if sys.platform == "win32" else "",
                    log_file=io.BytesIO(),
                    native_470_args=[],
                )

            self.assertEqual(code, 13)
            self.assertEqual(len(recorded), 1)
            self.assertEqual(
                Path(recorded[0][1]).resolve(),
                (REPO_ROOT / "tests/integration/test_audio_output_budget.py").resolve(),
            )

    def test_removed_lane_names_are_rejected(self):
        for lane in ("score_edit", "score_audio", "score_positions", "excerpts", "assets", "upstream_gtest"):
            with self.subTest(lane=lane):
                argv = [
                    "run_lanes",
                    "--engine-source",
                    "engine",
                    "--assets",
                    "assets",
                    "--soundfont",
                    "font",
                    "--output",
                    "output",
                    "--only",
                    lane,
                ]
                errors = io.StringIO()
                with patch.object(sys, "argv", argv), redirect_stderr(errors), self.assertRaises(SystemExit) as stopped:
                    run_lanes.main()
                self.assertEqual(stopped.exception.code, 1)
                self.assertEqual(errors.getvalue(), f"Error: Unknown lane: {lane}\n")

    def test_semantic_coverage_and_checker_failure_are_both_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            platform = "windows" if sys.platform == "win32" else "linux"
            expected = root / "repo/vendor/engrave/tests/data/golden" / platform / "one"
            actual = root / "out/golden/actual/one"
            for fixture in (expected, actual):
                fixture.mkdir(parents=True)
                (fixture / "score.mscx").write_text("score")
                (fixture / "semantic.json").write_text("{}")

            def check(exit_code):
                log = io.BytesIO()
                result = subprocess.CompletedProcess([], exit_code)
                with patch.object(run_lanes.subprocess, "run", return_value=result):
                    code = run_lanes.run_lane(
                        lane="semantic_check",
                        repo_root=root / "repo",
                        build_dir=root / "build",
                        engine_source=root / "engine",
                        assets=root / "assets",
                        soundfont=root / "font",
                        output=root / "out",
                        testcwd=root / "cwd",
                        base_env={},
                        exe_suffix="",
                        log_file=log,
                        native_470_args=[],
                    )
                return code, log.getvalue().decode()

            self.assertEqual(check(17), (17, "semantic coverage: 1 of 1 baseline fixtures\n"))
            (actual / "semantic.json").unlink()
            self.assertEqual(check(0), (1, "semantic coverage: 0 of 1 baseline fixtures\n"))


if __name__ == "__main__":
    unittest.main()
