"""Native MuseScore MCP Export Options Integration Test.

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

import argparse
import asyncio
import copy
from fractions import Fraction
import json
import logging
import os
from pathlib import Path
import sys
import time
import xml.etree.ElementTree as ET

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]

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
                    value = "rest" if pitch is None else ":".join(
                        pitch.findtext(field, default)
                        for field, default in (("step", ""), ("alter", "0"), ("octave", ""))
                    )
                    ties = tuple(sorted(tie.get("type") for tie in element.findall("tie")))
                    events.append((part_index, measure_index, str(start), str(duration),
                                   element.findtext("voice", "1"), element.findtext("staff", "1"), value, ties))
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
    from PIL import Image
    import io
    data = data_or_path.read_bytes() if isinstance(data_or_path, Path) else data_or_path
    with Image.open(io.BytesIO(data)) as image:
        alpha = image.convert("RGBA").getchannel("A")
        minimum, maximum = alpha.getextrema()
        return {"min_alpha": minimum, "max_alpha": maximum,
                "has_transparency": minimum < 255,
                "non_opaque_pixels": sum(alpha.histogram()[:255])}


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
        if data[pos:pos+4] != b"MTrk":
            raise ValueError(f"Expected MTrk at byte {pos}")
        track_len = int.from_bytes(data[pos+4:pos+8], "big")
        track_end = pos + 8 + track_len
        track_data = data[pos+8:track_end]
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
                meta_data = track_data[tpos:tpos+meta_len]
                tpos += meta_len
                events.append({"type": "meta", "tick": current_tick, "meta_type": meta_type, "data": meta_data})
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
                    d2 = track_data[tpos+1]
                    tpos += 2
                    if cmd == 0x90:
                        ev_type = "note_on" if d2 > 0 else "note_off"
                        events.append({"type": ev_type, "tick": current_tick, "channel": channel, "pitch": d1, "velocity": d2})
                    elif cmd == 0x80:
                        events.append({"type": "note_off", "tick": current_tick, "channel": channel, "pitch": d1, "velocity": d2})
                    elif cmd == 0xB0:
                        events.append({"type": "controller", "tick": current_tick, "channel": channel, "controller": d1, "value": d2})
                    elif cmd == 0xE0:
                        events.append({"type": "pitch_bend", "tick": current_tick, "channel": channel, "value": d1 | (d2 << 7)})
                elif cmd in (0xC0, 0xD0):
                    d1 = track_data[tpos]
                    tpos += 1
                    if cmd == 0xC0:
                        events.append({"type": "program", "tick": current_tick, "channel": channel, "program": d1})
                    elif cmd == 0xD0:
                        events.append({"type": "channel_aftertouch", "tick": current_tick, "channel": channel, "pressure": d1})
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
                rpns.append({
                    "track": track_idx,
                    "tick": ev["tick"],
                    "channel": ev["channel"],
                    "controller": ev["controller"],
                    "value": ev["value"],
                })
    return rpns


def inspect_wav_audio(path: Path) -> dict:
    import numpy as np
    import soundfile as sf
    audio, rate = sf.read(path, always_2d=True)
    assert rate == 44100 and audio.shape[1] == 2 and len(audio) > 0
    assert np.isfinite(audio).all()
    peak = float(np.max(np.abs(audio)))
    assert peak > 0.005, f"Audio signal too low: peak={peak}"
    return {"sample_rate": rate, "channels": audio.shape[1],
            "duration_seconds": len(audio) / rate, "peak": peak}


async def main() -> None:
    parser = argparse.ArgumentParser(description="Test native MuseScore MCP export options")
    parser.add_argument("--binary", type=Path, default=Path(os.environ.get("MUSIC_MCP_BINARY", ROOT / "build/windows/musescore-mcp.exe")),
                        help="Path to musescore-mcp executable")
    parser.add_argument("--output", type=Path, default=Path(os.environ.get("MUSIC_MCP_OUTPUT", ROOT / "artifacts/test-export-options")),
                        help="Output directory for test artifacts")
    parser.add_argument("--soundfont", type=Path, default=None,
                        help="Optional soundfont path for WAV export")
    parser.add_argument("--resources", type=Path, default=None,
                        help="Optional resources directory containing instruments.xml")
    args = parser.parse_args()

    binary = args.binary.resolve()
    out_dir = args.output.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    soundfont_path = args.soundfont.resolve() if args.soundfont else None
    if soundfont_path and not soundfont_path.is_file():
        parser.error("SoundFont does not exist")

    protocol_errors: list[str] = []

    class ProtocolLog(logging.Handler):
        def emit(self, record):
            if record.levelno >= logging.ERROR:
                protocol_errors.append(record.getMessage())

    logging.getLogger("mcp.client.stdio").addHandler(ProtocolLog())

    child_env = dict(os.environ)
    if os.environ.get("MUSIC_MCP_CLEAN_ENV"):
        if os.name == "nt":
            child_env = {
                "SystemRoot": os.environ["SystemRoot"],
                "PATH": str(Path(os.environ["SystemRoot"]) / "System32"),
                "TEMP": os.environ["TEMP"],
                "TMP": os.environ["TMP"],
            }
        else:
            child_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}

    child_args = []
    if soundfont_path:
        child_args += ["--soundfont", str(soundfont_path)]
    if args.resources and args.resources.is_dir():
        child_args += ["--resources", str(args.resources.resolve())]
    elif os.environ.get("MUSIC_MCP_RESOURCES"):
        child_args += ["--resources", os.environ["MUSIC_MCP_RESOURCES"]]

    params = StdioServerParameters(command=str(binary), args=child_args, env=child_env, cwd=str(out_dir))

    report = {
        "status": "running",
        "binary": str(binary),
        "soundfont_supplied": bool(soundfont_path),
        "calls": [],
        "evidence": {},
    }
    started = time.perf_counter()

    # Base multi-part score for general exports
    test_score = {
        "title": "Export Options Suite / 选项套件",
        "tempo": 120,
        "parts": [
            {"name": "Violin", "midiProgram": 41, "measures": [bar(67, 69, 71, 74), bar(72, 71, 69, 67)]},
            {"name": "Piano", "midiProgram": 1, "measures": [
                {"notes": [{"pitches": [48, 55, 60], "ticks": 1920}]},
                {"notes": [{"pitches": [50, 57, 62], "ticks": 1920}]},
            ]},
            {"name": "Cello", "midiProgram": 43, "measures": [bar(48, 48, 55, 55), bar(50, 50, 57, 57)]},
        ],
    }

    with (out_dir / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                init_result = await session.initialize()
                report["protocol_version"] = init_result.protocol_version

                async def call(tool_name: str, arguments: dict, expect_error: bool = False) -> dict | None:
                    response = await session.call_tool(tool_name, arguments)
                    assert bool(response.is_error) == expect_error, f"Tool {tool_name} error mismatch: {response}"
                    report["calls"].append({"tool": tool_name, "error": bool(response.is_error)})
                    payload = response.structured_content
                    if payload is None and response.content and hasattr(response.content[0], "text"):
                        try:
                            payload = json.loads(response.content[0].text)
                        except Exception:
                            payload = {"text": response.content[0].text}
                    return payload

                # Create test document
                created = await call("score_create", {"score": test_score})
                doc_id = created["documentId"]
                doc_identity = {"documentId": doc_id}

                # Retrieve baseline representation
                baseline = await call("score_get", doc_identity)
                orig_xml_path = out_dir / "original.musicxml"
                orig_xml_path.write_text(baseline["musicxml"], encoding="utf-8")
                orig_notes, orig_measures = canonical_notes(orig_xml_path)

                # =========================================================================
                # 1. New score_export MSCX uncompressed XML export & import roundtrip
                # =========================================================================
                mscx_path = out_dir / "score.mscx"
                export_mscx_res = await call("score_export", {**doc_identity, "format": "mscx", "path": str(mscx_path)})
                assert mscx_path.is_file() and mscx_path.stat().st_size > 0, "MSCX file not created"
                assert export_mscx_res["bytes"] == mscx_path.stat().st_size

                mscx_content = mscx_path.read_text(encoding="utf-8")
                mscx_tree = ET.fromstring(mscx_content)
                assert mscx_tree.tag == "museScore", f"MSCX root tag was {mscx_tree.tag}, expected museScore"
                assert mscx_tree.find("Score") is not None, "MSCX missing Score element"

                # Import the exported MSCX back via score_open
                opened_mscx = await call("score_open", {"path": str(mscx_path)})
                mscx_doc_id = opened_mscx["documentId"]
                reopened_mscx = await call("score_get", {"documentId": mscx_doc_id})
                reopened_xml_path = out_dir / "reopened_mscx.musicxml"
                reopened_xml_path.write_text(reopened_mscx["musicxml"], encoding="utf-8")
                reopened_notes, reopened_measures = canonical_notes(reopened_xml_path)

                assert orig_notes == reopened_notes, "Musical notes altered during MSCX export/import roundtrip"
                assert orig_measures == reopened_measures, "Measure counts altered during MSCX roundtrip"
                await call("score_close", {"documentId": mscx_doc_id})

                report["evidence"]["mscx_roundtrip"] = {
                    "bytes": mscx_path.stat().st_size,
                    "root_element": mscx_tree.tag,
                    "version": mscx_tree.get("version"),
                    "parts": len(orig_measures),
                    "measures_per_part": orig_measures,
                    "note_events": len(orig_notes),
                    "roundtrip_exact_match": True,
                }

                # =========================================================================
                # 2. PNG DPI sizing 72 vs 144
                # =========================================================================
                png_72_path = out_dir / "score_72dpi.png"
                png_144_path = out_dir / "score_144dpi.png"

                await call("score_export", {**doc_identity, "format": "png", "pngDpi": 72, "path": str(png_72_path)})
                await call("score_export", {**doc_identity, "format": "png", "pngDpi": 144, "path": str(png_144_path)})

                w72, h72 = read_png_dimensions(png_72_path)
                w144, h144 = read_png_dimensions(png_144_path)

                assert w72 > 0 and h72 > 0, f"Invalid 72 DPI dimensions: {w72}x{h72}"
                assert abs(w144 - 2 * w72) <= 1, f"Expected 144 DPI width ~2x 72 DPI: got {w144} vs {w72}"
                assert abs(h144 - 2 * h72) <= 1, f"Expected 144 DPI height ~2x 72 DPI: got {h144} vs {h72}"

                report["evidence"]["png_dpi"] = {
                    "dpi_72_dimensions": {"width": w72, "height": h72, "bytes": png_72_path.stat().st_size},
                    "dpi_144_dimensions": {"width": w144, "height": h144, "bytes": png_144_path.stat().st_size},
                    "width_ratio": round(w144 / w72, 3),
                    "height_ratio": round(h144 / h72, 3),
                }

                # =========================================================================
                # 3. Transparent alpha vs opaque white background
                # =========================================================================
                png_trans_path = out_dir / "score_transparent.png"
                png_opaque_path = out_dir / "score_opaque.png"

                await call("score_export", {**doc_identity, "format": "png", "transparent": True, "path": str(png_trans_path)})
                await call("score_export", {**doc_identity, "format": "png", "transparent": False, "path": str(png_opaque_path)})

                alpha_trans = inspect_png_alpha(png_trans_path)
                alpha_opaque = inspect_png_alpha(png_opaque_path)

                assert alpha_trans["has_transparency"], f"Transparent PNG had no transparent pixels: {alpha_trans}"
                assert alpha_trans["min_alpha"] < 255, f"Transparent PNG minimum alpha is {alpha_trans['min_alpha']}"
                assert not alpha_opaque["has_transparency"], f"Opaque PNG contains transparent pixels: {alpha_opaque}"
                assert alpha_opaque["min_alpha"] == 255 and alpha_opaque["max_alpha"] == 255, (
                    f"Opaque PNG alphas not completely 255: {alpha_opaque}"
                )

                report["evidence"]["png_transparency"] = {
                    "transparent_png": alpha_trans,
                    "opaque_png": alpha_opaque,
                }

                # =========================================================================
                # 4. SVG and PNG drawPageBackground option accepts
                # =========================================================================
                svg_nobg_path = out_dir / "score_nobg.svg"
                svg_withbg_path = out_dir / "score_withbg.svg"
                png_nobg_path = out_dir / "score_nobg.png"
                png_withbg_path = out_dir / "score_withbg.png"

                await call("score_export", {**doc_identity, "format": "svg", "drawPageBackground": False, "path": str(svg_nobg_path)})
                await call("score_export", {**doc_identity, "format": "svg", "drawPageBackground": True, "path": str(svg_withbg_path)})
                await call("score_export", {**doc_identity, "format": "png", "drawPageBackground": False, "path": str(png_nobg_path)})
                await call("score_export", {**doc_identity, "format": "png", "drawPageBackground": True, "path": str(png_withbg_path)})

                svg_nobg_tree = ET.parse(svg_nobg_path).getroot()
                svg_withbg_tree = ET.parse(svg_withbg_path).getroot()
                assert svg_nobg_tree.tag.endswith("svg"), "score_nobg.svg invalid root"
                assert svg_withbg_tree.tag.endswith("svg"), "score_withbg.svg invalid root"
                def white_paths(root):
                    return sum(e.tag.endswith("path") and e.get("fill", "").replace(" ", "")
                               == "rgb(100%,100%,100%)" for e in root.iter())
                assert white_paths(svg_withbg_tree) > white_paths(svg_nobg_tree)
                from PIL import Image
                with Image.open(png_withbg_path) as image:
                    assert image.convert("RGBA").getpixel((0, 0)) == (255, 255, 255, 255)
                with Image.open(png_nobg_path) as image:
                    assert image.convert("RGBA").getpixel((0, 0))[3] == 0
                with Image.open(png_opaque_path) as image:
                    assert image.convert("RGBA").getpixel((0, 0)) == (255, 255, 255, 255)

                svg_nobg_bytes = svg_nobg_path.read_bytes()
                svg_withbg_bytes = svg_withbg_path.read_bytes()

                assert png_nobg_path.stat().st_size > 0 and png_withbg_path.stat().st_size > 0

                report["evidence"]["background_options"] = {
                    "svg_nobg_bytes": len(svg_nobg_bytes),
                    "svg_withbg_bytes": len(svg_withbg_bytes),
                    "png_nobg_bytes": png_nobg_path.stat().st_size,
                    "png_withbg_bytes": png_withbg_path.stat().st_size,
                }

                # =========================================================================
                # 5. Invalid options type / out-of-format / range leave destination sentinel
                #    unchanged and session survives
                # =========================================================================
                invalid_cases = [
                    # Invalid types
                    ("type_pngDpi_str", {"format": "png", "pngDpi": "high"}),
                    ("type_pngDpi_bool", {"format": "png", "pngDpi": True}),
                    ("type_transparent_str", {"format": "png", "transparent": "false"}),
                    ("type_drawPageBackground_str", {"format": "svg", "drawPageBackground": "yes"}),
                    ("type_expandRepeats_num", {"format": "midi", "expandRepeats": 123}),
                    ("type_exportRPNs_str", {"format": "midi", "exportRPNs": "false"}),
                    # Out-of-format options
                    ("out_of_format_pngDpi_svg", {"format": "svg", "pngDpi": 144}),
                    ("out_of_format_pngDpi_midi", {"format": "midi", "pngDpi": 144}),
                    ("out_of_format_transparent_svg", {"format": "svg", "transparent": False}),
                    ("out_of_format_transparent_midi", {"format": "midi", "transparent": False}),
                    ("out_of_format_drawPageBackground_midi", {"format": "midi", "drawPageBackground": True}),
                    ("out_of_format_drawPageBackground_musicxml", {"format": "musicxml", "drawPageBackground": True}),
                    ("out_of_format_expandRepeats_png", {"format": "png", "expandRepeats": True}),
                    ("out_of_format_exportRPNs_png", {"format": "png", "exportRPNs": True}),
                    ("out_of_format_page_midi", {"format": "midi", "page": 0}),
                    # Out-of-range options
                    ("out_of_range_pngDpi_too_low", {"format": "png", "pngDpi": 35}),
                    ("out_of_range_pngDpi_too_high", {"format": "png", "pngDpi": 1201}),
                    ("out_of_range_page_negative", {"format": "svg", "page": -1}),
                    ("out_of_range_page_beyond_pages", {"format": "svg", "page": 999}),
                    # Mix format restrictions (wav/flac/ogg/mp3 only)
                    ("out_of_format_mix_svg", {"format": "svg", "mix": {"master": {"volumeDb": 0.0}}}),
                    ("out_of_format_mix_png", {"format": "png", "mix": {"master": {"volumeDb": 0.0}}}),
                    ("out_of_format_mix_midi", {"format": "midi", "mix": {"master": {"volumeDb": 0.0}}}),
                    ("out_of_format_mix_musicxml", {"format": "musicxml", "mix": {"master": {"volumeDb": 0.0}}}),
                    ("out_of_format_mix_pdf", {"format": "pdf", "mix": {"master": {"volumeDb": 0.0}}}),
                    ("out_of_format_mix_mscz", {"format": "mscz", "mix": {"master": {"volumeDb": 0.0}}}),
                    # Mix range restrictions
                    ("out_of_range_mix_master_volumeDb_high", {"format": "wav", "mix": {"master": {"volumeDb": 24.1}}}),
                    ("out_of_range_mix_master_volumeDb_low", {"format": "wav", "mix": {"master": {"volumeDb": -120.1}}}),
                    ("out_of_range_mix_master_balance_high", {"format": "wav", "mix": {"master": {"balance": 1.1}}}),
                    ("out_of_range_mix_master_balance_low", {"format": "wav", "mix": {"master": {"balance": -1.1}}}),
                    ("out_of_range_mix_track_volumeDb_high", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "volumeDb": 24.1}]}}),
                    ("out_of_range_mix_track_volumeDb_low", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "volumeDb": -120.1}]}}),
                    ("out_of_range_mix_track_balance_high", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "balance": 1.1}]}}),
                    ("out_of_range_mix_track_balance_low", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "balance": -1.1}]}}),
                    ("out_of_range_mix_trackId_negative", {"format": "wav", "mix": {"tracks": [{"trackId": -1}]}}),
                    # Mix unknown / forbidden keys
                    ("unknown_key_mix_root", {"format": "wav", "mix": {"unknownProp": True}}),
                    ("unknown_key_mix_master", {"format": "wav", "mix": {"master": {"unknownProp": True}}}),
                    ("forbidden_key_mix_master_solo", {"format": "wav", "mix": {"master": {"solo": True}}}),
                    ("forbidden_key_mix_master_muted", {"format": "wav", "mix": {"master": {"muted": True}}}),
                    ("forbidden_key_mix_track_forceMute", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "forceMute": True}]}}),
                    ("forbidden_key_mix_track_output", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "output": {}}]}}),
                    ("unknown_key_mix_track", {"format": "wav", "mix": {"tracks": [{"trackId": 0, "unknownProp": 1}]}}),
                    # Semantic mix rejections: duplicate and unknown trackIds
                    ("duplicate_trackId_mix", {"format": "wav", "mix": {"tracks": [{"trackId": 0}, {"trackId": 0}]}}),
                    ("unknown_trackId_mix", {"format": "wav", "mix": {"tracks": [{"trackId": 999999}]}}),
                ]

                sentinel_evidence = []
                for case_name, bad_opts in invalid_cases:
                    sentinel_file = out_dir / f"sentinel_{case_name}.dat"
                    sentinel_payload = f"SENTINEL_UNCHANGED_CONTENT_{case_name}".encode("utf-8")
                    sentinel_file.write_bytes(sentinel_payload)

                    # Export attempt must fail with expect_error=True
                    export_args = {**doc_identity, "path": str(sentinel_file), **bad_opts}
                    await call("score_export", export_args, expect_error=True)

                    # Sentinel file MUST NOT be modified or replaced
                    actual_sentinel = sentinel_file.read_bytes()
                    assert actual_sentinel == sentinel_payload, (
                        f"Destination sentinel was modified for case {case_name}!"
                    )

                    # MCP Session MUST survive: verify immediate valid call
                    alive_check = await call("score_get", doc_identity)
                    assert alive_check["documentId"] == doc_id, f"Session lost document state after {case_name}"

                    sentinel_evidence.append({
                        "case": case_name,
                        "options": bad_opts,
                        "sentinel_preserved": True,
                        "session_survived": True,
                    })

                report["evidence"]["invalid_options_sentinel_preservation"] = {
                    "total_cases_tested": len(sentinel_evidence),
                    "all_sentinels_preserved": True,
                    "session_survived_all": True,
                    "cases": sentinel_evidence,
                }

                # =========================================================================
                # 6. MIDI expandRepeats with 2-measure MusicXML repeat fixture
                # =========================================================================
                opened_repeat = await call("score_open", {"musicxml": REPEAT_FIXTURE_MUSICXML})
                repeat_doc_id = opened_repeat["documentId"]
                repeat_identity = {"documentId": repeat_doc_id}

                midi_expanded_path = out_dir / "repeat_expanded.mid"
                midi_unexpanded_path = out_dir / "repeat_unexpanded.mid"

                await call("score_export", {**repeat_identity, "format": "midi", "expandRepeats": True, "path": str(midi_expanded_path)})
                await call("score_export", {**repeat_identity, "format": "midi", "expandRepeats": False, "path": str(midi_unexpanded_path)})

                midi_expanded = parse_midi_file(midi_expanded_path.read_bytes())
                midi_unexpanded = parse_midi_file(midi_unexpanded_path.read_bytes())

                count_exp, notes_exp = count_note_on_events(midi_expanded)
                count_unexp, notes_unexp = count_note_on_events(midi_unexpanded)

                assert count_exp > count_unexp, (
                    f"expandRepeats=True must produce more Note-On events: got {count_exp} vs {count_unexp}"
                )
                assert count_exp == 2 * count_unexp, (
                    f"Expected 2-measure repeat to double Note-Ons: got {count_exp} vs {count_unexp}"
                )

                repeat_evidence = {
                    "note_ons_expanded": count_exp,
                    "note_ons_unexpanded": count_unexp,
                    "ratio": count_exp / count_unexp if count_unexp else None,
                    "expanded_pitches": [n["pitch"] for n in notes_exp],
                    "unexpanded_pitches": [n["pitch"] for n in notes_unexp],
                }

                # Also test WAV expandRepeats if soundfont is supplied
                if soundfont_path:
                    wav_exp_path = out_dir / "repeat_expanded.wav"
                    wav_unexp_path = out_dir / "repeat_unexpanded.wav"
                    await call("score_export", {**repeat_identity, "format": "wav", "expandRepeats": True, "path": str(wav_exp_path)})
                    await call("score_export", {**repeat_identity, "format": "wav", "expandRepeats": False, "path": str(wav_unexp_path)})

                    wav_exp_info = inspect_wav_audio(wav_exp_path)
                    wav_unexp_info = inspect_wav_audio(wav_unexp_path)

                    assert wav_exp_info["duration_seconds"] > wav_unexp_info["duration_seconds"] + 3.5, (
                        f"WAV repeat expanded duration ({wav_exp_info['duration_seconds']}s) "
                        f"not longer than unexpanded ({wav_unexp_info['duration_seconds']}s)"
                    )
                    repeat_evidence["wav_audio"] = {
                        "expanded_duration": wav_exp_info["duration_seconds"],
                        "unexpanded_duration": wav_unexp_info["duration_seconds"],
                    }

                await call("score_close", repeat_identity)
                report["evidence"]["midi_expand_repeats"] = repeat_evidence

                # =========================================================================
                # 7. MIDI RPN true vs false actual bytes differ and pitch bend parameter
                #    configuration is verified via parsed events
                # =========================================================================
                rpn_true_path = out_dir / "score_rpn_true.mid"
                rpn_false_path = out_dir / "score_rpn_false.mid"

                await call("score_export", {**doc_identity, "format": "midi", "exportRPNs": True, "path": str(rpn_true_path)})
                await call("score_export", {**doc_identity, "format": "midi", "exportRPNs": False, "path": str(rpn_false_path)})

                rpn_true_bytes = rpn_true_path.read_bytes()
                rpn_false_bytes = rpn_false_path.read_bytes()

                assert rpn_true_bytes != rpn_false_bytes, (
                    "MIDI files exported with exportRPNs=True and exportRPNs=False must differ in bytes"
                )

                parsed_rpn_true = parse_midi_file(rpn_true_bytes)
                parsed_rpn_false = parse_midi_file(rpn_false_bytes)

                rpns_in_true = extract_rpn_controllers(parsed_rpn_true)
                rpns_in_false = extract_rpn_controllers(parsed_rpn_false)

                # Upstream exportmidi.cpp writes pitch bend sensitivity (RPN 00 00 = 12 semitones)
                # and resets RPN to 127 127 when exportRPNs is True.
                assert len(rpns_in_true) > 0, "exportRPNs=True must emit RPN controller messages"
                assert len(rpns_in_false) == 0, f"exportRPNs=False must not emit RPN messages: {rpns_in_false}"

                # Assert pitch bend sensitivity parameter message structure:
                # CC 100=0 (LRPN), CC 101=0 (HRPN), CC 6=12 (HDATA)
                pitch_bend_sens_found = any(
                    ev["controller"] == 6 and ev["value"] == 12 for ev in rpns_in_true
                )
                assert pitch_bend_sens_found, "Pitch bend sensitivity data entry (CC 6 = 12) not found in RPN events"

                # Assert underlying musical Note-On events remain identical
                _, notes_rpn_true = count_note_on_events(parsed_rpn_true)
                _, notes_rpn_false = count_note_on_events(parsed_rpn_false)
                assert [(n["tick"], n["channel"], n["pitch"]) for n in notes_rpn_true] == [
                    (n["tick"], n["channel"], n["pitch"]) for n in notes_rpn_false
                ], "Musical Note-On events changed between RPN export options"

                report["evidence"]["rpn_export"] = {
                    "bytes_differ": True,
                    "rpn_true_bytes_count": len(rpn_true_bytes),
                    "rpn_false_bytes_count": len(rpn_false_bytes),
                    "rpn_controllers_emitted_when_true": len(rpns_in_true),
                    "rpn_controllers_emitted_when_false": len(rpns_in_false),
                    "pitch_bend_sensitivity_verified": True,
                    "note_events_identical": True,
                }

                # =========================================================================
                # 8. WAV forwarding only if soundfont supplied
                # =========================================================================
                wav_path = out_dir / "score.wav"
                if soundfont_path:
                    wav_res = await call("score_export", {**doc_identity, "format": "wav", "path": str(wav_path)})
                    assert wav_path.is_file() and wav_path.stat().st_size > 0
                    assert wav_res["bytes"] == wav_path.stat().st_size

                    wav_info = inspect_wav_audio(wav_path)
                    report["evidence"]["wav_export"] = {
                        "executed": True,
                        "soundfont_used": str(soundfont_path),
                        "audio": wav_info,
                    }
                else:
                    sentinel_wav = out_dir / "sentinel_wav_nosoundfont.dat"
                    sentinel_wav.write_bytes(b"SENTINEL_WAV_NO_SOUNDFONT")
                    await call("score_export", {**doc_identity, "format": "wav", "path": str(sentinel_wav)}, expect_error=True)
                    assert sentinel_wav.read_bytes() == b"SENTINEL_WAV_NO_SOUNDFONT"
                    survived = await call("score_get", doc_identity)
                    assert survived["documentId"] == doc_id
                    report["evidence"]["wav_export"] = {
                        "executed": False,
                        "reason": "soundfont not supplied",
                        "error_cleanly_rejected": True,
                        "sentinel_preserved": True,
                        "session_survived": True,
                    }

                # =========================================================================
                # 9. Metadata export with tracks populated from score and consistent with mix
                # =========================================================================
                meta_path = out_dir / "score_metadata.json"
                meta_res = await call("score_export", {**doc_identity, "format": "metadata", "path": str(meta_path)})
                assert meta_path.is_file() and meta_path.stat().st_size > 0
                assert meta_res["bytes"] == meta_path.stat().st_size
                meta_data = json.loads(meta_path.read_text(encoding="utf-8"))
                assert "tracks" in meta_data, "Metadata JSON must contain 'tracks'"
                assert isinstance(meta_data["tracks"], list) and len(meta_data["tracks"]) > 0, (
                    "Metadata 'tracks' must not be empty"
                )
                first_meta_track = meta_data["tracks"][0]
                for required_key in ("trackId", "name", "partId", "instrumentId", "volumeDb", "balance", "solo", "muted", "forceMute"):
                    assert required_key in first_meta_track, f"Metadata track missing required field {required_key}"
                report["evidence"]["metadata_export"] = {
                    "executed": True,
                    "tracks_count": len(meta_data["tracks"]),
                    "first_track": first_meta_track,
                }

                # =========================================================================
                # 10. Metadata stays available for a 16-part score (too many audio stems)
                # =========================================================================
                # score_create caps the composition contract at 15 parts, so the
                # 16-part case is imported exactly as a client would encounter it.
                part_names = [f"Part {index + 1}" for index in range(16)]
                part_list_xml = "".join(
                    f'<score-part id="P{index + 1}"><part-name>{name}</part-name></score-part>'
                    for index, name in enumerate(part_names)
                )
                part_bodies = "".join(
                    f'<part id="P{index + 1}"><measure number="1"><attributes><divisions>1</divisions>'
                    '<time><beats>4</beats><beat-type>4</beat-type></time>'
                    '<clef><sign>G</sign><line>2</line></clef></attributes>'
                    '<note><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration>'
                    '<type>quarter</type></note>'
                    '<note><pitch><step>D</step><octave>4</octave></pitch><duration>1</duration>'
                    '<type>quarter</type></note>'
                    '<note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration>'
                    '<type>quarter</type></note>'
                    '<note><pitch><step>F</step><octave>4</octave></pitch><duration>1</duration>'
                    '<type>quarter</type></note></measure></part>'
                    for index in range(16)
                )
                sixteen_part_xml = (
                    '<?xml version="1.0" encoding="UTF-8"?>'
                    '<score-partwise version="4.0"><part-list>'
                    + part_list_xml + '</part-list>' + part_bodies + '</score-partwise>'
                )
                large_doc = await call("score_open", {"musicxml": sixteen_part_xml})
                large_identity = {"documentId": large_doc["documentId"]}
                large_get = await call("score_get", large_identity)
                assert large_get["metadata"]["tracks"] == [], (
                    "16-part metadata tracks must be empty, got "
                    f"{large_get['metadata'].get('tracks')}"
                )
                large_meta_path = out_dir / "sixteen_parts_metadata.json"
                large_meta_job = await call(
                    "score_export_start",
                    {**large_identity, "format": "metadata", "path": str(large_meta_path)},
                )
                deadline = time.monotonic() + 60
                large_meta_state = None
                while time.monotonic() < deadline:
                    large_meta_state = await call("score_job_get", {"jobId": large_meta_job["jobId"]})
                    if large_meta_state["state"] in ("completed", "failed", "cancelled"):
                        break
                    await asyncio.sleep(0.02)
                assert large_meta_state is not None and large_meta_state["state"] == "completed", large_meta_state
                assert json.loads(large_meta_path.read_text(encoding="utf-8"))["tracks"] == []
                await call("score_close", large_identity)
                report["evidence"]["sixteen_part_metadata"] = {
                    "executed": True,
                    "score_get_succeeded": True,
                    "tracks": [],
                    "background_metadata": "completed",
                }

                # Clean up main document
                await call("score_close", doc_identity)

    assert not protocol_errors, f"Protocol errors during run: {protocol_errors}"
    stderr_log = (out_dir / "stderr.log").read_text(encoding="utf-8")
    assert "ERROR:" not in stderr_log, f"Native engine reported ERROR in stderr: {stderr_log}"

    report["seconds"] = round(time.perf_counter() - started, 3)
    report["status"] = "passed"

    evidence_json = out_dir / "export_options_evidence.json"
    evidence_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "calls"}, indent=2))


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), timeout=180))
