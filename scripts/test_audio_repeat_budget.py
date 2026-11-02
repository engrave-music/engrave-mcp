"""Check repeat-aware audio projection against live and encoded WAV exports."""
from __future__ import annotations

import argparse
import asyncio
import json
import math
from pathlib import Path

from test_audio_output_budget import live_budget
from test_export_options import REPEAT_FIXTURE_MUSICXML
from v2_client import Client


async def capture(args, repeats):
    xml = REPEAT_FIXTURE_MUSICXML.replace('direction="backward"', f'direction="backward" times="{repeats}"')
    source = (args.output / f"repeat-{repeats}.musicxml").resolve()
    source.write_text(xml, encoding="utf-8")
    snapshot = (args.output / f"repeat-{repeats}.mscz").resolve()
    async with Client(args, args.output) as client:
        doc = (await client.tool("score_open", {"path": str(source)}))["document"]
        duration = (await client.tool("score_outline", {"document": doc}))["outline"]["duration"]
        await client.tool("score_export", {"document": doc, "kind": "score",
            "options": {"format": "mscz"}, "destination": {"kind": "file", "path": str(snapshot)}})
        if repeats == 1000:
            no_repeat = (args.output / "no-repeat-live.wav").resolve()
            await client.tool("score_export", {"document": doc, "kind": "audio",
                "options": {"format": "wav", "use_repeats": False},
                "destination": {"kind": "file", "path": str(no_repeat)}})
            assert 0 < no_repeat.stat().st_size < 1024**3
            repeated = (args.output / "repeated-live.wav").resolve()
            failed = await client.tool("score_export", {"document": doc, "kind": "audio",
                "options": {"format": "wav", "use_repeats": True},
                "destination": {"kind": "file", "path": str(repeated)}}, error=True)
            assert failed["error"]["code"] == "ResourceLimit" and not repeated.exists(), failed
        await client.tool("score_close", {"document": doc})
    return snapshot, duration


async def capture_mid_measure_end(args):
    # The extra 2/4 bar puts the unexpanded end inside the next expanded 4/4
    # bar. The public measure-position API cannot provide that tick's time.
    xml = REPEAT_FIXTURE_MUSICXML.replace('direction="backward"', 'direction="backward" times="1000"')
    extra = '''    <measure number="3">
      <attributes><time><beats>2</beats><beat-type>4</beat-type></time></attributes>
      <note><rest/><duration>2</duration><voice>1</voice><type>half</type></note>
    </measure>
'''
    xml = xml.replace('  </part>\n</score-partwise>', extra + '  </part>\n</score-partwise>')
    source = (args.output / "mid-measure.musicxml").resolve()
    snapshot = (args.output / "mid-measure.mscz").resolve()
    output = (args.output / "mid-measure-live.wav").resolve()
    source.write_text(xml, encoding="utf-8")
    async with Client(args, args.output) as client:
        doc = (await client.tool("score_open", {"path": str(source)}))["document"]
        await client.tool("score_export", {"document": doc, "kind": "score",
            "options": {"format": "mscz"}, "destination": {"kind": "file", "path": str(snapshot)}})
        await client.tool("score_export", {"document": doc, "kind": "audio",
            "options": {"format": "wav", "use_repeats": False},
            "destination": {"kind": "file", "path": str(output)}})
        await client.tool("score_close", {"document": doc})
    return snapshot, output.stat().st_size


def projection(seconds):
    # Stereo 32-bit WAV: rounded seconds, half-second slack, 3-second tail,
    # and 4096 bytes for the header, as in native/audio_budget.h.
    return math.ceil((seconds + 3.5) * 44100) * 8 + 4096


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    large, large_duration = asyncio.run(capture(args, 1000))
    short, expanded_duration = asyncio.run(capture(args, 4))
    mid_measure, mid_measure_size = asyncio.run(capture_mid_measure_end(args))
    assert large_duration == 4000 and expanded_duration == 16, (large_duration, expanded_duration)

    results = {"live_no_repeat_bytes": (args.output / "no-repeat-live.wav").stat().st_size,
               "live_expanded_seconds": large_duration}
    for repeats, seconds in ((False, 4), (True, expanded_duration)):
        options = {"format": "wav", "use_repeats": repeats}
        expected = projection(seconds)
        produced = live_budget(args, short, options, 2 * expected, "success")
        rejected = live_budget(args, short, options, 2 * expected - 1, "ResourceLimit")
        assert "projected audio" in rejected["message"], rejected
        # WAV is uncompressed: at most half a second of rounding, 4096 header
        # bytes, and one millisecond for the engine's time truncation.
        difference = expected - produced["output_bytes"]
        assert 0 <= difference <= math.ceil(0.501 * 44100) * 8 + 4096, (expected, produced)
        results["repeats" if repeats else "no_repeats"] = {
            "seconds": seconds, "projected_bytes": expected,
            "encoded_bytes": produced["output_bytes"], "projection_minus_encoded": difference,
            "one_byte_below_projection": rejected["result"]}
    # Use the actual captured 1000-repeat snapshot with a budget that fits
    # its unexpanded WAV and is far below the expanded projection.
    results["large_no_repeat_bounded"] = live_budget(
        args, large, {"format": "wav", "use_repeats": False}, 2 * projection(4), "success")
    results["mid_measure_no_repeat_at_encoded_size"] = live_budget(
        args, mid_measure, {"format": "wav", "use_repeats": False}, 2 * mid_measure_size, "success")
    # This score cannot be projected, so the engine itself must stop the file
    # one byte below its actual extent and deliver no bytes to the sink.
    results["mid_measure_engine_overrun"] = live_budget(
        args, mid_measure, {"format": "wav", "use_repeats": False}, 2 * mid_measure_size - 1, "ResourceLimit")
    assert "audio intermediate byte limit exceeded" in results["mid_measure_engine_overrun"]["message"]
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
