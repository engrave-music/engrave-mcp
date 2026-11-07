"""Verify tool registry regeneration, large UTF-8 descriptions and no-op builds."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from support.paths import REPO_ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    dump = binary.parent / ("registry_dump.exe" if os.name == "nt" else "registry_dump")
    source = REPO_ROOT / "engrave_mcp/schema_defs.cpp"
    original = source.read_bytes()
    description = "Open a score; decoded cap 8 MiB, subject to lower transport line budget."
    marker_unit = "xmake schema regeneration 乐谱 "
    marker = marker_unit * 800
    old = ('"' + description + '"').encode("utf-8")
    replacement = (
        '([] { std::string s = "' + description + '"; for (int i = 0; i < 800; ++i) '
        's += "' + marker_unit + '"; return s; }()).c_str()'
    ).encode("utf-8")
    if original.count(old) != 1:
        raise RuntimeError("registry description to mutate is missing or ambiguous")
    changed_source = original.replace(old, replacement, 1)
    environment = dict(os.environ)
    prefix = REPO_ROOT / ".pixi/envs/native"
    if os.name == "nt":
        environment["PATH"] = str(prefix / "Library/bin") + os.pathsep + environment.get("PATH", "")

    def run(name, command, expected=0, **kwargs):
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=240,
            **kwargs,
        )
        (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode != expected:
            raise AssertionError(f"{name}: exit {result.returncode}, expected {expected}: {result.stderr}")
        return result.stdout

    def build(name):
        return run(name, ["xmake", "build", "-P", ".", "-y", "-j", "4", "registry_dump", "engrave_mcp"])

    schema = REPO_ROOT / "engrave_mcp/tools.json"
    generator = [
        sys.executable,
        str(REPO_ROOT / "tests/support/generate_tool_schema.py"),
        "--dump",
        str(dump),
    ]
    try:
        source.write_bytes(changed_source)
        log = build("changed")
        assert "schema_defs.cpp" in log and log.count("compiling.release") == 1, log
        run(
            "original-schema-rejected",
            [*generator, "--schema", str(schema), "--check"],
            expected=1,
        )
        generated = output / "regenerated.json"
        run("generate", [*generator, "--schema", str(generated), "--write"])
        run(
            "generated-schema-check",
            [*generator, "--schema", str(generated), "--check"],
        )
        registry = json.loads(generated.read_text(encoding="utf-8"))
        assert registry[0]["description"] == description + marker
        requests = [
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "schema-test", "version": "1"},
                },
            },
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        ]
        stdout = run(
            "live-list",
            [str(binary), "--resources", str(args.assets.resolve())],
            input="".join(json.dumps(r) + "\n" for r in requests),
        )
        response = next(json.loads(line) for line in stdout.splitlines() if json.loads(line).get("id") == 2)
        assert response["result"]["tools"][0]["description"] == description + marker
    finally:
        if source.read_bytes() != changed_source:
            raise RuntimeError("registry source changed during the test; refusing to overwrite it")
        source.write_bytes(original)
        build("restored")
    run("restored-schema-check", [*generator, "--schema", str(schema), "--check"])
    unchanged = build("unchanged")
    assert "compiling.release" not in unchanged and "linking.release" not in unchanged, unchanged
    report = {
        "registry_regenerated": True,
        "large_utf8_schema_roundtrip": True,
        "one_translation_unit_rebuilt": True,
        "noop_build": True,
        "source_restored": True,
    }
    (output / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
