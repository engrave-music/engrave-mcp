"""Native engrave-mcp Export Options Integration Test.

Exercises native export options introduced to score_export via the standard
Python MCP SDK v2 client:
- score_export MSCX uncompressed XML export and score_open import roundtrip
- PNG DPI sizing: 72 DPI vs 144 DPI resolution and dimension verification
- PNG transparency: transparent alpha (min alpha < 255) vs opaque white background (alpha == 255)
- SVG and PNG page background toggle (drawPageBackground)
- Robustness: invalid option types, out-of-format options, and out-of-range options
  leave destination sentinels unchanged while the MCP session survives
- MIDI repeat expansion (expandRepeats): 2-measure repeat fixture produces more Note-On events
- MIDI RPN (exportRPNs): parameter message sequences differ and upstream pitch bend
  sensitivity (RPN 00 00 = 12 semitones) is asserted via parsed MIDI controller events
- WAV rendering forwarded when --soundfont is supplied, or cleanly rejected when omitted.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import json
import xml.etree.ElementTree as ET
from fractions import Fraction
from pathlib import Path

from support.paths import REPO_ROOT

ROOT = REPO_ROOT

# Tiny 2-measure MusicXML repeat fixture:
# Measure 1: forward repeat barline, 1 whole note C4
# Measure 2: 1 whole note E4, backward repeat barline
REPEAT_FIXTURE_MUSICXML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 4.0 Partwise//EN" "http://www.musicxml.org/dtds/partwise.dtd">
<score-partwise version="4.0">
  <part-list>
    <score-part id="P1">
      <part-name>RepeatFixture</part-name>
      <midi-instrument id="P1-I1">
        <midi-channel>1</midi-channel>
        <midi-program>1</midi-program>
      </midi-instrument>
    </score-part>
  </part-list>
  <part id="P1">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <key><fifths>0</fifths></key>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
      </attributes>
      <barline location="left">
        <bar-style>heavy-light</bar-style>
        <repeat direction="forward"/>
      </barline>
      <note>
        <pitch><step>C</step><octave>4</octave></pitch>
        <duration>4</duration>
        <voice>1</voice>
        <type>whole</type>
      </note>
    </measure>
    <measure number="2">
      <note>
        <pitch><step>E</step><octave>4</octave></pitch>
        <duration>4</duration>
        <voice>1</voice>
        <type>whole</type>
      </note>
      <barline location="right">
        <bar-style>light-heavy</bar-style>
        <repeat direction="backward"/>
      </barline>
    </measure>
  </part>
</score-partwise>
"""


def bar(*pitches: int | None) -> dict:
    """Helper to construct a 4/4 bar of 4 quarter notes."""
    return {"notes": [{"pitches": [p] if p is not None else [], "ticks": 480} for p in pitches]}


def canonical_notes(path_or_xml: Path | str) -> tuple[list, list]:
    """Parse MusicXML and return sorted canonical notes and measure counts."""
    if isinstance(path_or_xml, Path):
        root = ET.parse(path_or_xml).getroot()
    elif "<" in path_or_xml:
        root = ET.fromstring(path_or_xml)
    else:
        root = ET.parse(path_or_xml).getroot()

    events = []
    measures = []
    for part_index, part in enumerate(root.findall("part")):
        divisions = 1
        part_measures = part.findall("measure")
        measures.append(len(part_measures))
        for measure_index, measure in enumerate(part_measures):
            cursor = Fraction(0)
            previous_start = Fraction(0)
            for element in measure:
                if element.tag == "attributes":
                    divisions = int(element.findtext("divisions", str(divisions)))
                elif element.tag in ("backup", "forward"):
                    length = Fraction(int(element.findtext("duration", "0")), divisions)
                    cursor += length if element.tag == "forward" else -length
                elif element.tag == "note":
                    duration = Fraction(int(element.findtext("duration", "0")), divisions)
                    chord = element.find("chord") is not None
                    start = previous_start if chord else cursor
                    if not chord:
                        previous_start = cursor
                        cursor += duration
                    pitch = element.find("pitch")
                    value = (
                        "rest"
                        if pitch is None
                        else ":".join(
                            pitch.findtext(field, default)
                            for field, default in (
                                ("step", ""),
                                ("alter", "0"),
                                ("octave", ""),
                            )
                        )
                    )
                    ties = tuple(sorted(tie.get("type") for tie in element.findall("tie")))
                    events.append(
                        (
                            part_index,
                            measure_index,
                            str(start),
                            str(duration),
                            element.findtext("voice", "1"),
                            element.findtext("staff", "1"),
                            value,
                            ties,
                        )
                    )
    if not events:
        raise ValueError("MusicXML contains no notes")
    return sorted(events), measures


