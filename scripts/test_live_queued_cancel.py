"""Real long audio export plus queued cancellation must not kill its owner."""

import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np
import soundfile as sf
from bench_export_latency import Client


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resources", "soundfont", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument(
        "--tempo",
        type=int,
        choices=(150, 300),
        default=300,
        help="Use 150 on faster hosts to keep real rendering past the watchdog window",
    )
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    score = out / "dense.musicxml"
    notes = "".join(
        f"<note><pitch><step>{step}</step><octave>4</octave></pitch><duration>120</duration><type>16th</type></note>"
        for step in "CDEFGABC" * 2
    )
    parts = []
    for p in range(1, 13):
        measures = []
        for m in range(1, 257):
            attributes = (
                (
                    "<attributes><divisions>480</divisions><time><beats>4</beats><beat-type>4</beat-type></time>"
                    "<clef><sign>G</sign><line>2</line></clef></attributes>"
                    f"<direction><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>{a.tempo}</per-minute>"
                    f'</metronome></direction-type><sound tempo="{a.tempo}"/></direction>'
                )
                if m == 1
                else ""
            )
            measures.append(f'<measure number="{m}">{attributes}{notes}</measure>')
        parts.append(f'<part id="P{p}">{"".join(measures)}</part>')
    part_list = "".join(
        f'<score-part id="P{p}"><part-name>Part {p}</part-name><midi-instrument id="I{p}"><midi-channel>{p}</midi-channel><midi-program>1</midi-program></midi-instrument></score-part>'
        for p in range(1, 13)
    )
    score.write_text(
        f'<?xml version="1.0" encoding="UTF-8"?><score-partwise version="4.0"><part-list>{part_list}</part-list>{"".join(parts)}</score-partwise>',
        encoding="utf-8",
    )
    c = Client(a.binary, a.resources, out / "server", "live", a.soundfont)
    result = {}
    try:
        doc = c.tool("score_open", {"path": str(score)})["document"]

        def args(path):
            return dict(
                name="score_export",
                arguments=dict(
                    document=doc,
                    kind="audio",
                    options={"format": "wav"},
                    destination={"kind": "file", "path": str(path)},
                ),
            )

        running_path, queued_path = out / "running.wav", out / "queued.wav"
        started = time.perf_counter()
        running = c.send("tools/call", args(running_path))
        time.sleep(0.6)
        queued = c.send("tools/call", args(queued_path))
        c.rpc("ping", {})  # Let the tool coroutine register its queued dispatch.
        cancel_start = time.perf_counter()
        c.send("notifications/cancelled", {"requestId": queued}, True)
        # Inspect every response directly to detect duplicates and wrong IDs.
        response = c.q.get(timeout=10)
        result.update(cancel_seconds=time.perf_counter() - cancel_start, queued_response=response)
        if response.get("error", {}).get("code") == -32603:
            result["exit"] = c.p.wait(timeout=3)
            raise AssertionError(result)
        assert response.get("id") == queued and response.get("error", {}).get("code") == -32800, response
        assert result["cancel_seconds"] < 1, result
        assert not queued_path.exists()
        # Flood more queued cancellations beyond queue capacity. This tests
        # immediate capacity release while the real owner remains busy.
        for i in range(80):
            ident = c.send("tools/call", args(queued_path))
            c.rpc("ping", {})
            c.send("notifications/cancelled", {"requestId": ident}, True)
            reply = c.q.get(timeout=3)
            assert reply.get("id") == ident and reply.get("error", {}).get("code") == -32800, reply
        response = c.q.get(timeout=150)
        assert response.get("id") == running and response.get("result", {}).get("structuredContent", {}).get("ok"), (
            response
        )
        result["running_seconds"] = time.perf_counter() - started
        assert result["running_seconds"] > 5.6, result
        samples, rate = sf.read(running_path, always_2d=True)
        assert rate == 44100 and samples.shape[1] == 2 and np.isfinite(samples).all()
        assert len(samples) > 0 and np.max(np.abs(samples)) > 0.01
        result["frames"] = len(samples)
        result["sha256"] = hashlib.sha256(running_path.read_bytes()).hexdigest()
        assert not queued_path.exists()
        assert not list(out.rglob(".engrave-stage-*"))
        reference = out / "reference.wav"
        c.tool("score_export", args(reference)["arguments"])
        assert hashlib.sha256(reference.read_bytes()).hexdigest() == result["sha256"]
        result["matches_uncancelled_reference"] = True
        # Reusing exactly the cancelled destination proves no reservation remains.
        retry = args(queued_path)["arguments"]
        retry.update(kind="score", options={"format": "mscx"})
        c.tool("score_export", retry)
        assert queued_path.stat().st_size > 0
        # Exercise success/cancel races on quick operations and require exactly
        # one terminal response, including with repeated notifications.
        for i in range(20):
            ident = c.send("tools/call", dict(name="score_export", arguments=retry))
            if i % 2:
                time.sleep(0.01)
            for _ in range(2):
                c.send("notifications/cancelled", {"requestId": ident}, True)
            reply = c.q.get(timeout=20)
            assert reply.get("id") == ident, reply
            assert reply.get("error", {}).get("code") == -32800 or reply.get("result", {}).get(
                "structuredContent", {}
            ).get("ok"), reply
        c.tool("score_outline", {"document": doc})
        assert c.q.empty() and not c.pending, "duplicate response"
        assert not list(out.rglob(".engrave-stage-*"))
        result["passed"] = True
    finally:
        if c.p.poll() is None:
            c.close()
        else:
            c.log.close()
        (out / "results.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
