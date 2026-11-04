"""Cancellation during a slow second SVG flush must preserve publication reporting.

Run with the test-hook host on Windows and POSIX; no timing-only phase inference.
"""

import argparse
import hashlib
import json
import os
import time
from pathlib import Path

from bench_export_latency import ROOT, Client, export_args


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("binary", "resources", "output"):
        p.add_argument("--" + key, type=Path, required=True)
    a = p.parse_args()
    a.output = a.output.resolve()
    os.environ["ENGRAVE_MCP_TEST_SLOW_SECOND_FLUSH"] = "1"
    results = []
    for fail in (False, True):
        out = a.output / ("partial" if fail else "complete")
        c = Client(a.binary, a.resources, out)
        try:
            doc = c.tool("score_open", {"path": str(ROOT / "fixtures/golden/repeat70.mscx")})["document"]
            measure = c.tool("score_outline", {"document": doc})["outline"]["measures"][0]["id"]
            c.tool(
                "score_edit",
                {
                    "document": doc,
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
            pages = out / "pages"
            pages.mkdir(exist_ok=True)
            if fail:
                (pages / "page-0002.svg").mkdir(exist_ok=True)
            ident = c.send(
                "tools/call",
                {
                    "name": "score_export",
                    "arguments": export_args(doc, "svg", pages, True),
                },
            )
            deadline = time.monotonic() + 30
            while "PUBLICATION_SECOND_FLUSH" not in c.log_path.read_text():
                assert time.monotonic() < deadline, "flush seam not reached"
                assert c.p.poll() is None
                time.sleep(0.01)
            first = pages / "page-0001.svg"
            before = hashlib.sha256(first.read_bytes()).hexdigest()
            assert list(pages.glob(".engrave-stage-*")), "second artifact must still be staged"
            start = time.monotonic()
            c.send("notifications/cancelled", {"requestId": ident}, True)
            response = c.receive(ident)
            elapsed = time.monotonic() - start
            assert elapsed > 5, elapsed
            result = response["result"]["structuredContent"]
            if fail:
                assert result["error"]["code"] == "Io", result
                reported = result["error"]["published_artifacts"]
                assert reported == [first.as_posix()], result
                assert (pages / "page-0002.svg").is_dir()
            else:
                assert result["ok"], result
                reported = [v["path"] for v in result["artifacts"]]
                assert len(reported) >= 2, result
            assert before == hashlib.sha256(first.read_bytes()).hexdigest()
            visible = sorted(p.as_posix() for p in pages.glob("*.svg") if p.is_file())
            assert visible == sorted(reported), (visible, reported)
            for path in visible:
                data = Path(path).read_bytes()
                assert b"<svg" in data and data.rstrip().endswith(b"</svg>"), path
            assert not list(pages.glob(".engrave-stage-*"))
            assert c.tool("score_outline", {"document": doc})["ok"]
            results.append(dict(partial=fail, cancel_to_result_s=elapsed, response=response))
        finally:
            c.close()
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
    print("PASS: slow publication survives watchdog; complete/partial results and cleanup verified")


if __name__ == "__main__":
    main()
