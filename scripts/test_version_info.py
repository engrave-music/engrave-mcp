"""Platform/version info: MSCX, MusicXML, metadata and the $v header on an edited score."""
from __future__ import annotations
import argparse
import asyncio
import json
import os
import re
import subprocess
import sys
import time
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


def read_engine_version(source_root: Path) -> str:
    version_cmake = source_root / "version.cmake"
    if version_cmake.is_file():
        text = version_cmake.read_text(encoding="utf-8")
        def value(name: str) -> str:
            m = re.search(r'set\(' + name + r'\s+"([^"]*)"\)', text)
            return m.group(1) if m else "0"
        major = value("MUSE_APP_VERSION_MAJOR")
        minor = value("MUSE_APP_VERSION_MINOR")
        patch = value("MUSE_APP_VERSION_PATCH")
        return f"{major}.{minor}.{patch}"
    return "4.7.6"


def engine_revision(source_root: Path):
    """The 7-character engine commit the build reads at configure time."""
    result = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True)
    return result.stdout.strip()[:7]


def glyph_runs(path: Path):
    """Return (y, x, glyph paths) runs. Cairo's SVG surface has no <text>; each
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
                runs.append((y, current[0][0], tuple(glyph for _, glyph in current)))
                current = []
            current.append(item)
        runs.append((y, current[0][0], tuple(glyph for _, glyph in current)))
    return runs


def version_runs(path: Path):
    """Return header and footer 5-glyph runs of a version page ("4.5.0")."""
    runs = [run for run in glyph_runs(path) if len(run[2]) == 5]
    assert runs, "SVG has no five-glyph header/footer runs"
    header_y = min(run[0] for run in runs)
    footer_y = max(run[0] for run in runs)
    assert footer_y > header_y + 100, (header_y, footer_y)
    header = [run for run in runs if abs(run[0] - header_y) < 0.01]
    # Sort the footer fields by their actual x position; glyph paths are not
    # ordered like the page and repeat across fields.
    footer = sorted((run for run in runs if abs(run[0] - footer_y) < 0.01),
                    key=lambda run: run[1])
    assert len(header) == 1, header
    assert len(footer) == 2, footer
    return header[0][2], footer[0][2], footer[1][2]


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
            engine_root = args.upstream.resolve()
            expected_version = read_engine_version(engine_root)
            engine_rev = engine_revision(engine_root)
            assert version == expected_version, version
            assert re.fullmatch(r"[0-9a-fA-F]{7}", revision), revision
            assert revision == engine_rev, (revision, engine_rev)
            platform = next((tag.text for tag in mscx.iter("metaTag")
                             if tag.get("name") == "platform"), None)
            assert platform == expected_platform(), platform
            report["mscx"] = {"programVersion": version, "programRevision": revision,
                              "engineRevision": engine_rev, "platform": platform}

            metadata_path = out / "metadata.json"
            await call("score_export", {**document, "format": "metadata", "path": str(metadata_path)})
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            # Like MuseScore Studio, only writing a file stamps the version; a created
            # score has none and the MSCX export above must not change the document.
            assert metadata["mscoreVersion"] == "", metadata.get("mscoreVersion")
            report["metadata"] = {"mscoreVersion": metadata["mscoreVersion"]}

            musicxml_path = out / "created.musicxml"
            await call("score_export", {**document, "format": "musicxml", "path": str(musicxml_path)})
            musicxml = ET.parse(musicxml_path).getroot()
            software = musicxml.findtext("identification/encoding/software")
            assert software == f"MuseScore Studio {expected_version}", software
            report["musicxml"] = {"software": software}
            await call("score_close", document)

            # The fixture records programVersion 4.5.0, so the unedited $v header
            # shows the file's version; editing marks the score dirty and the
            # header must switch to the running application version.
            fixture_mscx_path = ROOT / "fixtures/version/header.mscx"
            if expected_version != "4.7.6":
                fixture_content = fixture_mscx_path.read_text(encoding="utf-8")
                fixture_content = fixture_content.replace("<oddFooterC>4.7.6</oddFooterC>", f"<oddFooterC>{expected_version}</oddFooterC>")
                fixture_content = fixture_content.replace("<evenFooterC>4.7.6</evenFooterC>", f"<evenFooterC>{expected_version}</evenFooterC>")
                patched_fixture = out / "header.mscx"
                patched_fixture.write_text(fixture_content, encoding="utf-8")
                fixture_mscx_path = patched_fixture

            fixture = await call("score_open", {"path": str(fixture_mscx_path)})
            fixture_document = {"documentId": fixture["documentId"]}

            # Exporting MSCZ stamps the running program's version into the open
            # score in Writer::writeScore; the export must stay read-only.
            fixture_metadata_before = out / "header-metadata-before.json"
            await call("score_export", {**fixture_document, "format": "metadata",
                                        "path": str(fixture_metadata_before)})
            metadata_before = json.loads(fixture_metadata_before.read_text(encoding="utf-8"))
            assert metadata_before["mscoreVersion"] == "4.5.0", metadata_before.get("mscoreVersion")

            fixture_mscz = out / "header.mscz"
            await call("score_export", {**fixture_document, "format": "mscz", "path": str(fixture_mscz)})

            fixture_metadata_after = out / "header-metadata-after.json"
            await call("score_export", {**fixture_document, "format": "metadata",
                                        "path": str(fixture_metadata_after)})
            metadata_after = json.loads(fixture_metadata_after.read_text(encoding="utf-8"))
            assert metadata_after["mscoreVersion"] == "4.5.0", metadata_after.get("mscoreVersion")

            # The background worker reloads a snapshot serialized by that same
            # writer; it must apply the source document's version too.
            fixture_metadata_background = out / "header-metadata-background.json"
            job = await call("score_export_start", {**fixture_document, "format": "metadata",
                                                    "path": str(fixture_metadata_background)})
            deadline = time.monotonic() + 60
            while job["state"] not in ("completed", "failed", "cancelled"):
                assert time.monotonic() < deadline, f"background metadata export stalled: {job}"
                await asyncio.sleep(0.02)
                job = await call("score_job_get", {"jobId": job["jobId"]})
            assert job["state"] == "completed", job
            metadata_background = json.loads(fixture_metadata_background.read_text(encoding="utf-8"))
            assert metadata_background["mscoreVersion"] == "4.5.0", \
                metadata_background.get("mscoreVersion")
            report["fixture_metadata"] = {"before_mscz": metadata_before["mscoreVersion"],
                                          "after_mscz": metadata_after["mscoreVersion"],
                                          "background": metadata_background["mscoreVersion"]}

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
                                    "edited_matches": f"MUSE_APP_VERSION ({expected_version})"}

            # Excerpt generation serializes and reloads the document as well; the
            # reload must keep the source file's stamps, not the writer's.
            await call("score_generate_excerpts", fixture_document)
            fixture_metadata_excerpts = out / "header-metadata-excerpts.json"
            await call("score_export", {**fixture_document, "format": "metadata",
                                        "path": str(fixture_metadata_excerpts)})
            metadata_excerpts = json.loads(fixture_metadata_excerpts.read_text(encoding="utf-8"))
            assert metadata_excerpts["mscoreVersion"] == "4.5.0", \
                metadata_excerpts.get("mscoreVersion")
            report["fixture_metadata"]["after_excerpts"] = metadata_excerpts["mscoreVersion"]
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
    parser.add_argument('--upstream', '--engine-source', dest='upstream', type=Path,
                        default=ROOT / 'vendor/musescore',
                        help='Path to engine source root')
    asyncio.run(asyncio.wait_for(run(parser.parse_args()), timeout=180))
