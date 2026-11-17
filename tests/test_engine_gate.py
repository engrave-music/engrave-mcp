"""The host wrapper dispatches both argparse spellings to the requested fork.

It also selects this project's Pixi environment and compile cache unless the
caller chose them.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

WRAPPER = Path(__file__).with_name("engine_gate.py")
HOST_MANIFEST = str(Path(__file__).resolve().parents[1] / "pixi.toml")
HOST_CCACHE_DIR = str(Path(__file__).resolve().parents[1] / "build/.build_cache")


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
                    args = [*source_args, "--output", "unchanged"]
                    self.assertEqual(
                        self.dispatch(args),
                        ["--pixi-manifest", HOST_MANIFEST, "--ccachedir", HOST_CCACHE_DIR, *args],
                    )
            for manifest_args in (["--pixi-manifest", "other.toml"], ["--pixi-manifest=other.toml"]):
                with self.subTest(args=manifest_args):
                    args = ["--engine-source", str(root), *manifest_args]
                    self.assertEqual(self.dispatch(args), ["--ccachedir", HOST_CCACHE_DIR, *args])
            for cache_args in (["--ccachedir", "other-cache"], ["--ccachedir=other-cache"]):
                with self.subTest(args=cache_args):
                    args = ["--engine-source", str(root), "--pixi-manifest=other.toml", *cache_args]
                    self.assertEqual(self.dispatch(args), args)


if __name__ == "__main__":
    unittest.main()
