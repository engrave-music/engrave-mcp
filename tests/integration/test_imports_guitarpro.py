"""End-to-end Guitar Pro / Power Tab import lane checks.

The script uses the official Python MCP SDK and the pinned upstream fixtures in
vendor/engrave/src/importexport/guitarpro/tests/data through the host.

Run with:
    python tests/integration/test_imports_guitarpro.py
"""

import argparse
import asyncio
import json
import os
import struct
import xml.etree.ElementTree as ET
import zipfile
from collections import Counter
from fractions import Fraction
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

FIXTURES = REPO_ROOT / "vendor/engrave/src/importexport/guitarpro/tests/data"
OUT = Path(os.environ.get("ENGRAVE_MCP_OUTPUT", REPO_ROOT / "artifacts/imports-guitarpro"))

# Quarter-note durations used by the .mscx reference scores.
MSCX_QUARTERS = {
    "long": Fraction(16),
    "breve": Fraction(8),
    "whole": Fraction(4),
    "half": Fraction(2),
    "quarter": Fraction(1),
    "eighth": Fraction(1, 2),
    "16th": Fraction(1, 4),
    "32nd": Fraction(1, 8),
    "64th": Fraction(1, 16),
    "128th": Fraction(1, 32),
    "256th": Fraction(1, 64),
    "512th": Fraction(1, 128),
    "1024th": Fraction(1, 256),
}

# Pick one pinned fixture per input lane. A reference .mscx is available for
# every format except Power Tab (the repository only carries synthetic PTB
# streams), so PTB is checked against its documented note content.
CASES = [
    {"lane": "gp3", "file": "tempo.gp3", "reference": "tempo.gp3-ref.mscx"},
    {"lane": "gp4", "file": "sforzato.gp4", "reference": "sforzato.gp4-ref.mscx"},
    {
        "lane": "gp5",
        "file": "heavy-accent.gp5",
        "reference": "heavy-accent.gp5-ref.mscx",
    },
    {"lane": "gpx", "file": "sforzato.gpx", "reference": "sforzato.gpx-ref.mscx"},
    {"lane": "gp", "file": "sforzato.gp", "reference": "sforzato.gp-ref.mscx"},
    {
        "lane": "ptb",
        "file": "ptb-baseline.ptb",
        "reference": None,
        # Test fixture comment: two whole notes in 4/4, fret 5 on the first
        # string of E4 B3 G3 D3 A2 E2 and G2 D2 A1 E1.
        "expected": [(69, Fraction(4)), (48, Fraction(4))],
    },
]

MIDI_SEMITONES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def _mscx_duration_quarters(element):
    duration_type = element.findtext("durationType")
    if duration_type is None:
        return None
    if duration_type == "measure":
        value = element.findtext("duration", "")
        if "/" not in value:
            return None
        numerator, denominator = value.split("/", 1)
        return Fraction(int(numerator), int(denominator)) * 4
    base = MSCX_QUARTERS.get(duration_type)
    if base is None:
        return None
    dots = int(element.findtext("dots", "0") or 0)
    return base * (Fraction(2) - Fraction(1, 2**dots))


def _walk_mscx(element, factor, events):
    for child in element:
        if child.tag == "Tuplet":
            normal = int(child.findtext("normalNotes", "0") or 0)
            actual = int(child.findtext("actualNotes", "0") or 0)
            ratio = Fraction(normal, actual) if normal and actual else Fraction(1)
            _walk_mscx(child, factor * ratio, events)
        elif child.tag == "Chord":
            duration = _mscx_duration_quarters(child)
            if duration is None:
                continue
            duration *= factor
            for note in child.findall("Note"):
                pitch = note.findtext("pitch")
                if pitch is not None:
                    events.append((int(pitch), duration))
        elif child.tag == "Rest":
            continue
        else:
            _walk_mscx(child, factor, events)


def mscx_note_events(path):
    root = ET.parse(path).getroot()
    score = root.find("Score")
    if score is None:
        score = root
    events = []
    for staff in score.findall("Staff"):
        _walk_mscx(staff, Fraction(1), events)
    return events


