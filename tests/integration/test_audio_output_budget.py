"""Exercise live audio encoder and staging quotas using the test host."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
from pathlib import Path

from engrave_mcp_testing.audio_budget import live_budget
from engrave_mcp_testing.mcp_client import Client
from engrave_mcp_testing.paths import REPO_ROOT


async def open_source(args):
    async with Client(args, args.output) as client:
        doc = (
            await client.tool(
                "score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/evening.musicxml")}
            )
        )["document"]
        duration = (await client.tool("score_outline", {"document": doc}))["outline"]["duration"]
        path = (args.output / "source.mscz").resolve()
        await client.tool(
            "score_export",
            {"document": doc, "format": "mscz", "path": str(path)},
        )
        await client.tool("score_close", {"document": doc})
    return path, duration


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source, duration = asyncio.run(open_source(args))
    seconds = duration + 3.5  # nearest-second outline plus half-second and 3-second tail
    frames = math.ceil(seconds * 44100)
    wav16, wav24 = frames * 4 + 4096, frames * 6 + 4096
    # Audio renders straight into staging, so the whole output budget bounds
    # the one file: 16-bit fits between the two estimates and 24-bit does not.
    wav_budget = (wav16 + wav24) // 2
    mp3_128 = math.ceil(seconds * 128000 / 8 + 1024 * 1024)
    mp3_512 = math.ceil(seconds * 512000 / 8 + 1024 * 1024)
    mp3_budget = (mp3_128 + mp3_512) // 2
    evidence = {
        "duration_seconds_rounded": duration,
        "tiny_wav": live_budget(args, source, {"format": "wav"}, 64, "ResourceLimit"),
        "wav16": live_budget(args, source, {"format": "wav", "bit_depth": 16}, wav_budget, "success"),
        "wav16_large_budget": live_budget(args, source, {"format": "wav", "bit_depth": 16}, 2 * wav_budget, "success"),
        "wav24": live_budget(
            args,
            source,
            {"format": "wav", "bit_depth": 24},
            wav_budget,
            "ResourceLimit",
        ),
        "mp3_128": live_budget(args, source, {"format": "mp3", "bitrate_kbps": 128}, mp3_budget, "success"),
        "mp3_512": live_budget(args, source, {"format": "mp3", "bitrate_kbps": 512}, mp3_budget, "success"),
    }
    # Nominal bitrate is not an encoded-size bound. Test the encoder's actual
    # file extent, including header rewrites, rather than a duration estimate.
    evidence["mp3_512_one_byte_short"] = live_budget(
        args,
        source,
        {"format": "mp3", "bitrate_kbps": 512},
        evidence["mp3_512"]["output_bytes"] - 1,
        "ResourceLimit",
    )
    # A budget equal to the encoded size is 100% of it, twice the retired
    # half-budget cap; one byte less must fail without leaving staging behind.
    wav16_size = evidence["wav16"]["output_bytes"]
    evidence["wav16_exact_budget"] = live_budget(
        args, source, {"format": "wav", "bit_depth": 16}, wav16_size, "success"
    )
    evidence["wav16_one_byte_short"] = live_budget(
        args, source, {"format": "wav", "bit_depth": 16}, wav16_size - 1, "ResourceLimit"
    )
    assert wav16_size > wav_budget // 2, (wav16_size, wav_budget)
    assert evidence["wav16"]["sha256"] == evidence["wav16_large_budget"]["sha256"]
    assert evidence["wav16"]["sha256"] == evidence["wav16_exact_budget"]["sha256"]
    (args.output / "audio-budget-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
