"""Live open, independent cold-to-PDF sessions, mixed-document soak.

Raw samples include every timed request; RSS probes are outside latency timers.
Open/close churn is measured separately from the fixed-document live soak because
DEFECT-ENGRAVE-001 is an inherited document-lifetime leak.
"""

import argparse
import hashlib
import json
import time
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_mcp_testing.process_client import Client, export_args, rss_bytes, summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resources", "soundfont", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--open-only", action="store_true")
    p.add_argument("--soak-only", action="store_true")
    p.add_argument("--operations", type=int, default=1000)
    a = p.parse_args()
    assert a.operations >= 1000 and a.operations % 100 == 0
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=True)
    result = dict(
        binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
        opens=[],
        cold=[],
    )

    def save():
        (a.output / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")

    def timed(c, tool, args):
        start = time.perf_counter()
        reply = c.tool(tool, args)
        return reply, (time.perf_counter() - start) * 1000

    for fixture in () if a.soak_only else ("repeat70.mscx", "lyrics.mscx"):
        path = str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden" / fixture)
        c = Client(a.binary, a.resources, a.output / fixture, "live", a.soundfont)
        samples, rss = [], []
        try:
            for i in range(101):
                doc, ms = timed(c, "score_open", dict(path=path))
                c.tool("score_close", dict(document=doc["document"]))
                if i:
                    samples.append(ms)
                    rss.append(rss_bytes(c.p.pid))
        finally:
            c.close()
        events = []
        for line in c.log_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("OPEN_TIMING "):
                events.append(json.loads(line[len("OPEN_TIMING ") :]))
        result["opens"].append(
            dict(
                fixture=fixture,
                ms=summary(samples),
                samples_ms=samples,
                rss_bytes=rss,
                observer_events=events,
            )
        )
        save()
        if a.open_only:
            continue
        samples = []
        for i in range(30):
            start = time.perf_counter()
            c = Client(a.binary, a.resources, a.output / fixture / str(i), "live", a.soundfont)
            try:
                doc = c.tool("score_open", dict(path=path))["document"]
                c.tool("score_export", export_args(doc, "pdf", a.output / "cold.pdf"))
                samples.append((time.perf_counter() - start) * 1000)
            finally:
                c.close()
        result["cold"].append(dict(fixture=fixture, ms=summary(samples), samples_ms=samples))
        save()
    if not a.open_only:
        c = Client(a.binary, a.resources, a.output / "soak", "live", a.soundfont)
        try:
            docs = [
                c.tool("score_open", dict(path=str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden" / f)))[
                    "document"
                ]
                for f in ("repeat70.mscx", "lyrics.mscx")
            ]
            measures = [c.tool("score_outline", dict(document=d))["outline"]["measures"][0]["id"] for d in docs]
            svg = a.output / "svg"
            svg.mkdir(exist_ok=True)
            samples = []
            # Each 10-operation cycle visits both documents and all five operation kinds.
            # 200 excluded warmups initialize audio, caches and both edit states.
            for i in range(200 + a.operations):
                kind, di = i % 5, (i // 5) % 2
                doc = docs[di]
                if kind == 0:
                    name, args = "score_export", export_args(doc, "svg", svg)
                elif kind == 1:
                    name, args = (
                        "score_export",
                        export_args(doc, "pdf", a.output / "soak.pdf"),
                    )
                elif kind == 2:
                    name, args = (
                        "score_export",
                        export_args(doc, "mscz", a.output / "soak.mscz"),
                    )
                elif kind == 3:
                    name, args = (
                        "score_export",
                        {"document": doc, "format": "wav", "path": str(a.output / "soak.wav")},
                    )
                else:
                    name, args = (
                        "score_edit",
                        dict(
                            document=doc,
                            ops=[
                                dict(
                                    op="set_layout_break",
                                    measure=measures[di],
                                    kind="page",
                                    on=bool((i // 10) % 2),
                                )
                            ],
                        ),
                    )
                _, ms = timed(c, name, args)
                if i >= 200:
                    samples.append(dict(operation=kind, document=di, ms=ms, rss=rss_bytes(c.p.pid)))
            blocks = [
                dict(
                    ms=summary([r["ms"] for r in samples[i : i + 100]]),
                    mean_ms=sum(r["ms"] for r in samples[i : i + 100]) / 100,
                    by_kind={
                        str(k): summary([r["ms"] for r in samples[i : i + 100] if r["operation"] == k])
                        for k in range(5)
                    },
                    rss=summary([r["rss"] for r in samples[i : i + 100]]),
                )
                for i in range(0, a.operations, 100)
            ]
            result["soak"] = dict(
                warmup=200,
                samples=samples,
                blocks=blocks,
                drift_percent=100 * (blocks[-1]["mean_ms"] / blocks[0]["mean_ms"] - 1),
                drift_definition="last/first 100-operation arithmetic mean minus one; identical operation/document mix",
            )
        finally:
            c.close()
        save()
    print(json.dumps({k: v for k, v in result.items() if k != "soak"}, default=str)[:1000])
    if "soak" in result:
        print("drift_percent", result["soak"]["drift_percent"])


if __name__ == "__main__":
    main()