def musicxml_note_events(path):
    root = ET.parse(path).getroot()
    events = []
    for part in root.findall("part"):
        divisions = 1
        transpose = 0
        for measure in part.findall("measure"):
            for element in measure:
                if element.tag == "attributes":
                    divisions = int(element.findtext("divisions", str(divisions)))
                    # <transpose> describes written-to-sounding pitch.
                    transpose_element = element.find("transpose")
                    if transpose_element is not None:
                        chromatic = int(float(transpose_element.findtext("chromatic", "0") or "0"))
                        octave_change = int(transpose_element.findtext("octave-change", "0") or "0")
                        transpose = chromatic + 12 * octave_change
                elif element.tag == "note":
                    if element.find("grace") is not None:
                        continue
                    duration_text = element.findtext("duration")
                    pitch_element = element.find("pitch")
                    if duration_text is None or pitch_element is None:
                        continue
                    step = pitch_element.findtext("step")
                    octave = int(pitch_element.findtext("octave"))
                    alter = int(float(pitch_element.findtext("alter", "0") or "0"))
                    # Normalize to sounding pitch so it can be compared with
                    # both the .mscx reference and the MIDI export.
                    pitch = (octave + 1) * 12 + MIDI_SEMITONES[step] + alter + transpose
                    events.append((pitch, Fraction(int(duration_text), divisions)))
    return events


def _read_varlen(data, position):
    value = 0
    for _ in range(4):
        if position >= len(data):
            raise ValueError("truncated MIDI variable-length value")
        byte = data[position]
        position += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, position
    raise ValueError("invalid MIDI variable-length value")


def midi_note_events(path):
    data = path.read_bytes()
    if data[:4] != b"MThd" or len(data) < 14:
        raise ValueError("invalid MIDI header")
    header_length = struct.unpack(">I", data[4:8])[0]
    _format, track_count, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise ValueError("SMPTE MIDI timing is not supported by this check")
    if not division:
        raise ValueError("invalid MIDI division")

    position = 8 + header_length
    events = []
    for _ in range(track_count):
        if data[position : position + 4] != b"MTrk":
            raise ValueError("invalid MIDI track")
        track_length = struct.unpack(">I", data[position + 4 : position + 8])[0]
        track = data[position + 8 : position + 8 + track_length]
        position += 8 + track_length

        tick = 0
        index = 0
        running = None
        active = {}
        while index < len(track):
            delta, index = _read_varlen(track, index)
            tick += delta
            if index >= len(track):
                raise ValueError("truncated MIDI event")
            status = track[index]
            if status & 0x80:
                index += 1
                if status < 0xF0:
                    running = status
            else:
                if running is None:
                    raise ValueError("MIDI data byte without running status")
                status = running

            if status == 0xFF:
                if index >= len(track):
                    raise ValueError("truncated MIDI meta event")
                meta_type = track[index]
                index += 1
                length, index = _read_varlen(track, index)
                if index + length > len(track):
                    raise ValueError("truncated MIDI meta event")
                index += length
                if meta_type == 0x2F:
                    break
            elif status in (0xF0, 0xF7):
                length, index = _read_varlen(track, index)
                if index + length > len(track):
                    raise ValueError("truncated MIDI system event")
                index += length
                running = None
            else:
                event_type = status & 0xF0
                if event_type in (0x80, 0x90):
                    if index + 2 > len(track):
                        raise ValueError("truncated MIDI note event")
                    note = track[index]
                    velocity = track[index + 1]
                    index += 2
                    key = (status & 0x0F, note)
                    if event_type == 0x90 and velocity > 0:
                        active.setdefault(key, []).append(tick)
                    else:
                        starts = active.get(key)
                        if starts:
                            start = starts.pop(0)
                            events.append((note, Fraction(tick - start, division)))
                elif event_type in (0xC0, 0xD0):
                    index += 1
                else:
                    index += 2
        if any(starts for starts in active.values()):
            raise ValueError("unterminated MIDI notes")
    return events


