"""The host wrapper dispatches both argparse spellings to the requested fork.

It also selects this project's Pixi environment and Bazel caches unless the
caller chose them.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

WRAPPER = Path(__file__).with_name("engine_gate.py")
PROJECT_ROOT = Path(__file__).resolve().parents[1]
HOST_MANIFEST = str(PROJECT_ROOT / "pixi.toml")
HOST_REPO_CACHE = str(PROJECT_ROOT / "build/.bazel_repo_cache")
HOST_DISK_CACHE = str(PROJECT_ROOT / "build/.bazel_disk_cache")


class EngineGateDispatch(unittest.TestCase):
    def dispatch(self, args):
        result = subprocess.run([sys.executable, str(WRAPPER), *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_engine_source_forms(self):
        with tempfile.TemporaryDirectory(prefix="fork with spaces ") as directory:
            root = Path(directory)
            (root / "tests").mkdir()
            (root / "tests/gate.py").write_text("import json, sys; print(json.dumps(sys.argv[1:]))", encoding="utf8")
            for source_args in (["--engine-source", str(root)], ["--engine-source=" + str(root)]):
                with self.subTest(args=source_args):
                    args = [*source_args, "--output", "unchanged", "--no-audio", "--goldens-only"]
                    self.assertEqual(
                        self.dispatch(args),
                        [
                            "--pixi-manifest",
                            HOST_MANIFEST,
                            "--repo-cache",
                            HOST_REPO_CACHE,
                            "--disk-cache",
                            HOST_DISK_CACHE,
                            *args,
                        ],
                    )
            for manifest_args in (["--pixi-manifest", "other.toml"], ["--pixi-manifest=other.toml"]):
                with self.subTest(args=manifest_args):
                    args = ["--engine-source", str(root), *manifest_args]
                    self.assertEqual(
                        self.dispatch(args),
                        ["--repo-cache", HOST_REPO_CACHE, "--disk-cache", HOST_DISK_CACHE, *args],
                    )
            for cache_args in (
                ["--repo-cache", "other-repo", "--disk-cache", "other-disk"],
                ["--repo-cache=other-repo", "--disk-cache=other-disk"],
            ):
                with self.subTest(args=cache_args):
                    args = ["--engine-source", str(root), "--pixi-manifest=other.toml", *cache_args]
                    self.assertEqual(self.dispatch(args), args)


if __name__ == "__main__":
    unittest.main()
