"""MIDI/KAR importer acceptance: generated SMF, musical events, lyrics and recovery."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import xml.etree.ElementTree as ET
from collections import Counter
from fractions import Fraction
from pathlib import Path

from engrave_mcp_testing.export_formats import count_note_on_events, parse_midi_file
from engrave_mcp_testing.paths import REPO_ROOT
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def vlq(value):
    result = bytearray([value & 127])
    while value > 127:
        value >>= 7
        result.insert(0, (value & 127) | 128)
    return bytes(result)


def track(events):
    data = bytearray()
    previous = 0
    for tick, order, event in sorted(events):
        data += vlq(tick - previous) + event
        previous = tick
    data += b"\x00\xff\x2f\x00"
    return b"MTrk" + len(data).to_bytes(4, "big") + data


def meta(kind, text):
    value = text.encode("utf-8")
    return bytes([255, kind]) + vlq(len(value)) + value


def fixture(karaoke=False, midi_format=1):
    conductor = [
        (0, 0, b"\xff\x51\x03\x07\xa1\x20"),
        (0, 1, b"\xff\x58\x04\x04\x02\x18\x08"),
    ]
    melodic = [(0, 0, bytes([0xC0, 40])), (0, 1, meta(3, "Violin"))]
    bass = [(0, 0, bytes([0xC1, 42])), (0, 1, meta(3, "Cello"))]
    expected = []
    for destination, channel, pitches in [
        (melodic, 0, [60, 62, 64, 67]),
        (bass, 1, [48, 50, 52, 55]),
    ]:
        for index, pitch in enumerate(pitches):
            destination.extend(
                [
                    (index * 480, 3, bytes([0x90 + channel, pitch, 90])),
                    ((index + 1) * 480, 2, bytes([0x80 + channel, pitch, 0])),
                ]
            )
            expected.append(pitch)
    if karaoke:
        conductor.append((0, 3, meta(1, "@KMIDI KARAOKE FILE")))
        for index, text in enumerate(["/你", "好", "\\世", "界"]):
            conductor.append((index * 480, 4, meta(1, text)))
    chunks = (
        [track(conductor), track(melodic), track(bass)] if midi_format == 1 else [track(conductor + melodic + bass)]
    )
    return b"MThd\x00\x00\x00\x06" + midi_format.to_bytes(2, "big") + len(chunks).to_bytes(
        2, "big"
    ) + b"\x01\xe0" + b"".join(chunks), expected


def xml_notes(xml):
    root = ET.fromstring(xml)
    values = []
    steps = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    for part in root.findall("part"):
        divisions = 1
        for measure in part.findall("measure"):
            divisions = int(measure.findtext("attributes/divisions", str(divisions)))
            for note in measure.findall("note"):
                pitch = note.find("pitch")
                if pitch is None:
                    continue
                midi = (
                    (int(pitch.findtext("octave")) + 1) * 12
                    + steps[pitch.findtext("step")]
                    + int(pitch.findtext("alter", "0"))
                )
                values.append((midi, Fraction(int(note.findtext("duration")), divisions)))
    return values, [n.text or "" for n in root.findall(".//lyric/text")]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ["--resources", str(args.resources.resolve())] if args.resources else []
    params = StdioServerParameters(
        command=str(args.binary.resolve()),
        args=child_args,
        env=dict(os.environ),
        cwd=str(out),
    )
    report = {"status": "running", "formats": {}, "rejected": []}
    with (out / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()

                async def call(name, values, error=False):
                    r = await client.call_tool(name, values)
                    assert bool(r.is_error) == error, r
                    if error:
                        return None
                    return r.structured_content if r.structured_content is not None else json.loads(r.content[0].text)

                serial = 0

                async def export(doc, fmt, dest):
                    await call(
                        "score_export",
                        {
                            "document": doc,
                            "kind": "score",
                            "options": {"format": fmt},
                            "destination": {"kind": "file", "path": str(dest)},
                        },
                    )

                async def score_xml(doc):
                    nonlocal serial
                    serial += 1
                    dest = out / f"query-{serial}.musicxml"
                    await export(doc, "musicxml", dest)
                    return dest.read_text(encoding="utf-8")

                for extension, karaoke, midi_format in [
                    ("mid", False, 1),
                    ("midi", False, 1),
                    ("kar", True, 1),
                    ("format0.mid", False, 0),
                ]:
                    data, expected = fixture(karaoke, midi_format)
                    source = out / f"input.{extension}"
                    source.write_bytes(data)
                    doc = await call("score_open", {"path": str(source)})
                    info = await call("score_outline", {"document": doc["document"]})
                    notes, lyrics = xml_notes(await score_xml(doc["document"]))
                    assert Counter(p for p, d in notes) == Counter(expected), (
                        extension,
                        notes,
                    )
                    assert all(d == 1 for _, d in notes), (extension, notes)
                    # Pinned upstream (importmidi_lyrics.cpp:135-159,270-279) appends an
                    # entry to BestTrack.matchedLyricTimes for every note track matching a
                    # lyric onset, then inserts all of them into the single best track's
                    # staff. Both fixture tracks start a note on every lyric onset, so the
                    # melody staff receives two copies of each syllable. Kept for vendor/engrave
                    # parity rather than diverging from the pinned import algorithm.
                    if karaoke:
                        assert lyrics == [s for s in "你好世界" for _ in range(2)], lyrics
                    else:
                        assert not lyrics, lyrics
                    dest = out / f"roundtrip-{extension}.mid"
                    await export(doc["document"], "midi", dest)
                    parsed = parse_midi_file(dest.read_bytes())
                    count, onsets = count_note_on_events(parsed)
                    assert count == len(expected) and Counter(e["pitch"] for e in onsets) == Counter(expected)
                    assert sorted(Fraction(e["tick"], parsed["division"]) for e in onsets) == [0, 0, 1, 1, 2, 2, 3, 3]
                    report["formats"][extension] = {
                        "notes": count,
                        "lyrics": lyrics,
                        "pages": info["outline"]["pages"],
                        "parts": len(info["outline"]["parts"]),
                    }
                    await call("score_close", {"document": doc["document"]})
                valid, _ = fixture()

                def single_track(payload):
                    return (
                        b"MThd\x00\x00\x00\x06\x00\x00\x00\x01\x01\xe0"
                        + b"MTrk"
                        + len(payload).to_bytes(4, "big")
                        + payload
                    )

                malformed = [
                    ("bad-header", b"bad midi"),
                    ("short-header", b"MThd"),
                    ("truncated-track", valid[:-12]),
                    ("zero-division", valid[:12] + b"\x00\x00" + valid[14:]),
                    ("zero-tracks", valid[:10] + b"\x00\x00" + valid[12:]),
                    ("format0-multiple-tracks", valid[:8] + b"\x00\x00" + valid[10:]),
                    ("unknown-format", valid[:8] + b"\x00\x03" + valid[10:]),
                    ("invalid-smpte", valid[:12] + b"\x81\x40" + valid[14:]),
                    ("huge-meta-length", single_track(b"\x00\xff\x01\xff\xff\xff\x7f")),
                    ("huge-sysex-length", single_track(b"\x00\xf0\xff\xff\xff\x7f")),
                    (
                        "missing-running-status",
                        single_track(b"\x00\x40\x40\x00\xff\x2f\x00"),
                    ),
                    ("invalid-vlq", single_track(b"\x80\x80\x80\x80\x00\xff\x2f\x00")),
                    (
                        "short-tempo",
                        single_track(b"\x00\xff\x51\x01\x7f\x00\xff\x2f\x00"),
                    ),
                    (
                        "zero-tempo",
                        single_track(b"\x00\xff\x51\x03\x00\x00\x00\x00\xff\x2f\x00"),
                    ),
                    (
                        "overflow-time-signature",
                        single_track(b"\x00\xff\x58\x04\x04\xff\x18\x08\x00\xff\x2f\x00"),
                    ),
                    ("missing-eot", single_track(b"\x00\x90\x40\x40")),
                    ("empty", b""),
                ]
                for name, data in malformed:
                    path = out / f"{name}.mid"
                    path.write_bytes(data)
                    await call("score_open", {"path": str(path)}, True)
                    report["rejected"].append(name)
                # The failed import cache/state must not contaminate the next valid file.
                for index in range(5):
                    path = out / f"recovery-{index}.mid"
                    path.write_bytes(valid)
                    doc = await call("score_open", {"path": str(path)})
                    notes, lyrics = xml_notes(await score_xml(doc["document"]))
                    assert len(notes) == 8 and not lyrics
                    await call("score_close", {"document": doc["document"]})
                reference_cases = [
                    "division",
                    "instrument_grand",
                    "instrument_grand2",
                    "instrument_channels",
                    "instrument_3staff_organ",
                ]
                for name in reference_cases:
                    scores = []
                    for suffix in [".mid", "-ref.mscx"]:
                        source = args.upstream / "src/importexport/midi/tests/midiimport_data" / (name + suffix)
                        doc = await call("score_open", {"path": str(source.resolve())})
                        notes, lyrics = xml_notes(await score_xml(doc["document"]))
                        scores.append((Counter(notes), lyrics))
                        await call("score_close", {"document": doc["document"]})
                    assert scores[0] == scores[1], (name, scores)
                report["upstream_default_reference_cases"] = reference_cases
                report["status"] = "passed"
                report["recovery_cycles"] = 5
    (out / "evidence.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--resources", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument(
        "--upstream",
        type=Path,
        default=REPO_ROOT / "vendor/engrave",
    )
    asyncio.run(run(p.parse_args()))
