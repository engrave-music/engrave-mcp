"""Release-host audio latency/RSS and unsynchronized cancellation measurements.

Records samples before enforcing cancellation limits. Does not infer callback
phase from wall time: cancellation at 50 ms may hit setup, synthesis or encoding.
"""

import argparse
import hashlib
import json
import time
from pathlib import Path

from support.paths import REPO_ROOT
from support.process_client import Client, rss_bytes, summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resources", "soundfont", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--warm", type=int, default=1000)
    p.add_argument("--cold", type=int, default=30)
    a = p.parse_args()
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=True)
    result = {
        "binary_sha256": hashlib.sha256(a.binary.read_bytes()).hexdigest(),
        "cold": [],
        "reuse": [],
        "cancel": [],
    }

    def opened(label):
        c = Client(a.binary, a.resources, a.output / label, "live", a.soundfont)
        doc = c.tool("score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
            "document"
        ]
        args = dict(
            document=doc,
            kind="audio",
            options={"format": "wav"},
            destination={"kind": "file", "path": str(a.output / (label + ".wav"))},
        )
        return c, args

    def save():
        (a.output / "results.json").write_text(json.dumps(result, indent=2))

    for i in range(a.cold):
        c, args = opened("cold-" + str(i))
        try:
            start = time.perf_counter()
            c.tool("score_export", args)
            result["cold"].append((time.perf_counter() - start) * 1000)
        finally:
            c.close()
    c, args = opened("reuse")
    try:
        for i in range(a.warm + 1):
            start = time.perf_counter()
            c.tool("score_export", args)
            result["reuse"].append(dict(ms=(time.perf_counter() - start) * 1000, rss=rss_bytes(c.p.pid)))
            if i % 100 == 0:
                save()
        for i in range(30):
            path = a.output / f"cancel-{i}.wav"
            args["destination"]["path"] = str(path)
            ident = c.send("tools/call", dict(name="score_export", arguments=args))
            time.sleep(0.05)
            start = time.perf_counter()
            c.send("notifications/cancelled", dict(requestId=ident), True)
            response = c.receive(ident)
            result["cancel"].append(dict(ms=(time.perf_counter() - start) * 1000, response=response))
            assert response.get("error", {}).get("code") == -32800, response
            assert not path.exists()
    finally:
        c.close()
        save()
    result["summary"] = dict(
        cold_ms=summary(result["cold"]),
        first_ms=result["reuse"][0]["ms"],
        warm_ms=summary([r["ms"] for r in result["reuse"][1:]]),
        cancel_ms=summary([r["ms"] for r in result["cancel"]]),
        blocks=[
            dict(
                ms=summary([r["ms"] for r in result["reuse"][i : i + 100]]),
                rss=summary([r["rss"] for r in result["reuse"][i : i + 100]]),
            )
            for i in range(1, len(result["reuse"]), 100)
        ],
    )
    save()
    print(json.dumps(result["summary"], indent=2))
    assert result["summary"]["cancel_ms"]["p95"] <= 50 and result["summary"]["cancel_ms"]["max"] <= 100, (
        "Audio cancellation gate failed; see saved samples"
    )


if __name__ == "__main__":
    main()
