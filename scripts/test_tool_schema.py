"""Contract tests for the native MCP tool schema, with an optional live lane.

The pure lane validates native/tools.json with jsonschema's Draft 2020-12
validator using independently authored user scenarios. The live lane starts
the native host, compares tools/list with the file, then creates a tiny score
and exercises valid and rejected tools/call requests.
"""
from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from typing import Any

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "native" / "tools.json"
PROTOCOL_VERSION = "2025-11-25"
NUL = "\x00"

EXPECTED_TOOL_NAMES = [
    "score_create",
    "score_open",
    "score_get",
    "score_replace_measures",
    "score_export",
    "score_close",
    "score_query_region",
    "score_set_pitches",
    "score_generate_excerpts",
    "score_list_excerpts",
    "runtime_add_font",
    "runtime_set_soundfont",
    "score_export_start",
    "score_job_get",
    "score_job_cancel",
    "score_audio_tracks",
]

EXPORT_FORMATS = [
    "musicxml",
    "mxl",
    "mscz",
    "mscx",
    "midi",
    "svg",
    "png",
    "pdf",
    "metadata",
    "measurePositions",
    "segmentPositions",
    "wav",
    "flac",
    "ogg",
    "mp3",
]

OPEN_FORMATS = [
    "mscz",
    "mscx",
    "mxl",
    "musicxml",
    "xml",
    "midi",
    "mid",
    "kar",
    "gp",
    "gp3",
    "gp4",
    "gp5",
    "gp6",
    "gp7",
    "gpx",
    "gtp",
    "ptb",
]


def bar(*pitches: int | None) -> dict[str, Any]:
    return {
        "notes": [
            {"pitches": [] if pitch is None else [pitch], "ticks": 480}
            for pitch in pitches
        ]
    }


def load_tools(path: Path) -> list[dict[str, Any]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, list) or not raw:
        raise AssertionError(f"{path} must contain a non-empty array of tools")
    tools: list[dict[str, Any]] = []
    for position, tool in enumerate(raw, 1):
        if not isinstance(tool, dict):
            raise AssertionError(f"tool {position} is not an object")
        for key in ("name", "description", "inputSchema"):
            if key not in tool:
                raise AssertionError(f"tool {position} is missing {key}")
        if not isinstance(tool["name"], str) or not tool["name"]:
            raise AssertionError(f"tool {position} has an invalid name")
        if not isinstance(tool["description"], str) or not tool["description"]:
            raise AssertionError(f"{tool['name']} has an empty description")
        if not isinstance(tool["inputSchema"], dict):
            raise AssertionError(f"{tool['name']} inputSchema is not an object")
        tools.append(tool)
    return tools


def object_schemas(node: Any, path: str = "$"):
    if isinstance(node, dict):
        if node.get("type") == "object":
            yield path, node
        for key, value in node.items():
            yield from object_schemas(value, f"{path}.{key}")
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from object_schemas(value, f"{path}[{index}]")


def check_schema_contracts(tools: list[dict[str, Any]]) -> None:
    names = [tool["name"] for tool in tools]
    if names != EXPECTED_TOOL_NAMES:
        raise AssertionError(
            f"tool set/order mismatch\nexpected: {EXPECTED_TOOL_NAMES}\nactual:   {names}"
        )

    for tool in tools:
        schema = tool["inputSchema"]
        try:
            Draft202012Validator.check_schema(schema)
        except Exception as exc:  # SchemaError carries the useful location/message.
            raise AssertionError(f"{tool['name']} is not a valid Draft 2020-12 schema: {exc}") from exc
        for path, object_schema in object_schemas(schema):
            if object_schema.get("additionalProperties") is not False:
                raise AssertionError(
                    f"{tool['name']} object schema {path} must set additionalProperties=false"
                )

    for name in ("score_replace_measures", "score_set_pitches"):
        operation_id = _tool_schema(tools, name)["properties"]["operationId"]
        description = operation_id.get("description", "").lower()
        if "maxLength" in operation_id:
            raise AssertionError(f"{name}.operationId must not imply a unicode-character limit")
        if "bytes" not in description:
            raise AssertionError(f"{name}.operationId must describe its 128-byte limit")

    open_format = _tool_schema(tools, "score_open")["properties"]["format"]
    if open_format.get("enum") != OPEN_FORMATS:
        raise AssertionError(
            f"score_open.format must enumerate the path-route input formats: {OPEN_FORMATS}"
        )
    if "pattern" not in _tool_schema(tools, "score_open")["properties"]["data"]:
        raise AssertionError("score_open.data must constrain its base64 alphabet")


def _tool_schema(tools: list[dict[str, Any]], name: str) -> dict[str, Any]:
    return next(tool["inputSchema"] for tool in tools if tool["name"] == name)


@dataclass(frozen=True)
class Case:
    name: str
    tool: str
    arguments: dict[str, Any]
    valid: bool


