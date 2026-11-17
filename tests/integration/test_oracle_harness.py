"""Verify inputs authenticated by the MCP golden capture."""

import asyncio
import sys
import unittest
from pathlib import Path
from unittest import mock

from engrave_mcp_testing import golden


class TestCapturedInputProvenance(unittest.TestCase):
    def test_capture_uses_the_authenticated_converter(self):
        engine = golden.REPO_ROOT / "vendor/engrave/tests/engrave_testing/composition_fixture.py"
        self.assertEqual(Path(sys.modules["engrave_testing.composition_fixture"].__file__).resolve(), engine.resolve())

    def test_capture_refuses_unreviewed_converter_output(self):

        source = golden.FIXTURE_DIR / "created.json"
        sent = []

        async def fake_required(session, name, arguments):
            sent.append(arguments)
            return {}

        original = golden.composition_musicxml
        with mock.patch.object(golden, "required", fake_required):
            asyncio.run(golden.open_fixture(None, source))
            self.assertEqual(len(sent), 1)
            with mock.patch.object(golden, "composition_musicxml", lambda score: original(score) + "<!-- x -->"):
                with self.assertRaisesRegex(RuntimeError, "unreviewed MusicXML"):
                    asyncio.run(golden.open_fixture(None, source))
        self.assertEqual(len(sent), 1)


if __name__ == "__main__":
    unittest.main()
