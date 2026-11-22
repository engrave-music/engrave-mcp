"""Live audio parity, reuse, cancellation and watchdog acceptance probe."""

import argparse
import concurrent.futures
import hashlib
import json
import os
import time
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_mcp_testing.process_client import Client, rss_bytes, summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "test-binary", "resources", "soundfont", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument(
        "--reuse-iterations",
        type=int,
        default=21,
        help="exports in one live session for reuse timing and RSS (minimum 5)",
    )
    a = p.parse_args()
    if a.reuse_iterations < 5:
        p.error("--reuse-iterations must be at least 5")
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=True)
    results = {"parity": [], "cancellation": [], "reuse": []}

    def client(mode, label, test=False):
        return Client(
            a.test_binary if test else a.binary,
            a.resources,
            a.output / label,
            mode,
            a.soundfont,
        )

    def opened(c):
        return c.tool("score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
            "document"
        ]

    def args(doc, kind, fmt, path, overrides=None):
        return {"document": doc, "format": fmt, "path": str(path), "options": {**(overrides or {})}}

    def wait_marker(c, marker, offset):
        until = time.monotonic() + 20
        while marker not in c.log_path.read_text(errors="replace")[offset:]:
            if time.monotonic() > until:
                raise AssertionError("missing " + marker)
            time.sleep(0.001)

    def parity(fmt, label, mix):
        hashes = {}
        for mode in ("fresh", "live"):
            c = client(mode, f"parity-{fmt}-{label}-{mode}")
            try:
                doc = opened(c)
                path = a.output / f"{fmt}-{label}-{mode}"
                c.tool("score_export", args(doc, "audio", fmt, path, mix))
                hashes[mode] = hashlib.sha256(path.read_bytes()).hexdigest()
                if mode == "live":
                    # Changing per-export mix must not contaminate a later render.
                    c.tool(
                        "score_export",
                        args(doc, "audio", fmt, path, {"master_volume_db": -20}),
                    )
                    c.tool("score_export", args(doc, "audio", fmt, path, mix))
                    assert hashlib.sha256(path.read_bytes()).hexdigest() == hashes[mode]
            finally:
                c.close()
        return dict(
            format=fmt,
            mix=label,
            hashes=hashes,
            identical=len(set(hashes.values())) == 1,
        )

    mixes = [
        ("default", {}),
        (
            "mix",
            {
                "master_volume_db": -6,
                "master_balance": 0.3,
                "tracks": [{"track": 0, "volume_db": -3, "soundfont": 0}],
            },
        ),
    ]
    cases = [(fmt, label, mix) for fmt in ("wav", "flac", "opus", "mp3") for label, mix in mixes]
    # Parity compares output hashes only, so its sessions may overlap; it
    # finishes before the timing and RSS sections below start.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results["parity"] = list(pool.map(lambda case: parity(*case), cases))
    c = client("live", "reuse")
    try:
        doc = opened(c)
        # Remove and restore selection on the same Engine. A second distinct
        # SoundFont is not part of the pinned test assets.
        selected_path = a.output / "selection.wav"
        selection_args = args(doc, "audio", "wav", selected_path)
        c.tool("score_export", selection_args)
        selected_hash = hashlib.sha256(selected_path.read_bytes()).hexdigest()
        c.tool("runtime_configure", {"soundfonts": []})
        assert c.tool("score_export", selection_args, False)["error"]["code"] == "Unsupported"
        c.tool(
            "runtime_configure",
            {"soundfonts": [str(a.soundfont.resolve())]},
        )
        c.tool("score_export", selection_args)
        assert hashlib.sha256(selected_path.read_bytes()).hexdigest() == selected_hash
        results["selection_restore"] = "A / disabled / A: identical WAV"
        c.tool(
            "score_export",
            args(doc, "audio", "wav", a.output / "release.test-live-noncooperative.wav"),
        )
        results["release_hook_inert"] = True
        # A fresh session below retains a meaningful first-export measurement.
    finally:
        c.close()
    c = client("live", "reuse-first")
    try:
        doc = opened(c)
        for i in range(a.reuse_iterations):
            start = time.perf_counter()
            c.tool("score_export", args(doc, "audio", "wav", a.output / "reuse.wav"))
            results["reuse"].append(dict(ms=(time.perf_counter() - start) * 1000, rss=rss_bytes(c.p.pid)))
    finally:
        c.close()
    # One cold export, then the warm exports split into halves.
    mid = 1 + (len(results["reuse"]) - 1) // 2
    results["reuse_summary"] = dict(
        first_ms=results["reuse"][0]["ms"],
        warm_ms=summary([r["ms"] for r in results["reuse"][1:]]),
        first_half_rss=summary([r["rss"] for r in results["reuse"][1:mid]]),
        last_half_rss=summary([r["rss"] for r in results["reuse"][mid:]]),
    )
    for kind, fmt in [("audio", "wav"), ("render", "pdf"), ("score", "mscx")]:
        c = client("live", "cancel-" + kind, True)
        samples = []
        try:
            doc = opened(c)
            for i in range(30):
                path = a.output / f"{kind}.test-live-cancel"
                offset = len(c.log_path.read_text(errors="replace"))
                ident = c.send(
                    "tools/call",
                    dict(name="score_export", arguments=args(doc, kind, fmt, path)),
                )
                wait_marker(c, "LIVE_CANCEL_POLL", offset)
                start = time.perf_counter()
                c.send("notifications/cancelled", dict(requestId=ident), True)
                c.cancelled(ident, doc)
                response = None
                samples.append((time.perf_counter() - start) * 1000)

                assert not path.exists()
            c.tool(
                "score_export",
                args(doc, "audio", "wav", a.output / f"recovery-{kind}.wav"),
            )
        finally:
            c.close()
        results["cancellation"].append(
            dict(
                kind=kind,
                phase="owner engine callback: poll 2 render / poll 8 audio or writer",
                method="test-only hold at callback; notification write through cancellation response",
                ms=summary(samples),
                samples_ms=samples,
            )
        )
    # Audio keeps the production 5 s watchdog end to end; the others only
    # need the claim path, so they use the test-hook duration.
    short_watchdog = dict(
        ENGRAVE_MCP_TEST_WATCHDOG_MS="200",
        ENGRAVE_MCP_TEST_WATCHDOG_SETTLE_MS="40",
    )
    for kind, fmt in [("audio", "wav"), ("render", "pdf"), ("score", "mscx")]:
        env = {} if kind == "audio" else short_watchdog
        os.environ.update(env)
        try:
            c = client("live", "watchdog-" + kind, True)
        finally:
            for key in env:
                os.environ.pop(key, None)
        doc = opened(c)
        path = a.output / f"{kind}.test-live-noncooperative"
        ident = c.send(
            "tools/call",
            dict(name="score_export", arguments=args(doc, kind, fmt, path)),
        )
        wait_marker(c, "LIVE_NONCOOPERATIVE", 0)
        start = time.perf_counter()
        c.send("notifications/cancelled", dict(requestId=ident), True)
        response = c.receive(ident)
        elapsed = time.perf_counter() - start
        assert response.get("error", {}).get("code") == -32603, response
        if kind == "audio":
            assert 4.9 <= elapsed < 6.5, elapsed
        else:
            assert 0.18 <= elapsed < 2.0, elapsed
        assert c.p.wait(timeout=2) == 70
        assert not path.exists()
        c.log.close()
        results.setdefault("watchdog", []).append(dict(kind=kind, seconds=elapsed, exit=70, response=response))
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
    assert all(r["identical"] for r in results["parity"]), (
        "parity drift: inspect recorded hashes; do not regenerate pins"
    )
    assert all(r["ms"]["p95"] <= 50 and r["ms"]["max"] <= 100 for r in results["cancellation"])
    print(json.dumps({k: v for k, v in results.items() if k not in ("reuse", "parity")}, indent=2))


if __name__ == "__main__":
    main()
