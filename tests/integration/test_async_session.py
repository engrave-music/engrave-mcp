"""Exercise session scheduling, live export, cancellation and shutdown over stdio."""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from support.mcp_client import REPO_ROOT, TOOLS, VALIDATORS, Client

RETIRED = {
    "score_get",
    "score_replace_measures",
    "score_query_region",
    "score_set_pitches",
    "score_generate_excerpts",
    "score_list_excerpts",
    "runtime_add_font",
    "runtime_set_soundfont",
    "runtime_job",
    "score_export_start",
    "score_job_get",
    "score_job_cancel",
    "score_audio_tracks",
}


def export(doc, path):
    return {
        "document": doc,
        "kind": "score",
        "options": {"format": "musicxml"},
        "destination": {"kind": "file", "path": str(path)},
    }


async def eof_pipeline(args, out):
    command = [str(args.binary.resolve())]
    if args.resources:
        command += ["--resources", str(args.resources.resolve())]
    if args.soundfont:
        command += ["--soundfont", str(args.soundfont.resolve())]
    messages = [
        {
            "jsonrpc": "2.0",
            "id": 0,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "eof", "version": "1"},
            },
        },
        {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
    ]
    messages.extend(
        {
            "jsonrpc": "2.0",
            "id": i,
            "method": "tools/call",
            "params": {
                "name": "score_create",
                "arguments": {"template_id": "Treble_Clef"},
            },
        }
        for i in range(1, 5)
    )
    with (out / "eof-pipeline-stderr.log").open("wb") as log:
        proc = await asyncio.create_subprocess_exec(
            *command,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=log,
        )
        wire = b"".join((json.dumps(message) + "\n").encode() for message in messages)
        stdout, _ = await asyncio.wait_for(proc.communicate(wire), 15)
        assert proc.returncode == 0, proc.returncode
        assert {x["id"] for x in map(json.loads, stdout.splitlines())} == {
            0,
            1,
            2,
            3,
            4,
        }