def build_cases() -> list[Case]:
    cases: list[Case] = []

    def add(name: str, tool: str, arguments: dict[str, Any], valid: bool) -> None:
        cases.append(Case(name, tool, arguments, valid))

    doc = {"documentId": "score-1"}
    create_score = {
        "score": {
            "title": "Contract",
            "tempo": 120,
            "parts": [
                {
                    "name": "Piano",
                    "midiProgram": 1,
                    "measures": [
                        {
                            "notes": [
                                {"pitches": [60], "ticks": 480},
                                {"pitches": [], "ticks": 480},
                                {"pitches": [64], "ticks": 480},
                                {"pitches": [], "ticks": 480},
                            ]
                        }
                    ],
                }
            ],
        }
    }
    replacement = {
        "documentId": "score-1",
        "partIndex": 0,
        "startMeasure": 0,
        "measures": [bar(60, 62, 64, 65)],
    }
    region = {
        "documentId": "score-1",
        "partIndex": 0,
        "firstMeasure": 0,
        "measureCount": 1,
    }
    pitch_change = {
        "documentId": "score-1",
        "baseRevision": 1,
        "operationId": "pitch-op-1",
        "changes": [{"noteId": "AAAAAA_AAAAAA", "pitch": 60}],
    }
    export = {"documentId": "score-1", "format": "musicxml", "path": "out.musicxml"}
    async_export = dict(export)
    job = {"jobId": "job-1"}

    # score_create
    add("create minimal", "score_create", create_score, True)
    add("create tempo bound low", "score_create",
        {"score": {**create_score["score"], "tempo": 20}}, True)
    add("create tempo bound high", "score_create",
        {"score": {**create_score["score"], "tempo": 300}}, True)
    add("create tempo below range", "score_create",
        {"score": {**create_score["score"], "tempo": 19}}, False)
    add("create tempo float", "score_create",
        {"score": {**create_score["score"], "tempo": 120.5}}, False)
    add("create unknown root field", "score_create",
        {"score": create_score["score"], "unexpected": 1}, False)
    add("create unknown score field", "score_create",
        {"score": {**create_score["score"], "unexpected": 1}}, False)
    add("create unknown part field", "score_create",
        {"score": {"parts": [{**create_score["score"]["parts"][0], "unexpected": 1}]}}, False)
    add("create unknown note field", "score_create",
        {"score": {"parts": [{"name": "P", "midiProgram": 1, "measures": [
            {"notes": [{"pitches": [60], "ticks": 480, "unexpected": 1}]}
        ]}]}}, False)
    add("create title NUL", "score_create",
        {"score": {**create_score["score"], "title": "a" + NUL + "b"}}, False)
    add("create part name NUL", "score_create",
        {"score": {"parts": [{**create_score["score"]["parts"][0], "name": "a" + NUL}]}}, False)

    # score_open: exactly one of path, musicxml or data; format only with data.
    base64_doc = "U2NvcmU="
    add("open path only", "score_open", {"path": "scores/a.musicxml"}, True)
    add("open musicxml only", "score_open", {"musicxml": "<score-partwise/>"}, True)
    add("open data only", "score_open", {"data": base64_doc, "format": "mscz"}, True)
    for fmt in OPEN_FORMATS:
        add(f"open data format {fmt}", "score_open",
            {"data": base64_doc, "format": fmt}, True)
    add("open both inputs", "score_open",
        {"path": "scores/a.musicxml", "musicxml": "<score-partwise/>"}, False)
    add("open data and path", "score_open",
        {"path": "scores/a.musicxml", "data": base64_doc, "format": "mscz"}, False)
    add("open data and musicxml", "score_open",
        {"musicxml": "<score-partwise/>", "data": base64_doc, "format": "musicxml"}, False)
    add("open data missing format", "score_open", {"data": base64_doc}, False)
    add("open format without data", "score_open",
        {"path": "scores/a.musicxml", "format": "musicxml"}, False)
    add("open musicxml with format", "score_open",
        {"musicxml": "<score-partwise/>", "format": "musicxml"}, False)
    add("open data unknown format", "score_open",
        {"data": base64_doc, "format": "aac"}, False)
    add("open data bad character", "score_open",
        {"data": "U2NvcmU!", "format": "mscz"}, False)
    add("open data unpadded", "score_open",
        {"data": "U2NvcmU", "format": "mscz"}, False)
    add("open data excess padding", "score_open",
        {"data": "U2NvcmU===", "format": "mscz"}, False)
    add("open data misplaced padding", "score_open",
        {"data": "U2Nvcm=A=", "format": "mscz"}, False)
    add("open data empty", "score_open", {"data": "", "format": "mscz"}, False)
    add("open data NUL", "score_open",
        {"data": "U2NvcmU" + NUL, "format": "mscz"}, False)
    add("open neither input", "score_open", {}, False)
    add("open empty path", "score_open", {"path": ""}, False)
    add("open NUL path", "score_open", {"path": "a" + NUL + ".musicxml"}, False)
    add("open NUL musicxml", "score_open", {"musicxml": "<x>" + NUL}, False)
    add("open unknown field", "score_open", {"path": "a.mxl", "unexpected": 1}, False)

    # score_get
    add("get master", "score_get", doc, True)
    add("get default excerpt", "score_get", {**doc, "excerptId": -1}, True)
    add("get excerpt zero", "score_get", {**doc, "excerptId": 0}, True)
    add("get excerpt int max", "score_get", {**doc, "excerptId": 2147483647}, True)
    add("get excerpt below -1", "score_get", {**doc, "excerptId": -2}, False)
    add("get excerpt above int", "score_get", {**doc, "excerptId": 2147483648}, False)
    add("get excerpt float", "score_get", {**doc, "excerptId": 1.5}, False)
    add("get excerpt bool", "score_get", {**doc, "excerptId": True}, False)
    add("get missing document", "score_get", {}, False)
    add("get malformed document", "score_get", {"documentId": "score-0"}, False)
    add("get NUL document", "score_get", {"documentId": "score-1" + NUL}, False)

    # score_replace_measures: optional baseRevision+operationId pair.
    add("replace without context", "score_replace_measures", replacement, True)
    add("replace truncation without context", "score_replace_measures",
        {**replacement, "measures": [bar(60)]}, True)
    add("replace with paired context", "score_replace_measures",
        {**replacement, "baseRevision": 1, "operationId": "replace-op-1"}, True)
    add("replace only baseRevision", "score_replace_measures",
        {**replacement, "baseRevision": 1}, False)
    add("replace only operationId", "score_replace_measures",
        {**replacement, "operationId": "replace-op-1"}, False)
    add("replace baseRevision zero", "score_replace_measures",
        {**replacement, "baseRevision": 0, "operationId": "op"}, False)
    add("replace baseRevision float", "score_replace_measures",
        {**replacement, "baseRevision": 1.5, "operationId": "op"}, False)
    add("replace baseRevision bool", "score_replace_measures",
        {**replacement, "baseRevision": True, "operationId": "op"}, False)
    add("replace partIndex negative", "score_replace_measures",
        {**replacement, "partIndex": -1}, False)
    add("replace partIndex above bound", "score_replace_measures",
        {**replacement, "partIndex": 15}, False)
    add("replace partIndex float", "score_replace_measures",
        {**replacement, "partIndex": 1.5}, False)
    add("replace partIndex bool", "score_replace_measures",
        {**replacement, "partIndex": False}, False)
    add("replace startMeasure negative", "score_replace_measures",
        {**replacement, "startMeasure": -1}, False)
    add("replace startMeasure above bound", "score_replace_measures",
        {**replacement, "startMeasure": 128}, False)
    add("replace empty measures", "score_replace_measures",
        {**replacement, "measures": []}, False)
    add("replace missing measures", "score_replace_measures",
        {"documentId": "score-1", "partIndex": 0, "startMeasure": 0}, False)
    add("replace unknown measure field", "score_replace_measures",
        {**replacement, "measures": [{"notes": [{"pitches": [60], "ticks": 480}], "x": 1}]}, False)
    add("replace note pitch below range", "score_replace_measures",
        {**replacement, "measures": [{"notes": [{"pitches": [11], "ticks": 480}]}]}, False)
    add("replace note pitch above range", "score_replace_measures",
        {**replacement, "measures": [{"notes": [{"pitches": [128], "ticks": 480}]}]}, False)
    add("replace note pitch float", "score_replace_measures",
        {**replacement, "measures": [{"notes": [{"pitches": [60.5], "ticks": 480}]}]}, False)
    add("replace note ticks unsupported", "score_replace_measures",
        {**replacement, "measures": [{"notes": [{"pitches": [60], "ticks": 480.5}]}]}, False)
    add("replace empty operationId", "score_replace_measures",
        {**replacement, "baseRevision": 1, "operationId": ""}, False)
    add("replace NUL operationId", "score_replace_measures",
        {**replacement, "baseRevision": 1, "operationId": "a" + NUL}, False)

    # score_query_region
    add("query base", "score_query_region", region, True)
    add("query upper bounds", "score_query_region",
        {**region, "partIndex": 255, "firstMeasure": 1023, "measureCount": 128, "voice": 3}, True)
    add("query voice zero", "score_query_region", {**region, "voice": 0}, True)
    add("query missing partIndex", "score_query_region",
        {"documentId": "score-1", "firstMeasure": 0, "measureCount": 1}, False)
    add("query missing firstMeasure", "score_query_region",
        {"documentId": "score-1", "partIndex": 0, "measureCount": 1}, False)
    add("query missing measureCount", "score_query_region",
        {"documentId": "score-1", "partIndex": 0, "firstMeasure": 0}, False)
    add("query partIndex negative", "score_query_region", {**region, "partIndex": -1}, False)
    add("query partIndex above bound", "score_query_region", {**region, "partIndex": 256}, False)
    add("query partIndex float", "score_query_region", {**region, "partIndex": 1.5}, False)
    add("query partIndex bool", "score_query_region", {**region, "partIndex": True}, False)
    add("query firstMeasure negative", "score_query_region", {**region, "firstMeasure": -1}, False)
    add("query firstMeasure above bound", "score_query_region",
        {**region, "firstMeasure": 1024}, False)
    add("query measureCount zero", "score_query_region", {**region, "measureCount": 0}, False)
    add("query measureCount above bound", "score_query_region",
        {**region, "measureCount": 129}, False)
    add("query measureCount float", "score_query_region", {**region, "measureCount": 1.5}, False)
    add("query voice negative", "score_query_region", {**region, "voice": -1}, False)
    add("query voice above bound", "score_query_region", {**region, "voice": 4}, False)
    add("query voice bool", "score_query_region", {**region, "voice": False}, False)
    add("query entityId is unknown", "score_query_region", {**region, "excerptId": 0}, False)

    # score_set_pitches
    add("pitch base", "score_set_pitches", pitch_change, True)
    add("pitch 0 and 127", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": 0},
                                     {"noteId": "B_B", "pitch": 127}]}, True)
    add("pitch max batch", "score_set_pitches",
        {**pitch_change, "changes": [
            {"noteId": f"A_{index}", "pitch": 60} for index in range(256)
        ]}, True)
    add("pitch empty changes", "score_set_pitches", {**pitch_change, "changes": []}, False)
    add("pitch oversized batch", "score_set_pitches",
        {**pitch_change, "changes": [
            {"noteId": f"A_{index}", "pitch": 60} for index in range(257)
        ]}, False)
    add("pitch missing baseRevision", "score_set_pitches",
        {key: value for key, value in pitch_change.items() if key != "baseRevision"}, False)
    add("pitch zero baseRevision", "score_set_pitches",
        {**pitch_change, "baseRevision": 0}, False)
    add("pitch float baseRevision", "score_set_pitches",
        {**pitch_change, "baseRevision": 1.5}, False)
    add("pitch bool baseRevision", "score_set_pitches",
        {**pitch_change, "baseRevision": True}, False)
    add("pitch missing operationId", "score_set_pitches",
        {key: value for key, value in pitch_change.items() if key != "operationId"}, False)
    add("pitch empty operationId", "score_set_pitches",
        {**pitch_change, "operationId": ""}, False)
    add("pitch NUL operationId", "score_set_pitches",
        {**pitch_change, "operationId": "a" + NUL}, False)
    add("pitch negative", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": -1}]}, False)
    add("pitch above 127", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": 128}]}, False)
    add("pitch float", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": 60.5}]}, False)
    add("pitch bool", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": True}]}, False)
    add("pitch empty noteId", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "", "pitch": 60}]}, False)
    add("pitch NUL noteId", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A" + NUL + "_B", "pitch": 60}]}, False)
    add("pitch malformed noteId", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "not-a-note-id", "pitch": 60}]}, False)
    add("pitch oversized noteId", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A" * 24 + "_B", "pitch": 60}]}, False)
    add("pitch unknown change field", "score_set_pitches",
        {**pitch_change, "changes": [{"noteId": "A_A", "pitch": 60, "x": 1}]}, False)

    # score_export: sync parser options.
    for fmt in EXPORT_FORMATS:
        add(f"export {fmt} minimal", "score_export",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}"}, True)
    add("export page SVG", "score_export",
        {"documentId": "score-1", "format": "svg", "path": "out.svg", "page": 0}, True)
    add("export page int max", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "page": 2147483647}, True)
    add("export png options", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "pngDpi": 36, "drawPageBackground": True, "transparent": False}, True)
    add("export png max dpi", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 1200}, True)
    add("export svg background", "score_export",
        {"documentId": "score-1", "format": "svg", "path": "out.svg",
         "drawPageBackground": True}, True)
    add("export midi playback options", "score_export",
        {"documentId": "score-1", "format": "midi", "path": "out.mid",
         "expandRepeats": False, "exportRPNs": False}, True)
    add("export wav playback options", "score_export",
        {"documentId": "score-1", "format": "wav", "path": "out.wav",
         "expandRepeats": False, "exportRPNs": False}, True)
    for fmt in ("flac", "ogg", "mp3"):
        add(f"export {fmt} sync playback options", "score_export",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}",
             "expandRepeats": False, "exportRPNs": False}, True)
    add("export excerpt default", "score_export",
        {**export, "excerptId": -1}, True)
    add("export excerpt zero", "score_export", {**export, "excerptId": 0}, True)
    add("export excerpt int max", "score_export",
        {**export, "excerptId": 2147483647}, True)
    add("export excerpt below -1", "score_export", {**export, "excerptId": -2}, False)
    add("export excerpt above int", "score_export",
        {**export, "excerptId": 2147483648}, False)
    add("export excerpt float", "score_export", {**export, "excerptId": 1.5}, False)
    add("export page negative", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "page": -1}, False)
    add("export page above int", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "page": 2147483648}, False)
    add("export page on midi", "score_export",
        {"documentId": "score-1", "format": "midi", "path": "out.mid", "page": 0}, False)
    add("export pngDpi on svg", "score_export",
        {"documentId": "score-1", "format": "svg", "path": "out.svg", "pngDpi": 144}, False)
    add("export pngDpi below range", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 35}, False)
    add("export pngDpi above range", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 1201}, False)
    add("export pngDpi float", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 144.5}, False)
    add("export pngDpi bool", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": True}, False)
    add("export transparent on svg", "score_export",
        {"documentId": "score-1", "format": "svg", "path": "out.svg",
         "transparent": False}, False)
    add("export background on midi", "score_export",
        {"documentId": "score-1", "format": "midi", "path": "out.mid",
         "drawPageBackground": True}, False)
    add("export playback option on png", "score_export",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "expandRepeats": False}, False)
    add("export RPN option on musicxml", "score_export",
        {**export, "exportRPNs": False}, False)
    add("export unknown format", "score_export",
        {"documentId": "score-1", "format": "aac", "path": "out.aac"}, False)
    add("export empty path", "score_export", {**export, "path": ""}, False)
    add("export NUL path", "score_export", {**export, "path": "out" + NUL}, False)
    add("export missing document", "score_export",
        {"format": "musicxml", "path": "out.musicxml"}, False)
    add("export missing format", "score_export",
        {"documentId": "score-1", "path": "out.musicxml"}, False)
    add("export missing path", "score_export",
        {"documentId": "score-1", "format": "musicxml"}, False)

    # Background exports share the synchronous export option contract.
    for fmt in EXPORT_FORMATS:
        add(f"async {fmt} minimal", "score_export_start",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}"}, True)
    add("async page SVG", "score_export_start",
        {"documentId": "score-1", "format": "svg", "path": "out.svg", "page": 0}, True)
    add("async page max", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "page": 2147483647}, True)
    add("async png options", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "pngDpi": 36, "drawPageBackground": True, "transparent": False}, True)
    add("async png max dpi", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 1200}, True)
    add("async svg background", "score_export_start",
        {"documentId": "score-1", "format": "svg", "path": "out.svg",
         "drawPageBackground": True}, True)
    for fmt in ("midi", "wav"):
        add(f"async {fmt} playback options", "score_export_start",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}",
             "expandRepeats": False, "exportRPNs": False}, True)
    add("async expected revision", "score_export_start",
        {**async_export, "expectedRevision": 1}, True)
    add("async expected revision uint64 max", "score_export_start",
        {**async_export, "expectedRevision": 18446744073709551615}, True)
    add("async excerpt default", "score_export_start",
        {**async_export, "excerptId": -1}, True)
    add("async excerpt zero", "score_export_start",
        {**async_export, "excerptId": 0}, True)
    add("async excerpt max", "score_export_start",
        {**async_export, "excerptId": 2147483647}, True)
    add("async excerpt above parser max", "score_export_start",
        {**async_export, "excerptId": 2147483648}, False)
    add("async excerpt below -1", "score_export_start",
        {**async_export, "excerptId": -2}, False)
    add("async excerpt float", "score_export_start",
        {**async_export, "excerptId": 1.5}, False)
    add("async page negative", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png", "page": -1}, False)
    add("async page above parser max", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "page": 2147483648}, False)
    add("async page on midi", "score_export_start",
        {"documentId": "score-1", "format": "midi", "path": "out.mid", "page": 0}, False)
    add("async pngDpi on svg", "score_export_start",
        {"documentId": "score-1", "format": "svg", "path": "out.svg", "pngDpi": 144}, False)
    add("async pngDpi below range", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 35}, False)
    add("async pngDpi above range", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png", "pngDpi": 1201}, False)
    add("async transparent on svg", "score_export_start",
        {"documentId": "score-1", "format": "svg", "path": "out.svg",
         "transparent": False}, False)
    add("async background on midi", "score_export_start",
        {"documentId": "score-1", "format": "midi", "path": "out.mid",
         "drawPageBackground": True}, False)
    for fmt in ("flac", "ogg", "mp3"):
        add(f"async {fmt} playback options", "score_export_start",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}",
             "expandRepeats": False}, True)
        add(f"async {fmt} RPN options", "score_export_start",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}",
             "exportRPNs": False}, True)
    add("async playback option on png", "score_export_start",
        {"documentId": "score-1", "format": "png", "path": "out.png",
         "expandRepeats": False}, False)
    add("async expectedRevision zero", "score_export_start",
        {**async_export, "expectedRevision": 0}, False)
    add("async expectedRevision negative", "score_export_start",
        {**async_export, "expectedRevision": -1}, False)
    add("async expectedRevision float", "score_export_start",
        {**async_export, "expectedRevision": 1.5}, False)
    add("async expectedRevision bool", "score_export_start",
        {**async_export, "expectedRevision": True}, False)
    add("async unknown format", "score_export_start",
        {"documentId": "score-1", "format": "aac", "path": "out.aac"}, False)
    add("async empty path", "score_export_start", {**async_export, "path": ""}, False)
    add("async NUL path", "score_export_start",
        {**async_export, "path": "out" + NUL}, False)
    add("async missing document", "score_export_start",
        {"format": "musicxml", "path": "out.musicxml"}, False)
    add("async missing format", "score_export_start",
        {"documentId": "score-1", "path": "out.musicxml"}, False)
    add("async missing path", "score_export_start",
        {"documentId": "score-1", "format": "musicxml"}, False)

    # score_close, excerpts and runtime tools.
    add("close base", "score_close", doc, True)
    add("close missing document", "score_close", {}, False)
    add("generate excerpts", "score_generate_excerpts", doc, True)
    add("list excerpts", "score_list_excerpts", doc, True)
    add("generate excerpts missing document", "score_generate_excerpts", {}, False)
    add("add font", "runtime_add_font", {"path": "fonts/Test.ttf"}, True)
    add("add font empty path", "runtime_add_font", {"path": ""}, False)
    add("add font NUL path", "runtime_add_font", {"path": "a" + NUL}, False)
    add("set soundfont", "runtime_set_soundfont", {"path": "soundfont.sf2"}, True)
    add("set soundfont empty path", "runtime_set_soundfont", {"path": ""}, False)
    add("set soundfont NUL path", "runtime_set_soundfont", {"path": "a" + NUL}, False)

    # Job handles.
    for tool in ("score_job_get", "score_job_cancel"):
        add(f"{tool} base", tool, job, True)
        add(f"{tool} missing jobId", tool, {}, False)
        add(f"{tool} empty jobId", tool, {"jobId": ""}, False)
        add(f"{tool} NUL jobId", tool, {"jobId": "job" + NUL}, False)
        add(f"{tool} non-string jobId", tool, {"jobId": 1}, False)

    # Every tool rejects unknown top-level fields.
    unknown_bases = {
        "score_create": create_score,
        "score_open": {"path": "a.musicxml"},
        "score_get": doc,
        "score_replace_measures": replacement,
        "score_export": export,
        "score_close": doc,
        "score_query_region": region,
        "score_set_pitches": pitch_change,
        "score_generate_excerpts": doc,
        "score_list_excerpts": doc,
        "runtime_add_font": {"path": "fonts/Test.ttf"},
        "runtime_set_soundfont": {"path": "soundfont.sf2"},
        "score_export_start": async_export,
        "score_job_get": job,
        "score_job_cancel": job,
    }
    for tool, arguments in unknown_bases.items():
        add(f"{tool} unknown top-level field", tool, {**arguments, "unexpected": 1}, False)

    # score_audio_tracks contract
    add("score_audio_tracks master", "score_audio_tracks", doc, True)
    add("score_audio_tracks excerpt", "score_audio_tracks", {**doc, "excerptId": 0}, True)
    add("score_audio_tracks unknown", "score_audio_tracks", {**doc, "extra": True}, False)

    # mix export option on score_export and score_export_start
    audio_formats = ("wav", "flac", "ogg", "mp3")
    non_audio_formats = ("svg", "png", "midi", "musicxml", "mxl", "mscz", "mscx", "pdf", "metadata", "measurePositions", "segmentPositions")
    valid_mixes = [
        ("empty mix", {}),
        ("master only", {"master": {"volumeDb": -6.0, "balance": 0.5}}),
        ("master volumeDb bounds low", {"master": {"volumeDb": -120}}),
        ("master volumeDb bounds high", {"master": {"volumeDb": 24}}),
        ("master balance bounds low", {"master": {"balance": -1}}),
        ("master balance bounds high", {"master": {"balance": 1}}),
        ("tracks empty", {"tracks": []}),
        ("tracks single minimal", {"tracks": [{"trackId": 0}]}),
        ("tracks single full", {"tracks": [{"trackId": 0, "volumeDb": -12.0, "balance": -0.5, "solo": True, "muted": False}]}),
        ("tracks volume bounds", {"tracks": [{"trackId": 0, "volumeDb": -120}, {"trackId": 1, "volumeDb": 24}]}),
        ("tracks balance bounds", {"tracks": [{"trackId": 0, "balance": -1}, {"trackId": 1, "balance": 1}]}),
        ("tracks solo only", {"tracks": [{"trackId": 0, "solo": False}]}),
        ("tracks muted only", {"tracks": [{"trackId": 0, "muted": True}]}),
        ("tracks max items 256", {"tracks": [{"trackId": i} for i in range(256)]}),
        ("master and tracks", {"master": {"volumeDb": 0, "balance": 0}, "tracks": [{"trackId": 0, "volumeDb": 0, "balance": 0, "solo": False, "muted": False}]}),
    ]

    for label, mix_obj in valid_mixes:
        for fmt in audio_formats:
            add(f"export {fmt} {label}", "score_export",
                {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}", "mix": mix_obj}, True)
            add(f"async {fmt} {label}", "score_export_start",
                {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}", "mix": mix_obj}, True)

    # allOf format restriction: mix is rejected on non-audio formats
    for fmt in non_audio_formats:
        add(f"export {fmt} with mix rejected", "score_export",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}", "mix": {}}, False)
        add(f"async {fmt} with mix rejected", "score_export_start",
            {"documentId": "score-1", "format": fmt, "path": f"out.{fmt}", "mix": {}}, False)

    # mix rejection cases on audio formats
    invalid_mix_cases = [
        ("mix not an object", "invalid_mix_string"),
        ("mix unknown root field", {"master": {"volumeDb": 0}, "unexpected": 1}),
        ("master volumeDb below -120", {"master": {"volumeDb": -121}}),
        ("master volumeDb above 24", {"master": {"volumeDb": 25}}),
        ("master volumeDb string", {"master": {"volumeDb": "0"}}),
        ("master balance below -1", {"master": {"balance": -1.1}}),
        ("master balance above 1", {"master": {"balance": 1.1}}),
        ("master forbidden key solo", {"master": {"solo": True}}),
        ("master forbidden key muted", {"master": {"muted": True}}),
        ("master forbidden key forceMute", {"master": {"forceMute": True}}),
        ("master unknown field", {"master": {"unexpected": 1}}),
        ("track trackId negative", {"tracks": [{"trackId": -1}]}),
        ("track trackId float", {"tracks": [{"trackId": 0.5}]}),
        ("track trackId bool", {"tracks": [{"trackId": True}]}),
        ("track missing trackId", {"tracks": [{"volumeDb": 0}]}),
        ("track volumeDb below -120", {"tracks": [{"trackId": 0, "volumeDb": -121}]}),
        ("track volumeDb above 24", {"tracks": [{"trackId": 0, "volumeDb": 25}]}),
        ("track balance below -1", {"tracks": [{"trackId": 0, "balance": -1.1}]}),
        ("track balance above 1", {"tracks": [{"trackId": 0, "balance": 1.1}]}),
        ("track solo not bool", {"tracks": [{"trackId": 0, "solo": 1}]}),
        ("track muted not bool", {"tracks": [{"trackId": 0, "muted": 1}]}),
        ("track forbidden key forceMute", {"tracks": [{"trackId": 0, "forceMute": True}]}),
        ("track forbidden legacy output object", {"tracks": [{"trackId": 0, "output": {"volumeDb": 0}}]}),
        ("track unknown field", {"tracks": [{"trackId": 0, "unexpected": 1}]}),
        ("tracks exceeding 256 items", {"tracks": [{"trackId": i} for i in range(257)]}),
    ]

    for label, bad_mix in invalid_mix_cases:
        add(f"export wav {label}", "score_export",
            {"documentId": "score-1", "format": "wav", "path": "out.wav", "mix": bad_mix}, False)
        add(f"async wav {label}", "score_export_start",
            {"documentId": "score-1", "format": "wav", "path": "out.wav", "mix": bad_mix}, False)
    return cases


def run_pure(tools: list[dict[str, Any]]) -> int:
    check_schema_contracts(tools)
    validators = {
        tool["name"]: Draft202012Validator(tool["inputSchema"]) for tool in tools
    }
    failures: list[str] = []
    cases = build_cases()
    for case in cases:
        validator = validators.get(case.tool)
        if validator is None:
            failures.append(f"{case.name}: unknown tool {case.tool}")
            continue
        actual = validator.is_valid(case.arguments)
        if actual != case.valid:
            detail = "accepted" if actual else "rejected"
            errors = "; ".join(
                f"{'/'.join(str(part) for part in error.absolute_path) or '<root>'}: {error.message}"
                for error in validator.iter_errors(case.arguments)
            )
            failures.append(
                f"{case.name}: expected valid={case.valid}, schema {detail}; {errors}"
            )
    if failures:
        raise AssertionError(
            f"{len(failures)} of {len(cases)} contract cases failed:\n"
            + "\n".join(failures[:20])
        )
    return len(cases)


def check_generator(schema: Path) -> None:
    """The legacy entry point must never regenerate or overwrite its authority."""
    generator = ROOT / "scripts" / "generate_tool_schema.py"
    with tempfile.TemporaryDirectory(prefix="music-schema-generator-") as directory:
        copied = Path(directory) / "tools.json"
        original = schema.read_bytes()
        copied.write_bytes(original)
        output = Path(directory) / "generated" / "tool_schema.inc"
        base = [sys.executable, str(generator), "--schema", str(copied)]
        subprocess.run(base, check=True, capture_output=True)
        subprocess.run(base + ["--output", str(output)], check=True, capture_output=True)
        written = output.read_bytes()
        timestamp = output.stat().st_mtime_ns
        subprocess.run(base + ["--output", str(output)], check=True, capture_output=True)
        if output.read_bytes() != written or output.stat().st_mtime_ns != timestamp:
            raise AssertionError("Unchanged schema generation rewrote its output")
        blocked = subprocess.run(base + ["--output", str(copied)], capture_output=True)
        if blocked.returncode == 0 or copied.read_bytes() != original:
            raise AssertionError("Generator overwrote the authoritative schema")
        if not written.startswith(b'R"schema(') or not written.endswith(b')schema"\n'):
            raise AssertionError("Generator did not emit a C++ raw string expression")


class LiveSession:
    def __init__(self, binary: Path, assets: Path | None, directory: Path):
        self.binary = binary
        self.assets = assets
        self.directory = directory
        self.serial = 0
        self.pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self.proc: asyncio.subprocess.Process | None = None
        self.reader: asyncio.Task[None] | None = None
        self.stderr = None

    async def __aenter__(self) -> "LiveSession":
        command = [str(self.binary.resolve())]
        if self.assets is not None:
            command += ["--resources", str(self.assets.resolve())]
        self.stderr = (self.directory / "server-stderr.log").open("wb")
        self.proc = await asyncio.create_subprocess_exec(
            *command,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=self.stderr,
            cwd=str(self.directory),
            limit=16 * 1024 * 1024,
        )
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.reader = asyncio.create_task(self._read())
        await self.call(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "tool-schema-contract", "version": "1"},
            },
        )
        await self.notify("notifications/initialized", {})
        return self

    async def _read(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        try:
            while line := await self.proc.stdout.readline():
                message = json.loads(line)
                if not isinstance(message, dict) or "id" not in message:
                    continue
                identity = message["id"]
                future = self.pending.pop(identity, None)
                if future is None:
                    raise AssertionError(f"Unmatched response id: {identity!r}")
                if not future.done():
                    future.set_result(message)
            if self.pending:
                raise RuntimeError("Server exited with pending requests")
        except asyncio.CancelledError:
            raise
        except BaseException as exc:
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(exc)
            self.pending.clear()
            raise

    async def notify(self, method: str, params: dict[str, Any]) -> None:
        assert self.proc is not None and self.proc.stdin is not None
        line = json.dumps({"jsonrpc": "2.0", "method": method, "params": params})
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        await self.proc.stdin.drain()

    async def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        assert self.proc is not None and self.proc.stdin is not None
        self.serial += 1
        identity = self.serial
        future: asyncio.Future[dict[str, Any]] = (
            asyncio.get_running_loop().create_future()
        )
        self.pending[identity] = future
        line = json.dumps(
            {"jsonrpc": "2.0", "id": identity, "method": method, "params": params}
        )
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        await self.proc.stdin.drain()
        return await asyncio.wait_for(future, 90)

    async def ok(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        message = await self.call(
            "tools/call", {"name": name, "arguments": arguments}
        )
        if "error" in message:
            raise AssertionError(f"{name} unexpectedly failed: {message['error']!r}")
        result = message.get("result")
        if not isinstance(result, dict) or result.get("isError"):
            raise AssertionError(f"{name} unexpectedly failed: {message!r}")
        payload = result.get("structuredContent")
        if isinstance(payload, dict):
            return payload
        return result

    async def rejected(self, name: str, arguments: dict[str, Any]) -> str:
        message = await self.call(
            "tools/call", {"name": name, "arguments": arguments}
        )
        if "error" in message:
            return "jsonrpc-error"
        result = message.get("result")
        if isinstance(result, dict) and result.get("isError") is True:
            return "isError"
        raise AssertionError(f"{name} was not rejected: {message!r}")

    async def __aexit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        assert self.proc is not None
        if self.proc.stdin is not None:
            self.proc.stdin.close()
        try:
            try:
                await asyncio.wait_for(self.proc.wait(), 15)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
                raise AssertionError("MCP host did not exit after stdin close")
            if self.reader is not None:
                await asyncio.wait_for(self.reader, 5)
            if exc_type is None and self.proc.returncode != 0:
                self.stderr.flush()
                tail = (self.directory / "server-stderr.log").read_text(
                    encoding="utf-8", errors="replace"
                )[-4000:]
                raise AssertionError(
                    f"MCP host exited with {self.proc.returncode}: {tail}"
                )
        finally:
            if self.proc.returncode is None:
                self.proc.kill()
                await self.proc.wait()
            if self.reader is not None and not self.reader.done():
                self.reader.cancel()
                try:
                    await self.reader
                except asyncio.CancelledError:
                    pass
            if self.stderr is not None:
                self.stderr.close()


async def _run_live(binary: Path, assets: Path | None, tools: list[dict[str, Any]]) -> dict[str, Any]:
    expected = {tool["name"]: tool["inputSchema"] for tool in tools}
    evidence: dict[str, Any] = {}
    with tempfile.TemporaryDirectory(prefix="musescore-schema-") as temp:
        directory = Path(temp)
        async with LiveSession(binary, assets, directory) as session:
            message = await session.call("tools/list", {})
            if "error" in message:
                raise AssertionError(f"tools/list failed: {message['error']!r}")
            result = message.get("result")
            advertised = result.get("tools") if isinstance(result, dict) else None
            if not isinstance(advertised, list):
                raise AssertionError(f"tools/list has no tools array: {message!r}")
            advertised_names = [
                tool.get("name") for tool in advertised if isinstance(tool, dict)
            ]
            if advertised_names != [tool["name"] for tool in tools]:
                raise AssertionError(
                    f"advertised tool names differ\nfile:      {[tool['name'] for tool in tools]}\n"
                    f"advertised: {advertised_names}"
                )
            for tool in advertised:
                if not isinstance(tool, dict):
                    raise AssertionError(f"advertised tool is not an object: {tool!r}")
                name = tool["name"]
                Draft202012Validator.check_schema(tool["inputSchema"])
                if tool["inputSchema"] != expected[name]:
                    raise AssertionError(f"{name} advertised schema differs from file")
            evidence["advertised_tools"] = len(advertised)

            created = await session.ok(
                "score_create",
                {
                    "score": {
                        "title": "Schema live",
                        "tempo": 120,
                        "parts": [
                            {
                                "name": "Piano",
                                "midiProgram": 1,
                                "measures": [bar(60, 62, 64, 65), bar(67, 69, 71, 72)],
                            },
                            {
                                "name": "Bass",
                                "midiProgram": 43,
                                "measures": [bar(48, 50, 52, 53), bar(55, 57, 59, 60)],
                            },
                        ],
                    }
                },
            )
            document = created["documentId"]
            revision = created["revision"]

            tracks = await session.ok("score_audio_tracks", {"documentId": document})
            if not isinstance(tracks.get("tracks"), list) or len(tracks["tracks"]) != 2:
                raise AssertionError(f"Expected 2 audio tracks: {tracks!r}")
            for tr in tracks["tracks"]:
                for key in ("trackId", "name", "partId", "instrumentId", "volumeDb", "balance", "solo", "muted", "forceMute"):
                    if key not in tr:
                        raise AssertionError(f"Audio track missing {key}: {tr!r}")
            evidence["audio_tracks"] = len(tracks["tracks"])

            query = await session.ok(
                "score_query_region",
                {
                    "documentId": document,
                    "partIndex": 0,
                    "firstMeasure": 0,
                    "measureCount": 1,
                },
            )
            notes = query.get("notes")
            if query.get("revision") != revision:
                raise AssertionError("Region projection revision did not match score revision")
            if not isinstance(notes, list) or not notes:
                raise AssertionError(f"score_query_region returned no notes: {query!r}")
            note = next(
                (
                    item
                    for item in notes
                    if isinstance(item, dict)
                    and isinstance(item.get("id"), str)
                    and isinstance(item.get("pitch"), int)
                ),
                None,
            )
            if note is None:
                raise AssertionError(f"score_query_region note shape is invalid: {query!r}")

            new_pitch = 62 if note["pitch"] != 62 else 64
            edited = await session.ok(
                "score_set_pitches",
                {
                    "documentId": document,
                    "baseRevision": query.get("revision", revision),
                    "operationId": "schema-live-pitch-1",
                    "changes": [{"noteId": note["id"], "pitch": new_pitch}],
                },
            )
            if int(edited.get("changedNotes", 0)) < 1:
                raise AssertionError(f"score_set_pitches changed no notes: {edited!r}")
            evidence["valid_query_notes"] = len(notes)
            evidence["valid_edit_changed_notes"] = int(edited["changedNotes"])

            rejections = {
                "mix on non-audio format": await session.rejected(
                    "score_export",
                    {
                        "documentId": document,
                        "format": "svg",
                        "path": str(directory / "rejected.svg"),
                        "mix": {"master": {"volumeDb": 0}},
                    },
                ),
                "unknown field": await session.rejected(
                    "score_get", {"documentId": document, "unexpected": 1}
                ),
                "unpaired replacement context": await session.rejected(
                    "score_replace_measures",
                    {
                        "documentId": document,
                        "partIndex": 0,
                        "startMeasure": 0,
                        "measures": [bar(60, 60, 60, 60)],
                        "baseRevision": revision,
                    },
                ),
                "invalid region": await session.rejected(
                    "score_query_region",
                    {
                        "documentId": document,
                        "partIndex": 0,
                        "firstMeasure": 0,
                        "measureCount": 0,
                    },
                ),
                "unsupported sync format option": await session.rejected(
                    "score_export",
                    {
                        "documentId": document,
                        "format": "png",
                        "path": str(directory / "rejected.png"),
                        "expandRepeats": True,
                    },
                ),
                "unsupported async format option": await session.rejected(
                    "score_export_start",
                    {
                        "documentId": document,
                        "format": "pdf",
                        "path": str(directory / "rejected.pdf"),
                        "expandRepeats": True,
                    },
                ),
            }
            evidence["rejections"] = rejections

            await session.ok("score_close", {"documentId": document})
            evidence["valid_close"] = True
    evidence["status"] = "passed"
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--schema",
        type=Path,
        default=DEFAULT_SCHEMA,
        help="Path to native/tools.json",
    )
    parser.add_argument(
        "--binary",
        type=Path,
        help="Optional musescore-mcp executable; enables the live MCP lane",
    )
    parser.add_argument(
        "--assets",
        type=Path,
        help="Optional native resources directory passed as --resources",
    )
    args = parser.parse_args()

    schema_path = args.schema.resolve()
    if not schema_path.is_file():
        parser.error(f"schema file does not exist: {schema_path}")
    if args.assets is not None and args.binary is None:
        parser.error("--assets requires --binary")
    if args.binary is not None:
        if not args.binary.is_file():
            parser.error(f"binary does not exist: {args.binary}")
        if args.assets is not None and not args.assets.is_dir():
            parser.error(f"assets directory does not exist: {args.assets}")

    tools = load_tools(schema_path)
    pure_cases = run_pure(tools)
    check_generator(schema_path)
    summary: dict[str, Any] = {
        "status": "passed",
        "schema": str(schema_path),
        "tools": len(tools),
        "pure_contract_cases": pure_cases,
        "generator_contract": "passed",
        "live": False,
    }
    if args.binary is not None:
        live = asyncio.run(_run_live(args.binary, args.assets, tools))
        summary["live"] = True
        summary["live_evidence"] = live
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
