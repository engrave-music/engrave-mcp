"""Controls for the fork gate wrapper's inherited dependency search path."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT


class GateWrapperTests(unittest.TestCase):
    def test_preserves_inherited_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tests").mkdir()
            (root / "tools").mkdir()
            dependencies = root / "dependencies"
            dependencies.mkdir()
            (dependencies / "gate_dependency_control.py").write_text("VALUE = 42\n")
            (root / "tests/gate.py").write_text(
                "import os, sys, gate_dependency_control\n"
                "assert gate_dependency_control.VALUE == 42\n"
                "assert os.environ['PYTHONPATH'].split(os.pathsep) == sys.argv[-1].split(os.pathsep)\n"
            )
            env = os.environ.copy()
            env["PYTHONPATH"] = str(dependencies)
            expected = os.pathsep.join(map(str, (root / "tests", root / "tools", dependencies)))
            subprocess.run(
                [sys.executable, str(REPO_ROOT / "tests/engine_gate.py"), "--engine-source", str(root), expected],
                env=env,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
