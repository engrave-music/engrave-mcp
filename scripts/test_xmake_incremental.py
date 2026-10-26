"""Verify schema regeneration, large UTF-8 schemas, and no-op xmake builds."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    schema = ROOT / "native/tools.json"
    original = schema.read_bytes()
    tools = json.loads(original)
    # Exceeds a single MSVC literal's capacity and exercises UTF-8 boundaries.
    marker = "xmake schema regeneration 乐谱 " * 800
    tools[0]["description"] += marker
    environment = dict(os.environ)
    prefix = ROOT / ".pixi/envs/native"
    if os.name == "nt":
        environment["PATH"] = str(prefix / "Library/bin") + os.pathsep + environment.get("PATH", "")

    def build(name):
        result = subprocess.run(["xmake", "build", "-y", "-j", "4", "musescore_mcp"],
                                cwd=ROOT, env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=240)
        log = result.stdout + result.stderr
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        result.check_returncode()
        return log

    try:
        schema.write_text(json.dumps(tools, ensure_ascii=False, indent=2), encoding="utf-8")
        changed = build("changed")
        assert "noqt_engine.cpp" in changed, changed
        assert changed.count("compiling.release") == 1, changed
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "schema-test", "version": "1"}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        ]
        process = subprocess.run([str(binary), "--resources", str(ROOT / ".runtime/assets")],
                                 input="".join(json.dumps(r) + "\n" for r in requests),
                                 env=environment, capture_output=True, text=True, encoding="utf-8",
                                 timeout=30, check=True)
        response = next(json.loads(line) for line in process.stdout.splitlines()
                        if json.loads(line).get("id") == 2)
        assert response["result"]["tools"][0]["description"].endswith(marker)
    finally:
        schema.write_bytes(original)
        build("restored")
    unchanged = build("unchanged")
    assert "compiling.release" not in unchanged and "linking.release" not in unchanged
    report = {"schema_regenerated": True, "large_utf8_schema_roundtrip": True,
              "one_translation_unit_rebuilt": True, "noop_build": True, "source_restored": True}
    (output / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
