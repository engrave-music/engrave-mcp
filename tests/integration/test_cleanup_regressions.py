"""Output-link isolation and authenticated golden provenance regressions."""

import hashlib
import json
import shutil
import sys
import tempfile
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
                run_lanes.setup_link(engines[0] / "tests/data", output / "cwd/vendor/engrave/tests/data", output)
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


if __name__ == "__main__":
    unittest.main()
