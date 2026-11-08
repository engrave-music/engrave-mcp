"""Two-request watchdog and publication-report preflight regressions (test host)."""

import argparse
import json
import os
import re
import time
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_mcp_testing.process_client import Client, export_args


def wait_log(c, marker):
    end = time.monotonic() + 30
    while marker not in c.log_path.read_text():
        assert c.p.poll() is None
        assert time.monotonic() < end, marker
        time.sleep(0.005)


def prepare(c):
    doc = c.tool("score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/repeat70.mscx")})[
        "document"
    ]
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
    return doc


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ("binary", "resources", "output"):
        p.add_argument("--" + k, type=Path, required=True)
    a = p.parse_args()
    a.output = a.output.resolve()
    os.environ["ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS"] = "5100"
    os.environ["ENGRAVE_MCP_TEST_SLOW_SECOND_FLUSH"] = "1"
    results = []
    for partial in (False, True):
        out = a.output / ("partial" if partial else "complete")
        c = Client(a.binary, a.resources, out)
        try:
            doc = prepare(c)
            (out / ".test-live-noncooperative").mkdir(exist_ok=True)
            first = c.send(
                "tools/call",
                {
                    "name": "score_export",
                    "arguments": export_args(doc, "svg", out / ".test-live-noncooperative", True),
                },
            )
            wait_log(c, "LIVE_NONCOOPERATIVE")
            pages = out / "pages"
            pages.mkdir(exist_ok=True)
            if partial:
                (pages / "page-0002.svg").mkdir(exist_ok=True)
            second = c.send(
                "tools/call",
                {
                    "name": "score_export",
                    "arguments": export_args(doc, "svg", pages, True),
                },
            )
            c.send("notifications/cancelled", {"requestId": first}, True)
            response1 = c.receive(first)
            assert response1["error"]["code"] == -32603, response1
            wait_log(c, "WATCHDOG_CLAIMED")
            wait_log(c, "PUBLICATION_SECOND_FLUSH")
            wait_log(c, "WATCHDOG_LOCK_WAITING")
            response2 = c.receive(second)["result"]["structuredContent"]
            if partial:
                assert response2["error"]["code"] == "Io", response2
                reported = response2["error"]["published_artifacts"]
            else:
                assert response2["ok"], response2
                reported = [v["path"] for v in response2["artifacts"]]
            visible = sorted(f.as_posix() for f in pages.glob("*.svg") if f.is_file())
            assert sorted(reported) == visible and visible
            assert not list(pages.glob(".engrave-stage-*"))
            assert c.tool("score_outline", {"document": doc})["ok"]
            results.append(dict(partial=partial, first=response1, second=response2))
        finally:
            c.close()
    os.environ.pop("ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS")
    os.environ.pop("ENGRAVE_MCP_TEST_SLOW_SECOND_FLUSH")
    # Same real two-page export, injected report budget immediately on either
    # side of its computed bound. No thousands-page rendering is necessary.
    target = a.output / "boundary-pages"
    target.mkdir(exist_ok=True)
    bound = None
    for label in ("measure", "under", "over"):
        if bound is not None:
            os.environ["ENGRAVE_MCP_TEST_REPORT_LIMIT"] = str(bound + (1 if label == "under" else -1))
        c = Client(a.binary, a.resources, a.output / label)
        try:
            doc = prepare(c)
            result = c.tool(
                "score_export",
                export_args(doc, "svg", target, True),
                ok=label != "over",
            )
            if label == "measure":
                bound = int(re.findall(r"PUBLICATION_REPORT_BOUND (\d+)", c.log_path.read_text())[-1])
            if label == "over":
                assert result["error"]["code"] == "ResourceLimit", result
                assert not list(target.iterdir()), list(target.iterdir())
            else:
                assert len(result["artifacts"]) >= 2
                for artifact in result["artifacts"]:
                    Path(artifact["path"]).unlink()
            assert not list(target.glob(".engrave-stage-*"))
            results.append(dict(boundary=label, bound=bound, result=result))
        finally:
            c.close()
    os.environ.pop("ENGRAVE_MCP_TEST_REPORT_LIMIT", None)
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
    print("PASS: cross-request complete/partial publication and report-size boundary")


if __name__ == "__main__":
    main()
