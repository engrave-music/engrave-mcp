"""Run the native S2 acceptance test and independently validate its envelopes."""

from __future__ import annotations

import argparse
import json
import subprocess
from collections import Counter
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from jsonschema import Draft202012Validator


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--replacement-soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = REPO_ROOT
    args.output.mkdir(parents=True, exist_ok=True)
    command = [
        str(p.resolve())
        for p in (
            args.binary,
            args.resources,
            args.soundfont,
            args.replacement_soundfont,
            root / "vendor/engrave/tests/data",
        )
    ]
    result = subprocess.run(
        command,
        cwd=root,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    (args.output / "host_api.stdout.log").write_text(result.stdout, encoding="utf-8")
    (args.output / "host_api.stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        print(result.stderr[-10000:])
        print(f"host_api_test exited {result.returncode}")
        return 1
    tools = {t["name"]: t for t in json.loads((root / "engrave_mcp/tools.json").read_text(encoding="utf-8"))}
    inputs = {name: Draft202012Validator(t["inputSchema"]) for name, t in tools.items()}

    counts: Counter[str] = Counter()
    ops_examples_checked = 0
    for line in result.stdout.splitlines():
        if not line.startswith("HOST_CASE "):
            continue
        case = json.loads(line[len("HOST_CASE ") :])
        name, arguments, payload = case["tool"], case["arguments"], case["result"]
        assert isinstance(payload["ok"], bool)
        if payload["ok"]:
            inputs[name].validate(arguments)
            if name == "runtime_info" and arguments == {"scope": "ops"}:
                op_props = set(tools["score_edit"]["inputSchema"]["properties"]["ops"]["items"]["properties"])
                for entry in payload["ops"]:
                    fields = set(entry["fields"])
                    assert fields <= op_props, (entry["op"], fields - op_props)
                    assert set(entry["required"]) <= fields, (entry["op"], entry["required"])
                    inputs["score_edit"].validate({"document": "doc-A-1", "ops": [entry["example"]]})
                    ops_examples_checked += 1
        counts[name] += 1
    required = {
        "score_open",
        "score_create",
        "score_close",
        "score_outline",
        "score_query",
        "runtime_configure",
        "score_edit",
        "runtime_info",
        "checkpoint",
        "restore_checkpoint",
        "drop_checkpoint",
    }
    if set(counts) != required or "host_api_test passed" not in result.stdout:
        raise AssertionError(f"incomplete host test output: {counts}")
    assert ops_examples_checked > 0, "missing runtime_info operation examples"
    print(
        json.dumps(
            {
                "status": "passed",
                "native_exit": result.returncode,
                "schema_checked_responses": sum(counts.values()),
                "schema_checked_op_examples": ops_examples_checked,
                "tools": counts,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
