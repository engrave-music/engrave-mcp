"""Shared native edit fixture helpers."""

from __future__ import annotations

import xml.etree.ElementTree as ET


def fixture(tied: bool = False) -> str:
    root = ET.Element("score-partwise", version="4.0")
    listing = ET.SubElement(root, "part-list")
    for pi in range(2):
        score_part = ET.SubElement(listing, "score-part", id=f"P{pi + 1}")
        ET.SubElement(score_part, "part-name").text = f"Part {pi + 1}"
    for pi in range(2):
        part = ET.SubElement(root, "part", id=f"P{pi + 1}")
        for mi in range(2):
            measure = ET.SubElement(part, "measure", number=str(mi + 1))
            if mi == 0:
                attrs = ET.SubElement(measure, "attributes")
                ET.SubElement(attrs, "divisions").text = "1"
                time = ET.SubElement(attrs, "time")
                ET.SubElement(time, "beats").text = "4"
                ET.SubElement(time, "beat-type").text = "4"
                clef = ET.SubElement(attrs, "clef")
                ET.SubElement(clef, "sign").text = "G"
                ET.SubElement(clef, "line").text = "2"
            for ni, step in enumerate(["C", "D", "E", "F"]):
                note = ET.SubElement(measure, "note")
                p = ET.SubElement(note, "pitch")
                ET.SubElement(p, "step").text = "C" if tied and pi == 0 and ni < 2 else step
                ET.SubElement(p, "octave").text = str(4 - pi)
                ET.SubElement(note, "duration").text = "1"
                if tied and pi == 0 and mi == 0 and ni < 2:
                    kind = "start" if ni == 0 else "stop"
                    ET.SubElement(note, "tie", type=kind)
                ET.SubElement(note, "voice").text = "1"
                ET.SubElement(note, "type").text = "quarter"
                if tied and pi == 0 and mi == 0 and ni < 2:
                    ET.SubElement(ET.SubElement(note, "notations"), "tied", type=kind)
    return ET.tostring(root, encoding="unicode")
