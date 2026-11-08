"""Negative controls for imports in each repository's test roots."""

import tempfile
import unittest
from pathlib import Path

from engrave_mcp_testing.layout import audit_test_layout


class LayoutTests(unittest.TestCase):
    def test_rejects_test_imports_in_every_test_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for repo in (root, root / "vendor/engrave"):
                (repo / "tests").mkdir(parents=True)
                for folder in ("tests", "engrave/tests", "src/example/tests"):
                    with self.subTest(repo=repo, folder=folder):
                        path = repo / folder / "layout_control.py"
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("from tests.integration.test_excerpts import main\n")
                        try:
                            errors = audit_test_layout(root)
                            self.assertEqual(len(errors), 1, errors)
                            self.assertIn(str(path), errors[0])
                        finally:
                            path.unlink()
            self.assertEqual(audit_test_layout(root), [])


if __name__ == "__main__":
    unittest.main()
