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

BUILD = REPO_ROOT / "build/release"
SUFFIX = ".exe" if os.name == "nt" else ""
NAMES = [
    "score_open",
    "score_create",
    "score_close",
    "score_outline",
    "score_query",
    "score_read_music",
    "score_edit",
    "score_export",
    "runtime_info",
    "checkpoint",
    "restore_checkpoint",
    "drop_checkpoint",
    "score_write_phrase",
    "runtime_configure",
]


def inspect_schema(node, *, inside_any_of=False):
    assert isinstance(node, dict), node
    forbidden = {
        "oneOf",
        "allOf",
        "unevaluatedProperties",
        "not",
        "if",
        "then",
        "else",
        "const",
        "outputSchema",
        "$ref",
        "$defs",
        "definitions",
        "patternProperties",
        "dependentSchemas",
    }
    assert not (forbidden & node.keys()), node
    has_type = "type" in node
    has_any_of = "anyOf" in node
    assert has_type != has_any_of, node
    if has_type:
        assert isinstance(node["type"], str), node
    else:
        assert not inside_any_of and isinstance(node["anyOf"], list) and node["anyOf"], node
    if "enum" in node:
        assert has_type, node
    if node.get("type") == "array":
        assert "items" in node, node
    if "properties" in node:
        assert node.get("type") == "object" and node.get("additionalProperties") is False, node
        assert set(node.get("required", [])) <= node["properties"].keys(), node
    for prop in node.get("properties", {}).values():
        inspect_schema(prop)
    for keyword in ("items", "additionalProperties", "contains", "propertyNames"):
        if keyword in node and isinstance(node[keyword], dict):
            inspect_schema(node[keyword])
    for branch in node.get("anyOf", []):
        inspect_schema(branch, inside_any_of=True)
    for item in node.get("prefixItems", []):
        inspect_schema(item)


def check_values(validator, arguments, accepted, rejected):
    for value in accepted:
        validator.validate(arguments(value))
    for value in rejected:
        case = arguments(value)
        assert not validator.is_valid(case), case


