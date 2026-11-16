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
            # Exercise main's workspace setup twice with the same --output.
            # Stop immediately after setup, before starting any lane subprocess.
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

    def test_upstream_gtest_is_isolated_from_parallel_lanes(self):
        self.assertIn("upstream_gtest", run_lanes.DEFAULT_LANES)
        self.assertIn("upstream_gtest", run_lanes.EXCLUSIVE_LANES)

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
                return 0, 0.01
            finally:
                with lock:
                    active.remove(lane)
                    completion_order.append(lane)

        results = run_lanes.execute_schedule(["golden", "native_includes", "upstream_gtest"], 2, fake_execute)
        self.assertEqual(
            {lane: code for lane, (code, _) in results.items()},
            {"golden": 0, "native_includes": 0, "upstream_gtest": 0},
        )
        self.assertEqual(overlaps["golden"], {"golden", "native_includes"})
        self.assertEqual(overlaps["native_includes"], {"golden", "native_includes"})
        self.assertEqual(overlaps["upstream_gtest"], {"upstream_gtest"})
        self.assertEqual(completion_order[-1], "upstream_gtest")

    def test_upstream_gtest_lane_passes_serial_jobs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out"
            output.mkdir()
            recorded = []

            def fake_run(cmd, **kwargs):
                recorded.append(cmd)
                return subprocess.CompletedProcess(cmd, 0)

            with patch.object(run_lanes.subprocess, "run", side_effect=fake_run):
                code = run_lanes.run_lane(
                    lane="upstream_gtest",
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

            self.assertEqual(code, 0)
            self.assertEqual(len(recorded), 1)
            cmd = recorded[0]
            self.assertEqual(
                Path(cmd[1]).resolve(),
                (REPO_ROOT / "vendor/engrave/tests/integration/test_upstream_gtest.py").resolve(),
            )
            jobs_idx = cmd.index("--jobs")
            self.assertEqual(cmd[jobs_idx + 1], "1")


if __name__ == "__main__":
    unittest.main()
