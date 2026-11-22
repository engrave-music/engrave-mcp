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


def inspect_schema(node):
    if isinstance(node, list):
        for item in node:
            inspect_schema(item)
    if isinstance(node, dict):
        assert (
            not {"oneOf", "unevaluatedProperties", "not", "if", "then", "else", "const", "outputSchema"} & node.keys()
        ), node
        for item in node.values():
            inspect_schema(item)


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
                "restore_checkpoint",
                "drop_checkpoint",
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
