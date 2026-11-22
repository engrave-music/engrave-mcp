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
        assert not {"unevaluatedProperties", "not", "if", "then", "else", "const"} & node.keys(), node
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
            destructiveHint=name in {"score_export", "score_close", "restore_checkpoint", "drop_checkpoint"},
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
    by_name["score_write_phrase"].validate(
        {"document": "doc-A-1", "lanes": [{"measure": 1, "events": [{"pitches": ["Eb4"], "duration": "3/8"}]}]}
    )
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
