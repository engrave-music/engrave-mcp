"""Exercise v2 session scheduling, capture, cancellation and shutdown over stdio."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import time

import psutil

from v2_client import Client, ROOT, TOOLS, VALIDATORS

RETIRED = {"score_get", "score_replace_measures", "score_query_region", "score_set_pitches",
           "score_generate_excerpts", "score_list_excerpts", "runtime_add_font", "runtime_set_soundfont",
           "score_export_start", "score_job_get", "score_job_cancel", "score_audio_tracks"}

def export(doc, path, action="start"):
    return {"document": doc, "action": action, "kind": "score", "options": {"format": "musicxml"},
            "destination": {"kind": "file", "path": str(path)}}


async def eof_pipeline(args, out):
    command = [str(args.binary.resolve())]
    if args.resources: command += ["--resources", str(args.resources.resolve())]
    if args.soundfont: command += ["--soundfont", str(args.soundfont.resolve())]
    messages = [{"jsonrpc": "2.0", "id": 0, "method": "initialize",
                 "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                            "clientInfo": {"name": "eof", "version": "1"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}]
    messages.extend({"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {
        "name": "score_create", "arguments": {"template_id": "Treble_Clef"}}} for i in range(1, 5))
    with (out / "eof-pipeline-stderr.log").open("wb") as log:
        proc = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=log)
        wire = b"".join((json.dumps(message) + "\n").encode() for message in messages)
        stdout, _ = await asyncio.wait_for(proc.communicate(wire), 15)
        assert proc.returncode == 0, proc.returncode
        assert {x["id"] for x in map(json.loads, stdout.splitlines())} == {0, 1, 2, 3, 4}


async def transport_backpressure(args, out):
    command = [str(args.binary.resolve())]
    if args.resources: command += ["--resources", str(args.resources.resolve())]
    if args.soundfont: command += ["--soundfont", str(args.soundfont.resolve())]
    with (out / "backpressure-stderr.log").open("wb") as log:
        proc = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=log)
        initialize = {"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "pressure", "version": "1"}}}
        proc.stdin.write((json.dumps(initialize) + "\n").encode())
        await proc.stdin.drain()
        assert json.loads(await asyncio.wait_for(proc.stdout.readline(), 10))["id"] == 0
        proc.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n')
        total = 20000
        async def produce():
            for start in range(1, total + 1, 64):
                proc.stdin.write(b"".join((json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping"},
                    separators=(",", ":")) + "\n").encode() for i in range(start, min(start + 64, total + 1))))
                await proc.stdin.drain()
        producer = asyncio.create_task(produce())
        await asyncio.sleep(.5)  # Deliberately leave stdout unread while the writer fills its bounded queue.
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
        assert info["limits"] == {"input_bytes": 8388608, "base64_chars": 11184812, "batch_ops": 1024,
            "query_results": 4096, "output_bytes": "1073741824", "queue_capacity": 8, "deadline_ms": 120000,
            "export_workers": 2, "snapshot_bytes": "67108864", "transport_request_bytes": 4194304,
            "worker_request_bytes": 4194304}, info["limits"]
        doc = (await client.tool("score_open", {"path": str(ROOT / "fixtures/golden/linked_parts.mscx")}))["document"]
        outline = await client.tool("score_outline", {"document": doc})
        base = outline["revision"]
        notes = await client.tool("score_query", {"document": doc, "mode": "elements", "query": {"types": ["Note"]}})
        note = notes["elements"][0]["id"]
        await client.tool("score_describe", {"scope": "type", "type": "Note"})
        edits = [{"document": doc, "expected_revision": base, "ops": [
            {"op": "set_pitch", "note": {"kind": "id", "id": note}, "pitch": pitch}]} for pitch in (72, 74, 76, 77)]
        reads = [client.tool("score_query", {"document": doc, "mode": "elements", "query": {"limit": 2}}) for _ in range(4)]
        responses = await asyncio.gather(*(client.call("tools/call", {"name": "score_edit", "arguments": e}) for e in edits), *reads)
        writes = [r["result"]["structuredContent"] for r in responses[:4]]
        for result in writes: VALIDATORS["score_edit"].validate(result)
        assert sum(x["ok"] for x in writes) == 1 and all(x["ok"] or x["error"]["code"] == "RevisionConflict" for x in writes), writes
        revision = next(x["revision"] for x in writes if x["ok"])
        assert all(x["revision"] in (base, revision) for x in responses[4:]), responses[4:]

        reference = out / "reference.musicxml"
        direct = await client.tool("score_export", export(doc, reference, "run"))
        assert direct["source_revision"] == revision and reference.stat().st_size > 0
        snapshot_path = out / "snapshot.test-slow.musicxml"
        snapshot = await client.tool("score_export", {**export(doc, snapshot_path), "expected_revision": revision})
        assert snapshot["source_revision"] == revision
        changed = await client.tool("score_edit", {"document": doc, "expected_revision": revision, "ops": [
            {"op": "set_pitch", "note": {"kind": "id", "id": note}, "pitch": 79}]})
        assert changed["revision"] != revision
        conflict = await client.tool("score_export", {**export(doc, out / "stale.musicxml"), "expected_revision": revision}, error=True)
        assert conflict["error"]["code"] == "RevisionConflict"
        if args.test_hooks:
            status = (await client.tool("runtime_job", {"action": "get", "job": snapshot["job"]}))["status"]
            assert status["state"] in ("queued", "running"), status
        await client.tool("score_close", {"document": doc})
        done = await client.terminal(snapshot["job"])
        assert done["state"] == "succeeded" and done["source_revision"] == revision, done
        assert snapshot_path.read_bytes() == reference.read_bytes()

        # A failed second page publication must retain its first published path
        # in both a waiting export error and an inspected background job.
        partial_doc = (await client.tool("score_open", {"path": str(ROOT / "fixtures/golden/linked_parts.mscx")}))["document"]
        measure = (await client.tool("score_outline", {"document": partial_doc}))["outline"]["measures"][0]["id"]
        await client.tool("score_edit", {"document": partial_doc, "ops": [{"op": "set_layout_break",
            "measure": {"kind": "id", "id": measure}, "kind": "page", "on": True}]})
        for action in ("run", "start"):
            directory = out / f"partial-{action}"
            directory.mkdir(exist_ok=True)
            (directory / "page-0001.png").unlink(missing_ok=True)
            (directory / "page-0002.png").mkdir(exist_ok=True)
            result = await client.tool("score_export", {"document": partial_doc, "action": action, "kind": "render",
                "options": {"format": "png", "dpi": 36, "last_page": 1},
                "destination": {"kind": "directory", "path": str(directory)}}, error=action == "run")
            if action == "start":
                result = await client.terminal(result["job"])
                assert result["state"] == "failed", result
            assert result["error"]["code"] == "Io" and len(result["error"]["published_artifacts"]) == 1, result
            assert Path(result["error"]["published_artifacts"][0]).resolve() == (directory / "page-0001.png").resolve(), result
            assert (directory / "page-0001.png").is_file()
        await client.tool("score_close", {"document": partial_doc})

        if args.test_hooks and args.soundfont:
            asset_doc = (await client.tool("score_open", {"path": str(ROOT / "fixtures/evening.musicxml")}))["document"]
            audio_path = out / "asset-lease.test-slow.wav"
            audio_path.unlink(missing_ok=True)
            captured = await client.tool("score_export", {"document": asset_doc, "action": "start", "kind": "audio",
                "options": {"format": "wav"}, "destination": {"kind": "file", "path": str(audio_path)}})
            changed_runtime = await client.tool("runtime_info", {"action": "set_soundfonts", "files": []})
            assert changed_runtime["capabilities"]["soundfonts"] == []
            await client.tool("score_close", {"document": asset_doc})
            asset_result = await client.terminal(captured["job"])
            assert asset_result["state"] == "succeeded" and audio_path.stat().st_size > 0, asset_result
            restored = await client.tool("runtime_info", {"action": "set_soundfonts", "files": [str(args.soundfont.resolve())]})
            assert len(restored["capabilities"]["soundfonts"]) == 1
            evidence["captured_audio_asset_survives_runtime_change"] = True

        doc = (await client.tool("score_create", {"template_id": "Treble_Clef"}))["document"]
        if args.test_hooks:
            capture_path = out / "cancel.test-capture-slow.musicxml"
            capture_path.unlink(missing_ok=True)
            capture_id, capture_future = client.begin("tools/call", {"name": "score_export", "arguments":
                export(doc, capture_path, "run")})
            await client.proc.stdin.drain()
            for _ in range(300):
                if (await client.tool("test_capture_state", {}))["polling"]: break
                await asyncio.sleep(.01)
            else: raise AssertionError("engine capture did not enter its cancellation callback")
            await client.notify("notifications/cancelled", {"requestId": capture_id})
            capture_result = await asyncio.wait_for(capture_future, 10)
            assert capture_result["error"]["code"] == -32800 and not capture_path.exists(), capture_result
            deadline_path = out / "deadline.test-capture-deadline.musicxml"
            deadline_path.unlink(missing_ok=True)
            deadline_result = await client.tool("score_export", export(doc, deadline_path, "run"), error=True)
            assert deadline_result["error"]["code"] == "DeadlineExceeded" and not deadline_path.exists(), deadline_result
            jobs = []
            for i in range(8):
                path = out / f"capacity-{i}.test-slow.musicxml"
                path.unlink(missing_ok=True)
                jobs.append((await client.tool("score_export", export(doc, path)))["job"])
                if i == 0:
                    before = (await client.tool("test_capture_count", {}))["count"]
                    conflict = await client.tool("score_export", export(doc, path), error=True)
                    assert conflict["error"]["code"] == "InvalidArgument", conflict
                    bad = await client.tool("score_export", export(doc, out / "missing-parent" / "bad.musicxml"), error=True)
                    assert bad["error"]["code"] == "InvalidArgument", bad
                    assert (await client.tool("test_capture_count", {}))["count"] == before
            before = (await client.tool("test_capture_count", {}))["count"]
            rejected = await client.tool("score_export", export(doc, out / "overflow.test-slow.musicxml"), error=True)
            assert rejected["error"]["code"] == "ResourceLimit", rejected
            assert (await client.tool("test_capture_count", {}))["count"] == before
            states = [(await client.tool("runtime_job", {"action": "get", "job": j}))["status"]["state"] for j in jobs]
            assert states.count("running") == 2 and states.count("queued") == 6, states
            await client.tool("runtime_job", {"action": "cancel", "job": jobs[-1]})
            assert (await client.terminal(jobs[-1]))["state"] == "cancelled"
            await client.tool("runtime_job", {"action": "cancel", "job": jobs[0]})
            assert (await client.terminal(jobs[0]))["state"] == "cancelled"
            for job_id in jobs[1:-1]: await client.tool("runtime_job", {"action": "cancel", "job": job_id})
            for job_id in jobs[1:-1]: assert (await client.terminal(job_id))["state"] == "cancelled"
            assert not list(out.glob("capacity-*.test-slow.musicxml"))

            slow_id, slow_future = client.begin("tools/call", {"name": "score_export", "arguments":
                export(doc, out / "request-cancel.test-slow.musicxml", "run")})
            await client.proc.stdin.drain()
            await asyncio.sleep(.3)
            await client.notify("notifications/cancelled", {"requestId": slow_id})
            cancelled = await asyncio.wait_for(slow_future, 15)
            assert cancelled["error"]["code"] == -32800, cancelled
            assert not (out / "request-cancel.test-slow.musicxml").exists()

            stall_id, stall = client.begin("tools/call", {"name": "test_owner_sleep", "arguments": {}})
            await client.proc.stdin.drain()
            for _ in range(100):
                if (await client.tool("test_owner_state", {}))["busy"]: break
                await asyncio.sleep(.01)
            else: raise AssertionError("owner stall did not start")
            started = time.monotonic()
            status = await client.tool("runtime_job", {"action": "get", "job": jobs[0]})
            assert time.monotonic() - started < .8 and status["status"]["state"] == "cancelled"
            queued_id, queued = client.begin("tools/call", {"name": "score_edit", "arguments":
                {"document": doc, "ops": [{"op": "insert_measures", "count": 1}]}})
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
                if (await client.tool("test_owner_state", {}))["busy"]: break
                await asyncio.sleep(.01)
            else: raise AssertionError("second owner stall did not start")
            pending = [client.begin("tools/call", {"name": "score_outline", "arguments": {"document": doc}})
                       for _ in range(80)]
            await client.proc.stdin.drain()
            for identity, future in pending:
                if not future.done(): await client.notify("notifications/cancelled", {"requestId": identity})
            responses = await asyncio.wait_for(asyncio.gather(*(future for _, future in pending)), 15)
            codes = [x.get("error", {}).get("code") for x in responses]
            assert -32000 in codes and -32800 in codes, codes
            assert all(code in (-32000, -32800, None) for code in codes), codes
            await asyncio.wait_for(stall, 10)

            shutdown_path = out / "shutdown.test-slow.musicxml"
            shutdown_path.unlink(missing_ok=True)
            active = await client.tool("score_export", export(doc, shutdown_path))
            for _ in range(100):
                status = (await client.tool("runtime_job", {"action": "get", "job": active["job"]}))["status"]
                if status["state"] == "running": break
                await asyncio.sleep(.02)
            else: raise AssertionError("shutdown worker did not start")
            run_path = out / "shutdown-run.test-slow.musicxml"
            run_path.unlink(missing_ok=True)
            _, pending_run = client.begin("tools/call", {"name": "score_export", "arguments": export(doc, run_path, "run")})
            await client.proc.stdin.drain()
            for _ in range(100):
                if len(psutil.Process(client.proc.pid).children(recursive=True)) >= 2: break
                await asyncio.sleep(.02)
            else: raise AssertionError("pending run worker did not start")
            children = psutil.Process(client.proc.pid).children(recursive=True)
            assert len(children) >= 2, "running worker processes not observed"
            await client.close()
            result = (await pending_run)["result"]
            assert result["isError"] and result["structuredContent"]["error"]["code"] == "Cancelled", result
            VALIDATORS["score_export"].validate(result["structuredContent"])
            assert not shutdown_path.exists() and not run_path.exists() and not list(out.glob(".musescore-job-*"))
            assert all(not child.is_running() for child in children), [child.pid for child in children]
            evidence.update(queue_limit=8, running_limit=2, queued_and_running_cancel=True,
                            request_cancel=True, capture_cancel=True, capture_deadline=True,
                            queued_owner_cancel=True, running_owner_committed=True,
                            job_manager_responsive_while_owner_busy=True, eof_reaped_worker=True,
                            destination_reservation=True)
        else:
            await client.tool("score_close", {"document": doc})
    await eof_pipeline(args, out)
    evidence["backpressure_responses"] = await transport_backpressure(args, out)
    evidence.update(retired_tools=len(RETIRED), concurrent_edits=4, concurrent_queries=4,
                    snapshot_revision=revision, close_with_active_job=True, eof_pipeline=True,
                    published_artifacts_on_run_and_job_error=True,
                    output_schema_validation=True, runtime_limits=info["limits"], status="passed")
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
