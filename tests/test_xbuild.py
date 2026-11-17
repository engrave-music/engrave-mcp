"""Regression tests for scripts/xbuild.py configuration identity and stamp migration."""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[1]
XBUILD_PATH = REPO_ROOT / "scripts/xbuild.py"
SPEC = importlib.util.spec_from_file_location("xbuild_under_test", XBUILD_PATH)
assert SPEC and SPEC.loader
xbuild = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(xbuild)


class XBuildTests(unittest.TestCase):
    def test_canonical_default_build_directory_shares_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            default_res = xbuild.resolve_layout(root, "release", None, "x64")
            explicit_default = xbuild.resolve_layout(root, "release", Path("build/release"), "x64")
            dotted_default = xbuild.resolve_layout(root, "release", Path("./build/release"), "x64")
            custom = xbuild.resolve_layout(root, "release", Path("build/custom"), "x64")
            self.assertEqual(default_res[0], explicit_default[0])
            self.assertEqual(default_res[1], explicit_default[1])
            self.assertEqual(default_res[1], dotted_default[1])
            self.assertNotEqual(default_res[1], custom[1])

    def test_stale_stamp_triggers_clean_reconfigure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build, config, plat, _tools, _payload = xbuild.resolve_layout(root, "asan", None, "x64")
            conf = config / ".xmake" / plat / "x64" / "xmake.conf"
            conf.parent.mkdir(parents=True)
            conf.write_text("{}\n", encoding="utf-8")
            old_stamp = {"profile": "asan", "build": str(build), "plat": plat, "arch": "x64"}
            (config / ".xbuild-stamp.json").write_text(json.dumps(old_stamp), encoding="utf-8")

            calls = []

            def fake_call(cmd, cwd=None, env=None):
                calls.append(cmd)
                return 0

            with (
                patch.object(xbuild, "__file__", str(root / "scripts/xbuild.py")),
                patch.object(xbuild.subprocess, "call", side_effect=fake_call),
            ):
                rc = xbuild.main(["--profile", "asan", "--arch", "x64", "build", "engrave_mcp"])

            self.assertEqual(rc, 0)
            self.assertEqual(len(calls), 2)
            self.assertIn("-c", calls[0])
            self.assertEqual(calls[0][1], "f")
            self.assertIn(f"--ccachedir={root / 'build/.build_cache'}", calls[0])
            self.assertIn("--engrave_tests=y", calls[0])
            self.assertEqual(calls[1][1], "build")
            new_stamp = json.loads((config / ".xbuild-stamp.json").read_text(encoding="utf-8"))
            self.assertEqual(new_stamp["version"], 3)
            self.assertEqual(
                new_stamp["default_options"], [f"--ccachedir={root / 'build/.build_cache'}", "--engrave_tests=y"]
            )

            # Second invocation with current stamp skips configure.
            calls.clear()
            with (
                patch.object(xbuild, "__file__", str(root / "scripts/xbuild.py")),
                patch.object(xbuild.subprocess, "call", side_effect=fake_call),
            ):
                rc2 = xbuild.main(["--profile", "asan", "--arch", "x64", "build", "engrave_mcp"])

            self.assertEqual(rc2, 0)
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][1], "build")

    def test_configure_respects_option_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            cases = [
                (["--ccachedir=custom-cache"], ["--ccachedir=custom-cache", "--engrave_tests=y"]),
                (["--engrave_tests=n"], [f"--ccachedir={root / 'build/.build_cache'}", "--engrave_tests=n"]),
                (["--ccachedir", "custom-cache", "--engrave_tests", "n"], ["--ccachedir", "--engrave_tests"]),
            ]
            for supplied, expected in cases:
                with self.subTest(supplied=supplied):
                    with (
                        patch.object(xbuild, "__file__", str(root / "scripts/xbuild.py")),
                        patch.object(xbuild.subprocess, "call", return_value=0) as call,
                    ):
                        rc = xbuild.main(["configure", *supplied])
                    self.assertEqual(rc, 0)
                    self.assertEqual(call.call_args.args[0][-len(supplied) :], supplied)
                    options = [
                        arg for arg in call.call_args.args[0] if arg.startswith(("--ccachedir", "--engrave_tests"))
                    ]
                    self.assertCountEqual(options, expected)


if __name__ == "__main__":
    sys.exit(unittest.main())
