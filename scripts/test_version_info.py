"""Platform/version info: MSCX, MusicXML, metadata and the $v header on an edited score."""
from __future__ import annotations
import argparse
import asyncio
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
SVG = "{http://www.w3.org/2000/svg}"
XLINK = "{http://www.w3.org/1999/xlink}href"


def score_document():
    return {"title": "Version Test", "tempo": 120, "parts": [
        {"name": "Piano", "midiProgram": 1,
         "measures": [{"notes": [{"pitches": [60], "ticks": 1920}]}]}]}


def expected_platform():
    if sys.platform.startswith("win"):
        return "Microsoft Windows"
    if sys.platform.startswith("linux"):
        return "Linux"
    raise AssertionError(f"unsupported test platform: {sys.platform}")


def glyph_runs(path: Path):
    """Return (y, glyph paths) runs. Cairo's SVG surface has no <text>; each
    character is a <use> reference to a glyph path, so identity is compared by
    the referenced path data. Runs are split where the horizontal gap between
    characters is too large to be a word (separate header/footer fields)."""
    root = ET.parse(path).getroot()
    glyphs = {}
    for element in root.iter(f"{SVG}g"):
        name = element.get("id", "")
        if name.startswith("glyph-"):
            glyphs[name] = tuple(p.get("d", "") for p in element.iter(f"{SVG}path"))
    rows = {}
    for use in root.iter(f"{SVG}use"):
        href = (use.get(XLINK) or "").lstrip("#")
        glyph = glyphs.get(href)
        if not glyph:
            continue
        rows.setdefault(round(float(use.get("y", "0")), 3), []).append(
            (float(use.get("x", "0")), glyph))
    runs = []
    for y, items in rows.items():
        items.sort()
        current = [items[0]]
        for previous, item in zip(items, items[1:]):
            if item[0] - previous[0] > 24:
                runs.append((y, tuple(glyph for _, glyph in current)))
                current = []
            current.append(item)
        runs.append((y, tuple(glyph for _, glyph in current)))
    return runs


def version_runs(path: Path):
    """Return header and footer 5-glyph runs of a version page ("4.5.0")."""
    runs = [run for run in glyph_runs(path) if len(run[1]) == 5]
    assert runs, "SVG has no five-glyph header/footer runs"
    header_y = min(y for y, _ in runs)
    footer_y = max(y for y, _ in runs)
    assert footer_y > header_y + 100, (header_y, footer_y)
    header = [run for y, run in runs if abs(y - header_y) < 0.01]
    footer = sorted((run for y, run in runs if abs(y - footer_y) < 0.01),
                    key=lambda run: run[0][0] if run else 0)
    assert len(header) == 1, header
    assert len(footer) == 2, footer
    return header[0], footer[0], footer[1]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ['--resources', str(args.resources.resolve())]
    params = StdioServerParameters(command=str(args.binary.resolve()), args=child_args,
                                   env=dict(os.environ), cwd=str(out))
    report = {"status": "running"}

    async with stdio_client(params, errlog=open(out / "stderr.log", "w", encoding="utf-8")) as (read, write):
        async with ClientSession(read, write) as client:
            await client.initialize()

            async def call(name, values, error=False):
                response = await client.call_tool(name, values)
                assert bool(response.is_error) == error, response
                if error:
                    return None
                return response.structured_content if response.structured_content is not None \
                    else json.loads(response.content[0].text)

            created = await call("score_create", {"score": score_document()})
            document = {"documentId": created["documentId"]}

            mscx_path = out / "created.mscx"
            await call("score_export", {**document, "format": "mscx", "path": str(mscx_path)})
            mscx = ET.parse(mscx_path).getroot()
            version = mscx.findtext("programVersion")
            revision = mscx.findtext("programRevision") or ""
            assert version == "4.7.6", version
            assert re.fullmatch(r"[0-9a-fA-F]{7}", revision), revision
            platform = next((tag.text for tag in mscx.iter("metaTag")
                             if tag.get("name") == "platform"), None)
            assert platform == expected_platform(), platform
            report["mscx"] = {"programVersion": version, "programRevision": revision,
                              "platform": platform}

            metadata_path = out / "metadata.json"
            await call("score_export", {**document, "format": "metadata", "path": str(metadata_path)})
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            assert metadata["mscoreVersion"] == "4.7.6", metadata.get("mscoreVersion")
            report["metadata"] = {"mscoreVersion": metadata["mscoreVersion"]}

            musicxml_path = out / "created.musicxml"
            await call("score_export", {**document, "format": "musicxml", "path": str(musicxml_path)})
            musicxml = ET.parse(musicxml_path).getroot()
            software = musicxml.findtext("identification/encoding/software")
            assert software == "MuseScore Studio 4.7.6", software
            report["musicxml"] = {"software": software}
            await call("score_close", document)

            # The fixture records programVersion 4.5.0, so the unedited $v header
            # shows the file's version; editing marks the score dirty and the
            # header must switch to the running application version.
            fixture = await call("score_open", {"path": str(ROOT / "fixtures/version/header.mscx")})
            fixture_document = {"documentId": fixture["documentId"]}
            before_svg = out / "header-before.svg"
            await call("score_export", {**fixture_document, "format": "svg", "path": str(before_svg),
                                        "page": 0})
            header_before, footer_before_low, footer_before_high = version_runs(before_svg)
            assert header_before == footer_before_low, "unedited $v must show the file version"
            assert header_before != footer_before_high, "unedited $v already shows the app version"

            region = await call("score_query_region", {**fixture_document, "partIndex": 0,
                                                       "firstMeasure": 0, "measureCount": 1})
            notes = region["notes"]
            assert notes, "fixture has no note to edit"
            await call("score_set_pitches", {**fixture_document, "baseRevision": region["revision"],
                                            "operationId": "version-header-edit-1",
                                            "changes": [{"noteId": notes[0]["id"],
                                                         "pitch": notes[0]["pitch"] + 2}]})
            after_svg = out / "header-after.svg"
            await call("score_export", {**fixture_document, "format": "svg", "path": str(after_svg),
                                        "page": 0})
            header_after, footer_after_low, footer_after_high = version_runs(after_svg)
            assert header_after != header_before, "edited $v did not change"
            assert header_after == footer_after_high, "edited $v does not match the 4.7.6 reference"
            assert header_after != footer_after_low, "edited $v still matches the 4.5.0 reference"
            report["svg_header"] = {"encoding": "cairo-glyph-paths",
                                    "unedited_matches": "file programVersion (4.5.0)",
                                    "edited_matches": "MUSE_APP_VERSION (4.7.6)"}
            await call("score_close", fixture_document)

    assert "ERROR:" not in (out / "stderr.log").read_text(encoding="utf-8"), "Engine errors in stderr.log"
    report["status"] = "passed"
    (out / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    asyncio.run(asyncio.wait_for(run(parser.parse_args()), timeout=180))
