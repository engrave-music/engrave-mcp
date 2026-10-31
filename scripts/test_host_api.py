"""Run the native S2 acceptance test and independently validate its envelopes."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess

from jsonschema import Draft202012Validator


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--replacement-soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    command = [str(p.resolve()) for p in (args.binary, args.resources, args.soundfont, args.replacement_soundfont)]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
    (args.output / "host_api.stdout.log").write_text(result.stdout, encoding="utf-8")
    (args.output / "host_api.stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode:
        print(result.stderr[-10000:])
        print(f"host_api_test exited {result.returncode}")
        return 1
    tools = {t["name"]: t for t in json.loads((root / "native/tools.json").read_text(encoding="utf-8"))}
    inputs = {name: Draft202012Validator(t["inputSchema"]) for name, t in tools.items()}
    outputs = {name: Draft202012Validator(t["outputSchema"]) for name, t in tools.items()}
    counts: Counter[str] = Counter()
    for line in result.stdout.splitlines():
        if not line.startswith("HOST_CASE "):
            continue
        case = json.loads(line[len("HOST_CASE "):])
        name, arguments, payload = case["tool"], case["arguments"], case["result"]
        outputs[name].validate(payload)
        if payload["ok"]:
            inputs[name].validate(arguments)
        counts[name] += 1
    required = {"score_open", "score_create", "score_close", "score_outline", "score_query",
                "score_describe", "score_edit", "runtime_info"}
    if set(counts) != required or "host_api_test passed" not in result.stdout:
        raise AssertionError(f"incomplete host test output: {counts}")
    print(json.dumps({"status": "passed", "native_exit": result.returncode,
                      "schema_checked_responses": sum(counts.values()), "tools": counts}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
