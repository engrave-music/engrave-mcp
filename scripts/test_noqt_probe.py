"""Check the current engine's Qt-free import, layout and export probe."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    xml = out / "score.musicxml"
    proc = subprocess.run(
        [
            str(args.binary.resolve()),
            str(args.assets.resolve()),
            str(args.input.resolve()),
            str(xml),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    (out / "stdout.log").write_text(proc.stdout, encoding="utf-8")
    (out / "stderr.log").write_text(proc.stderr, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    match = re.search(r"parts=(\d+) pages=(\d+) bytes=(\d+)", proc.stdout)
    assert match and all(int(n) > 0 for n in match.groups()), proc.stdout
    result = ET.parse(xml).getroot()
    assert result.findall("./part") and result.findall(".//note/pitch")
    midi = Path(str(xml) + ".mid")
    assert midi.read_bytes().startswith(b"MThd")
    print(
        json.dumps(
            {
                "status": "passed",
                "parts": int(match[1]),
                "pages": int(match[2]),
                "musicxml_bytes": xml.stat().st_size,
                "midi_bytes": midi.stat().st_size,
            }
        )
    )


if __name__ == "__main__":
    main()
