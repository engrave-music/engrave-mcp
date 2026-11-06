"""Validate layout coordinates and repeat/tempo playback mappings through MCP."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import base64
import json
import math
import os
import xml.etree.ElementTree as ET
from pathlib import Path

from integration.test_export_options import REPEAT_FIXTURE_MUSICXML, read_png_dimensions
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from support.paths import REPO_ROOT


def timed_fixture():
    root = ET.fromstring(REPEAT_FIXTURE_MUSICXML)
    measures = root.findall("./part/measure")
    for index, measure in enumerate(measures):
        measure.remove(measure.find("note"))
        right_bar = measure.find("barline[@location='right']")
        if right_bar is not None:
            measure.remove(right_bar)
        direction = ET.SubElement(measure, "direction")
        ET.SubElement(direction, "sound", tempo=str(120 if index == 0 else 60))
        for pitch in ["C", None, "E", "G"] if index == 0 else ["E", "F"]:
            note = ET.SubElement(measure, "note")
            if pitch is None:
                ET.SubElement(note, "rest")
            else:
                p = ET.SubElement(note, "pitch")
                ET.SubElement(p, "step").text = pitch
                ET.SubElement(p, "octave").text = "4"
            ET.SubElement(note, "duration").text = "1" if index == 0 else "2"
            ET.SubElement(note, "type").text = "quarter" if index == 0 else "half"
        if right_bar is not None:
            measure.append(right_bar)
    return ET.tostring(root, encoding="unicode")


def mmrest_fixture():
    """Four empty measures collapsed into one multi-measure rest, then a note."""
    return """<score-partwise version="4.0">
