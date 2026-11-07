"""Live producer companion; independent reader evidence only for exposed fields."""

from __future__ import annotations

import asyncio
import copy
import json
import os
import xml.etree.ElementTree as ET

from integration.test_native_470_oracle import (
    compare_semantics,
    compare_xml,
    file_sha,
    read_json,
    require,
    run_dump,
    save_json,
)
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from support.native_470_producer import RecordingStream


def association_report(expected, actual):
    """Compare owner-bearing subtrees, never infer identity from XML membership."""
    rows = []
    for label, key in [
        ("notes/pitches/times/ties by staff/voice", "events"),
        ("numeric part IDs/names/staff structure", "parts"),
        ("measure times", "measures"),
        ("excerpt contents/part IDs", "excerpts"),
    ]:
        require(key in expected and key in actual, f"oracle lacks expected field {key}")
        rows.append(
            {
                "association": label,
                "status": "verified-by-oracle" if expected[key] == actual[key] else "mismatch",
            }
        )
    for label in (
        "EID-to-object",
        "part EIDs",
        "ordinary brace-to-staff",
        "style switch values",
        "note-link group identities",
    ):
        rows.append({"association": label, "status": "not-observable"})
    return rows


def write_association_table(output):
    rows = ["| Fixture / export | Association | Status |", "| --- | --- | --- |"]
    reports = sorted(output.glob("*/*/associations.json"))
    require(reports, "no independent association reports produced")
    for path in reports:
        for entry in read_json(path):
            rows.append(
                f"| {path.parent.parent.name}/{path.parent.name} | {entry['association']} | {entry['status']} |"
            )
    (output / "associations.md").write_text("\n".join(rows) + "\n", encoding="utf-8")


def shifted(value, delta=2):
    """Expected pitch change for every linked copy, retaining time/owner context."""
    result = copy.deepcopy(value)

    def visit(item):
        if isinstance(item, dict):
            if "pitch" in item:
                item["pitch"] += delta
                item["tpc1"] += 2 if delta == 2 else -5
                item["tpc2"] += 2 if delta == 2 else -5
            if "tieTo" in item:
                item["tieTo"][2] += delta
            for child in item.values():
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(result)
    return result


