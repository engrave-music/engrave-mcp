"""Summarize repeated bench_live_audio results without excluding observations.

Nearest-rank p95; seeded percentile bootstrap intervals. Per-run intervals
resample observations; pooled intervals resample runs, then observations within
each selected run. These do not model serial correlation or bound future maxima.
"""

import argparse
import json
import math
import random
import statistics
from pathlib import Path


def statistics_of(values):
    s = sorted(values)
    return dict(
        n=len(s),
        p50=statistics.median(s),
        p95=s[math.ceil(0.95 * len(s)) - 1],
        max=s[-1],
    )


def distribution(groups, resamples=2000, seed=20261102):
    result = statistics_of([x for g in groups for x in g])
    rng = random.Random(seed)
    estimates = []
    for _ in range(resamples):
        selected = rng.choices(groups, k=len(groups)) if len(groups) > 1 else groups
        estimates.append(statistics_of([x for g in selected for x in rng.choices(g, k=len(g))]))
    result["bootstrap_95_ci"] = {
        key: [
            sorted(r[key] for r in estimates)[49],
            sorted(r[key] for r in estimates)[1949],
        ]
        for key in ("p50", "p95", "max")
    }
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("results", type=Path, nargs="+")
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    runs = []
    cold, warm = [], []
    hashes = set()
    for path in a.results:
        d = json.loads(path.read_text(encoding="utf-8"))
        hashes.add(d["binary_sha256"])
        c, w = d["cold"], [x["ms"] for x in d["reuse"][1:]]
        if not c or not w or "summary" not in d:
            p.error(f"incomplete benchmark: {path}")
        cold.append(c)
        warm.append(w)
        runs.append(dict(path=str(path), cold=distribution([c]), warm=distribution([w])))
    if len(hashes) != 1:
        p.error("repeat analysis requires one identical executable hash")
    result = dict(
        binary_sha256=hashes.pop(),
        seed=20261102,
        resamples=2000,
        runs=runs,
        pooled=dict(cold=distribution(cold), warm=distribution(warm)),
        worst_run_p95=dict(
            cold=max(r["cold"]["p95"] for r in runs),
            warm=max(r["warm"]["p95"] for r in runs),
        ),
        limitations="Bootstrap assumes exchangeable observations within runs; empirical max CI is not a future bound. Five runs do not certify future tail behavior.",
    )
    a.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result["pooled"], indent=2))


if __name__ == "__main__":
    main()
