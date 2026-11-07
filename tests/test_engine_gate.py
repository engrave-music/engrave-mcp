"""The host wrapper dispatches both argparse spellings to the requested fork."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class EngineGateDispatch(unittest.TestCase):
    def test_engine_source_forms(self):
        with tempfile.TemporaryDirectory(prefix="fork with spaces ") as directory:
            root = Path(directory)
            (root / "tests").mkdir()
            (root / "tests/gate.py").write_text("import json, sys; print(json.dumps(sys.argv[1:]))", encoding="utf8")
            for source_args in (["--engine-source", str(root)], ["--engine-source=" + str(root)]):
                with self.subTest(args=source_args):
                    args = [*source_args, "--output", "unchanged"]
                    result = subprocess.run(
                        [sys.executable, str(Path(__file__).with_name("engine_gate.py")), *args],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout), args)


if __name__ == "__main__":
    unittest.main()
