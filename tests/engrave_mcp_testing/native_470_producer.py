"""Real MCP producer and fail-closed, per-format B1 comparison.

Imported lazily so baseline replay does not require the MCP Python package.
"""

from __future__ import annotations

import asyncio
import json
import os
import platform
from pathlib import Path

import anyio
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from engrave_mcp_testing.native_470 import (
    check_file,
    compare_semantics,
    compare_warnings,
    compare_xml,
    file_sha,
    read_json,
    require,
    run_dump,
    save_json,
    sha,
)


class RecordingStream:
    """Retain the JSON-RPC envelopes at the SDK stream boundary, including ids."""

    def __init__(self, stream, direction, transcript):
        self.stream, self.direction, self.transcript = stream, direction, transcript

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        return False

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return await self.receive()
        except anyio.EndOfStream:
            raise StopAsyncIteration

    def record(self, value):
        if isinstance(value, Exception):
            envelope = {"transport_error": str(value)}
        else:
            envelope = value.message.model_dump(mode="json", by_alias=True, exclude_none=True)
        self.transcript.write(
            json.dumps({"direction": self.direction, "envelope": envelope}, ensure_ascii=False) + "\n"
        )
        self.transcript.flush()

    async def send(self, value):
        self.record(value)
        await self.stream.send(value)

    async def receive(self):
        value = await self.stream.receive()
        self.record(value)
        return value


