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
from types import SimpleNamespace
from unittest.mock import patch

from engrave_mcp_testing import golden
from engrave_mcp_testing import lane_runner as run_lanes
from engrave_mcp_testing.paths import REPO_ROOT


class GoldenAudioReuseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.actual = root / "actual"
        self.audio = self.actual / "fixture.mscx/audio.json"
        self.audio.parent.mkdir(parents=True)
        self.audio.write_text('{"frames": 1}', encoding="utf-8")
        self.index = self.actual / "index.json"
        golden.build_index(self.actual)

        self.resources = root / "resources"
        self.resource = self.resources / "styles/default.mss"
        self.resource.parent.mkdir(parents=True)
        self.resource.write_bytes(b"style A")
        self.binary = root / "binary"
        self.binary.write_bytes(b"host binary")
        self.soundfont = root / "soundfont.sf3"
        self.soundfont.write_bytes(b"soundfont")
        fixture = root / "fixture.mscx"
        fixture.write_bytes(b"score fixture")
        self.fixtures = {fixture.name: fixture}
        self.provenance = golden.audio_capture_provenance(self.binary, self.resources, self.soundfont, self.fixtures)
        self.provenance_path = root / "provenance.json"
        self.write_provenance()
        self.assertEqual(self.reuse(), {fixture.name: self.audio})

    def write_provenance(self):
        self.provenance["indexSha256"] = hashlib.sha256(self.index.read_bytes()).hexdigest()
        golden.write_json(self.provenance_path, self.provenance)

    def reuse(self):
        return golden.reusable_golden_audio(self.actual, self.fixtures, self.binary, self.resources, self.soundfont)

    def test_golden_audio_rejects_swapped_index_and_tree(self):
        replacement = self.actual.with_name("replacement")
        audio = replacement / self.audio.relative_to(self.actual)
        audio.parent.mkdir(parents=True)
        audio.write_text('{"frames": 2}', encoding="utf-8")
        index = golden.build_index(replacement)
        self.assertEqual(golden.verify_golden_index(replacement, index), [])
        self.actual.rename(self.actual.with_name("original"))
        replacement.rename(self.actual)
        self.assertIsNone(self.reuse())

    def test_golden_audio_rejects_resource_content_change(self):
        self.resource.write_bytes(b"style B")
        self.assertIsNone(self.reuse())

    def test_golden_audio_rejects_extractor_digest_change(self):
        real_hash = golden._file_sha256
        extractor_files = (
            Path(golden.__file__),
            Path(golden.inspect.getfile(golden.describe_audio)),
            Path(golden.inspect.getfile(golden.composition_musicxml)),
            Path(golden.inspect.getfile(golden._engine_audio.CapturePolicy)),
        )
        for target in extractor_files:
            with self.subTest(extractor=target.name):
                resolved = target.resolve()

                def altered(path: Path, *, _target=resolved):
                    return "0" * 64 if path.resolve() == _target else real_hash(path)

                with patch.object(golden, "_file_sha256", side_effect=altered):
                    self.assertIsNone(self.reuse())

    def test_golden_audio_treats_bad_metadata_as_cache_miss(self):
        nested = b"[" * 2000 + b"]" * 2000
        for path in (self.provenance_path, self.index):
            original = path.read_bytes()
            for invalid in (b"{", b"[]", b"{}", b"\xff", nested):
                with self.subTest(path=path.name, invalid=invalid[:16]):
                    path.write_bytes(invalid)
                    if path == self.index:
                        self.write_provenance()
                    self.assertIsNone(self.reuse())
            path.write_bytes(original)
            self.write_provenance()
        self.index.unlink()
        self.assertIsNone(self.reuse())

    def test_interrupted_golden_capture_clears_old_provenance(self):
        args = SimpleNamespace(
            allow=self.actual.parent / "allow.json",
            output=self.actual.parent,
            binary=self.binary,
            resources=self.resources,
            soundfont=self.soundfont,
        )
        with (
            patch.object(golden, "fixture_files", return_value=list(self.fixtures.values())),
            patch.object(golden, "mnx_fixture_files", return_value=[]),
            patch.object(golden, "capture_tree", side_effect=OSError("capture interrupted")),
            self.assertRaisesRegex(OSError, "capture interrupted"),
        ):
            golden.command_compare(args)
        self.assertFalse(self.provenance_path.exists())


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

        def fake_execute(lane: str, completed):
            if lane == "semantic_check":
                self.assertEqual(completed["golden"][0], 7)
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

    def test_audio_lane_only_reuses_successful_golden_from_same_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "golden/actual").mkdir(parents=True)
            (root / "golden/actual/index.json").touch()
            (root / "golden/provenance.json").touch()
            for lanes, golden_exit, reuse_expected in (
                ("engine_audio", 0, False),
                ("golden,engine_audio", 1, False),
                ("golden,engine_audio", 0, True),
            ):
                with self.subTest(lanes=lanes, golden_exit=golden_exit):
                    audio_commands = []

                    def fake_run(cmd, **kwargs):
                        script = Path(cmd[1]).name
                        if script == "test_engine_audio.py":
                            audio_commands.append(cmd)
                        return subprocess.CompletedProcess(cmd, golden_exit if script == "golden.py" else 0)

                    argv = [
                        "run_lanes",
                        "--engine-source",
                        str(root),
                        "--assets",
                        str(root),
                        "--soundfont",
                        str(root / "soundfont"),
                        "--output",
                        str(root),
                        "--only",
                        lanes,
                        "--no-build",
                    ]
                    with (
                        patch.object(sys, "argv", argv),
                        patch.object(run_lanes, "check_preflight_eol"),
                        patch.object(run_lanes, "check_preflight_binaries"),
                        patch.object(run_lanes, "setup_link"),
                        patch.object(run_lanes.subprocess, "run", side_effect=fake_run),
                        redirect_stdout(io.StringIO()),
                        self.assertRaises(SystemExit) as stopped,
                    ):
                        run_lanes.main()
                    self.assertEqual(stopped.exception.code, golden_exit)
                    self.assertEqual(len(audio_commands), 1)
                    self.assertEqual("--reuse-golden-actual" in audio_commands[0], reuse_expected)

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