async def transport_backpressure(args, out):
    command = [str(args.binary.resolve())]
    if args.resources:
        command += ["--resources", str(args.resources.resolve())]
    if args.soundfont:
        command += ["--soundfont", str(args.soundfont.resolve())]
    with (out / "backpressure-stderr.log").open("wb") as log:
        proc = await asyncio.create_subprocess_exec(
            *command,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=log,
        )
        initialize = {
            "jsonrpc": "2.0",
            "id": 0,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "pressure", "version": "1"},
            },
        }
        proc.stdin.write((json.dumps(initialize) + "\n").encode())
        await proc.stdin.drain()
        assert json.loads(await asyncio.wait_for(proc.stdout.readline(), 10))["id"] == 0
        proc.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n')
        total = 20000

        async def produce():
            for start in range(1, total + 1, 64):
                proc.stdin.write(
                    b"".join(
                        (
                            json.dumps(
                                {"jsonrpc": "2.0", "id": i, "method": "ping"},
                                separators=(",", ":"),
                            )
                            + "\n"
                        ).encode()
                        for i in range(start, min(start + 64, total + 1))
                    )
                )
                await proc.stdin.drain()

        producer = asyncio.create_task(produce())
        await asyncio.sleep(0.5)  # Deliberately leave stdout unread while the writer fills its bounded queue.
        assert not producer.done(), "input was not backpressured while stdout was unread"

        async def consume():
            seen = set()
            for _ in range(total):
                message = json.loads(await proc.stdout.readline())
                assert message["result"] == {}, message
                seen.add(message["id"])
            assert seen == set(range(1, total + 1))

        await asyncio.wait_for(asyncio.gather(producer, consume()), 45)
        proc.stdin.close()
        await asyncio.wait_for(proc.wait(), 15)
        assert proc.returncode == 0, proc.returncode
    return total


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    evidence = {"binary": str(args.binary.resolve())}
    async with Client(args, out) as client:
        listed = (await client.call("tools/list", {}))["result"]["tools"]
        names = {x["name"] for x in listed}
        assert set(TOOLS) <= names and not names.intersection(RETIRED), names
        for name in RETIRED:
            response = await client.call("tools/call", {"name": name, "arguments": {}})
            assert response["error"]["code"] == -32602, (name, response)
        for name in TOOLS:
            result = await client.tool(name, [], error=True)
            assert result["error"]["code"] == "InvalidArgument", result
        info = await client.tool("runtime_info", {})
        assert info["limits"] == {
            "input_bytes": 8388608,
            "base64_chars": 11184812,
            "batch_ops": 1024,
            "query_results": 4096,
            "output_bytes": "1073741824",
            "deadline_ms": 120000,
            "cancellation": "cooperative",
            "transport_request_bytes": 4194304,
        }, info["limits"]
        doc = (
            await client.tool(
                "score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/linked_parts.mscx")}
            )
        )["document"]
        outline = await client.tool("score_outline", {"document": doc})
        base = outline["revision"]
        notes = await client.tool(
            "score_query",
            {"document": doc, "mode": "elements", "query": {"types": ["Note"]}},
        )
        note = notes["elements"][0]["id"]
        await client.tool("score_describe", {"scope": "type", "type": "Note"})
        edits = [
            {
                "document": doc,
                "expected_revision": base,
                "ops": [
                    {
                        "op": "set_pitch",
                        "note": {"kind": "id", "id": note},
                        "pitch": pitch,
                    }
                ],
            }
            for pitch in (72, 74, 76, 77)
        ]
        reads = [
            client.tool(
                "score_query",
                {"document": doc, "mode": "elements", "query": {"limit": 2}},
            )
            for _ in range(4)
        ]
        responses = await asyncio.gather(
            *(client.call("tools/call", {"name": "score_edit", "arguments": e}) for e in edits),
            *reads,
        )
        writes = [r["result"]["structuredContent"] for r in responses[:4]]
        for result in writes:
            VALIDATORS["score_edit"].validate(result)
        assert sum(x["ok"] for x in writes) == 1 and all(
            x["ok"] or x["error"]["code"] == "RevisionConflict" for x in writes
        ), writes
        revision = next(x["revision"] for x in writes if x["ok"])
        assert all(x["revision"] in (base, revision) for x in responses[4:]), responses[4:]

        reference = out / "reference.musicxml"
        direct = await client.tool("score_export", export(doc, reference))
        assert direct["source_revision"] == revision and reference.stat().st_size > 0
        snapshot_path = out / "snapshot.test-slow.musicxml"
        snapshot = await client.tool(
            "score_export",
            {**export(doc, snapshot_path), "expected_revision": revision},
        )
        assert snapshot["source_revision"] == revision
        changed = await client.tool(
            "score_edit",
            {
                "document": doc,
                "expected_revision": revision,
                "ops": [{"op": "set_pitch", "note": {"kind": "id", "id": note}, "pitch": 79}],
            },
        )
        assert changed["revision"] != revision
        conflict = await client.tool(
            "score_export",
            {**export(doc, out / "stale.musicxml"), "expected_revision": revision},
            error=True,
        )
        assert conflict["error"]["code"] == "RevisionConflict"
        await client.tool("score_close", {"document": doc})
        assert snapshot_path.read_bytes() == reference.read_bytes()

        # A failed second page publication must retain its first published path
        # in the synchronous export error.
        partial_doc = (
            await client.tool(
                "score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/linked_parts.mscx")}
            )
        )["document"]
        measure = (await client.tool("score_outline", {"document": partial_doc}))["outline"]["measures"][0]["id"]
        await client.tool(
            "score_edit",
            {
                "document": partial_doc,
                "ops": [
                    {
                        "op": "set_layout_break",
                        "measure": {"kind": "id", "id": measure},
                        "kind": "page",
                        "on": True,
                    }
                ],
            },
        )
        directory = out / "partial-live"
        directory.mkdir(exist_ok=True)
        (directory / "page-0001.png").unlink(missing_ok=True)
        (directory / "page-0002.png").mkdir(exist_ok=True)
        result = await client.tool(
            "score_export",
            {
                "document": partial_doc,
                "kind": "render",
                "options": {"format": "png", "dpi": 36, "last_page": 1},
                "destination": {"kind": "directory", "path": str(directory)},
            },
            error=True,
        )
        assert result["error"]["code"] == "Io" and len(result["error"]["published_artifacts"]) == 1, result
        assert Path(result["error"]["published_artifacts"][0]).resolve() == (directory / "page-0001.png").resolve(), (
            result
        )
        assert (directory / "page-0001.png").is_file()
        await client.tool("score_close", {"document": partial_doc})

        doc = (await client.tool("score_create", {"template_id": "Treble_Clef"}))["document"]
        if args.test_hooks:
            stall_id, stall = client.begin("tools/call", {"name": "test_owner_sleep", "arguments": {}})
            await client.proc.stdin.drain()
            for _ in range(100):
                if (await client.tool("test_owner_state", {}))["busy"]:
                    break
                await asyncio.sleep(0.01)
            else:
                raise AssertionError("owner stall did not start")
            queued_id, queued = client.begin(
                "tools/call",
                {
                    "name": "score_edit",
                    "arguments": {
                        "document": doc,
                        "ops": [{"op": "insert_measures", "count": 1}],
                    },
                },
            )
            await client.proc.stdin.drain()
            await client.notify("notifications/cancelled", {"requestId": queued_id})
            await client.notify("notifications/cancelled", {"requestId": stall_id})
            assert (await asyncio.wait_for(queued, 10))["error"]["code"] == -32800
            assert (await asyncio.wait_for(stall, 10))["result"]["structuredContent"]["slept_ms"] == 2000

            # Fill the transport's outstanding-call bound while the owner is
            # busy, then cancel queued reads and account for every response.
            _, stall = client.begin("tools/call", {"name": "test_owner_sleep", "arguments": {}})
            await client.proc.stdin.drain()
            for _ in range(100):
                if (await client.tool("test_owner_state", {}))["busy"]:
                    break
                await asyncio.sleep(0.01)
            else:
                raise AssertionError("second owner stall did not start")
            pending = [
                client.begin(
                    "tools/call",
                    {"name": "score_outline", "arguments": {"document": doc}},
                )
                for _ in range(80)
            ]
            await client.proc.stdin.drain()
            for identity, future in pending:
                if not future.done():
                    await client.notify("notifications/cancelled", {"requestId": identity})
            responses = await asyncio.wait_for(asyncio.gather(*(future for _, future in pending)), 15)
            codes = [x.get("error", {}).get("code") for x in responses]
            assert -32000 in codes and -32800 in codes, codes
            assert all(code in (-32000, -32800, None) for code in codes), codes
            await asyncio.wait_for(stall, 10)

        else:
            await client.tool("score_close", {"document": doc})
    await eof_pipeline(args, out)
    evidence["backpressure_responses"] = await transport_backpressure(args, out)
    evidence.update(
        retired_tools=len(RETIRED),
        concurrent_edits=4,
        concurrent_queries=4,
        export_revision=revision,
        eof_pipeline=True,
        published_artifacts_on_error=True,
        output_schema_validation=True,
        runtime_limits=info["limits"],
        status="passed",
    )
    (out / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path)
    parser.add_argument("--soundfont", type=Path)
    parser.add_argument("--test-hooks", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