async def live_edits(args, root, output, binary, assets, oracle_env):
    records = []
    params = StdioServerParameters(
        command=str(args.host_binary.resolve()),
        args=["--resources", str(args.host_resources.resolve())],
        env=dict(os.environ),
    )
    with (
        (output / "live.stderr").open("w", encoding="utf-8") as errors,
        (output / "live-mcp.jsonl").open("w", encoding="utf-8") as transcript,
    ):
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(
                RecordingStream(read, "host", transcript),
                RecordingStream(write, "client", transcript),
            ) as session:
                await session.initialize()
                for fixture in ("ties", "hidden_ties", "excerpts"):
                    for scenario in ("paired_rollback", "one_end"):
                        name = f"live_{fixture}_{scenario}"
                        dest = output / name
                        dest.mkdir()
                        record = {"case": name, "errors": [], "formats": {}}
                        doc = None
                        envelopes = []

                        async def call(tool, arguments, ok=True):
                            response = await asyncio.wait_for(session.call_tool(tool, arguments), 90)
                            value = response.structured_content
                            envelopes.append({"tool": tool, "arguments": arguments, "result": value})
                            save_json(dest / "envelopes.json", envelopes)
                            require(
                                isinstance(value, dict) and value.get("ok") == ok and bool(response.is_error) != ok,
                                f"{tool}: unexpected response {value}",
                            )
                            return value

                        try:
                            source = root / ("ties.mscx" if fixture == "hidden_ties" else fixture + ".mscx")
                            expected = read_json(
                                root / "expected" / ("ties.json" if fixture == "hidden_ties" else fixture + ".json")
                            )
                            tree = ET.parse(source)
                            if fixture == "hidden_ties":
                                ET.SubElement(tree.find(".//Part"), "show").text = "0"
                                source = dest / "hidden-source.mscx"
                                tree.write(source, encoding="utf-8", xml_declaration=True)
                                expected["parts"][0]["show"] = False
                            record["source_sha256"] = file_sha(source)
                            opened = await call("score_open", {"path": str(source.resolve())})
                            doc, revision = opened["document"], opened["revision"]

                            async def notes():
                                return await call(
                                    "score_query",
                                    {
                                        "document": doc,
                                        "mode": "elements",
                                        "query": {
                                            "types": ["Note"],
                                            "staves": [0],
                                            "props": ["pitch"],
                                            "limit": 100,
                                        },
                                    },
                                )

                            before = await notes()
                            targets = before["elements"]
                            require(
                                len(targets) == 2,
                                "expected two tie endpoints on staff zero",
                            )

                            def op(note, delta=1 if fixture == "excerpts" else 2):
                                return {
                                    "op": "set_pitch",
                                    "note": {"kind": "id", "id": note["id"]},
                                    "pitch": int(note["props"][0]["value"]["value"]) + delta,
                                }

                            edit = await call(
                                "score_edit",
                                {
                                    "document": doc,
                                    "expected_revision": revision,
                                    "ops": [op(n) for n in (targets if scenario == "paired_rollback" else targets[:1])],
                                },
                            )
                            require(
                                int(edit["revision"]) == int(revision) + 1,
                                "edit revision did not advance once",
                            )
                            revision = edit["revision"]
                            if scenario == "paired_rollback":
                                expected = shifted(expected, 1 if fixture == "excerpts" else 2)
                                clean = await notes()
                                measure = (await call("score_outline", {"document": doc}))["outline"]["measures"][0][
                                    "id"
                                ]
                                bad = await call(
                                    "score_edit",
                                    {
                                        "document": doc,
                                        "expected_revision": revision,
                                        "ops": [
                                            op(clean["elements"][0]),
                                            {
                                                "op": "set_pitch",
                                                "note": {"kind": "id", "id": measure},
                                                "pitch": 80,
                                            },
                                        ],
                                    },
                                    ok=False,
                                )
                                require(
                                    bad["error"]["op_index"] == 1,
                                    "batch failed at wrong operation",
                                )
                                after = await notes()
                                require(
                                    after == clean,
                                    "failed batch changed revision or queried notes",
                                )
                                record["failed_batch_unchanged"] = True
                            warning_ids = {e.findtext("eid") for e in tree.findall(".//Tie")}
                            expected_warnings = (
                                []
                                if scenario == "paired_rollback"
                                else [
                                    {
                                        "code": "B470_INVALID_TIE",
                                        "message": "Cannot write 4.70: tied notes have different pitches.",
                                        "object": {"kind": "tie", "id": eid},
                                    }
                                    for eid in sorted(warning_ids)
                                ]
                            )
                            from integration.test_native_470_oracle import compare_warnings

                            for fmt in ("mscx", "mscz"):
                                # Retain the historical -run evidence key for existing comparators.
                                folder = dest / f"{fmt}-run"
                                folder.mkdir()
                                path = folder / f"score.{fmt}"
                                refused = scenario == "one_end"
                                value = await call(
                                    "score_export",
                                    {
                                        "document": doc,
                                        "expected_revision": revision,
                                        "kind": "score",
                                        "options": {
                                            "format": fmt,
                                            "native_version": "4.70",
                                        },
                                        "destination": {
                                            "kind": "file",
                                            "path": str(path),
                                        },
                                    },
                                    ok=not refused,
                                )
                                require(
                                    value.get("source_revision", revision) == revision,
                                    "wrong export revision",
                                )
                                if refused:
                                    require(
                                        value["error"]["code"] == "Unsupported",
                                        "wrong refusal",
                                    )
                                    warnings = value["error"]["export_warnings"]
                                    require(
                                        not path.exists() and value.get("artifacts", []) == [],
                                        "published refusal",
                                    )
                                else:
                                    require(
                                        value["source_revision"] == revision,
                                        "wrong source revision",
                                    )
                                    warnings = value["export_warnings"]
                                    require(
                                        path.is_file() and len(value["artifacts"]) == 1,
                                        "missing artifact",
                                    )
                                    compare_xml(path, {})
                                    proc = run_dump(
                                        binary,
                                        assets,
                                        path,
                                        folder,
                                        "oracle",
                                        oracle_env,
                                    )
                                    require(
                                        proc.returncode == 0 and not proc.stderr,
                                        "oracle failed",
                                    )
                                    actual = json.loads(proc.stdout)
                                    save_json(
                                        folder / "associations.json",
                                        association_report(expected, actual),
                                    )
                                    compare_semantics(expected, actual)
                                compare_warnings(expected_warnings, warnings)
                                record["formats"][f"{fmt}-run"] = {
                                    "warnings": warnings,
                                    "refused": refused,
                                    "output_sha256": file_sha(path) if path.exists() else None,
                                }
                            record["revision"] = revision
                        except Exception as error:
                            record["errors"].append(str(error))
                        finally:
                            if doc is not None:
                                await call("score_close", {"document": doc})
                        require(
                            len(record["formats"]) == 2 or record["errors"],
                            "skipped exports",
                        )
                        record["status"] = "failed" if record["errors"] else "passed"
                        save_json(dest / "result.json", record)
                        records.append(record)
                        print(f"{record['status'].upper()} {name}", flush=True)
    return records
