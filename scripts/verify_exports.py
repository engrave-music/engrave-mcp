"""Check the native smoke-test exports against the MusicXML fixture."""

import argparse
import json
import xml.etree.ElementTree as ET
from fractions import Fraction
from pathlib import Path


def canonical_notes(path):
    score = ET.parse(path).getroot()
    events = []
    measures = []
    for part_index, part in enumerate(score.findall("part")):
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    original, measures = canonical_notes(args.source)
    exported, exported_measures = canonical_notes(args.output / "score.musicxml")
    if original != exported or measures != exported_measures:
        raise ValueError("MusicXML musical content changed during native roundtrip")
    if not (args.output / "score.mid").read_bytes().startswith(b"MThd\x00\x00\x00\x06"):
        raise ValueError("Invalid MIDI header")
    if not (args.output / "score.pdf").read_bytes().startswith(b"%PDF-"):
        raise ValueError("Invalid PDF header")
    svg = ET.parse(args.output / "score.svg").getroot()
    if not svg.tag.endswith("svg") or not any(element.tag.endswith("path") for element in svg.iter()):
        raise ValueError("Invalid SVG root")
    metadata = json.loads((args.output / "metadata.json").read_text(encoding="utf-8"))
    if not isinstance(metadata, dict):
        raise ValueError("Expected metadata object")
    report = {
        "musicxml_roundtrip": "exact note/rest, offset, duration, pitch, voice, staff and tie match",
        "parts": len(measures),
        "measures_per_part": measures,
        "events": len(original),
        "midi_pdf_svg": "format smoke checks only; rendering/audio not assessed",
    }
    (args.output / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
