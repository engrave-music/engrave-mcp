"""Stage-synchronized 4.70 interruption; requires engrave-mcp-test.

The test binary pauses its capture callback on serialization unit 2. The client
sends the ordinary MCP cancellation notification only after observing that marker.
No timing estimate selects the conversion stage. Production uses the same atomic
cancellation flag and callback, without the pause or stderr diagnostics.
"""

from __future__ import annotations

import argparse
import copy
import json
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from integration.test_native_470_conversion_cancel import Client, sha
from support.paths import REPO_ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tree = ET.parse(REPO_ROOT / "tests/data/native_470/notes.mscx")
    staff = tree.find("./Score/Staff")
    measures = list(staff.findall("Measure"))
    for _ in range(511):
        for measure in measures:
            staff.append(copy.deepcopy(measure))
    for parent in tree.iter():
        for eid in list(parent.findall("eid")):
            parent.remove(eid)
    source = out / "stress.mscx"
    tree.write(source, encoding="utf-8", xml_declaration=True)
    client = Client(args, out)
    results = []
    try:
        client.receive(
            client.send(
                "initialize",
                dict(
                    protocolVersion="2024-11-05",
                    capabilities={},
                    clientInfo=dict(name="write470-interruption", version="1"),
                ),
            )
        )
        client.send("notifications/initialized", {}, notification=True)
        opened = client.call("score_open", dict(path=str(source)))
        assert opened["ok"], opened
        for action in ("live",):
            for fmt in ("mscx", "mscz"):
                for reason in ("cancel", "deadline"):
                    destination = out / f"{action}-{fmt}.test-470-{reason}.{fmt}"
                    arguments = dict(
                        document=opened["document"],
                        kind="score",
                        options=dict(format=fmt, native_version="4.70"),
                        destination=dict(kind="file", path=str(destination)),
                    )
                    offset = (out / "stderr.log").stat().st_size
                    request = client.send("tools/call", dict(name="score_export", arguments=arguments))
                    until = time.monotonic() + 15
                    while True:
                        trace = (out / "stderr.log").read_bytes()[offset:].decode(errors="replace")
                        if "WRITE470_SERIALIZING units=2" in trace:
                            break
                        assert time.monotonic() < until, trace
                        time.sleep(0.002)
                    if reason == "cancel":
                        client.send(
                            "notifications/cancelled",
                            dict(requestId=request),
                            notification=True,
                        )
                    response = client.receive(request)
                    trace = (out / "stderr.log").read_bytes()[offset:].decode(errors="replace")
                    assert "WRITE470_RETURN units=2 error=Cancelled" in trace, trace
                    if reason == "cancel":
                        assert response.get("error", {}).get("code") == -32800, response
                    else:
                        value = response["result"]["structuredContent"]
                        assert not value["ok"] and value["error"]["code"] == "DeadlineExceeded", value
                    assert not destination.exists()
                    results.append(dict(format=fmt, reason=reason, trace=trace, response=response))
        # Recovery on the same document and process; complete, versioned bytes.
        destination = out / "recovery.mscx"
        recovery = client.call(
            "score_export",
            dict(
                document=opened["document"],
                kind="score",
                options=dict(format="mscx", native_version="4.70"),
                destination=dict(kind="file", path=str(destination)),
            ),
        )
        assert recovery["ok"], recovery
        assert ET.parse(destination).getroot().attrib["version"] == "4.70"
        (out / "results.json").write_text(
            json.dumps(
                dict(
                    binary_sha256=sha(args.binary),
                    source_sha256=sha(source),
                    records=results,
                    recovery=recovery,
                ),
                indent=2,
            )
        )
        print(f"PASS: {len(results)} serialization interruptions; same-session recovery succeeded")
    finally:
        client.close()


if __name__ == "__main__":
    main()
