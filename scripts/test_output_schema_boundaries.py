"""Exercise imported and edited score outputs against the emitted v2 schemas."""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from v2_client import TOOLS, VALIDATORS, Client

ROOT = Path(__file__).resolve().parents[1]


async def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "fixtures/version/header.mscx").read_text(encoding="utf-8")
    title, part_name, lyric = "T" * 5000, "P" * 5000, "L" * 66000
    source = source.replace("Version header fixture", title)
    source = source.replace("<longName>Piano</longName>", f"<longName>{part_name}</longName>")
    source = source.replace(
        "<durationType>whole</durationType>",
        f"<durationType>whole</durationType><Lyrics><eid>H_H</eid><text>{lyric}</text></Lyrics>",
        1,
    )
    path = args.output / "large-text.mscx"
    path.write_text(source, encoding="utf-8")
    async with Client(args, args.output) as client:
        listed = await client.call("tools/list", {})
        live = {tool["name"]: tool for tool in listed["result"]["tools"]}
        for name, definition in TOOLS.items():
            assert live[name]["outputSchema"] == definition["outputSchema"], name
        doc = (await client.tool("score_open", {"path": str(path.resolve())}))["document"]
        outline = await client.tool("score_outline", {"document": doc})
        assert outline["outline"]["title"] == title
        assert outline["outline"]["parts"][0]["name"] == part_name
        elements = await client.tool(
            "score_query",
            {"document": doc, "mode": "elements", "query": {"types": ["Lyrics"]}},
        )
        assert len(elements["elements"]) == 1, elements
        properties = await client.tool(
            "score_query",
            {
                "document": doc,
                "mode": "properties",
                "target": {"kind": "id", "id": elements["elements"][0]["id"]},
            },
        )
        strings = [
            p["value"]["value"]
            for p in properties["properties"]
            if isinstance(p["value"], dict) and p["value"].get("kind") == "string"
        ]
        assert any(lyric in value for value in strings), [
            (p["name"], p["value"].get("kind")) for p in properties["properties"] if isinstance(p["value"], dict)
        ]
        await client.tool("score_close", {"document": doc})

        doc = (await client.tool("score_create", {"template_id": "Treble_Clef"}))["document"]
        edits = [{"op": "add_part", "instrument_id": "flute"} for _ in range(260)]
        edited = await client.tool("score_edit", {"document": doc, "ops": edits})
        tracks = await client.tool("score_query", {"document": doc, "mode": "audio_tracks"})
        assert len(tracks["audio_tracks"]["tracks"]) == 261
        await client.tool("score_close", {"document": doc})

    # These DTO-shaped mutations independently exercise the remaining removed
    # collection bounds. They test the declared schema, not live engine output.
    synthetic = json.loads(json.dumps(outline))
    synthetic["outline"]["measures"] *= 100001
    synthetic["outline"]["parts"] *= 4097
    synthetic["outline"]["signatures"] *= 4097
    VALIDATORS["score_outline"].validate(synthetic)
    synthetic_edit = json.loads(json.dumps(edited))
    synthetic_edit["created"] = ["A_A"] * 65537
    synthetic_edit["diagnostics"] = [{"severity": "warning", "message": "test"}] * 4097
    synthetic_edit["diagnostics"][0] = {"severity": "warning", "message": "D" * 65537}
    VALIDATORS["score_edit"].validate(synthetic_edit)
    synthetic_properties = json.loads(json.dumps(properties))
    synthetic_properties["properties"] = synthetic_properties["properties"][:1] * 4097
    VALIDATORS["score_query"].validate(synthetic_properties)
    print(
        json.dumps(
            {
                "title_characters": len(title),
                "part_name_characters": len(part_name),
                "lyric_characters": len(lyric),
                "audio_tracks": 261,
                "synthetic_schema_measures": len(synthetic["outline"]["measures"]),
                "synthetic_created_ids": len(synthetic_edit["created"]),
            }
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