def read_png_dimensions(data_or_path: bytes | Path) -> tuple[int, int]:
    """Parse width and height in pixels directly from PNG IHDR chunk."""
    data = data_or_path.read_bytes() if isinstance(data_or_path, Path) else data_or_path
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("Invalid PNG header")
    if data[12:16] != b"IHDR":
        raise ValueError("PNG missing IHDR chunk")
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    return width, height


def inspect_png_alpha(data_or_path: bytes | Path) -> dict:
    import io

    from PIL import Image

    data = data_or_path.read_bytes() if isinstance(data_or_path, Path) else data_or_path
    with Image.open(io.BytesIO(data)) as image:
        alpha = image.convert("RGBA").getchannel("A")
        minimum, maximum = alpha.getextrema()
        return {
            "min_alpha": minimum,
            "max_alpha": maximum,
            "has_transparency": minimum < 255,
            "non_opaque_pixels": sum(alpha.histogram()[:255]),
        }


def parse_midi_file(data: bytes) -> dict:
    """Parse Standard MIDI File (format 0/1) tracks and events."""
    if not data.startswith(b"MThd"):
        raise ValueError("Invalid MIDI header")
    header_len = int.from_bytes(data[4:8], "big")
    midi_format = int.from_bytes(data[8:10], "big")
    num_tracks = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")

    pos = 8 + header_len
    tracks = []

    for _ in range(num_tracks):
        if data[pos : pos + 4] != b"MTrk":
            raise ValueError(f"Expected MTrk at byte {pos}")
        track_len = int.from_bytes(data[pos + 4 : pos + 8], "big")
        track_end = pos + 8 + track_len
        track_data = data[pos + 8 : track_end]
        pos = track_end

        events = []
        tpos = 0
        current_tick = 0
        running_status = None

        while tpos < len(track_data):
            # VLQ delta
            delta = 0
            while tpos < len(track_data):
                b = track_data[tpos]
                tpos += 1
                delta = (delta << 7) | (b & 0x7F)
                if not (b & 0x80):
                    break
            current_tick += delta

            if tpos >= len(track_data):
                break

            lead_byte = track_data[tpos]
            if lead_byte == 0xFF:
                tpos += 1
                meta_type = track_data[tpos]
                tpos += 1
                meta_len = 0
                while tpos < len(track_data):
                    b = track_data[tpos]
                    tpos += 1
                    meta_len = (meta_len << 7) | (b & 0x7F)
                    if not (b & 0x80):
                        break
                meta_data = track_data[tpos : tpos + meta_len]
                tpos += meta_len
                events.append(
                    {
                        "type": "meta",
                        "tick": current_tick,
                        "meta_type": meta_type,
                        "data": meta_data,
                    }
                )
                running_status = None
                if meta_type == 0x2F:
                    break
            elif lead_byte in (0xF0, 0xF7):
                tpos += 1
                sysex_len = 0
                while tpos < len(track_data):
                    b = track_data[tpos]
                    tpos += 1
                    sysex_len = (sysex_len << 7) | (b & 0x7F)
                    if not (b & 0x80):
                        break
                tpos += sysex_len
                running_status = None
            else:
                if lead_byte & 0x80:
                    status = lead_byte
                    tpos += 1
                else:
                    status = running_status
                running_status = status

                cmd = status & 0xF0
                channel = status & 0x0F

                if cmd in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    d1 = track_data[tpos]
                    d2 = track_data[tpos + 1]
                    tpos += 2
                    if cmd == 0x90:
                        ev_type = "note_on" if d2 > 0 else "note_off"
                        events.append(
                            {
                                "type": ev_type,
                                "tick": current_tick,
                                "channel": channel,
                                "pitch": d1,
                                "velocity": d2,
                            }
                        )
                    elif cmd == 0x80:
                        events.append(
                            {
                                "type": "note_off",
                                "tick": current_tick,
                                "channel": channel,
                                "pitch": d1,
                                "velocity": d2,
                            }
                        )
                    elif cmd == 0xB0:
                        events.append(
                            {
                                "type": "controller",
                                "tick": current_tick,
                                "channel": channel,
                                "controller": d1,
                                "value": d2,
                            }
                        )
                    elif cmd == 0xE0:
                        events.append(
                            {
                                "type": "pitch_bend",
                                "tick": current_tick,
                                "channel": channel,
                                "value": d1 | (d2 << 7),
                            }
                        )
                elif cmd in (0xC0, 0xD0):
                    d1 = track_data[tpos]
                    tpos += 1
                    if cmd == 0xC0:
                        events.append(
                            {
                                "type": "program",
                                "tick": current_tick,
                                "channel": channel,
                                "program": d1,
                            }
                        )
                    elif cmd == 0xD0:
                        events.append(
                            {
                                "type": "channel_aftertouch",
                                "tick": current_tick,
                                "channel": channel,
                                "pressure": d1,
                            }
                        )
                else:
                    raise ValueError(f"Unsupported MIDI status: {status}")

        tracks.append(events)

    return {"format": midi_format, "division": division, "tracks": tracks}


