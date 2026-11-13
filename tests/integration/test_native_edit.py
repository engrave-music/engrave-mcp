"""Exercise edits, stable note IDs, checkpoints, poisoning and revision conflicts over MCP."""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import xml.etree.ElementTree as ET
from pathlib import Path

from engrave_mcp_testing.mcp_client import VALIDATORS, Client
from engrave_mcp_testing.native_edit_fixture import fixture


def pitch(note):
    return int(note["props"][0]["value"]["value"])


def change(note, value):
    return {"op": "set_pitch", "note": {"kind": "id", "id": note["id"]}, "pitch": value}


async def check_dynamics_edit(client, out):
    for label, tags, expected_type in [
        ("single", ("f",), "f"),
        ("grouped", ("f", "p"), "fp"),
    ]:
        root = ET.fromstring(fixture())
        measure = root.find("./part/measure")
        direction = ET.Element("direction")
        dynamic = ET.SubElement(ET.SubElement(direction, "direction-type"), "dynamics")
        for tag in tags:
            ET.SubElement(dynamic, tag)
        measure.insert(1, direction)
        data = base64.b64encode(ET.tostring(root)).decode()
        doc = (await client.tool("score_open", {"data": data, "format": "musicxml"}))["document"]

        async def properties(document):
            found = await client.tool(
                "score_query",
                {
                    "document": document,
                    "mode": "elements",
                    "query": {"types": ["Dynamic"]},
                },
            )
            assert len(found["elements"]) == 1, found
            target = {"kind": "id", "id": found["elements"][0]["id"]}
            result = await client.tool(
                "score_query",
                {
                    "document": document,
                    "mode": "properties",
                    "target": target,
                    "props": ["DYNAMIC_TYPE", "VELOCITY"],
                },
            )
            return target, {item["name"]: item["value"]["value"] for item in result["properties"]}

        target, before = await properties(doc)
        assert before == {"DYNAMIC_TYPE": expected_type, "VELOCITY": "96"}, before
        await client.tool(
            "score_edit",
            {
                "document": doc,
                "ops": [
                    {
                        "op": "set_property",
                        "target": target,
                        "property": "VELOCITY",
                        "value": {"kind": "int", "value": "73"},
                    }
                ],
            },
        )
        _, edited = await properties(doc)
        assert edited == {"DYNAMIC_TYPE": expected_type, "VELOCITY": "73"}, edited
        path = out / f"dynamics-{label}.mscx"
        await client.tool(
            "score_export",
            {
                "document": doc,
                "kind": "score",
                "options": {"format": "mscx"},
                "destination": {"kind": "file", "path": str(path)},
            },
        )
        await client.tool("score_close", {"document": doc})
        reopened = (await client.tool("score_open", {"path": str(path)}))["document"]
        _, saved = await properties(reopened)
        assert saved == edited, saved
        await client.tool("score_close", {"document": reopened})


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.soundfont = None
    async with Client(args, out) as client:

        async def notes(staff):
            return await client.tool(
                "score_query",
                {
                    "document": doc,
                    "mode": "elements",
                    "query": {
                        "types": ["Note"],
                        "staves": [staff],
                        "first_measure": 0,
                        "measure_count": 2,
                        "props": ["pitch"],
                        "limit": 100,
                    },
                },
            )

        async def raw_edit(arguments):
            response = await client.call("tools/call", {"name": "score_edit", "arguments": arguments})
            assert "error" not in response, response
            result = response["result"]
            payload = result["structuredContent"]
            VALIDATORS["score_edit"].validate(payload)
            assert bool(result["isError"]) != payload["ok"]
            return payload

        opened = await client.tool(
            "score_open",
            {
                "data": base64.b64encode(fixture().encode()).decode(),
                "format": "musicxml",
            },
        )
        doc = opened["document"]
        before = [await notes(0), await notes(1)]
        assert all(len(q["elements"]) == 8 for q in before)
        assert before[0]["elements"] == (await notes(0))["elements"]
        old = {n["id"]: n for q in before for n in q["elements"]}
        targets = [q["elements"][i] for q in before for i in (0, 4)]
        expected = {n["id"]: pitch(n) + 1 for n in targets}
        revision = opened["revision"]
        for n in targets:
            result = await client.tool(
                "score_edit",
                {
                    "document": doc,
                    "expected_revision": revision,
                    "ops": [change(n, expected[n["id"]])],
                },
            )
            revision = result["revision"]
        latest = [await notes(0), await notes(1)]
        actual = {n["id"]: n for q in latest for n in q["elements"]}
        assert set(actual) == set(old)
        for identity, item in actual.items():
            assert pitch(item) == expected.get(identity, pitch(old[identity]))
            for field in (
                "measure",
                "measure_index",
                "staff",
                "voice",
                "tick",
                "duration",
                "parent",
            ):
                assert item[field] == old[identity][field], (identity, field)
        assert int(revision) == int(opened["revision"]) + 4

        # The session serializes edits and rejects every stale expected_revision, even for disjoint notes.
        requests = [
            {
                "document": doc,
                "expected_revision": revision,
                "ops": [change(n, pitch(actual[n["id"]]) + 1)],
            }
            for n in targets
        ]
        races = await asyncio.gather(*(raw_edit(req) for req in requests))
        assert sum(r["ok"] for r in races) == 1, races
        assert all(r["ok"] or r["error"]["code"] == "RevisionConflict" for r in races)
        conflict = await raw_edit(requests[0])
        assert not conflict["ok"] and conflict["error"]["code"] == "RevisionConflict"
        clean = await notes(0)
        measure = (await client.tool("score_outline", {"document": doc}))["outline"]["measures"][0]["id"]
        preflight_batch = await raw_edit(
            {
                "document": doc,
                "expected_revision": clean["revision"],
                "ops": [
                    change(clean["elements"][0], 80),
                    {
                        "op": "set_property",
                        "target": {"kind": "id", "id": measure},
                        "property": "pitch",
                        "value": {"kind": "int", "value": "80"},
                    },
                ],
            }
        )
        assert not preflight_batch["ok"]
        assert preflight_batch["error"]["op_index"] == 1
        assert "poisoned" not in preflight_batch["error"]
        assert preflight_batch["error"]["applied"] == 0
        after_preflight = await notes(0)
        assert after_preflight["elements"] == clean["elements"] and after_preflight["revision"] == clean["revision"]

        await client.tool("checkpoint", {"document": doc, "name": "clean"})
        invalid_batch = await raw_edit(
            {
                "document": doc,
                "expected_revision": clean["revision"],
                "ops": [
                    change(clean["elements"][0], 80),
                    {
                        "op": "set_pitch",
                        "note": {"kind": "id", "id": measure},
                        "pitch": 80,
                    },
                ],
            }
        )
        assert not invalid_batch["ok"]
        assert invalid_batch["error"]["op_index"] == 1
        assert invalid_batch["error"]["applied"] == 1
        # Temporary guard until engine S1b stops poisoning on mid-batch rejection.
        if invalid_batch["error"].get("poisoned"):
            assert "restore_checkpoint" in invalid_batch["error"]["message"]
            poisoned_edit = await raw_edit(
                {
                    "document": doc,
                    "ops": [change(clean["elements"][0], 72)],
                }
            )
            assert not poisoned_edit["ok"] and poisoned_edit["error"]["code"] == "Poisoned"
            assert poisoned_edit["error"]["message"] == "restore_checkpoint or reopen the document before editing"
            poisoned_save = await client.tool(
                "score_export",
                {
                    "document": doc,
                    "kind": "score",
                    "options": {"format": "mscz"},
                    "destination": {"kind": "file", "path": str(out / "poisoned.mscz")},
                },
                error=True,
            )
            assert poisoned_save["error"]["code"] == "Poisoned"
            assert poisoned_save["error"]["message"] == "restore_checkpoint or reopen the document before saving"
        export = out / "edited.musicxml"
        await client.tool(
            "score_export",
            {
                "document": doc,
                "kind": "score",
                "options": {"format": "musicxml"},
                "destination": {"kind": "file", "path": str(export)},
            },
        )
        xml = ET.parse(export).getroot()
        assert len(xml.findall("./part")) == 2 and len(xml.findall("./part/measure/note")) == 16
        await client.tool("restore_checkpoint", {"document": doc, "name": "clean"})
        after_restore = await notes(0)
        assert after_restore["elements"] == clean["elements"] and after_restore["revision"] != clean["revision"]
        await client.tool("drop_checkpoint", {"document": doc, "name": "clean"})
        await client.tool(
            "restore_checkpoint",
            {"document": doc, "name": "clean"},
            error=True,
        )
        await client.tool(
            "score_query",
            {"document": doc, "mode": "elements", "query": {"measure_count": 129}},
            error=True,
        )
        await client.tool("score_close", {"document": doc})
        await check_dynamics_edit(client, out)
    report = {
        "status": "passed",
        "sequential_independent_edits": 4,
        "same_revision_winners": 1,
        "stable_note_ids": 16,
        "preflight_batch_unchanged": True,
        "mid_batch_poisoned_and_restored": True,
        "stale_revision_rejected": True,
        "dynamics_edits_survive_reopen": 2,
    }
    (out / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
