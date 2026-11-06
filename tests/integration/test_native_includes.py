"""Regression checks for the native public-header boundary."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tempfile
import unittest
from pathlib import Path

from support.paths import REPO_ROOT

sys.path.insert(0, str(REPO_ROOT / "scripts"))
from check_native_includes import audit


class IncludeBoundaryTest(unittest.TestCase):
    def test_public_and_host_headers_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            native = root / "native"
            public = root / "public"
            native.mkdir()
            (public / "engrave").mkdir(parents=True)
            (public / "engrave/engine.h").touch()
            (native / "local.h").touch()
            source = native / "test.cpp"
            source.write_text(
                '#include "local.h"\n#include <engrave/engine.h>\n#include <vector>\n#include <asio/awaitable.hpp>\n'
            )
            self.assertEqual(audit(native, public), [])
            for include in (
                '"engrave/src/api/state.h"',
                "<engraving/dom/score.h>",
                "<muse/framework/global.h>",
                '"../internal.h"',
                "<engrave/../internal.h>",
                "<global/types.h>",
                "HEADER_MACRO",
            ):
                source.write_text(f"#include {include}\n")
                self.assertEqual(len(audit(native, public)), 1, include)


if __name__ == "__main__":
    unittest.main()
