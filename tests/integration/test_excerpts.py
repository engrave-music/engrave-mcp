"""Compatibility entry point for the engine-owned validator."""

import runpy
from pathlib import Path

_path = Path(__file__).resolve().parents[2] / "vendor/engrave/tests/integration/test_excerpts.py"
globals().update(runpy.run_path(str(_path), run_name="__main__" if __name__ == "__main__" else "engrave_validator"))