def check_typed_fields(by_name):
    document = "doc-A-1"
    eid = "A_A"
    layout = {"kind": "page", "index": 0}
    op_ref = {"from_op": 0, "item": 0}
    long_eid = "A" * 33
    long_text = "A" * 4097
    int32_max = 2_147_483_647
    export = by_name["score_export"]
    wav = {"document": document, "format": "wav", "path": "out.wav"}
    check_values(export, lambda v: {**wav, "options": {"bit_depth": v}}, [0, 16, 24, 32], [8, "16", False, None])
    check_values(
        export,
        lambda v: {**wav, "options": {"tracks": [{"track": 0, "soundfont": v}]}},
        [0, int32_max, "FluidR3Mono_GM.sf3", None],
        [-1, int32_max + 1, 1.5, False, {}, [], long_text],
    )
    check_values(
        by_name["score_query"],
        lambda v: {"document": document, "mode": "properties", "target": v},
        [eid, layout],
        [1, None, [], long_eid],
    )

    edit = by_name["score_edit"]
    spanner = {"op": "add_spanner", "type": "HairPin", "start": eid, "end": eid}
    key_sig = {"op": "set_key_sig", "measure": 1, "staff": 1, "key": 0}
    volta = {"op": "add_volta", "first_measure": 1, "last_measure": 2, "endings": [1]}
    position_values = [1, int32_max, eid, layout, op_ref]
    invalid_positions = [0, -1, int32_max + 1, 1.5, False, [], long_eid]
    for op, field, accepted, rejected in [
        (
            {"op": "enter_notes", "duration": "1/4", "pitches": [{"pitch": 60}]},
            "at",
            [{"measure": 1}, layout],
            [1, eid, [], None],
        ),
        ({"op": "insert_measures", "count": 1}, "kind", ["VBox"], [1, {}, None, long_text]),
        ({"op": "remove"}, "target", [eid, layout, op_ref], [1, False, [], long_eid]),
        ({"op": "generate_excerpts"}, "parts", [[eid, op_ref]], [[1], [None]]),
        (spanner, "start", [eid, layout, op_ref], [1, [], None, long_eid]),
        (spanner, "end", [eid, layout, op_ref], [0, int32_max + 1, 1.5, [], None, long_eid]),
        ({**spanner, "op": "add_spanner_to_bar_end"}, "end", [1, int32_max], [-1, 0, int32_max + 1]),
        ({"op": "set_repeat_barlines", "measure": 1}, "start", [True, False], [[], None]),
        ({"op": "set_repeat_barlines", "measure": 1}, "end", [True, False], [[], None]),
        (key_sig, "measure", position_values, invalid_positions),
        (key_sig, "staff", position_values, invalid_positions),
        (volta, "first_measure", position_values, invalid_positions),
        (volta, "last_measure", position_values, invalid_positions),
    ]:
        check_values(edit, lambda v: {"document": document, "ops": [{**op, field: v}]}, accepted, rejected)

    typed_values = [
        ("visible", True),
        ("scale", 2.5),
        ("text", "text"),
        ("endings", [-1, 2]),
        ("offset", {"x": 0.5, "y": 0}),
        ("color", {"r": 0, "g": 0, "b": 255, "a": 255}),
        ("color", None),
    ]
    invalid_values = [["bad"], [1.5], [True], [[1]]]
    for prop_name, value in typed_values:
        edit.validate(
            {
                "document": document,
                "ops": [{"op": "set_property", "target": eid, "property": prop_name, "value": value}],
            }
        )
        edit.validate(
            {
                "document": document,
                "ops": [
                    {"op": "add", "anchor": eid, "type": "Dynamic", "props": [{"name": prop_name, "value": value}]}
                ],
            }
        )
    check_values(
        edit,
        lambda v: {
            "document": document,
            "ops": [{"op": "set_property", "target": eid, "property": "endings", "value": v}],
        },
        [],
        invalid_values,
    )
    check_values(
        edit,
        lambda v: {
            "document": document,
            "ops": [{"op": "add", "anchor": eid, "type": "Dynamic", "props": [{"name": "endings", "value": v}]}],
        },
        [],
        invalid_values,
    )


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
    assert len(json.dumps(tools, separators=(",", ":")).encode()) < 30_000
    for tool in tools:
        assert set(tool) == {"name", "description", "inputSchema", "annotations"}
        name = tool["name"]
        read = name in {"score_read_music", "score_outline", "score_query", "runtime_info"}
        assert tool["annotations"] == dict(
            readOnlyHint=read,
            destructiveHint=name
            in {
                "score_export",
                "score_close",
                "score_edit",
                "score_write_phrase",
                "checkpoint",
                "restore_checkpoint",
                "drop_checkpoint",
                "runtime_configure",
            },
            idempotentHint=read,
            openWorldHint=False,
        )
        schema = tool["inputSchema"]
        assert schema["type"] == "object" and schema["additionalProperties"] is False
        assert not {"oneOf", "allOf", "anyOf"} & schema.keys()
        assert set(schema["required"]) <= schema["properties"].keys()
        assert "pattern" not in schema["properties"].get("data", {})
        Draft202012Validator.check_schema(schema)
        inspect_schema(schema)
    by_name = {t["name"]: Draft202012Validator(t["inputSchema"]) for t in tools}
    check_typed_fields(by_name)
    by_name["score_create"].validate({})
    phrase = by_name["score_write_phrase"]
    phrase.validate(
        {"document": "doc-A-1", "lanes": [{"measure": 1, "content": "grace16(D4) C###4 3:2(D4:e r:e E4:e) r:h |"}]}
    )
    assert not phrase.is_valid({"document": "doc-A-1", "lanes": []})
    assert not phrase.is_valid(
        {"document": "doc-A-1", "lanes": [{"measure": 1, "content": "C4", "events": [{"pitches": ["C4"]}]}]}
    )
    content = phrase.schema["properties"]["lanes"]["items"]["properties"]["content"]
    for term in ("C###4", "d36", "127", ":q", "tie", "3:2", "|", "grace16", "after16", "1/32"):
        assert term in content["description"], term
    edit = by_name["score_edit"]
    assert edit.is_valid(
        {
            "document": "doc-A-1",
            "ops": [
                {"op": "set_pitch", "tpc": -8},
                {"op": "enter_notes", "pitches": [{"pitch": 63, "tpc": 40}]},
                {"op": "add", "props": [{"name": "visible", "value": False}]},
            ],
        }
    )
    assert not edit.is_valid({"document": "doc-A-1", "ops": [{"op": "set_pitch", "tpc": 41}]})
    assert not edit.is_valid({"document": "doc-A-1", "ops": [{"op": "enter_notes", "pitches": "bogus"}]})
    assert not edit.is_valid(
        {"document": "doc-A-1", "ops": [{"op": "enter_notes", "pitches": [{"pitch": 63, "tpc": 41}]}]}
    )
    assert not edit.is_valid(
        {"document": "doc-A-1", "ops": [{"op": "enter_notes", "pitches": [{"pitch": 63, "tpc": -9}]}]}
    )
    assert not edit.is_valid({"document": "doc-A-1", "ops": [{"op": "add", "props": "bogus"}]})
    assert not edit.is_valid({"document": "doc-A-1", "ops": [{"op": "remove"}] * 1025})
    assert not by_name["score_read_music"].is_valid({"document": "doc-A-1", "measure": "1"})
    assert not by_name["score_export"].is_valid({"document": "doc-A-1", "format": "exe", "path": "out"})
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
    print(f"tool schema passed: {len(NAMES)} handlers")


if __name__ == "__main__":
    main()
