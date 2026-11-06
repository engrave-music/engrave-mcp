"""Exercise v2 audio-track queries, mix overrides and live exports."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import base64
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
import soundfile as sf
from integration.test_native_edit import fixture
from support.mcp_client import Client
from support.paths import REPO_ROOT

ROOT = REPO_ROOT
TRACK_BALANCE_LEAKAGE_LIMIT = 0.30


def analyze(path):
    samples, rate = sf.read(path, always_2d=True)
    assert rate == 44100 and samples.shape[1] == 2 and len(samples) > 0
    assert np.isfinite(samples).all()
    return {
        "rms": float(np.sqrt(np.mean(samples**2))),
        "peak": float(np.max(np.abs(samples))),
        "left": float(np.sqrt(np.mean(samples[:, 0] ** 2))),
        "right": float(np.sqrt(np.mean(samples[:, 1] ** 2))),
    }


def close_ratio(left, right, tolerance=0.01):
    assert abs(left - right) / max(right, 1e-9) <= tolerance, (left, right)


def header(fmt, data):
    if fmt == "flac":
        return data.startswith(b"fLaC")
    if fmt == "opus":
        return data.startswith(b"OggS")
    if fmt == "aac":
        return len(data) > 4 and data[0] == 0xFF and data[1] & 0xF0 == 0xF0
    if fmt == "mp3":
        return data.startswith(b"ID3") or any(
            data[i] == 0xFF and data[i + 1] & 0xE0 == 0xE0 for i in range(min(1024, len(data) - 1))
        )
    return False


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    evidence = {"checks": []}
    async with Client(args, out) as client:

        async def open_xml(xml):
            return (
                await client.tool(
                    "score_open",
                    {
                        "data": base64.b64encode(xml.encode()).decode(),
                        "format": "musicxml",
                    },
                )
            )["document"]

        async def outline(doc):
            return await client.tool("score_outline", {"document": doc})

        async def tracks(doc, score=None):
            values = {"document": doc, "mode": "audio_tracks"}
            if score is not None:
                values["score"] = {"excerpt": score}
            return (await client.tool("score_query", values))["audio_tracks"]

        async def export(doc, filename, options=None, score=None):
            path = out / filename
            fmt = path.suffix.lstrip(".")
            if fmt == "ogg":
                fmt = "opus"
            values = {"format": fmt, **(options or {})}
            if score is not None:
                values["score"] = {"excerpt": score}
            await client.tool(
                "score_export",
                {
                    "document": doc,
                    "kind": "audio",
                    "options": values,
                    "destination": {"kind": "file", "path": str(path)},
                },
            )
            assert path.is_file() and path.stat().st_size > 0
            return path

        xml = fixture()
        doc = await open_xml(xml)
        initial = await outline(doc)
        defaults = await tracks(doc)
        entries = defaults["tracks"]
        assert len(entries) == 2 and len({t["track"] for t in entries}) == 2
        assert [p["name"] for p in initial["outline"]["parts"]] == [t["part_name"] for t in entries]
        assert defaults["master"] == {
            "volume_db": 0.0,
            "balance": 0.0,
            "mute": False,
            "solo": False,
        }
        for track in entries:
            assert track["part_name"] and track["part_id"] and track["instrument_id"]
            assert track["mix"] == {
                "volume_db": 0.0,
                "balance": 0.0,
                "mute": False,
                "solo": False,
            }
        ids = [t["track"] for t in entries]
        evidence["checks"].append("track_defaults")

        if args.soundfont:
            reference = analyze(await export(doc, "reference.wav", {"master_volume_db": 0.0}))
            assert reference["rms"] > 0 and reference["peak"] > 0
            all_mute = analyze(
                await export(
                    doc,
                    "all-mute.wav",
                    {"tracks": [{"track": i, "mute": True} for i in ids]},
                )
            )
            assert all_mute["rms"] < 1e-4 and all_mute["peak"] <= 1e-4
            muted = [analyze(await export(doc, f"mute-{i}.wav", {"tracks": [{"track": i, "mute": True}]})) for i in ids]
            assert all(0 < item["rms"] < reference["rms"] for item in muted)
            solo = [analyze(await export(doc, f"solo-{i}.wav", {"tracks": [{"track": i, "solo": True}]})) for i in ids]
            close_ratio(solo[0]["rms"], muted[1]["rms"])
            close_ratio(solo[1]["rms"], muted[0]["rms"])
            solo_muted = analyze(
                await export(
                    doc,
                    "solo-mute.wav",
                    {"tracks": [{"track": ids[0], "solo": True, "mute": True}]},
                )
            )
            assert solo_muted["rms"] < 1e-4 and solo_muted["peak"] <= 1e-4
            both_solo = analyze(
                await export(
                    doc,
                    "both-solo.wav",
                    {"tracks": [{"track": i, "solo": True} for i in ids]},
                )
            )
            close_ratio(both_solo["rms"], reference["rms"])

            root = ET.fromstring(xml)
            root.find("part-list").remove(root.findall("part-list/score-part")[1])
            root.remove(root.findall("part")[1])
            single = await open_xml(ET.tostring(root, encoding="unicode"))
            single_id = (await tracks(single))["tracks"][0]["track"]
            base = analyze(
                await export(
                    single,
                    "gain-0.wav",
                    {"tracks": [{"track": single_id, "volume_db": 0.0}]},
                )
            )
            lower = analyze(
                await export(
                    single,
                    "gain-6.wav",
                    {"tracks": [{"track": single_id, "volume_db": -6.0}]},
                )
            )
            assert abs(20 * math.log10(lower["rms"] / base["rms"]) + 6) <= 0.2
            await client.tool("score_close", {"document": single})

            left = analyze(await export(doc, "master-left.wav", {"master_balance": -1.0}))
            right = analyze(await export(doc, "master-right.wav", {"master_balance": 1.0}))
            assert left["left"] > 0 and left["right"] <= 0.02 * left["left"]
            assert right["right"] > 0 and right["left"] <= 0.02 * right["right"]
            left = analyze(
                await export(
                    doc,
                    "track-left.wav",
                    {
                        "tracks": [
                            {"track": ids[0], "balance": -1.0},
                            {"track": ids[1], "mute": True},
                        ]
                    },
                )
            )
            right = analyze(
                await export(
                    doc,
                    "track-right.wav",
                    {
                        "tracks": [
                            {"track": ids[0], "balance": 1.0},
                            {"track": ids[1], "mute": True},
                        ]
                    },
                )
            )
            assert left["left"] > 0 and left["right"] <= TRACK_BALANCE_LEAKAGE_LIMIT * left["left"]
            assert right["right"] > 0 and right["left"] <= TRACK_BALANCE_LEAKAGE_LIMIT * right["right"]
            master = analyze(await export(doc, "master-12.wav", {"master_volume_db": -12.0}))
            assert abs(20 * math.log10(master["rms"] / reference["rms"]) + 12) <= 0.2
            for fmt in ("flac", "opus", "mp3", "aac"):
                path = await export(doc, f"compressed.{fmt}", {"master_volume_db": -3.0})
                assert header(fmt, path.read_bytes()), fmt
            evidence["checks"].extend(
                [
                    "mute",
                    "solo",
                    "track_gain",
                    "master_gain",
                    "master_balance",
                    "track_balance",
                    "compressed_formats",
                ]
            )

        parts = initial["outline"]["parts"]
        edited = await client.tool(
            "score_edit",
            {
                "document": doc,
                "ops": [
                    {
                        "op": "generate_excerpts",
                        "parts": [{"kind": "id", "id": part["id"]} for part in parts],
                    }
                ],
            },
        )
        excerpts = (await outline(doc))["outline"]["excerpts"]
        assert len(excerpts) >= 2
        excerpt = excerpts[0]["id"]
        assert (await tracks(doc, excerpt))["tracks"]
        if args.soundfont:
            assert analyze(await export(doc, "excerpt.wav", {"master_volume_db": -2.0}, score=excerpt))["rms"] > 0
        revision = edited["revision"]

        invalid_options = [
            {"tracks": [{"track": 999999, "mute": True}]},
            {"tracks": [{"track": ids[0]}, {"track": ids[0]}]},
            {"master_volume_db": 25.0},
            {"master_volume_db": -121.0},
            {"master_balance": 1.5},
            {"master_balance": -1.5},
            {"tracks": [{"track": ids[0], "forceMute": True}]},
            {"unexpected": True},
            {"tracks": [{"track": ids[0], "unexpected": True}]},
        ]
        for action in ("run",):
            for i, bad in enumerate(invalid_options):
                path = out / f"invalid-{action}-{i}.wav"
                path.write_bytes(b"unchanged")
                await client.tool(
                    "score_export",
                    {
                        "document": doc,
                        "kind": "audio",
                        "options": {"format": "wav", **bad},
                        "destination": {"kind": "file", "path": str(path)},
                    },
                    error=True,
                )
                assert path.read_bytes() == b"unchanged"
        for fmt, kind in (
            ("svg", "render"),
            ("png", "render"),
            ("pdf", "render"),
            ("midi", "score"),
            ("musicxml", "score"),
        ):
            for action in ("run",):
                path = out / f"invalid-{fmt}-{action}"
                directory = fmt in ("svg", "png")
                if directory:
                    path.mkdir(exist_ok=True)
                else:
                    path.write_bytes(b"unchanged")
                await client.tool(
                    "score_export",
                    {
                        "document": doc,
                        "kind": kind,
                        "options": {"format": fmt, "master_volume_db": 0},
                        "destination": {
                            "kind": "directory" if directory else "file",
                            "path": str(path),
                        },
                    },
                    error=True,
                )
                if directory:
                    assert not list(path.iterdir())
                else:
                    assert path.read_bytes() == b"unchanged"
        for name in (
            "score_audio_get_mix",
            "score_audio_set_mix",
            "score_audio_start",
            "score_audio_read",
            "score_audio_seek",
        ):
            response = await client.call("tools/call", {"name": name, "arguments": {}})
            assert response.get("error", {}).get("code") == -32602, response
        assert (await outline(doc))["revision"] == revision
        evidence["checks"].extend(
            [
                "excerpt_mix",
                "invalid_mix_and_sentinels",
                "removed_tools",
                "revision_unchanged",
            ]
        )
        await client.tool("score_close", {"document": doc})

        mix_doc = (await client.tool("score_open", {"path": str(ROOT / "tests/data/inputs/audio/mix.mscz")}))[
            "document"
        ]
        mix_tracks = (await tracks(mix_doc))["tracks"]
        assert len(mix_tracks) == 3
        cello = next(t for t in mix_tracks if t["part_name"] == "Cello")
        violin = next(t for t in mix_tracks if t["part_name"] == "Violin")
        assert cello["mix"]["mute"] and not violin["mix"]["mute"]
        if args.soundfont:
            default = analyze(await export(mix_doc, "saved-default.wav"))
            unmuted = analyze(
                await export(
                    mix_doc,
                    "saved-unmuted.wav",
                    {"tracks": [{"track": cello["track"], "mute": False}]},
                )
            )
            muted = analyze(
                await export(
                    mix_doc,
                    "saved-muted.wav",
                    {"tracks": [{"track": cello["track"], "mute": True}]},
                )
            )
            assert unmuted["rms"] > default["rms"] + 0.001
            close_ratio(muted["rms"], default["rms"])
        await client.tool("score_close", {"document": mix_doc})

        parts_doc = (await client.tool("score_open", {"path": str(ROOT / "tests/data/inputs/audio/mix_parts.mscz")}))[
            "document"
        ]
        saved_excerpts = {e["name"]: e["id"] for e in (await outline(parts_doc))["outline"]["excerpts"]}
        for name, expected_mute in (("Violin", False), ("Cello", True)):
            selected = (await tracks(parts_doc, saved_excerpts[name]))["tracks"]
            part = next(t for t in selected if t["part_name"] == name)
            assert part["mix"]["mute"] == expected_mute
            if args.soundfont:
                audio = analyze(await export(parts_doc, f"saved-{name}.wav", score=saved_excerpts[name]))
                if expected_mute:
                    assert audio["rms"] < 1e-4 and audio["peak"] <= 1e-4
                else:
                    assert audio["rms"] > 0 and audio["peak"] > 0
        await client.tool("score_close", {"document": parts_doc})
        evidence["checks"].extend(["saved_master_mix", "saved_excerpt_mix"])

        source = ROOT / "vendor/engrave/src/engraving/tests/instrumentchange_data/change-ref.mscx"
        changed_doc = (await client.tool("score_open", {"path": str(source)}))["document"]
        changed_tracks = (await tracks(changed_doc))["tracks"]
        assert len(changed_tracks) == 3 and len({t["track"] for t in changed_tracks}) == 3
        assert len([t for t in changed_tracks if t["part_id"] == "1"]) == 2
        if args.soundfont:
            sync = await export(changed_doc, "change-sync.wav")
            asynchronous = await export(changed_doc, "change-async.wav")
            assert sync.read_bytes() == asynchronous.read_bytes()
            mixed = await export(
                changed_doc,
                "change-mix-sync.wav",
                {"tracks": [{"track": changed_tracks[0]["track"], "mute": True}]},
            )
            mixed_async = await export(
                changed_doc,
                "change-mix-async.wav",
                {"tracks": [{"track": changed_tracks[0]["track"], "mute": True}]},
            )
            assert mixed.read_bytes() == mixed_async.read_bytes()
            assert analyze(mixed)["rms"] < analyze(sync)["rms"]
        await client.tool("score_close", {"document": changed_doc})
        evidence["checks"].append("multi_instrument_sync_async")

    evidence["status"] = "passed"
    (out / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    main()
