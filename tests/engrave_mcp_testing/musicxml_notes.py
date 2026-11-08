"""Canonical MusicXML note events for roundtrip assertions."""

import xml.etree.ElementTree as ET
from fractions import Fraction


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
