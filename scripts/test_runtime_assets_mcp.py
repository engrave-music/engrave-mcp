"""Exercise live font and SoundFont changes through the MCP host and its workers."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import shutil

import numpy as np
import soundfile as sf

from test_async_session import Client


ROOT = Path(__file__).resolve().parents[1]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    font_source = args.resources / "fonts/petaluma/PetalumaScript.otf"
    replacement_source = ROOT / "tests/engine_audio/sine.sf2"
    font_input = out / "mutable-font.otf"
    soundfont_input = out / "mutable-soundfont.sf2"
    shutil.copyfile(font_source, font_input)
    shutil.copyfile(replacement_source, soundfont_input)

    score = (ROOT / "fixtures/golden/linked_parts.mscx").read_text(encoding="utf-8")
    marker = "<height>10</height>"
    assert marker in score
    score = score.replace(marker,
        '<Text><style>title</style><text><font face="Petaluma Script"/>'
        'MMMWWWiii012345</text></Text>' + marker, 1)
    score_path = out / "font-score.mscx"
    score_path.write_text(score, encoding="utf-8")

    async with Client(args.binary, args.resources, args.soundfont, out) as client:
        document = (await client.tool("score_open", {"path": str(score_path)}))["documentId"]
        before_path, after_path = out / "before.svg", out / "after.svg"
        before = await client.tool("score_export_start", {
            "documentId": document, "format": "svg", "path": str(before_path)})
        assert (await client.terminal(before))["state"] == "completed"

        font = await client.tool("runtime_add_font", {"path": str(font_input)})
        assert font["family"] == "Petaluma Script" and font["contentHash"]
        assert Path(font["path"]).read_bytes() == font_input.read_bytes()
        again = await client.tool("runtime_add_font", {"path": str(font_input)})
        assert again["id"] == font["id"] and again["path"] == font["path"]
        font_input.write_bytes(b"invalid font")
        after = await client.tool("score_export_start", {
            "documentId": document, "format": "svg", "path": str(after_path)})
        assert (await client.terminal(after))["state"] == "completed"
        assert before_path.read_bytes() != after_path.read_bytes()
        assert Path(font["path"]).read_bytes() == font_source.read_bytes()
        await client.tool("score_close", {"documentId": document})

        composition = {"title": "SoundFont selection", "tempo": 120, "parts": [
            {"name": "Piano", "midiProgram": 1, "measures": [
                {"notes": [{"pitches": [60], "ticks": 1920}]}]}]}
        audio_doc = (await client.tool("score_create", {"score": composition}))["documentId"]
        fluid_path, captured_path = out / "fluid.wav", out / "captured.wav"
        sine_path, again_path = out / "sine.wav", out / "sine-again.wav"
        await client.tool("score_export", {"documentId": audio_doc, "format": "wav", "path": str(fluid_path)})
        captured = await client.tool("score_export_start", {
            "documentId": audio_doc, "format": "wav", "path": str(captured_path)})
        selected = await client.tool("runtime_set_soundfont", {"path": str(soundfont_input)})
        assert selected["contentHash"] and Path(selected["path"]).read_bytes() == soundfont_input.read_bytes()
        soundfont_input.write_bytes(b"invalid soundfont")
        await client.tool("score_export", {"documentId": audio_doc, "format": "wav", "path": str(sine_path)})
        assert (await client.terminal(captured))["state"] == "completed"
        assert captured_path.read_bytes() == fluid_path.read_bytes(), "Job lost its captured SoundFont"
        assert sine_path.read_bytes() != fluid_path.read_bytes(), "SoundFont change did not affect audio"
        for path in (fluid_path, sine_path):
            audio, rate = sf.read(path, always_2d=True)
            assert rate == 44100 and np.isfinite(audio).all() and np.max(np.abs(audio)) > 0.001
        await client.tool("runtime_set_soundfont", {"path": str(soundfont_input)}, error=True)
        await client.tool("score_export", {"documentId": audio_doc, "format": "wav", "path": str(again_path)})
        assert again_path.read_bytes() == sine_path.read_bytes(), "Invalid SoundFont changed the selection"
        await client.tool("score_close", {"documentId": audio_doc})

    print(json.dumps({"status": "passed", "font_worker_relayout": True,
                      "soundfont_worker_selection": True, "captured_soundfont": True,
                      "invalid_soundfont_rollback": True}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
