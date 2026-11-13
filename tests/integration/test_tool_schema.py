"""Verify the standalone tool registry, schemas, and independent wire cases."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from jsonschema import Draft202012Validator

BUILD = REPO_ROOT / ("build/xmake-windows" if os.name == "nt" else "build/xmake-linux")
SUFFIX = ".exe" if os.name == "nt" else ""
NAMES = [
    "score_open",
    "score_create",
    "score_close",
    "score_outline",
    "score_query",
    "score_describe",
    "score_edit",
    "score_export",
    "runtime_info",
    "checkpoint",
    "restore_checkpoint",
    "drop_checkpoint",
]
OPS = [
    "set_property",
    "reset_property",
    "set_style",
    "enter_notes",
    "set_pitch",
    "set_duration",
    "remove",
    "insert_measures",
    "remove_measures",
    "set_time_sig",
    "set_key_sig",
    "insert_clef",
    "set_layout_break",
    "add_part",
    "remove_part",
    "add_staff",
    "remove_staff",
    "generate_excerpts",
    "add_spanner",
    "add_tie",
    "add_tuplet",
    "add_text",
    "add",
]


def inspect_schema(node, root, *, input_schema):
    if isinstance(node, list):
        for child in node:
            inspect_schema(child, root, input_schema=input_schema)
    elif isinstance(node, dict):
        if "$ref" in node:
            ref = node["$ref"]
            assert isinstance(ref, str) and ref.startswith("#/$defs/"), ref
            assert ref[8:] in root["$defs"], ref
        kind = node.get("type")
        if kind == "object":
            assert node.get("additionalProperties") is False or node.get("unevaluatedProperties") is False
        if input_schema and kind == "string":
            assert "maxLength" in node or "enum" in node
        if input_schema and kind == "array":
            assert "maxItems" in node
        for child in node.values():
            inspect_schema(child, root, input_schema=input_schema)


def check_cases(tools):
    for tool in tools:
        warning = tool["outputSchema"]["$defs"].get("export_warning")
        if warning is None:
            continue
        validator = Draft202012Validator(warning)
        for obj in (
            {"kind": "", "id": "H_H"},
            {"kind": "tie", "id": ""},
            {"kind": "", "id": ""},
        ):
            assert not validator.is_valid({"code": "B470_INVALID_TIE", "message": "test", "object": obj}), tool["name"]
        for kind, identity in (
            ("tie", "locator:score:0/measure:0/tick:0/track:0/note:0/tieFor"),
            ("page_lock", "A_A..B_B"),
            ("group_bracket", "B_B:1"),
        ):
            validator.validate(
                {
                    "code": "B470_UNSUPPORTED_FEATURE",
                    "message": "test",
                    "object": {"kind": kind, "id": identity},
                }
            )
    inputs = {t["name"]: Draft202012Validator(t["inputSchema"]) for t in tools}
    outputs = {t["name"]: Draft202012Validator(t["outputSchema"]) for t in tools}
    doc = "doc-session-1"
    ref = {"kind": "id", "id": "A_A"}
    fraction = {"num": "1", "den": "4"}
    address = {
        "kind": "segment",
        "measure": "A_A",
        "staff": 0,
        "voice": 0,
        "tick": fraction,
        "segment_kind": "ChordRest",
    }
    ops = {
        "set_property": {
            "target": ref,
            "property": "PITCH",
            "value": {"kind": "int", "value": "60"},
        },
        "reset_property": {"target": ref, "property": "PITCH"},
        "set_style": {
            "score": {},
            "style": "spatium",
            "value": {"kind": "real", "value": 1.0},
        },
        "enter_notes": {
            "at": address,
            "duration": fraction,
            "pitches": [{"pitch": 60}],
        },
        "set_pitch": {"note": ref, "pitch": 62},
        "set_duration": {"chord_rest": ref, "duration": fraction},
        "remove": {"target": ref},
        "insert_measures": {"count": 1},
        "remove_measures": {"first": ref, "last": ref},
        "set_time_sig": {"measure": ref, "sig": fraction},
        "set_key_sig": {"measure": ref, "key": -1},
        "insert_clef": {"at": address, "clef": "G"},
        "set_layout_break": {"measure": ref, "kind": "page", "on": True},
        "add_part": {"instrument_id": "keyboard.piano"},
        "remove_part": {"part": ref},
        "add_staff": {"part": ref},
        "remove_staff": {"staff": ref},
        "generate_excerpts": {"parts": [ref]},
        "add_spanner": {"type": "Slur", "start": ref, "end": ref},
        "add_tie": {"note": ref},
        "add_tuplet": {"first": ref, "last": ref},
        "add_text": {"at": address, "style": "staff", "text": "hello"},
        "add": {"anchor": ref, "type": "Dynamic"},
    }
    assert list(ops) == OPS
    declared = tools[6]["inputSchema"]["$defs"]["op"]["oneOf"]
    declared_ops = [variant["properties"]["op"]["const"] for variant in declared]
    assert len(declared_ops) == len(set(declared_ops)) and set(declared_ops) == set(OPS)
    cases = []
    for name, fields in ops.items():
        item = {"op": name, **fields}
        cases.append(("score_edit", {"document": doc, "ops": [item]}, True))
        cases.append(("score_edit", {"document": doc, "ops": [{**item, "extra": 1}]}, False))
    file = {"kind": "file", "path": "out"}
    directory = {"kind": "directory", "path": "out"}

    def export(kind, options, destination=file):
        return {
            "document": doc,
            "kind": kind,
            "options": options,
            "destination": destination,
        }

    cases += [
        ("score_open", {"path": "a.mscz"}, True),
        ("score_open", {"data": "AAAA", "format": "mnx"}, True),
        ("score_open", {"path": "a", "data": "AAAA"}, False),
        ("score_open", {"data": "!bad!"}, False),
        ("score_create", {"template_id": "Treble_Clef"}, True),
        ("score_create", {"template_id": "a", "template_path": "b"}, False),
        ("score_close", {"document": doc}, True),
        ("score_close", {"document": "score-1"}, False),
        ("score_outline", {"document": doc, "score": {"excerpt": "A_A"}}, True),
        ("score_outline", {"document": doc, "expected_revision": "3"}, True),
        ("score_outline", {"document": doc, "expected_revision": 3}, False),
        (
            "score_query",
            {"document": doc, "mode": "elements", "expected_revision": "3"},
            True,
        ),
        ("score_query", {"document": doc, "mode": "properties", "target": ref}, True),
        (
            "score_query",
            {
                "document": doc,
                "mode": "properties",
                "target": {"kind": "op", "op": 0, "item": 0},
            },
            False,
        ),
        ("score_query", {"document": doc, "mode": "audio_tracks", "score": {}}, True),
        ("score_query", {"document": doc, "mode": "page_count", "score": {}}, True),
        ("score_describe", {"scope": "types"}, True),
        ("score_describe", {"scope": "type", "type": "Note"}, True),
        ("score_describe", {"scope": "styles"}, True),
        ("score_edit", {"document": doc, "expected_revision": 3, "ops": []}, False),
        ("score_edit", {"document": doc, "ops": [{"op": "future"}]}, False),
        (
            "score_edit",
            {
                "document": doc,
                "ops": [{"op": "remove", "target": {"kind": "op", "op": 0, "item": 0}}],
            },
            True,
        ),
        (
            "score_export",
            export("score", {"format": "musicxml", "score": {"excerpt": "A_A"}}),
            True,
        ),
        ("score_export", export("score", {"format": "mscz", "midi_rpns": True}), False),
        ("score_export", export("score", {"format": "mnx"}), False),
        (
            "score_export",
            export("render", {"format": "png", "score": {"excerpt": "A_A"}}, directory),
            True,
        ),
        ("score_export", export("render", {"format": "png"}), False),
        ("score_export", export("render", {"format": "pdf", "dpi": 144}), False),
        (
            "score_export",
            export("audio", {"format": "opus", "score": {"excerpt": "A_A"}}),
            True,
        ),
        ("score_export", export("audio", {"format": "flac", "bit_depth": 32}), False),
        (
            "score_export",
            export("positions", {"position_kind": "measure", "score": {"excerpt": "A_A"}}),
            True,
        ),
        ("score_export", export("positions", {"position_kind": "segment"}), True),
        (
            "score_export",
            export("positions", {"position_kind": "segment", "expand_repeats": False}),
            False,
        ),
        (
            "score_export",
            {**export("score", {"format": "mscz"}), "action": "run"},
            False,
        ),
        (
            "score_export",
            {**export("score", {"format": "mscz"}), "action": "start"},
            False,
        ),
        ("runtime_info", {}, True),
        ("runtime_info", {"action": "get"}, True),
        ("runtime_info", {"action": "add_font", "path": "font.ttf"}, True),
        ("runtime_info", {"action": "set_soundfonts", "files": []}, True),
        ("runtime_info", {"action": "set_soundfonts", "path": "font.sf3"}, False),
        ("checkpoint", {"document": doc, "name": "before"}, True),
        ("checkpoint", {"document": doc, "name": "before", "expected_revision": "3"}, True),
        ("checkpoint", {"document": doc}, False),
        ("restore_checkpoint", {"document": doc, "name": "before"}, True),
        ("restore_checkpoint", {"document": "score-1", "name": "before"}, False),
        ("drop_checkpoint", {"document": doc, "name": "before"}, True),
        ("drop_checkpoint", {"document": doc, "name": "before", "extra": 1}, False),
    ]
    for name, value, valid in cases:
        actual = inputs[name].is_valid(value)
        assert actual == valid, (
            name,
            value,
            valid,
            list(inputs[name].iter_errors(value)),
        )

    outline = {
        "title": "",
        "subtitle": "",
        "composer": "",
        "poet": "",
        "timesig": "4/4",
        "mscore_version": "5.0.0",
        "file_version": 500,
        "pages": None,
        "measure_count": 0,
        "key": 0,
        "duration": 0,
        "parts": [],
        "measures": [],
        "signatures": [],
        "excerpts": [],
    }
    mix = {"volume_db": 0, "balance": 0, "mute": False, "solo": False}
    capability = {
        "engine_version": "5.0.0",
        "import_formats": ["mscz", "mnx"],
        "export_formats": ["mscz"],
        "render_formats": ["png"],
        "audio_formats": ["opus"],
        "soundfonts": [],
        "audio_sample_rate": 44100,
    }
    limits = {
        "input_bytes": 8388608,
        "base64_chars": 11184812,
        "batch_ops": 1024,
        "query_results": 4096,
        "output_bytes": "1073741824",
        "deadline_ms": 120000,
        "cancellation": "cooperative",
        "transport_request_bytes": 4194304,
    }
    results = {
        "score_open": [{"document": doc, "revision": "1", "diagnostics": []}],
        "score_create": [{"document": doc, "revision": "1", "diagnostics": []}],
        "score_close": [{}],
        "score_outline": [
            {"revision": "1", "outline": {**outline, "excerpts": [{"id": "A_A", "name": "Part", "pages": None}]}},
            {
                "revision": "1",
                "outline": {**outline, "pages": 1, "excerpts": [{"id": "A_A", "name": "Part", "pages": 1}]},
            },
        ],
        "score_query": [
            {"mode": "page_count", "revision": "1", "pages": 1},
            {
                "mode": "elements",
                "revision": "1",
                "elements": [],
                "total": 0,
                "next_cursor": None,
            },
            {"mode": "properties", "revision": "1", "properties": []},
            {
                "mode": "audio_tracks",
                "revision": "1",
                "audio_tracks": {"tracks": [], "master": mix},
            },
        ],
        "score_describe": [
            {"scope": "types", "types": []},
            {"scope": "styles", "styles": []},
            {"scope": "type", "type": {"type": "Note", "props": []}},
        ],
        "score_edit": [
            {
                "revision": "2",
                "applied": 1,
                "ops": [{"created": [None, "A_A"]}],
                "created": ["A_A"],
                "removed": [],
                "replaced": [],
                "diagnostics": [],
            }
        ],
        "score_export": [{"source_revision": "2", "artifacts": []}],
        "runtime_info": [
            {
                "tool_revision": "2",
                "capabilities": capability,
                "templates": [],
                "fonts": [],
                "limits": limits,
            }
        ],
        "checkpoint": [{"revision": "1"}],
        "restore_checkpoint": [{"revision": "2"}],
        "drop_checkpoint": [{"revision": "2"}],
    }
    assert set(results) == set(NAMES)
    for name, examples in results.items():
        validator = outputs[name]
        validator.validate(
            {
                "ok": False,
                "error": {
                    "code": "InvalidArgument",
                    "message": "bad argument",
                    "op_index": 1,
                    "applied": 1,
                    "poisoned": True,
                },
            }
        )
        assert not validator.is_valid({"ok": False, "error": {"code": "unknown", "message": "bad"}})
        for example in examples:
            value = {"ok": True, **example}
            validator.validate(value)
            assert not validator.is_valid({**value, "unexpected": 1}), name
    bad_capability = {
        **results["runtime_info"][0],
        "capabilities": {**capability, "export_formats": ["mnx"]},
    }
    assert not outputs["runtime_info"].is_valid({"ok": True, **bad_capability})
    return len(cases), sum(map(len, results.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, default=REPO_ROOT / "engrave_mcp/tools.json")
    parser.add_argument("--dump", type=Path, default=BUILD / f"registry_dump{SUFFIX}")
    parser.add_argument("--registry-test", type=Path, default=BUILD / f"registry_test{SUFFIX}")
    args = parser.parse_args()
    generator = REPO_ROOT / "tests/engrave_mcp_testing/generate_tool_schema.py"
    subprocess.run(
        [
            sys.executable,
            str(generator),
            "--dump",
            str(args.dump),
            "--schema",
            str(args.schema),
            "--check",
        ],
        check=True,
    )
    subprocess.run([str(args.registry_test.resolve())], check=True)
    tools = json.loads(args.schema.read_bytes())
    assert [tool["name"] for tool in tools] == NAMES
    for tool in tools:
        assert set(tool) == {"name", "description", "inputSchema", "outputSchema"}
        assert tool["description"]
        for key in ("inputSchema", "outputSchema"):
            schema = tool[key]
            assert schema["$schema"] == "https://json-schema.org/draft/2020-12/schema"
            Draft202012Validator.check_schema(schema)
            inspect_schema(schema, schema, input_schema=key == "inputSchema")
    input_count, output_count = check_cases(tools)
    with tempfile.TemporaryDirectory(prefix="registry-contract-") as temp:
        copy = Path(temp) / "tools.json"
        copy.write_bytes(b"[]\n")
        base = [
            sys.executable,
            str(generator),
            "--dump",
            str(args.dump),
            "--schema",
            str(copy),
        ]
        assert subprocess.run(base + ["--check"], capture_output=True).returncode != 0
        subprocess.run(base + ["--write"], check=True, capture_output=True)
        assert copy.read_bytes() == args.schema.read_bytes()
        stamp = copy.stat().st_mtime_ns
        subprocess.run(base + ["--write"], check=True, capture_output=True)
        assert copy.stat().st_mtime_ns == stamp
        subprocess.run(base + ["--check"], check=True, capture_output=True)
    print(
        f"tool schema passed: {len(NAMES)} handlers, {len(OPS)} operations, "
        f"{input_count} input and {output_count} success cases"
    )


if __name__ == "__main__":
    main()