async def produce(args, cases, fixture_root, output, binary, assets, oracle_env):

    host = args.host_binary.resolve()
    host_hash = file_sha(host)
    require(host_hash == args.host_sha256, "host binary SHA-256 mismatch")
    require(args.host_resources.is_dir(), "host resources missing")
    records = []
    params = StdioServerParameters(
        command=str(host),
        args=["--resources", str(args.host_resources.resolve())],
        env=dict(os.environ),
    )
    with (
        (output / "host.stderr").open("w", encoding="utf-8") as errors,
        (output / "mcp.jsonl").open("w", encoding="utf-8") as transcript,
    ):
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(
                RecordingStream(read, "host", transcript),
                RecordingStream(write, "client", transcript),
            ) as session:
                initialized = await session.initialize()
                save_json(
                    output / "initialize.json",
                    initialized.model_dump(mode="json", by_alias=True),
                )
                for case in cases:
                    dest = output / case["id"]
                    dest.mkdir()
                    envelopes = []

                    async def call(tool, arguments):
                        response = await asyncio.wait_for(session.call_tool(tool, arguments), 90)
                        envelopes.append(
                            {
                                "tool": tool,
                                "arguments": arguments,
                                "response": response.model_dump(mode="json", by_alias=True),
                            }
                        )
                        save_json(dest / "envelopes.json", envelopes)
                        value = response.structured_content
                        require(
                            isinstance(value, dict) and isinstance(value.get("ok"), bool),
                            f"{tool}: missing host envelope",
                        )
                        require(
                            bool(response.is_error) == (not value["ok"]),
                            f"{tool}: inconsistent error envelope",
                        )
                        return value

                    source = check_file(fixture_root, case, "source")
                    record = {
                        "case": case["id"],
                        "category": case["category"],
                        "platform": platform.system(),
                        "source_sha256": file_sha(source),
                        "expected_semantics_sha256": case["expected_semantics_sha256"],
                        "expected_warnings_sha256": case["warnings_sha256"],
                        "host_sha256": host_hash,
                        "formats": {},
                        "errors": [],
                    }
                    doc = None
                    try:
                        opened = await call("score_open", {"path": str(source)})
                        require(opened["ok"], f"score_open failed: {opened}")
                        doc = opened["document"]
                        # Even the unsupported-target fixture gets both 4.70 probes;
                        # its frozen 4.90 request is additionally checked separately.
                        versions = ["4.70"] + ([case["target_version"]] if case["target_version"] != "4.70" else [])
                        for version in versions:
                            for fmt in ("mscx", "mscz"):
                                label = fmt if version == "4.70" else f"{version}-{fmt}"
                                folder = dest / label
                                folder.mkdir()
                                path = folder / f"score.{fmt}"
                                arguments = {
                                    "document": doc,
                                    "format": fmt,
                                    "path": str(path),
                                    "options": {"native_version": version},
                                }
                                request = json.dumps(
                                    {"name": "score_export", "arguments": arguments},
                                    ensure_ascii=False,
                                    separators=(",", ":"),
                                ).encode()
                                (folder / "request.json").write_bytes(request)
                                value = await call("score_export", arguments)
                                warnings = (
                                    value.get("export_warnings")
                                    if value["ok"]
                                    else value.get("error", {}).get("export_warnings")
                                )
                                save_json(folder / "warnings.json", warnings)
                                result = {
                                    "source_sha256": record["source_sha256"],
                                    "target_version": version,
                                    "request_sha256": sha(request),
                                    "host_result": value,
                                    "outcome": "written" if value["ok"] else "rejected",
                                    "output_sha256": file_sha(path) if path.is_file() else None,
                                    "warnings_sha256": file_sha(folder / "warnings.json"),
                                    "warnings_matched": False,
                                    "oracle_matched": None,
                                    "errors": [],
                                }
                                try:
                                    require(
                                        path.is_file() == value["ok"],
                                        "host skipped output or published on refusal",
                                    )
                                    if version != "4.70":
                                        require(
                                            not value["ok"] and value["error"]["code"] == "Unsupported",
                                            "unsupported target did not reach engine rejection",
                                        )
                                    if value["ok"]:
                                        require(
                                            len(value.get("artifacts", [])) == 1,
                                            "host artifact count mismatch",
                                        )
                                        artifact = value["artifacts"][0]
                                        require(
                                            Path(artifact["path"]).resolve() == path
                                            and int(artifact["size"]) == path.stat().st_size > 0,
                                            "host artifact mismatch",
                                        )
                                    expected_warnings = read_json(check_file(fixture_root, case, "warnings"))
                                    # 4.70 probes of the unsupported-version request use ordinary source expectations.
                                    if version == "4.70" and case["target_version"] != "4.70":
                                        expected_warnings = []
                                    try:
                                        compare_warnings(expected_warnings, warnings)
                                        result["warnings_matched"] = True
                                    except Exception as error:
                                        result["errors"].append(str(error))
                                    expected_success = case["category"] != "rejected" or (
                                        version == "4.70" and case["target_version"] != "4.70"
                                    )
                                    if value["ok"] != expected_success:
                                        result["errors"].append("writer outcome differs from frozen contract")
                                    # Always compare successful output, even when warnings failed.
                                    if value["ok"]:
                                        try:
                                            compare_xml(path, case.get("xml_checks", {}), source)
                                        except Exception as error:
                                            result["errors"].append(str(error))
                                        proc = run_dump(
                                            binary,
                                            assets,
                                            path,
                                            folder,
                                            "oracle",
                                            oracle_env,
                                        )
                                        result["oracle_exit"] = proc.returncode
                                        result["oracle_stdout_sha256"] = sha(proc.stdout)
                                        result["oracle_stderr_sha256"] = sha(proc.stderr)
                                        result["oracle_matched"] = False
                                        require(
                                            proc.returncode == 0 and not proc.stderr,
                                            "oracle failed or reported diagnostics",
                                        )
                                        expected = read_json(check_file(fixture_root, case, "expected_semantics"))
                                        actual = json.loads(proc.stdout)
                                        from engrave_mcp_testing.native_470_edits import association_report

                                        save_json(
                                            folder / "associations.json",
                                            association_report(expected, actual),
                                        )
                                        compare_semantics(expected, actual)
                                        result["oracle_matched"] = True
                                except Exception as error:
                                    result["errors"].append(str(error))
                                save_json(folder / "result.json", result)
                                record["formats"][label] = result
                    except Exception as error:
                        record["errors"].append(str(error))
                    finally:
                        if doc is not None:
                            try:
                                closed = await call("score_close", {"document": doc})
                                require(closed["ok"], "score_close failed")
                            except Exception as error:
                                record["errors"].append(str(error))
                        require(
                            file_sha(source) == record["source_sha256"],
                            "source changed during export",
                        )
                        save_json(dest / "result.json", record)
                    required = {"mscx", "mscz"}
                    if case["target_version"] != "4.70":
                        required |= {f"{case['target_version']}-{fmt}" for fmt in ("mscx", "mscz")}
                    if set(record["formats"]) != required:
                        record["errors"].append("host silently skipped fixture or format")
                    record["status"] = (
                        "failed"
                        if record["errors"] or any(result["errors"] for result in record["formats"].values())
                        else "passed"
                    )
                    save_json(dest / "result.json", record)
                    print(f"{record['status'].upper()} {case['id']}", flush=True)
                    records.append(record)
                from engrave_mcp_testing.native_470_corpus import corpus_parity

                records.extend(await corpus_parity(args, session, output, binary, assets, oracle_env))
    if getattr(args, "live_edits", False):
        from engrave_mcp_testing.native_470_edits import live_edits

        records.extend(await live_edits(args, fixture_root, output, binary, assets, oracle_env))
    require(file_sha(host) == host_hash, "host binary changed during run")
    return records
