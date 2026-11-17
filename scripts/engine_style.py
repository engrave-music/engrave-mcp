"""Default engine checkout path (`ENGINE_ROOT`) and fork-owned file selection helper."""

import sys
from pathlib import Path

ENGINE_ROOT = Path(__file__).resolve().parents[1] / "vendor/engrave"
sys.path.insert(0, str(ENGINE_ROOT / "tools"))
from style_files import engine_files as engine_files  # noqa: E402
