"""Compatibility exports for the fork-owned output comparators."""

from engrave_testing.integration.test_public_output import (
    compare as compare,
)
from engrave_testing.integration.test_public_output import (
    describe_audio_file as describe_audio_file,
)
from engrave_testing.integration.test_public_output import (
    describe_render as describe_render,
)
from engrave_testing.integration.test_public_output import (
    probe_command as probe_command,
)
from engrave_testing.integration.test_public_output import (
    run_probe as run_probe,
)

if __name__ == "__main__":
    raise SystemExit("Partial comparison CLI removed; use tests/engine_gate.py for validated captures")