def _write_truncated(source, target, keep):
    data = source.read_bytes()
    target.write_bytes(data[: max(1, min(keep, len(data) - 1))])


def _write_zip(path, entries):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            archive.writestr(name, data)


def _patch_zip_declared_size(path, size):
    data = bytearray(path.read_bytes())
    offset = data.find(b"PK\x01\x02")
    if offset < 0:
        raise ValueError("central directory not found")
    struct.pack_into("<I", data, offset + 24, size)
    path.write_bytes(data)


def build_malformed_cases(fixtures: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    cases = {}

    for lane, name in [
        ("gp3", "tempo.gp3"),
        ("gp4", "sforzato.gp4"),
        ("gp5", "heavy-accent.gp5"),
        ("gpx", "sforzato.gpx"),
        ("gp", "sforzato.gp"),
    ]:
        target = out / f"truncated-{lane}.{name.rsplit('.', 1)[1]}"
        _write_truncated(fixtures / name, target, 40)
        cases[f"truncated-{lane}"] = target

    target = out / "truncated-ptb.ptb"
    _write_truncated(fixtures / "ptb-baseline.ptb", target, 12)
    cases["truncated-ptb"] = target

    # The old ss[l] write indexed a 30-byte stack buffer with an arbitrary
    # length byte. It must now be rejected before that write.
    target = out / "old-header-oob.gp4"
    target.write_bytes(b"?FIC" + bytes([250]) + b"A" * 30 + b"\0" * 16)
    cases["old-header-oob"] = target

    # GPX/BCFZ decompression with a declared length far past the input.
    target = out / "truncated-bcfz.gpx"
    target.write_bytes(b"BCFZ" + struct.pack("<i", 4096) + b"\0" * 16)
    cases["truncated-bcfz"] = target

    target = out / "missing-score-gpif.gp"
    _write_zip(target, [("Content/PartConfiguration", b"\0" * 8)])
    cases["missing-score-gpif"] = target

    target = out / "declared-entry-cap.gp"
    _write_zip(target, [("Content/score.gpif", b"<GPIF/>")])
    _patch_zip_declared_size(target, 64 * 1024 * 1024 + 1)
    cases["declared-entry-cap"] = target

    target = out / "too-many-entries.gp"
    _write_zip(target, [(f"f{index}", b"") for index in range(4097)])
    cases["too-many-entries"] = target

    target = out / "oversized.gp4"
    target.write_bytes(b"?FIC" + b"\0" * (8 * 1024 * 1024))
    cases["oversized"] = target

    target = out / "empty.gp4"
    target.write_bytes(b"")
    cases["empty"] = target
    return cases


def _document_id(response):
    if response.is_error:
        raise AssertionError(response.content)
    return response.structured_content["document"]


async def _open(session, path):
    return _document_id(await session.call_tool("score_open", {"path": str(path)}))


async def _close(session, document_id):
    response = await session.call_tool("score_close", {"document": document_id})
    assert not response.is_error, response


async def _export(session, document, fmt, path):
    result = await session.call_tool(
        "score_export",
        {
            "document": document,
            "kind": "score",
            "options": {"format": fmt},
            "destination": {"kind": "file", "path": str(path)},
        },
    )
    assert not result.is_error, result
    assert path.stat().st_size == int(result.structured_content["artifacts"][0]["size"]) > 0


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    binary_name = "engrave-mcp.exe" if os.name == "nt" else "engrave-mcp"
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path(os.environ.get("ENGRAVE_MCP_BINARY", REPO_ROOT / "build/release" / binary_name)),
    )
    parser.add_argument(
        "--resources",
        type=Path,
        default=Path(os.environ.get("ENGRAVE_MCP_RESOURCES", REPO_ROOT / ".runtime/assets")),
    )
    parser.add_argument("--upstream", type=Path, default=REPO_ROOT / "vendor/engrave")
    parser.add_argument("--output", type=Path, default=OUT)
    args = parser.parse_args()
    upstream = args.upstream.resolve()
    fixtures = upstream / "src/importexport/guitarpro/tests/data"
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    resources = args.resources.resolve()
    params = StdioServerParameters(
        command=str(binary),
        args=["--resources", str(resources)],
        env=dict(os.environ),
        cwd=str(out),
    )

    report = {"cases": [], "malformed": [], "recovered": False}
    with (out / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                for case in CASES:
                    path = fixtures / case["file"]
                    assert path.is_file(), path
                    document_id = await _open(session, path)
                    try:
                        described = await session.call_tool("score_outline", {"document": document_id})
                        if described.is_error:
                            raise AssertionError(described.content)
                        metadata = described.structured_content["outline"]
                        assert isinstance(metadata, dict), metadata
                        assert metadata["measure_count"] > 0, metadata
                        assert metadata["duration"] > 0, metadata
                        assert len(metadata["parts"]) > 0, metadata

                        musicxml_path = out / f"{case['lane']}.musicxml"
                        midi_path = out / f"{case['lane']}.mid"
                        for output_format, target in (
                            ("musicxml", musicxml_path),
                            ("midi", midi_path),
                        ):
                            await _export(session, document_id, output_format, target)

                        xml_events = musicxml_note_events(musicxml_path)
                        midi_events = midi_note_events(midi_path)
                        if case.get("reference"):
                            reference_events = mscx_note_events(fixtures / case["reference"])
                            expected = Counter(reference_events)
                        else:
                            expected = Counter((pitch, duration) for pitch, duration in case["expected"])
                        xml_counter = Counter(xml_events)
                        midi_counter = Counter(midi_events)
                        expected_midi = None
                        if case.get("reference"):
                            reference_id = await _open(session, fixtures / case["reference"])
                            try:
                                reference_midi = out / f"{case['lane']}-reference.mid"
                                await _export(session, reference_id, "midi", reference_midi)
                                expected_midi = Counter(midi_note_events(reference_midi))
                            finally:
                                await _close(session, reference_id)
                        assert xml_counter == expected, (
                            case["lane"],
                            "MusicXML",
                            xml_counter,
                            expected,
                        )
                        if expected_midi is not None:
                            assert midi_counter == expected_midi, (
                                case["lane"],
                                "MIDI",
                                midi_counter,
                                expected_midi,
                            )
                        else:
                            assert Counter(p for p, d in midi_events) == Counter(p for p, d in xml_events)
                            assert all(d > 0 for p, d in midi_events)

                        report["cases"].append(
                            {
                                "lane": case["lane"],
                                "fixture": case["file"],
                                "notes": len(xml_events),
                                "events": sorted((pitch, str(duration)) for pitch, duration in xml_events),
                                "metadata": {
                                    "title": metadata["title"],
                                    "measures": metadata["measure_count"],
                                    "parts": len(metadata["parts"]),
                                },
                            }
                        )
                    finally:
                        await _close(session, document_id)

                malformed = build_malformed_cases(fixtures, out)
                for name, path in malformed.items():
                    response = await session.call_tool("score_open", {"path": str(path)})
                    if not response.is_error:
                        document_id = response.structured_content.get("document")
                        if document_id:
                            await _close(session, document_id)
                        raise AssertionError(f"malformed input was accepted: {name}")
                    report["malformed"].append(name)

                # The same server session must still import a valid file after
                # every rejection above.
                recovered_id = await _open(session, fixtures / "sforzato.gp4")
                try:
                    recovered = await session.call_tool("score_outline", {"document": recovered_id})
                    if recovered.is_error:
                        raise AssertionError(recovered.content)
                    assert recovered.structured_content["outline"]["measure_count"] > 0
                finally:
                    await _close(session, recovered_id)

                musicxml_fixture = upstream / "tests/data/inputs/evening.musicxml"
                if musicxml_fixture.is_file():
                    musicxml_id = await _open(session, musicxml_fixture)
                    await _close(session, musicxml_id)

                report["recovered"] = True

    report["status"] = "passed"
    (out / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "cases"}))


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), timeout=600))