def count_note_on_events(midi_info: dict) -> tuple[int, list[dict]]:
    """Count note-on events with velocity > 0 across all tracks."""
    note_ons = []
    for track in midi_info["tracks"]:
        for ev in track:
            if ev["type"] == "note_on" and ev["velocity"] > 0:
                note_ons.append(ev)
    return len(note_ons), note_ons


def extract_rpn_controllers(midi_info: dict) -> list[dict]:
    """Extract registered parameter number (RPN) controller messages.

    In MuseScore upstream (exportmidi.cpp), when exportRPNs is enabled:
    - CTRL_LRPN (CC 100) = 0
    - CTRL_HRPN (CC 101) = 0
    - CTRL_HDATA (CC 6) = 12 (pitch bend sensitivity: 12 semitones)
    - CTRL_LRPN (CC 100) = 127
    - CTRL_HRPN (CC 101) = 127
    """
    rpns = []
    for track_idx, track in enumerate(midi_info["tracks"]):
        for ev in track:
            if ev["type"] == "controller" and ev["controller"] in (6, 100, 101):
                rpns.append(
                    {
                        "track": track_idx,
                        "tick": ev["tick"],
                        "channel": ev["channel"],
                        "controller": ev["controller"],
                        "value": ev["value"],
                    }
                )
    return rpns


def inspect_wav_audio(path: Path) -> dict:
    import numpy as np
    import soundfile as sf

    audio, rate = sf.read(path, always_2d=True)
    assert rate == 44100 and audio.shape[1] == 2 and len(audio) > 0
    assert np.isfinite(audio).all()
    peak = float(np.max(np.abs(audio)))
    assert peak > 0.005, f"Audio signal too low: peak={peak}"
    return {
        "sample_rate": rate,
        "channels": audio.shape[1],
        "duration_seconds": len(audio) / rate,
        "peak": peak,
    }


