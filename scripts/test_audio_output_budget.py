"""Exercise live audio encoder and staging quotas using the test host."""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile

from v2_client import Client, ROOT


async def open_source(args):
    async with Client(args, args.output) as client:
        doc = (await client.tool("score_open", {"path": str(ROOT / "fixtures/evening.musicxml")}))["document"]
        duration = (await client.tool("score_outline", {"document": doc}))["outline"]["duration"]
        path = (args.output / "source.mscz").resolve()
        await client.tool("score_export", {"document": doc, "kind": "score",
            "options": {"format": "mscz"}, "destination": {"kind": "file", "path": str(path)}})
        await client.tool("score_close", {"document": doc})
    return path, duration


def live_budget(args, source, audio_options, limit, expected):
    async def execute():
        with tempfile.TemporaryDirectory(prefix="live-budget-", dir=args.output) as temporary:
            out = Path(temporary).resolve()
            configured = dict(vars(args), output=out)
            from types import SimpleNamespace
            previous = os.environ.get("ENGRAVE_MCP_TEST_OUTPUT_BYTES")
            os.environ["ENGRAVE_MCP_TEST_OUTPUT_BYTES"] = str(limit)
            try:
                async with Client(SimpleNamespace(**configured), out) as client:
                    doc = (await client.tool("score_open", {"path":str(source)}))["document"]
                    path = out / "output"
                    result = await client.tool("score_export", {"document":doc,"kind":"audio",
                        "options":audio_options,"destination":{"kind":"file","path":str(path)}},
                        error=expected != "success")
                    if expected == "success":
                        produced = path.stat().st_size
                        assert 0 < produced <= limit // 2
                        digest = hashlib.sha256(path.read_bytes()).hexdigest()
                    else:
                        assert result["error"]["code"] == expected, result
                        assert not path.exists() and not list(out.glob(".engrave-stage-*"))
                        produced, digest = 0, None
                    return {"limit":limit,"result":"success" if result["ok"] else result["error"]["code"],
                        "output_bytes":produced,"sha256":digest,
                        "message":"" if result["ok"] else result["error"]["message"]}
            finally:
                if previous is None: os.environ.pop("ENGRAVE_MCP_TEST_OUTPUT_BYTES", None)
                else: os.environ["ENGRAVE_MCP_TEST_OUTPUT_BYTES"] = previous
    return asyncio.run(execute())


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
    wav_budget = 2 * ((wav16 + wav24) // 2)
    mp3_128 = math.ceil(seconds * 128000 / 8 + 1024 * 1024)
    mp3_512 = math.ceil(seconds * 512000 / 8 + 1024 * 1024)
    mp3_budget = 2 * ((mp3_128 + mp3_512) // 2)
    evidence = {"duration_seconds_rounded": duration,
        "tiny_wav": live_budget(args, source, {"format": "wav"}, 64, "ResourceLimit"),
        "wav16": live_budget(args, source, {"format": "wav", "bit_depth": 16}, wav_budget, "success"),
        "wav16_large_budget": live_budget(args, source, {"format": "wav", "bit_depth": 16}, 2 * wav_budget, "success"),
        "wav24": live_budget(args, source, {"format": "wav", "bit_depth": 24}, wav_budget, "ResourceLimit"),
        "mp3_128": live_budget(args, source, {"format": "mp3", "bitrate_kbps": 128}, mp3_budget, "success"),
        "mp3_512": live_budget(args, source, {"format": "mp3", "bitrate_kbps": 512}, mp3_budget, "success")}
    # Nominal bitrate is not an encoded-size bound. Test the encoder's actual
    # file extent, including header rewrites, rather than a duration estimate.
    evidence["mp3_512_one_byte_short"] = live_budget(args, source,
        {"format": "mp3", "bitrate_kbps": 512},
        2 * evidence["mp3_512"]["output_bytes"] - 1, "ResourceLimit")
    assert evidence["wav16"]["sha256"] == evidence["wav16_large_budget"]["sha256"]
    (args.output / "audio-budget-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