<part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list>
<part id="P1">
<measure number="1"><attributes><divisions>1</divisions>
<time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef>
<measure-style><multiple-rest>4</multiple-rest></measure-style>
</attributes><note><rest/><duration>4</duration><voice>1</voice></note></measure>
<measure number="2"><note><rest/><duration>4</duration><voice>1</voice></note></measure>
<measure number="3"><note><rest/><duration>4</duration><voice>1</voice></note></measure>
<measure number="4"><note><rest/><duration>4</duration><voice>1</voice></note></measure>
<measure number="5"><note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note></measure>
</part></score-partwise>"""


def multipage_fixture():
    root = ET.Element("score-partwise", version="4.0")
    listing = ET.SubElement(root, "part-list")
    for index, name in enumerate(("Violin", "Cello")):
        part = ET.SubElement(listing, "score-part", id=f"P{index + 1}")
        ET.SubElement(part, "part-name").text = name
    for index in range(2):
        part = ET.SubElement(root, "part", id=f"P{index + 1}")
        for number in range(1, 61):
            measure = ET.SubElement(part, "measure", number=str(number))
            if number == 1:
                attrs = ET.SubElement(measure, "attributes")
                ET.SubElement(attrs, "divisions").text = "1"
                time = ET.SubElement(attrs, "time")
                ET.SubElement(time, "beats").text = "4"
                ET.SubElement(time, "beat-type").text = "4"
                clef = ET.SubElement(attrs, "clef")
                ET.SubElement(clef, "sign").text = "G" if index == 0 else "F"
                ET.SubElement(clef, "line").text = "2" if index == 0 else "4"
            for step in ("C", "E", "G", "C"):
                note = ET.SubElement(measure, "note")
                p = ET.SubElement(note, "pitch")
                ET.SubElement(p, "step").text = step
                ET.SubElement(p, "octave").text = "4" if index == 0 else "3"
                ET.SubElement(note, "duration").text = "1"
                ET.SubElement(note, "type").text = "quarter"
    return ET.tostring(root, encoding="unicode")


def check_geometry(data, pages):
    assert set(data) == {"page_size", "elements", "events"}
    size = data["page_size"]
    assert size["width"] > 0 and size["height"] > 0
    elements = data["elements"]
    assert [x["id"] for x in elements] == list(range(len(elements)))
    for item in elements:
        assert set(item) == {"id", "x", "y", "sx", "sy", "page"}
        assert 0 <= item["page"] < pages
        assert all(math.isfinite(item[k]) for k in ("x", "y", "sx", "sy"))
        assert 0 <= item["x"] < size["width"] and 0 <= item["y"] < size["height"]
        assert item["sx"] > 0 and item["sy"] > 0
    assert all(0 <= event["elid"] < len(elements) for event in data["events"])
    times = [event["position"] for event in data["events"]]
    assert times == sorted(times) and times[0] == 0


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ["--resources", str(args.resources.resolve())] if args.resources else []
    child_env = dict(os.environ)
    if os.environ.get("ENGRAVE_MCP_CLEAN_ENV"):
        child_env = (
            {
                "SystemRoot": os.environ["SystemRoot"],
                "PATH": str(Path(os.environ["SystemRoot"]) / "System32"),
                "TEMP": os.environ["TEMP"],
                "TMP": os.environ["TMP"],
            }
            if os.name == "nt"
            else {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
        )
    params = StdioServerParameters(command=str(args.binary.resolve()), args=child_args, env=child_env, cwd=str(out))
    evidence = {"binary": str(args.binary.resolve())}
    with (out / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()

                async def call(name, arguments, error=False):
                    r = await client.call_tool(name, arguments)
                    assert bool(r.is_error) == error, r
                    if error:
                        return None
                    return r.structured_content if r.structured_content is not None else json.loads(r.content[0].text)

                async def export(doc, fmt, name, **kw):
                    path = out / name
                    if fmt in ("measurePositions", "segmentPositions"):
                        kind, options = (
                            "positions",
                            {"position_kind": "measure" if fmt == "measurePositions" else "segment"},
                        )
                    elif fmt == "png":
                        kind, options = "render", {"format": fmt}
                    else:
                        kind, options = "score", {"format": fmt}
                    options.update(kw)
                    directory = fmt == "png"
                    if directory:
                        path.mkdir(exist_ok=True)
                    r = await call(
                        "score_export",
                        {
                            "document": doc,
                            "kind": kind,
                            "options": options,
                            "destination": {
                                "kind": "directory" if directory else "file",
                                "path": str(path),
                            },
                        },
                    )
                    assert len(r["artifacts"]) == 1
                    artifact = Path(r["artifacts"][0]["path"])
                    assert artifact.stat().st_size == int(r["artifacts"][0]["size"])
                    return artifact

                async def open_xml(xml):
                    return await call(
                        "score_open",
                        {
                            "data": base64.b64encode(xml.encode()).decode(),
                            "format": "musicxml",
                        },
                    )

                async def pages(doc):
                    return (await call("score_outline", {"document": doc}))["outline"]["pages"]

                tools = await client.list_tools()
                schema = next(t.input_schema for t in tools.tools if t.name == "score_export")
                assert any(x["properties"]["kind"].get("const") == "positions" for x in schema["oneOf"])
                doc = (await open_xml(timed_fixture()))["document"]
                before = await export(doc, "midi", "before.mid", midi_expand_repeats=False)
                mappings = {}
                for fmt, ids, times in [
                    ("measurePositions", [0, 1, 0, 1], [0, 2000, 6000, 8000]),
                    (
                        "segmentPositions",
                        list(range(6)) * 2,
                        [
                            0,
                            500,
                            1000,
                            1500,
                            2000,
                            4000,
                            6000,
                            6500,
                            7000,
                            7500,
                            8000,
                            10000,
                        ],
                    ),
                ]:
                    path = await export(doc, fmt, fmt + ".json")
                    data = json.loads(path.read_text("utf-8"))
                    check_geometry(data, 1)
                    assert len(data["elements"]) == (2 if fmt == "measurePositions" else 6)
                    if fmt == "segmentPositions":
                        # The public DTO serializes these truncated coordinates as
                        # JSON numbers (possibly with a decimal point), not integers.
                        assert all(float(e["x"]).is_integer() and float(e["y"]).is_integer() for e in data["elements"])
                    assert [e["elid"] for e in data["events"]] == ids
                    assert [e["position"] for e in data["events"]] == times
                    again = await export(doc, fmt, fmt + "-again.json")
                    assert json.loads(again.read_text("utf-8")) == data
                    mappings[fmt] = data
                for i, segment in enumerate(mappings["segmentPositions"]["elements"]):
                    measure = mappings["measurePositions"]["elements"][0 if i < 4 else 1]
                    assert segment["page"] == measure["page"]
                    assert measure["x"] - 1 <= segment["x"] <= measure["x"] + measure["sx"]
                after = await export(doc, "midi", "after.mid", midi_expand_repeats=False)
                assert before.read_bytes() == after.read_bytes()
                png = await export(doc, "png", "png-page", dpi=144)
                w, h = read_png_dimensions(png)
                size = mappings["measurePositions"]["page_size"]
                assert (w, h) == (
                    math.ceil(size["width"] * 144 / 1200),
                    math.ceil(size["height"] * 144 / 1200),
                )
                sentinel = out / "sentinel.json"
                for fmt in ("measurePositions", "segmentPositions"):
                    for option in [
                        {"page": 0},
                        {"midi_expand_repeats": False},
                        {"dpi": 144},
                    ]:
                        sentinel.write_bytes(b"unchanged")
                        await call(
                            "score_export",
                            {
                                "document": doc,
                                "kind": "positions",
                                "options": {
                                    "position_kind": "measure" if fmt == "measurePositions" else "segment",
                                    **option,
                                },
                                "destination": {"kind": "file", "path": str(sentinel)},
                            },
                            True,
                        )
                        assert sentinel.read_bytes() == b"unchanged"
                await call("score_close", {"document": doc})
                opened = await open_xml(multipage_fixture())
                page_count = await pages(opened["document"])
                assert page_count >= 2
                for fmt, count, interval in [
                    ("measurePositions", 60, 2000),
                    ("segmentPositions", 240, 500),
                ]:
                    path = await export(opened["document"], fmt, "pages-" + fmt + ".json")
                    data = json.loads(path.read_text("utf-8"))
                    check_geometry(data, page_count)
                    assert len(data["elements"]) == count and len(data["events"]) == count
                    assert set(x["page"] for x in data["elements"]) == set(range(page_count))
                    assert [e["position"] for e in data["events"]] == [i * interval for i in range(count)]
                await call("score_close", {"document": opened["document"]})
                # Upstream PositionJsonWriter iterates firstMeasureMM/nextMeasureMM:
                # the four-measure rest is one element whose playback position spans
                # the whole rest (4 * 2000 ms at the default 120 BPM), then measure 5.
                mmrest = (await open_xml(mmrest_fixture()))["document"]
                for fmt in ("measurePositions", "segmentPositions"):
                    path = await export(mmrest, fmt, "mmrest-" + fmt + ".json")
                    data = json.loads(path.read_text("utf-8"))
                    check_geometry(data, 1)
                    assert len(data["elements"]) == 2, (fmt, data["elements"])
                    assert [e["position"] for e in data["events"]] == [0, 8000], (
                        fmt,
                        data["events"],
                    )
                await call("score_close", {"document": mmrest})
                evidence.update(
                    status="passed",
                    tempo_repeat_events=mappings,
                    multipage={"pages": page_count, "measures": 60, "segments": 240},
                    multi_measure_rest={
                        "underlyingMeasures": 5,
                        "elements": 2,
                        "events": [0, 8000],
                    },
                    midi_unchanged=True,
                    png_dimensions=[w, h],
                    invalid_options=6,
                )
                # Sequences from pinned upstream engraving/tests/repeat_tests.cpp.
                # Test data remains in vendor/engrave rather than being copied into this repo.
                cases = {
                    "repeat06": [1, 2, 3, 2, 4, 5, 6],  # simple volta
                    "repeat07": [1, 2, 3, 4, 5, 6, 1, 2, 3],  # D.C. al fine
                    "repeat08": [
                        1,
                        2,
                        3,
                        4,
                        5,
                        6,
                        2,
                        3,
                        4,
                        7,
                        8,
                        9,
                        10,
                        11,
                    ],  # D.S. al coda
                    "repeat14": [
                        1,
                        2,
                        3,
                        4,
                        5,
                        6,
                        7,
                        8,
                        9,
                        10,
                        2,
                        3,
                        4,
                        5,
                        6,
                        7,
                        8,
                        11,
                        12,
                        2,
                        3,
                        4,
                        5,
                        6,
                        7,
                        8,
                        13,
                        14,
                        15,
                        16,
                        17,
                        18,
                        16,
                        17,
                        18,
                        19,
                        20,
                        21,
                        22,
                        23,
                        5,
                        6,
                        7,
                        24,
                        25,
                        26,
                    ],
                }
                for name, expected in cases.items():
                    fixture = args.upstream / "src/engraving/tests/repeat_data" / f"{name}.mscx"
                    opened = await call("score_open", {"path": str(fixture.resolve())})
                    for fmt in ("measurePositions", "segmentPositions"):
                        path = await export(opened["document"], fmt, f"{name}-{fmt}.json")
                        data = json.loads(path.read_text("utf-8"))
                        check_geometry(data, await pages(opened["document"]))
                        if fmt == "measurePositions":
                            assert [e["elid"] + 1 for e in data["events"]] == expected, name
                    await call("score_close", {"document": opened["document"]})
                evidence["upstream_repeat_fixtures"] = list(cases)
    (out / "evidence.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding="utf-8")
    print(
        json.dumps(
            {k: v for k, v in evidence.items() if k != "tempo_repeat_events"},
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--resources", type=Path)
    p.add_argument(
        "--upstream",
        type=Path,
        default=REPO_ROOT / "vendor/engrave",
    )
    p.add_argument("--output", type=Path, required=True)
    asyncio.run(run(p.parse_args()))
