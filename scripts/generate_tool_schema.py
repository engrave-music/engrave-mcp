"""Write the source-controlled MCP schema when the public contract changes."""
import json
from pathlib import Path


def obj(properties, required):
    return dict(type="object", properties=properties, required=required, additionalProperties=False)


string = {"type": "string", "minLength": 1}
doc_id = {"documentId": string}
note = obj({"pitches": {"type": "array", "items": {"type": "integer", "minimum": 12, "maximum": 127},
                        "maxItems": 8, "uniqueItems": True},
            "ticks": {"type": "integer", "enum": [120, 240, 480, 960, 1920]}}, ["pitches", "ticks"])
measure = obj({"notes": {"type": "array", "items": note, "minItems": 1, "maxItems": 16}}, ["notes"])
measures = {"type": "array", "items": measure, "minItems": 1, "maxItems": 128}
part = obj({"name": {"type": "string", "maxLength": 256},
            "midiProgram": {"type": "integer", "minimum": 1, "maximum": 128},
            "measures": measures}, ["name", "midiProgram", "measures"])
score = obj({"title": {"type": "string", "maxLength": 256},
             "tempo": {"type": "integer", "minimum": 20, "maximum": 300},
             "parts": {"type": "array", "items": part, "minItems": 1, "maxItems": 15}}, ["parts"])


def tool(name, description, schema):
    return dict(name=name, description=description, inputSchema=schema)


tools = [
    tool("score_create", "Create an editable score. Fixed 4/4, 480 ticks/quarter; each bar totals 1920 ticks. "
         "pitches is a MIDI chord; [] means rest. All parts have equal measure counts. tempo defaults to 100.", obj({"score": score}, ["score"])),
    tool("score_open", "Import MusicXML text or local MusicXML/MXL/MSCZ/MSCX path (exactly one). "
         "Imported documents support reading/export; structured measure replacement requires score_create.",
         obj({"path": string, "musicxml": string}, [])),
    tool("score_get", "Read title, pages, parts, metadata, canonical MusicXML and editable composition if present.", obj(doc_id, ["documentId"])),
    tool("score_replace_measures", "Atomically replace existing measures in a score_create document. "
         "partIndex and startMeasure are zero-based. Invalid replacement leaves original intact.",
         obj({**doc_id, "partIndex": {"type": "integer", "minimum": 0, "maximum": 14},
              "startMeasure": {"type": "integer", "minimum": 0, "maximum": 127}, "measures": measures},
             ["documentId", "partIndex", "startMeasure", "measures"])),
    tool("score_export", "Export to a local file atomically, replacing an existing file. Parent directory must exist. "
         "page is zero-based for SVG/PNG; PDF exports all pages. WAV requires --soundfont at startup.",
         obj({**doc_id, "format": {"type": "string", "enum": ["musicxml", "mxl", "mscz", "midi", "svg", "png", "pdf", "metadata", "wav"]},
              "path": string, "page": {"type": "integer", "minimum": 0}}, ["documentId", "format", "path"])),
    tool("score_close", "Close a document and release score/playback resources. Handles expire after close.", obj(doc_id, ["documentId"])),
]
(Path(__file__).resolve().parents[1] / "native/tools.json").write_text(json.dumps(tools, indent=2) + "\n", encoding="utf-8")
