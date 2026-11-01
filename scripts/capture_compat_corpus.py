"""Export the A0 fixtures and edit scenarios as a new forward-compat pin.

Use the pinned host build and record the commits it was built from. The output
directory must not already exist: previously captured bytes are never replaced.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path

from golden import (EDIT_SCENARIOS, FIXTURE_DIR, apply_edit_scenario, export, fixture_files,
                    open_fixture, required)
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from test_compat_corpus import digest, semantic_dump, write_json


def features(source: Path, edit: bool, score: Path, semantic: dict) -> list[str]:
    found = {"notes and rests", "tempo map"}
    if edit:
        found.add("edited score")
    if source.suffix == ".gp":
        found.add("Guitar Pro import")
    if source.suffix == ".mid":
        found.add("MIDI import")
    with zipfile.ZipFile(score) as archive:
        xml = b"\n".join(archive.read(name) for name in archive.namelist()
                         if name.lower().endswith(".mscx"))
    if re.search(rb"<(?:eid|linkedTo|startElement|endElement|NoteEID|mmRestLast|"
                 rb"startMeasure|endMeasure|sharedPart)\b", xml):
        found.add("EID references in writer bytes")
    if b"<mmRestLast>" in xml:
        found.add("multi-measure-rest references in writer bytes")

    def visit(value):
        if isinstance(value, list):
            for item in value:
                visit(item)
        elif isinstance(value, dict):
            if value.get("excerpts"):
                found.add("linked parts and excerpts")
            if value.get("spanners"):
                found.add("spanners")
            if value.get("lyrics"):
                found.add("lyrics")
            if "tieTo" in value:
                found.add("ties")
            if value.get("startRepeat") or value.get("endRepeat"):
                found.add("repeats")
            if value.get("group") == "tablature":
                found.add("tablature")
            if "instrumentId" in value and value.get("show") is False:
                found.add("hidden instruments")
            for item in value.values():
                visit(item)

    visit(semantic)
    return sorted(found)


async def capture_one(session, source: Path, name: str, scenario: dict | None,
                      target: Path, binary: Path, resources: Path, provenance: dict) -> dict:
    opened = await open_fixture(session, source)
    identity = {"document": opened["document"]}
    try:
        if scenario:
            with tempfile.TemporaryDirectory(prefix="compat-edit-") as raw:
                await apply_edit_scenario(session, identity, name, scenario, Path(raw))
        score = target / f"{name}.mscz"
        error = await export(session, identity, "mscz", score)
        if error:
            raise RuntimeError(f"{name}: score_export: {error}")
        dump = semantic_dump(binary, resources, score)
        expected = f"{name}.semantic.json"
        write_json(target / expected, dump)
        entry = {"file": score.name, "semantic": expected, "sha256": digest(score),
                 "features": features(source, bool(scenario), score, dump),
                 "source": source.relative_to(Path(__file__).resolve().parents[1]).as_posix(),
                 **provenance}
        print(f"captured {score.name}", flush=True)
        return entry
    finally:
        await required(session, "score_close", identity)


async def capture(args) -> None:
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    resources = args.resources.resolve()
    soundfont = args.soundfont.resolve()
    target = args.output.resolve()
    dump_binary = binary.parent / ("semantic_dump.exe" if sys.platform.startswith("win") else "semantic_dump")
    if not dump_binary.is_file():
        raise RuntimeError(f"semantic_dump not found next to {binary}")
    provenance = {key: getattr(args, key) for key in
                  ("writer_sha", "upstream_sha", "framework_sha", "superproject_commit")}
    entries = []
    params = StdioServerParameters(command=str(binary),
                                   args=["--resources", str(resources), "--soundfont", str(soundfont)])
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as log:
        try:
            async with stdio_client(params, errlog=log) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    for source in fixture_files():
                        entries.append(await capture_one(session, source, source.name, None, target,
                                                         dump_binary, resources, provenance))
                    for name, scenario in EDIT_SCENARIOS.items():
                        entries.append(await capture_one(session, FIXTURE_DIR / scenario["source"], name,
                                                         scenario, target, dump_binary, resources, provenance))
        except Exception:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
    write_json(target / "manifest.json", {"schema": 1, "entries": entries})
    print(f"captured {len(entries)} corpus entries into {target}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resources", "soundfont", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    for name in ("writer-sha", "upstream-sha", "framework-sha", "superproject-commit"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    for key in ("writer_sha", "upstream_sha", "framework_sha", "superproject_commit"):
        if not re.fullmatch(r"[0-9a-f]{7,40}", getattr(args, key)):
            parser.error(f"{key} must be a lowercase hexadecimal commit SHA")
    for name in ("binary", "resources", "soundfont"):
        if not getattr(args, name).exists():
            parser.error(f"{name} does not exist: {getattr(args, name)}")
    asyncio.run(asyncio.wait_for(capture(args), timeout=1800))
    return 0


if __name__ == "__main__":
    sys.exit(main())