async def main() -> None:
    import base64

    from integration.test_native_edit import fixture
    from PIL import Image
    from support.mcp_client import Client

    parser = argparse.ArgumentParser(description="V2 score, render and audio export options")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    evidence = {}

    async with Client(args, out) as client:

        async def open_xml(xml):
            return (
                await client.tool(
                    "score_open",
                    {
                        "data": base64.b64encode(xml.encode()).decode(),
                        "format": "musicxml",
                    },
                )
            )["document"]

        async def export(doc, kind, options, name, action="run"):
            fmt = options.get("format")
            directory = kind == "render" and fmt in ("svg", "png")
            destination = out / name
            if directory:
                destination.mkdir(exist_ok=True)
            result = await client.tool(
                "score_export",
                {
                    "document": doc,
                    "kind": kind,
                    "options": options,
                    "destination": {
                        "kind": "directory" if directory else "file",
                        "path": str(destination),
                    },
                },
            )
            artifacts = result["artifacts"]
            assert artifacts and all(Path(item["path"]).stat().st_size == int(item["size"]) > 0 for item in artifacts)
            return [Path(item["path"]) for item in artifacts]

        doc = await open_xml(fixture())
        revision = (await client.tool("score_outline", {"document": doc}))["revision"]
        original = (await export(doc, "score", {"format": "musicxml"}, "original.musicxml"))[0]
        original_notes, original_measures = canonical_notes(original)
        mscx = (await export(doc, "score", {"format": "mscx"}, "score.mscx"))[0]
        assert ET.parse(mscx).getroot().tag == "museScore"
        reopened = (await client.tool("score_open", {"path": str(mscx)}))["document"]
        reopened_xml = (await export(reopened, "score", {"format": "musicxml"}, "reopened.musicxml"))[0]
        assert canonical_notes(reopened_xml) == (original_notes, original_measures)
        await client.tool("score_close", {"document": reopened})
        evidence["mscx_roundtrip"] = True

        def png(name, **options):
            return export(
                doc,
                "render",
                {"format": "png", "first_page": 0, "last_page": 0, **options},
                name,
            )

        p72 = (await png("png72", dpi=72))[0]
        p144 = (await png("png144", dpi=144))[0]
        w72, h72 = read_png_dimensions(p72)
        w144, h144 = read_png_dimensions(p144)
        assert w72 > 0 and h72 > 0 and abs(w144 - 2 * w72) <= 1 and abs(h144 - 2 * h72) <= 1
        transparent = (await png("transparent", transparent=True))[0]
        opaque = (await png("opaque", transparent=False))[0]
        assert inspect_png_alpha(transparent)["has_transparency"]
        assert not inspect_png_alpha(opaque)["has_transparency"]
        nobg = (await png("nobg", background=False))[0]
        withbg = (await png("withbg", background=True))[0]
        with Image.open(nobg) as image:
            assert image.convert("RGBA").getpixel((0, 0))[3] == 0
        with Image.open(withbg) as image:
            assert image.convert("RGBA").getpixel((0, 0)) == (255, 255, 255, 255)
        with Image.open(opaque) as image:
            assert image.convert("RGBA").getpixel((0, 0)) == (255, 255, 255, 255)
        svg_nobg = (
            await export(
                doc,
                "render",
                {"format": "svg", "first_page": 0, "last_page": 0, "background": False},
                "svg-nobg",
            )
        )[0]
        svg_withbg = (
            await export(
                doc,
                "render",
                {"format": "svg", "first_page": 0, "last_page": 0, "background": True},
                "svg-withbg",
            )
        )[0]

        def white_paths(path):
            return sum(
                e.tag.endswith("path") and e.get("fill", "").replace(" ", "") == "rgb(100%,100%,100%)"
                for e in ET.parse(path).getroot().iter()
            )

        assert white_paths(svg_withbg) > white_paths(svg_nobg)
        evidence["render"] = {
            "dpi72": [w72, h72],
            "dpi144": [w144, h144],
            "alpha_and_background": True,
        }

        bad_cases = [
            ("render", {"format": "png", "dpi": "high"}),
            ("render", {"format": "png", "dpi": True}),
            ("render", {"format": "png", "transparent": "false"}),
            ("render", {"format": "svg", "background": "yes"}),
            ("score", {"format": "midi", "midi_expand_repeats": 123}),
            ("score", {"format": "midi", "midi_rpns": "false"}),
            ("render", {"format": "svg", "dpi": 144}),
            ("score", {"format": "midi", "dpi": 144}),
            ("render", {"format": "svg", "transparent": False}),
            ("score", {"format": "midi", "transparent": False}),
            ("score", {"format": "midi", "background": True}),
            ("score", {"format": "musicxml", "background": True}),
            ("render", {"format": "png", "midi_expand_repeats": True}),
            ("render", {"format": "png", "midi_rpns": True}),
            ("score", {"format": "midi", "first_page": 0}),
            ("render", {"format": "png", "dpi": 35}),
            ("render", {"format": "png", "dpi": 1201}),
            ("render", {"format": "svg", "first_page": -1}),
            ("render", {"format": "svg", "first_page": 999}),
        ]
        for kind, fmt in [
            ("render", "svg"),
            ("render", "png"),
            ("score", "midi"),
            ("score", "musicxml"),
            ("render", "pdf"),
            ("score", "mscz"),
        ]:
            bad_cases.append((kind, {"format": fmt, "master_volume_db": 0}))
        for key, val in [
            ("master_volume_db", 24.1),
            ("master_volume_db", -120.1),
            ("master_balance", 1.1),
            ("master_balance", -1.1),
        ]:
            bad_cases.append(("audio", {"format": "wav", key: val}))
        for key, val in [
            ("volume_db", 24.1),
            ("volume_db", -120.1),
            ("balance", 1.1),
            ("balance", -1.1),
            ("forceMute", True),
            ("output", {}),
            ("unexpected", 1),
        ]:
            bad_cases.append(("audio", {"format": "wav", "tracks": [{"track": 0, key: val}]}))
        bad_cases.extend(
            [
                ("audio", {"format": "wav", "tracks": [{"track": -1}]}),
                ("audio", {"format": "wav", "unexpected": True}),
                ("audio", {"format": "wav", "master": {"unexpected": True}}),
                ("audio", {"format": "wav", "master_solo": True}),
                ("audio", {"format": "wav", "master_mute": True}),
                ("audio", {"format": "wav", "tracks": [{"track": 0}, {"track": 0}]}),
                ("audio", {"format": "wav", "tracks": [{"track": 999999}]}),
            ]
        )
        for i, (kind, options) in enumerate(bad_cases):
            directory = kind == "render" and options["format"] in ("svg", "png")
            target = out / f"sentinel-{i}"
            if directory:
                target.mkdir(exist_ok=True)
                sentinel = target / "keep"
            else:
                sentinel = target
            sentinel.write_bytes(b"unchanged")
            await client.tool(
                "score_export",
                {
                    "document": doc,
                    "kind": kind,
                    "options": options,
                    "destination": {
                        "kind": "directory" if directory else "file",
                        "path": str(target),
                    },
                },
                error=True,
            )
            assert sentinel.read_bytes() == b"unchanged"
            if directory:
                assert {x.name for x in target.iterdir()} == {"keep"}
            assert (await client.tool("score_outline", {"document": doc}))["revision"] == revision
        evidence["invalid_options"] = len(bad_cases)

        repeat = await open_xml(REPEAT_FIXTURE_MUSICXML)
        expanded = (
            await export(
                repeat,
                "score",
                {"format": "midi", "midi_expand_repeats": True},
                "expanded.mid",
            )
        )[0]
        unexpanded = (
            await export(
                repeat,
                "score",
                {"format": "midi", "midi_expand_repeats": False},
                "unexpanded.mid",
            )
        )[0]
        count_exp, _ = count_note_on_events(parse_midi_file(expanded.read_bytes()))
        count_unexp, _ = count_note_on_events(parse_midi_file(unexpanded.read_bytes()))
        assert count_exp == 2 * count_unexp and count_unexp > 0
        wav_exp = (await export(repeat, "audio", {"format": "wav", "use_repeats": True}, "expanded.wav"))[0]
        wav_unexp = (
            await export(
                repeat,
                "audio",
                {"format": "wav", "use_repeats": False},
                "unexpanded.wav",
            )
        )[0]
        assert inspect_wav_audio(wav_exp)["duration_seconds"] > inspect_wav_audio(wav_unexp)["duration_seconds"] + 3.5
        await client.tool("score_close", {"document": repeat})
        evidence["repeat_note_ons"] = [count_exp, count_unexp]

        rpn_on = (await export(doc, "score", {"format": "midi", "midi_rpns": True}, "rpn-on.mid"))[0]
        rpn_off = (await export(doc, "score", {"format": "midi", "midi_rpns": False}, "rpn-off.mid"))[0]
        assert rpn_on.read_bytes() != rpn_off.read_bytes()
        on, off = (
            parse_midi_file(rpn_on.read_bytes()),
            parse_midi_file(rpn_off.read_bytes()),
        )
        rpns_on, rpns_off = extract_rpn_controllers(on), extract_rpn_controllers(off)
        assert rpns_on and not rpns_off and any(e["controller"] == 6 and e["value"] == 12 for e in rpns_on)
        _, notes_on = count_note_on_events(on)
        _, notes_off = count_note_on_events(off)
        assert [(n["tick"], n["channel"], n["pitch"]) for n in notes_on] == [
            (n["tick"], n["channel"], n["pitch"]) for n in notes_off
        ]
        evidence["rpn_controllers"] = [len(rpns_on), len(rpns_off)]
        wav = (await export(doc, "audio", {"format": "wav"}, "score.wav"))[0]
        evidence["wav"] = inspect_wav_audio(wav)
        info = await client.tool("score_query", {"document": doc, "mode": "audio_tracks"})
        assert len(info["audio_tracks"]["tracks"]) == len(
            (await client.tool("score_outline", {"document": doc}))["outline"]["parts"]
        )
        await client.tool("score_close", {"document": doc})

        parts = "".join(f'<score-part id="P{i}"><part-name>Part {i}</part-name></score-part>' for i in range(1, 17))
        bodies = "".join(
            f'<part id="P{i}"><measure number="1"><attributes><divisions>1</divisions>'
            "<time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>"
            "<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note>"
            "</measure></part>"
            for i in range(1, 17)
        )
        large = await open_xml(
            '<score-partwise version="4.0"><part-list>' + parts + "</part-list>" + bodies + "</score-partwise>"
        )
        assert (
            len(
                (await client.tool("score_query", {"document": large, "mode": "audio_tracks"}))["audio_tracks"][
                    "tracks"
                ]
            )
            == 16
        )
        native = (await export(large, "score", {"format": "mscz"}, "sixteen.mscz"))[0]
        restored = (await client.tool("score_open", {"path": str(native)}))["document"]
        assert (
            len(
                (await client.tool("score_query", {"document": restored, "mode": "audio_tracks"}))["audio_tracks"][
                    "tracks"
                ]
            )
            == 16
        )
        await client.tool("score_close", {"document": restored})
        await client.tool("score_close", {"document": large})
        evidence["sixteen_part_tracks"] = 16

    evidence["status"] = "passed"
    (out / "export_options_evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence))


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), timeout=180))
