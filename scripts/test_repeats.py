"""Repeat playback order test suite.

Exercises repeat playback order by opening repeat test fixtures (.mscx)
via the native MCP server, exporting measurePositions, converting the
repeat-expanded measure element sequence into 1-based measure numbers,
and comparing against expected sequences defined in upstream repeat_tests.cpp.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import re
import sys
import time
from typing import Any
import xml.etree.ElementTree as ET

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]


def extract_measure_numbers(mscx_path: Path) -> list[int]:
    """Parse MSCX XML and compute 1-based measure numbers for each Measure."""
    tree = ET.parse(mscx_path)
    root = tree.getroot()
    staff = next((s for s in root.findall("./Score/Staff") if s.findall("Measure")), None)
    if staff is None:
        staff = next((s for s in root.iter("Staff") if s.findall("Measure")), None)
    if staff is None:
        return []

    measure_no = 0
    measure_numbers: list[int] = []
    for child in staff:
        is_box = child.tag in ("VBox", "HBox", "TBox")
        if not (is_box or child.tag == "Measure"):
            continue
        no_offset = int(child.findtext("noOffset", "0"))
        irregular = is_box or child.findtext("irregular") in ("1", "true")
        measure_no += no_offset
        current_no = measure_no
        if not irregular:
            measure_no += 1

        has_section_break = False
        start_with_one = True
        for lb in child.findall("LayoutBreak"):
            if lb.findtext("subtype") == "section":
                has_section_break = True
                if lb.findtext("startWithMeasureOne") in ("0", "false"):
                    start_with_one = False
        if has_section_break and start_with_one:
            measure_no = 0

        if child.tag == "Measure":
            measure_numbers.append(current_no + 1)
    return measure_numbers


def parse_repeat_tests(cpp_path: Path, data_dir: Path) -> list[dict[str, Any]]:
    """Extract repeat tests and expected sequences from repeat_tests.cpp."""
    text = cpp_path.read_text(encoding="utf-8")
    matches = re.findall(r"TEST_F\s*\(\s*Engraving_RepeatTests\s*,\s*(\w+)\s*\)\s*\{([^}]+)\}", text)
    tests: list[dict[str, Any]] = []

    for name, body in matches:
        clean_body = re.sub(r"//.*", "", body).strip()
        m = re.search(r'repeat\s*\(\s*"([^"]+)"\s*,\s*u?"([^"]*)"\s*\);', clean_body)
        if not m:
            tests.append({"name": name, "fixture": "", "status": "SKIP",
                          "reason": "Non-standard helper or arguments", "mappable": False})
            continue

        fixture_name, expected = m.group(1), m.group(2)
        fixture_path = data_dir / fixture_name
        if not fixture_path.exists():
            tests.append({"name": name, "fixture": fixture_name, "status": "SKIP",
                          "reason": f"Fixture not found: {fixture_name}", "mappable": False})
            continue

        tests.append({
            "name": name, "fixture": fixture_name, "path": fixture_path,
            "expected": re.sub(r"\s+", "", expected), "status": "PENDING", "mappable": True,
        })
    return tests


async def execute_tests(binary: Path, resources: Path, output_dir: Path, upstream: Path) -> int:
    output_dir.mkdir(parents=True, exist_ok=True)
    cpp_path = upstream / "src/engraving/tests/repeat_tests.cpp"
    data_dir = upstream / "src/engraving/tests/repeat_data"

    if not cpp_path.exists():
        print(f"ERROR: repeat_tests.cpp not found at {cpp_path}", file=sys.stderr)
        return 1

    tests = parse_repeat_tests(cpp_path, data_dir)
    env = dict(os.environ)
    native_lib = ROOT / ".pixi/envs/native/Library/bin"
    if os.name == "nt" and native_lib.exists():
        env["PATH"] = str(native_lib) + os.pathsep + env.get("PATH", "")

    params = StdioServerParameters(
        command=str(binary.resolve()),
        args=["--resources", str(resources.resolve())],
        env=env,
        cwd=str(output_dir.resolve()),
    )

    results: list[dict[str, Any]] = []
    t0 = time.perf_counter()
    # One server session: a crash fails the lane instead of being retried.
    results.clear()
    t0 = time.perf_counter()
    with (output_dir / "stderr.log").open("w", encoding="utf-8") as stderr_file:
        async with stdio_client(params, errlog=stderr_file) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                async def call(name: str, arguments: dict[str, Any]) -> tuple[bool, Any]:
                    res = await session.call_tool(name, arguments)
                    content = (res.structured_content if res.structured_content is not None
                               else (json.loads(res.content[0].text) if res.content else {}))
                    return bool(res.is_error), content

                for test in tests:
                    tname = test["name"]
                    if not test.get("mappable", False):
                        results.append({"name": tname, "fixture": test.get("fixture", ""),
                                        "status": "SKIP", "reason": test.get("reason", "Unmappable")})
                        print(f"  [SKIP] {tname}: {test.get('reason', '')}")
                        continue

                    fixture_path = test["path"]
                    err_open, opened = await call("score_open", {"path": str(fixture_path.resolve())})
                    if err_open or not isinstance(opened, dict) or "documentId" not in opened:
                        results.append({"name": tname, "fixture": test["fixture"], "status": "FAIL",
                                        "expected": test["expected"], "actual": "",
                                        "error": f"score_open failed: {opened}"})
                        print(f"  [FAIL] {tname} (score_open failed: {opened})")
                        continue

                    doc_id = opened["documentId"]
                    try:
                        pos_path = output_dir / f"{tname}_positions.json"
                        err_exp, exported = await call("score_export", {
                            "documentId": doc_id, "format": "measurePositions",
                            "path": str(pos_path.resolve()),
                        })
                        if err_exp or not pos_path.exists():
                            results.append({"name": tname, "fixture": test["fixture"], "status": "FAIL",
                                            "expected": test["expected"], "actual": "",
                                            "error": f"score_export failed: {exported}"})
                            print(f"  [FAIL] {tname} (score_export failed: {exported})")
                            continue

                        data = json.loads(pos_path.read_text("utf-8"))
                        events = data.get("events", [])
                        m_nos = extract_measure_numbers(fixture_path)
                        if any(not 0 <= e["elid"] < len(m_nos) for e in events):
                            raise RuntimeError(f"{tname}: measure id outside the {len(m_nos)} parsed measures")
                        actual_nos = [m_nos[e["elid"]] for e in events]
                        actual_str = ";".join(str(n) for n in actual_nos)
                        expected_str = test["expected"]

                        status = "PASS" if actual_str == expected_str else "FAIL"
                        results.append({
                            "name": tname, "fixture": test["fixture"], "status": status,
                            "expected": expected_str, "actual": actual_str,
                        })
                        if status == "PASS":
                            print(f"  [PASS] {tname}")
                        else:
                            print(f"  [FAIL] {tname} expected={expected_str} actual={actual_str}")
                    finally:
                        await call("score_close", {"documentId": doc_id})

    elapsed = round(time.perf_counter() - t0, 3)
    total = len(results)
    passed = sum(1 for r in results if r["status"] == "PASS")
    failed = sum(1 for r in results if r["status"] == "FAIL")
    skipped = sum(1 for r in results if r["status"] == "SKIP")

    report: dict[str, Any] = {
        "summary": {"total": total, "passed": passed, "failed": failed,
                    "skipped": skipped, "elapsed_seconds": elapsed},
        "results": results,
    }
    (output_dir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"\nRepeat test suite finished in {elapsed}s: {passed}/{total} passed, {failed} failed, {skipped} skipped.")
    return 1 if failed > 0 else 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="Path to musescore-mcp server binary")
    parser.add_argument("--resources", type=Path, required=True, help="Path to .runtime/assets directory")
    parser.add_argument("--output", type=Path, required=True, help="Scratch output directory")
    parser.add_argument("--upstream", type=Path, default=ROOT / "vendor/musescore",
                        help="Path to vendor/musescore submodule")
    args = parser.parse_args()

    exit_code = asyncio.run(
        execute_tests(binary=args.binary, resources=args.resources,
                      output_dir=args.output, upstream=args.upstream)
    )
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
