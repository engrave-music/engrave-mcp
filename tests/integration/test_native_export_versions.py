"""B3 live v2 native-version/warning contract; retains actual oracle input artifacts.

This checks the pinned restricted writer, not full Phase B acceptance.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import hashlib
import json
import os
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from jsonschema import Draft202012Validator
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from support.paths import REPO_ROOT

ROOT = REPO_ROOT


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tools = {t["name"]: t for t in json.loads((ROOT / "engrave_mcp/tools.json").read_text())}
    validators = {n: Draft202012Validator(t["outputSchema"]) for n, t in tools.items()}
    params = StdioServerParameters(
        command=str(args.binary.resolve()),
        args=["--resources", str(args.resources.resolve())],
        env=dict(os.environ),
    )
    envelopes = []
    with (out / "stderr.log").open("w") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                async def call(name, arguments, failed=False):
                    response = await session.call_tool(name, arguments)
                    value = response.structured_content
                    validators[name].validate(value)
                    assert bool(response.is_error) == failed, value
                    assert value["ok"] != failed, value
                    envelopes.append({"tool": name, "arguments": arguments, "result": value})
                    return value

                async def export(doc, label, version=None, action="run", fmt="mscx", refused=False):
                    path = out / f"{label}.{fmt}"
                    options = {"format": fmt}
                    if version is not None:
                        options["native_version"] = version
                    value = await call(
                        "score_export",
                        {
                            "document": doc,
                            "kind": "score",
                            "options": options,
                            "destination": {"kind": "file", "path": str(path)},
                        },
                        refused and action == "run",
                    )
                    assert path.exists() != refused, value
                    return value, path

                source = ROOT / "vendor/engrave/engrave/tests/data/write470/minimal.mscx"
                doc = (await call("score_open", {"path": str(source)}))["document"]
                default, a = await export(doc, "default")
                explicit, b = await export(doc, "explicit", "5.00")
                assert a.read_bytes() == b.read_bytes()
                assert default["export_warnings"] == explicit["export_warnings"] == []
                # Compare the frozen 5.00 capture using only the engine's documented revision normalization.
                frozen = ROOT / "vendor/engrave/engrave/tests/data/write470/engrave19.mscx"
                frozen_doc = (await call("score_open", {"path": str(frozen.with_suffix(".mscz"))}))["document"]
                _, frozen_out = await export(frozen_doc, "frozen")
                import re

                def normalize(data):
                    return re.sub(
                        rb"<programRevision(?:\s*/>|>[^<]*</programRevision>)",
                        b"<programRevision/>",
                        data,
                    )

                assert normalize(frozen_out.read_bytes()) == normalize(frozen.read_bytes())
                notes_source = ROOT / "tests/data/native_470/notes.mscx"
                doc70 = (await call("score_open", {"path": str(notes_source)}))["document"]
                sync, xml = await export(doc70, "sync470", "4.70")
                job, archive = await export(doc70, "archive470", "4.70", "run", "mscz")
                assert sync["export_warnings"] == job["export_warnings"]
                # Frozen contract: any warning carries a B470_* identity and a {kind,id} object.
                # The minimal score needs no downgrade warning once part identity is preserved.
                assert all(w["code"].startswith("B470_") for w in sync["export_warnings"])
                assert all(set(w.get("object", {})) == {"kind", "id"} for w in sync["export_warnings"])
                assert ET.fromstring(xml.read_bytes()).attrib["version"] == "4.70"
                with zipfile.ZipFile(archive) as z:
                    score = next(n for n in z.namelist() if n.endswith(".mscx"))
                    assert ET.fromstring(z.read(score)).attrib["version"] == "4.70"
                for action in ("run",):
                    bad, path = await export(doc, "bad-" + action, "4.90", action, refused=True)
                    assert bad["error"]["code"] == "Unsupported", bad
                    expected = json.loads(
                        (ROOT / "tests/data/native_470/expected/unsupported_target.warnings.json").read_text()
                    )
                    assert bad["error"]["export_warnings"] == expected, bad
                    for malformed in (490, "", "x" * 65):
                        invalid = await call(
                            "score_export",
                            {
                                "document": doc,
                                "kind": "score",
                                "options": {
                                    "format": "mscx",
                                    "native_version": malformed,
                                },
                                "destination": {
                                    "kind": "file",
                                    "path": str(out / "malformed.mscx"),
                                },
                            },
                            failed=True,
                        )
                        assert invalid["error"]["code"] == "InvalidArgument", invalid
                        assert not (out / "malformed.mscx").exists()
                degraded_source = ROOT / "tests/data/native_470/page_locks.source.mscx"
                degraded_doc = (await call("score_open", {"path": str(degraded_source)}))["document"]
                expected = json.loads((ROOT / "tests/data/native_470/expected/page_locks.warnings.json").read_text())
                for fmt in ("mscx", "mscz"):
                    for action in ("run",):
                        value, _ = await export(
                            degraded_doc,
                            "degraded-" + fmt + "-" + action,
                            "4.70",
                            action,
                            fmt,
                        )
                        assert value["export_warnings"] == expected, value
                refused_source = ROOT / "tests/data/native_470/ties_mismatched.mscx"
                refused_doc = (await call("score_open", {"path": str(refused_source)}))["document"]
                expected = json.loads(
                    (ROOT / "tests/data/native_470/expected/ties_mismatched.warnings.json").read_text()
                )
                for fmt in ("mscx", "mscz"):
                    for action in ("run",):
                        value, _ = await export(
                            refused_doc,
                            "refused-" + fmt + "-" + action,
                            "4.70",
                            action,
                            fmt,
                            refused=True,
                        )
                        assert value["error"]["code"] == "Unsupported", value
                        assert value["error"]["export_warnings"] == expected, value
                # Schema controls: engine-owned strings/arrays have no invented request bounds.
                large = dict(sync)
                large["export_warnings"] = [{"code": "x" * 5000, "message": "m" * 70000}] * 4100
                validators["score_export"].validate(large)
                # Check every schema embedding the shared warning definition,
                # including errors and runtime jobs. B470 objects need both fields.
                for tool in tools.values():
                    warning = tool["outputSchema"]["$defs"].get("export_warning")
                    if warning is None:
                        continue
                    validator = Draft202012Validator(warning)
                    validator.validate({"code": "OTHER", "message": "no object required"})
                    validator.validate(expected[0])
                    for kind, identity in (
                        (
                            "tie",
                            "locator:score:0/measure:0/tick:0/track:0/note:0/tieFor",
                        ),
                        ("page_lock", "A_A..B_B"),
                        ("group_bracket", "B_B:1"),
                    ):
                        validator.validate(
                            {
                                "code": "B470_UNSUPPORTED_FEATURE",
                                "message": "test",
                                "object": {"kind": kind, "id": identity},
                            }
                        )
                    for obj in (
                        None,
                        {},
                        {"kind": "tie"},
                        {"id": "H_H"},
                        {"kind": "", "id": "H_H"},
                        {"kind": "tie", "id": ""},
                        {"kind": "", "id": ""},
                    ):
                        invalid = {"code": "B470_INVALID_TIE", "message": "test"}
                        if obj is not None:
                            invalid["object"] = obj
                        assert not validator.is_valid(invalid), (tool["name"], invalid)
                for token in (doc, doc70, frozen_doc, degraded_doc, refused_doc):
                    await call("score_close", {"document": token})
    (out / "envelopes.json").write_text(json.dumps(envelopes, indent=2), encoding="utf-8")
    (out / "provenance.json").write_text(
        json.dumps(
            {
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "target_version": "4.70",
                "scope": "restricted writer; independent oracle pending",
                "artifacts": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (xml, archive)},
            },
            indent=2,
        )
    )
    print(json.dumps({"status": "passed", "envelopes": len(envelopes), "output": str(out)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
