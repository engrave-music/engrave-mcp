"""Background audio lane: owner concurrency, serialization, cancellation and teardown.

Requires the engrave-mcp-test binary: a destination containing ".test-audio-hold"
holds the running render on the audio lane (never its owner-thread setup) until
"<destination>.release" exists or the export is cancelled.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import queue
import subprocess
import threading
import time
from pathlib import Path

from engrave_mcp_testing.mcp_client import Client
from engrave_mcp_testing.paths import REPO_ROOT

SOURCE = REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx"
SINE = REPO_ROOT / "vendor/engrave/tests/data/engine_audio/sine.sf2"
WATCHDOG_MS = 1000


def wav(doc, path):
    return {"document": doc, "format": "wav", "path": str(path)}


def staging_left(directory):
    return list(directory.glob(".engrave-stage-*"))


class Lane:
    def __init__(self, client, out):
        self.client, self.out = client, out
        self.log = out / "stderr.log"

    def count(self, marker):
        return self.log.read_bytes().count(marker.encode())

    async def marker(self, marker, count=1):
        deadline = time.monotonic() + 30
        while self.count(marker) < count:
            assert self.client.proc.returncode is None, (marker, self.client.proc.returncode)
            assert time.monotonic() < deadline, marker
            await asyncio.sleep(0.005)

    async def open(self):
        return (await self.client.tool("score_open", {"path": str(SOURCE)}))["document"]

    async def revision(self, doc):
        return (await self.client.tool("score_outline", {"document": doc}))["revision"]

    async def held(self, doc, name):
        """Start an audio export and return once its render holds on the lane."""
        path = self.out / f"{name}.test-audio-hold.wav"
        path.unlink(missing_ok=True)
        Path(str(path) + ".release").unlink(missing_ok=True)
        identity, future = self.client.begin("tools/call", {"name": "score_export", "arguments": wav(doc, path)})
        await self.client.proc.stdin.drain()
        await self.marker(f"AUDIO_LANE_HOLD {path.name}")
        return identity, future, path

    @staticmethod
    def release(path):
        Path(str(path) + ".release").touch()

    @staticmethod
    def result(response):
        assert "result" in response, response
        return response["result"]["structuredContent"]

    async def cancel_silently(self, identity, future):
        """Legacy cancellation sends no response: the ping proves none arrived first."""
        await self.client.notify("notifications/cancelled", {"requestId": identity})
        await self.client.call("ping", {})
        assert not future.done(), future.result()
        self.client.pending.pop(identity)


async def scenarios(args, out):
    evidence = {}
    os.environ["ENGRAVE_MCP_TEST_WATCHDOG_MS"] = str(WATCHDOG_MS)
    try:
        client = await Client(args, out).__aenter__()
    finally:
        os.environ.pop("ENGRAVE_MCP_TEST_WATCHDOG_MS", None)
    try:
        lane = Lane(client, out)
        doc = await lane.open()
        base = await lane.revision(doc)
        reference_path = out / "reference.wav"
        reference = await client.tool("score_export", wav(doc, reference_path))
        assert reference["source_revision"] == base, reference
        reference_bytes = reference_path.read_bytes()

        # 1. Query and edit the exporting document while its render runs: the
        # export keeps its setup revision and pre-edit audio.
        identity, future, path = await lane.held(doc, "same-doc")
        note = (await client.tool("score_query", {"document": doc, "mode": "elements", "query": {"types": ["Note"]}}))[
            "elements"
        ][0]["id"]
        edited = await client.tool(
            "score_edit",
            {"document": doc, "expected_revision": base, "ops": [{"op": "set_pitch", "note": note, "pitch": 30}]},
        )
        assert edited["revision"] != base and not future.done()
        lane.release(path)
        held = lane.result(await asyncio.wait_for(future, 60))
        assert held["ok"] and held["source_revision"] == base, held
        assert path.read_bytes() == reference_bytes
        after_path = out / "after-edit.wav"
        after = await client.tool("score_export", wav(doc, after_path))
        assert after["source_revision"] == edited["revision"], after
        assert after_path.read_bytes() != reference_bytes
        evidence["same_document_edit"] = {"export_revision": base, "edited_revision": edited["revision"]}

        # 2. Another document is edited and queried meanwhile.
        other = (await client.tool("score_create", {"template_id": "Treble_Clef"}))["document"]
        fresh = await lane.open()
        identity, future, path = await lane.held(fresh, "other-doc")
        other_edit = await client.tool(
            "score_edit", {"document": other, "ops": [{"op": "insert_measures", "count": 2}]}
        )
        await client.tool("score_query", {"document": other, "mode": "elements", "query": {"limit": 2}})
        assert other_edit["ok"] and not future.done()
        lane.release(path)
        assert lane.result(await asyncio.wait_for(future, 60))["ok"]
        assert path.read_bytes() == reference_bytes
        evidence["other_document_edit"] = True

        # 3. Closing the exporting document succeeds at once; the render does not need it.
        closing = await lane.open()
        closing_revision = await lane.revision(closing)
        identity, future, path = await lane.held(closing, "closed-doc")
        await client.tool("score_close", {"document": closing})
        missing = await client.tool("score_outline", {"document": closing}, error=True)
        assert missing["error"]["code"] == "NotFound" and not future.done(), missing
        lane.release(path)
        held = lane.result(await asyncio.wait_for(future, 60))
        assert held["ok"] and held["source_revision"] == closing_revision, held
        assert path.read_bytes() == reference_bytes
        evidence["close_during_render"] = True

        # 4. Replacing the SoundFonts mid-render succeeds at once; the running
        # render keeps its fonts and the next export uses the new selection.
        identity, future, path = await lane.held(fresh, "soundfont")
        await client.tool("runtime_configure", {"soundfonts": [str(SINE)]})
        assert not future.done()
        lane.release(path)
        assert lane.result(await asyncio.wait_for(future, 60))["ok"]
        assert path.read_bytes() == reference_bytes
        sine_path = out / "sine.wav"
        await client.tool("score_export", wav(fresh, sine_path))
        assert sine_path.read_bytes() != reference_bytes
        await client.tool("runtime_configure", {"soundfonts": [str(args.soundfont.resolve())]})
        restored_path = out / "restored.wav"
        await client.tool("score_export", wav(fresh, restored_path))
        assert restored_path.read_bytes() == reference_bytes
        evidence["soundfont_change_during_render"] = True

        # 5. A second audio export waits in the host queue, not ResourceLimit,
        # and starts only after the first has finished.
        first_id, first, first_path = await lane.held(fresh, "serial-1")
        queued = lane.count("AUDIO_LANE_QUEUED")
        second_path = out / "serial-2.test-audio-hold.wav"
        second_path.unlink(missing_ok=True)
        Path(str(second_path) + ".release").unlink(missing_ok=True)
        second_id, second = client.begin("tools/call", {"name": "score_export", "arguments": wav(fresh, second_path)})
        await client.proc.stdin.drain()
        await lane.marker("AUDIO_LANE_QUEUED", queued + 1)
        await client.tool("score_query", {"document": fresh, "mode": "page_count"})
        assert not first.done() and not second.done()
        assert lane.count(f"AUDIO_LANE_HOLD {second_path.name}") == 0
        lane.release(first_path)
        assert lane.result(await asyncio.wait_for(first, 60))["ok"]
        await lane.marker(f"AUDIO_LANE_HOLD {second_path.name}")
        lane.release(second_path)
        assert lane.result(await asyncio.wait_for(second, 60))["ok"]
        assert first_path.read_bytes() == second_path.read_bytes() == reference_bytes

        # Cancelling the queued export drops it without disturbing the running one.
        running_id, running, running_path = await lane.held(fresh, "serial-3")
        queued = lane.count("AUDIO_LANE_QUEUED")
        dropped_path = out / "serial-4.test-audio-hold.wav"
        dropped_path.unlink(missing_ok=True)
        Path(str(dropped_path) + ".release").unlink(missing_ok=True)
        dropped_id, dropped = client.begin(
            "tools/call", {"name": "score_export", "arguments": wav(fresh, dropped_path)}
        )
        await client.proc.stdin.drain()
        await lane.marker("AUDIO_LANE_QUEUED", queued + 1)
        await lane.cancel_silently(dropped_id, dropped)
        assert not running.done()
        lane.release(running_path)
        assert lane.result(await asyncio.wait_for(running, 60))["ok"]
        assert running_path.read_bytes() == reference_bytes
        await client.tool("score_export", wav(fresh, out / "after-drop.wav"))
        assert lane.count(f"AUDIO_LANE_HOLD {dropped_path.name}") == 0 and not dropped_path.exists()
        evidence["serialized_and_queued_cancel"] = True

        # 6. Cancelling a running render frees the lane quickly, leaves nothing
        # staged and never reaches the watchdog's exit 70.
        identity, future, path = await lane.held(fresh, "cancel")
        await lane.cancel_silently(identity, future)
        recovery = out / "after-cancel.wav"
        await client.tool("score_export", wav(fresh, recovery))
        assert recovery.read_bytes() == reference_bytes
        await asyncio.sleep((WATCHDOG_MS + 500) / 1000)
        assert client.proc.returncode is None
        await client.call("ping", {})
        assert not path.exists() and not staging_left(out)
        evidence["running_cancel"] = True

        # 7a. EOF shuts the session down: the held render is cancelled and the
        # owner joins the lane before the Engine goes.
        identity, future, path = await lane.held(fresh, "shutdown")
        client.proc.stdin.close()
        shutdown = lane.result(await asyncio.wait_for(future, 15))
        assert shutdown["error"]["code"] == "Cancelled", shutdown
        assert await asyncio.wait_for(client.proc.wait(), 15) == 0
        await asyncio.wait_for(client.reader, 5)
        assert not path.exists() and not staging_left(out)
        evidence["shutdown_during_render"] = True
    finally:
        await client.__aexit__(None, None, None)
    return evidence


def write_failure(args, out):
    """7b. A failed stdout write (exit 74) cancels the held render within the 500 ms cleanup window."""
    out.mkdir(parents=True, exist_ok=True)
    command = [str(args.binary.resolve()), "--resources", str(args.resources.resolve())]
    command += ["--soundfont", str(args.soundfont.resolve())]
    # terminal_diagnostic writes only to a pipe.
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    log = bytearray()

    def drain():
        while data := proc.stderr.readline():
            log.extend(data)

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    def send(method, params, identity=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if identity is not None:
            message["id"] = identity
        proc.stdin.write((json.dumps(message) + "\n").encode())
        proc.stdin.flush()

    def read():
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
        return json.loads(lines.get(timeout=30))

    path = out / "exit74.test-audio-hold.wav"
    path.unlink(missing_ok=True)
    Path(str(path) + ".release").unlink(missing_ok=True)
    try:
        send(
            "initialize",
            {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "lane", "version": "1"}},
            1,
        )
        assert read()["id"] == 1
        send("notifications/initialized", {})
        send("tools/call", {"name": "score_open", "arguments": {"path": str(SOURCE)}}, 2)
        doc = read()["result"]["structuredContent"]["document"]
        send("tools/call", {"name": "score_export", "arguments": wav(doc, path)}, 3)
        deadline = time.monotonic() + 30
        while f"AUDIO_LANE_HOLD {path.name}".encode() not in log:
            assert proc.poll() is None and time.monotonic() < deadline, bytes(log)
            time.sleep(0.005)
        proc.stdout.close()
        send("tools/list", {}, 4)
        assert proc.wait(timeout=10) == 74
        reader.join(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        (out / "stderr.log").write_bytes(log)
    assert b"exit 74" in log and b"owners cleaned" in log, bytes(log)
    assert not path.exists() and not staging_left(out)
    return {"exit": 74, "owners_cleaned": True}


async def legacy_gate_wait_cancel(args, out):
    """Legacy cancellation while an audio export blocks on publication_gate must prevent publication."""
    out.mkdir(parents=True, exist_ok=True)
    barrier = out / "write.barrier"
    arm = Path(str(barrier) + ".arm")
    pdf_path = out / "owner.pdf"
    for p in (barrier, arm, pdf_path):
        p.unlink(missing_ok=True)
    os.environ["ENGRAVE_MCP_TEST_WRITE_BARRIER"] = str(barrier)
    try:
        client = await Client(args, out).__aenter__()
    finally:
        os.environ.pop("ENGRAVE_MCP_TEST_WRITE_BARRIER", None)
    try:
        lane = Lane(client, out)
        doc = await lane.open()
        # 1. Start and hold the audio export on the audio lane first so its
        # owner-thread prepare_audio has already finished.
        audio_id, audio_future, audio_path = await lane.held(doc, "cancelled-at-gate")

        # 2. Arm the synchronous report barrier so the owner PDF holds publication_gate.mutex.
        arm.touch()
        _, pdf_future = client.begin(
            "tools/call",
            {"name": "score_export", "arguments": {"document": doc, "format": "pdf", "path": str(pdf_path)}},
        )
        await client.proc.stdin.drain()
        await lane.marker("WRITE_REGISTERED")

        # 3. Release the held audio render so it reaches begin_publication and blocks on the gate.
        lane.release(audio_path)
        await lane.marker("PUBLICATION_LOCK_WAITING")

        # 4. Send legacy cancellation while audio is blocked at the gate, wait for CANCEL_HANDLED,
        # then release the PDF report barrier.
        cancels = lane.count("CANCEL_HANDLED")
        await client.notify("notifications/cancelled", {"requestId": audio_id})
        await lane.marker("CANCEL_HANDLED", cancels + 1)
        barrier.touch()

        pdf_res = lane.result(await asyncio.wait_for(pdf_future, 30))
        assert pdf_res["ok"] and pdf_path.is_file(), pdf_res
        # Run a follow-up audio export on the same serialized lane so the cancelled
        # audio export is guaranteed to have finished unwinding and staging cleanup.
        followup_path = out / "after-gate-cancel.wav"
        followup_path.unlink(missing_ok=True)
        followup = await client.tool("score_export", wav(doc, followup_path))
        assert followup["ok"] and followup_path.is_file(), followup
        assert not audio_future.done(), audio_future.result()
        client.pending.pop(audio_id, None)
        assert not audio_path.exists(), "cancelled audio export published after gate wait"
        assert not staging_left(out), staging_left(out)
    finally:
        barrier.touch()
        await client.__aexit__(None, None, None)
    return {"legacy_gate_wait_cancelled": True}


def sync_report_failure_with_audio_active(args, out):
    """Synchronous publish_report failure on the owner must still give the active audio lane its 500 ms cleanup."""
    out.mkdir(parents=True, exist_ok=True)
    command = [str(args.binary.resolve()), "--resources", str(args.resources.resolve())]
    command += ["--soundfont", str(args.soundfont.resolve())]
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    log = bytearray()

    def drain():
        while data := proc.stderr.readline():
            log.extend(data)

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    def send(method, params, identity=None):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if identity is not None:
            message["id"] = identity
        proc.stdin.write((json.dumps(message) + "\n").encode())
        proc.stdin.flush()

    def read():
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
        return json.loads(lines.get(timeout=30))

    audio_path = out / "sync-fail.test-audio-hold.wav"
    pdf_path = out / "owner-sync-fail.pdf"
    for p in (audio_path, Path(str(audio_path) + ".release"), pdf_path):
        p.unlink(missing_ok=True)
    try:
        send(
            "initialize",
            {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "lane", "version": "1"}},
            1,
        )
        assert read()["id"] == 1
        send("notifications/initialized", {})
        send("tools/call", {"name": "score_open", "arguments": {"path": str(SOURCE)}}, 2)
        doc = read()["result"]["structuredContent"]["document"]
        send("tools/call", {"name": "score_export", "arguments": wav(doc, audio_path)}, 3)
        deadline = time.monotonic() + 30
        while f"AUDIO_LANE_HOLD {audio_path.name}".encode() not in log:
            assert proc.poll() is None and time.monotonic() < deadline, bytes(log)
            time.sleep(0.005)
        # Close stdout, then trigger a synchronous owner publish_report write failure while audio is active.
        proc.stdout.close()
        send(
            "tools/call",
            {"name": "score_export", "arguments": {"document": doc, "format": "pdf", "path": str(pdf_path)}},
            4,
        )
        assert proc.wait(timeout=10) == 74
        reader.join(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        (out / "stderr.log").write_bytes(log)
    assert b"exit 74" in log and b"owners cleaned" in log, bytes(log)
    assert not audio_path.exists() and not staging_left(out)
    return {"exit": 74, "owners_cleaned": True}


async def post_failure_recovery(args, out):
    """Failed release_lane post recovers via complete(); failed complete post exits 74 instead of wedging."""
    out.mkdir(parents=True, exist_ok=True)
    release_trigger = out / "fail-release.trigger"
    complete_trigger = out / "fail-complete.trigger"
    first_path = out / "post-first.test-audio-hold.wav"
    second_path = out / "post-second.wav"
    third_path = out / "post-third.wav"
    for p in (
        release_trigger,
        complete_trigger,
        first_path,
        Path(str(first_path) + ".release"),
        second_path,
        third_path,
    ):
        p.unlink(missing_ok=True)

    os.environ["ENGRAVE_MCP_TEST_FAIL_AUDIO_RELEASE_POST"] = str(release_trigger)
    os.environ["ENGRAVE_MCP_TEST_FAIL_AUDIO_COMPLETE_POST"] = str(complete_trigger)
    try:
        client = await Client(args, out).__aenter__()
    finally:
        os.environ.pop("ENGRAVE_MCP_TEST_FAIL_AUDIO_RELEASE_POST", None)
        os.environ.pop("ENGRAVE_MCP_TEST_FAIL_AUDIO_COMPLETE_POST", None)
    try:
        lane = Lane(client, out)
        doc = await lane.open()
        # 1. Inject release_lane post failure while a second audio export is queued.
        _, first_future, held_path = await lane.held(doc, "post-first")
        _, second_future = client.begin("tools/call", {"name": "score_export", "arguments": wav(doc, second_path)})
        await client.proc.stdin.drain()
        await lane.marker("AUDIO_LANE_QUEUED")
        release_trigger.touch()
        lane.release(held_path)
        first_res = lane.result(await asyncio.wait_for(first_future, 60))
        second_res = lane.result(await asyncio.wait_for(second_future, 60))
        assert first_res["ok"] and held_path.is_file(), first_res
        assert second_res["ok"] and second_path.is_file(), second_res
        assert not release_trigger.exists()

        # 2. Inject complete post failure: process must exit 74 explicitly rather than hanging.
        complete_trigger.touch()
        client.begin("tools/call", {"name": "score_export", "arguments": wav(doc, third_path)})
        await client.proc.stdin.drain()
        rc = await asyncio.wait_for(client.proc.wait(), 30)
        assert rc == 74, rc
    finally:
        await client.__aexit__(None, None, None)
    return {"release_post_recovered": True, "complete_post_exit": 74}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    session = out / "session"
    session.mkdir(parents=True, exist_ok=True)
    evidence = asyncio.run(scenarios(args, session))
    evidence["write_failure_during_render"] = write_failure(args, out / "exit74")
    evidence["legacy_gate_wait_cancel"] = asyncio.run(legacy_gate_wait_cancel(args, out / "gate_cancel"))
    evidence["sync_report_failure_with_audio_active"] = sync_report_failure_with_audio_active(args, out / "sync_exit74")
    evidence["post_failure_recovery"] = asyncio.run(post_failure_recovery(args, out / "post_fail"))
    (out / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence))


if __name__ == "__main__":
    main()
