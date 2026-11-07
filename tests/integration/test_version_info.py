"""Platform/version info: MSCX, MusicXML, metadata and the $v header on an edited score."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SVG = "{http://www.w3.org/2000/svg}"
XLINK = "{http://www.w3.org/1999/xlink}href"


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
            m = re.search(r"set\(" + name + r'\s+"([^"]*)"\)', text)
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
        capture_output=True,
        text=True,
        check=True,
    )
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
        rows.setdefault(round(float(use.get("y", "0")), 3), []).append((float(use.get("x", "0")), glyph))
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
    footer = sorted((run for run in runs if abs(run[0] - footer_y) < 0.01), key=lambda run: run[1])
    assert len(header) == 1, header
    assert len(footer) == 2, footer
    return header[0][2], footer[0][2], footer[1][2]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ["--resources", str(args.resources.resolve())]
    params = StdioServerParameters(
        command=str(args.binary.resolve()),
        args=child_args,
        env=dict(os.environ),
        cwd=str(out),
    )
    report = {"status": "running"}

    async with (
        stdio_client(params, errlog=open(out / "stderr.log", "w", encoding="utf-8")) as (read, write),
        ClientSession(read, write) as client,
    ):
        await client.initialize()

        async def call(name, values, error=False):
            response = await client.call_tool(name, values)
            assert bool(response.is_error) == error, response
            if error:
                return None
            return (
                response.structured_content
                if response.structured_content is not None
                else json.loads(response.content[0].text)
            )

        async def export(document, fmt, path, action="run"):
            render = fmt == "svg"
            if render:
                path.mkdir(exist_ok=True)
            return await call(
                "score_export",
                {
                    "document": document,
                    "kind": "render" if render else "score",
                    "options": {"format": fmt},
                    "destination": {
                        "kind": "directory" if render else "file",
                        "path": str(path),
                    },
                },
            )

        async def outline_version(document):
            return (await call("score_outline", {"document": document}))["outline"]["mscore_version"]

        created = await call("score_create", {"template_id": "Treble_Clef", "title": "Version Test"})
        document = created["document"]

        mscx_path = out / "created.mscx"
        await export(document, "mscx", mscx_path)
        mscx = ET.parse(mscx_path).getroot()
        version = mscx.findtext("programVersion")
        revision = mscx.findtext("programRevision") or ""
        engine_root = args.upstream.resolve()
        expected_version = read_engine_version(engine_root)
        engine_rev = engine_revision(engine_root)
        assert version == expected_version, version
        assert re.fullmatch(r"[0-9a-fA-F]{7}", revision), revision
        assert revision == engine_rev, (revision, engine_rev)
        platform = next(
            (tag.text for tag in mscx.iter("metaTag") if tag.get("name") == "platform"),
            None,
        )
        assert platform == expected_platform(), platform
        report["mscx"] = {
            "programVersion": version,
            "programRevision": revision,
            "engineRevision": engine_rev,
            "platform": platform,
        }

        # Like MuseScore Studio, only writing a file stamps the version; a created
        # score has none and the MSCX export above must not change the document.
        assert await outline_version(document) == ""
        report["metadata"] = {"mscoreVersion": await outline_version(document)}

        musicxml_path = out / "created.musicxml"
        await export(document, "musicxml", musicxml_path)
        musicxml = ET.parse(musicxml_path).getroot()
        software = musicxml.findtext("identification/encoding/software")
        assert software == f"MuseScore Studio {expected_version}", software
        report["musicxml"] = {"software": software}
        await call("score_close", {"document": document})

        # The fixture records programVersion 4.5.0. The owner retains that
        # source version, but exports render a newly written MSCZ snapshot.
        fixture_mscx_path = REPO_ROOT / "vendor/engrave/tests/data/inputs/version/header.mscx"
        if expected_version != "4.7.6":
            fixture_content = fixture_mscx_path.read_text(encoding="utf-8")
            fixture_content = fixture_content.replace(
                "<oddFooterC>4.7.6</oddFooterC>",
                f"<oddFooterC>{expected_version}</oddFooterC>",
            )
            fixture_content = fixture_content.replace(
                "<evenFooterC>4.7.6</evenFooterC>",
                f"<evenFooterC>{expected_version}</evenFooterC>",
            )
            patched_fixture = out / "header.mscx"
            patched_fixture.write_text(fixture_content, encoding="utf-8")
            fixture_mscx_path = patched_fixture

        fixture = await call("score_open", {"path": str(fixture_mscx_path)})
        fixture_document = fixture["document"]

        # Both repeated synchronous exports must leave the owner alone.
        metadata_before = await outline_version(fixture_document)
        assert metadata_before == "4.5.0", metadata_before

        fixture_mscz = out / "header.mscz"
        await export(fixture_document, "mscz", fixture_mscz)

        metadata_after = await outline_version(fixture_document)
        assert metadata_after == "4.5.0", metadata_after

        # Metadata-file export was removed from the tool contract. The repeated MSCZ export
        # still must not alter the owner document's source version.
        await export(fixture_document, "mscz", out / "header-background.mscz")
        metadata_background = await outline_version(fixture_document)
        assert metadata_background == "4.5.0", metadata_background
        report["fixture_metadata"] = {
            "before_mscz": metadata_before,
            "after_mscz": metadata_after,
            "background": metadata_background,
        }

        # Each archive is stamped by the writer. Reopen both archives and
        # render them: their outlines and $v headers describe the written
        # snapshot, not the owner document's source provenance.
        snapshot_versions = {}
        for label, path in (
            ("run", fixture_mscz),
            ("start", out / "header-background.mscz"),
        ):
            with zipfile.ZipFile(path) as archive:
                written_version = ET.fromstring(archive.read("score.mscx")).findtext("programVersion")
            assert written_version == expected_version, (label, written_version)
            reopened = (await call("score_open", {"path": str(path)}))["document"]
            reopened_version = await outline_version(reopened)
            assert reopened_version == expected_version, (label, reopened_version)
            snapshot_dir = out / f"header-snapshot-{label}"
            await export(reopened, "svg", snapshot_dir)
            header, footer_low, footer_high = version_runs(snapshot_dir / "page-0001.svg")
            assert header == footer_high, f"{label} snapshot render must show the written version"
            assert header != footer_low, f"{label} snapshot render still shows the source version"
            snapshot_versions[label] = {
                "archive_programVersion": written_version,
                "reopened_mscoreVersion": reopened_version,
                "rendered_header": expected_version,
            }
            await call("score_close", {"document": reopened})
        report["snapshots"] = snapshot_versions
        assert await outline_version(fixture_document) == "4.5.0"

        before_dir = out / "header-before"
        await export(fixture_document, "svg", before_dir)
        before_svg = before_dir / "page-0001.svg"
        header_before, footer_before_low, footer_before_high = version_runs(before_svg)
        assert header_before == footer_before_low, "live render must show the source version"
        assert header_before != footer_before_high, "live render must preserve owner provenance"
        assert await outline_version(fixture_document) == "4.5.0"

        region = await call(
            "score_query",
            {
                "document": fixture_document,
                "mode": "elements",
                "query": {
                    "types": ["Note"],
                    "first_measure": 0,
                    "measure_count": 1,
                    "props": ["pitch"],
                },
            },
        )
        notes = region["elements"]
        assert notes, "fixture has no note to edit"
        await call(
            "score_edit",
            {
                "document": fixture_document,
                "expected_revision": region["revision"],
                "ops": [
                    {
                        "op": "set_pitch",
                        "note": {"kind": "id", "id": notes[0]["id"]},
                        "pitch": int(notes[0]["props"][0]["value"]["value"]) + 2,
                    }
                ],
            },
        )
        after_dir = out / "header-after"
        await export(fixture_document, "svg", after_dir)
        after_svg = after_dir / "page-0001.svg"
        header_after, footer_after_low, footer_after_high = version_runs(after_svg)
        assert header_after == header_before, "editing changed the written snapshot version header"
        assert header_after == footer_after_low, "edited $v does not match the source version"
        assert header_after != footer_after_high, "editing changed owner provenance"
        report["svg_header"] = {
            "encoding": "cairo-glyph-paths",
            "unedited_live": "4.5.0",
            "edited_live": "4.5.0",
        }

        # Excerpt generation must also preserve the owner source version.
        outline = (await call("score_outline", {"document": fixture_document}))["outline"]
        await call(
            "score_edit",
            {
                "document": fixture_document,
                "ops": [
                    {
                        "op": "generate_excerpts",
                        "parts": [{"kind": "id", "id": part["id"]} for part in outline["parts"]],
                    }
                ],
            },
        )
        metadata_excerpts = await outline_version(fixture_document)
        assert metadata_excerpts == "4.5.0", metadata_excerpts
        report["fixture_metadata"]["after_excerpts"] = metadata_excerpts
        await call("score_close", {"document": fixture_document})

    assert "ERROR:" not in (out / "stderr.log").read_text(encoding="utf-8"), "Engine errors in stderr.log"
    report["status"] = "passed"
    (out / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--upstream",
        "--engine-source",
        dest="upstream",
        type=Path,
        default=REPO_ROOT / "vendor/engrave",
        help="Path to engine source root",
    )
    asyncio.run(asyncio.wait_for(run(parser.parse_args()), timeout=180))
