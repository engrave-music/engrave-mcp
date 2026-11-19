"""Verify tool registry regeneration, large UTF-8 descriptions and Bazel incremental builds."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_testing.paths import pixi_env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    build_dir = binary.parent
    dump = build_dir / ("registry_dump.exe" if os.name == "nt" else "registry_dump")
    source = REPO_ROOT / "engrave_mcp/schema_defs.cpp"
    original = source.read_bytes()
    description = "Open a score; decoded cap 8 MiB, subject to lower transport line budget."
    marker_unit = "bazel schema regeneration 乐谱 "
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
    prefix = pixi_env(REPO_ROOT)
    if os.name == "nt":
        environment["PATH"] = str(prefix / "Library/bin") + os.pathsep + environment.get("PATH", "")
    elif (prefix / "lib").is_dir():
        environment["LD_LIBRARY_PATH"] = str(prefix / "lib") + os.pathsep + environment.get("LD_LIBRARY_PATH", "")

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
        combined = result.stdout + result.stderr
        (output / f"{name}.log").write_text(combined, encoding="utf-8")
        if result.returncode != expected:
            raise AssertionError(f"{name}: exit {result.returncode}, expected {expected}: {result.stderr}")
        return combined

    def build(name):
        return run(
            name,
            [
                sys.executable,
                str(REPO_ROOT / "scripts/bbuild.py"),
                "--build-directory",
                str(build_dir),
                "-j",
                "4",
                "build",
                "registry_dump",
                "engrave_mcp",
            ],
        )

    schema = REPO_ROOT / "engrave_mcp/tools.json"
    generator = [
        sys.executable,
        str(REPO_ROOT / "tests/engrave_mcp_testing/generate_tool_schema.py"),
        "--dump",
        str(dump),
    ]

    try:
        source.write_bytes(changed_source)
        build("changed")
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
        live_res = subprocess.run(
            [str(binary), "--resources", str(args.assets.resolve())],
            cwd=REPO_ROOT,
            env=environment,
            input="".join(json.dumps(r) + "\n" for r in requests),
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
        )
        (output / "live-list.log").write_text(live_res.stdout + live_res.stderr, encoding="utf-8")
        assert live_res.returncode == 0, live_res.stderr
        response = next(json.loads(line) for line in live_res.stdout.splitlines() if json.loads(line).get("id") == 2)
        assert response["result"]["tools"][0]["description"] == description + marker
    finally:
        if source.read_bytes() != changed_source:
            raise RuntimeError("registry source changed during the test; refusing to overwrite it")
        source.write_bytes(original)
        build("restored")
    run("restored-schema-check", [*generator, "--schema", str(schema), "--check"])
    mtime_before = binary.stat().st_mtime_ns
    build("unchanged")
    assert binary.stat().st_mtime_ns == mtime_before, "no-op build restaged binary"
    report = {
        "registry_regenerated": True,
        "large_utf8_schema_roundtrip": True,
        "noop_build": True,
        "source_restored": True,
    }
    (output / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
