"""Exercise the native server through the independent official Python MCP client."""
import asyncio
import copy
import json
import logging
import os
from pathlib import Path
import time
import xml.etree.ElementTree as ET

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("MUSIC_MCP_OUTPUT", ROOT / "artifacts/mcp-smoke"))


def bar(*pitches):
    return {"notes": [{"pitches": [p] if p is not None else [], "ticks": 480} for p in pitches]}


async def main():
    protocol_errors = []
    class ProtocolLog(logging.Handler):
        def emit(self, record):
            if record.levelno >= logging.ERROR:
                protocol_errors.append(record.getMessage())
    protocol_log = ProtocolLog()
    logging.getLogger("mcp.client.stdio").addHandler(protocol_log)
    OUT.mkdir(parents=True, exist_ok=True)
    score = {"title": "Native Evening / 原生晚曲", "tempo": 100, "parts": [
        {"name": "Violin", "midiProgram": 41, "measures": [bar(67, 69, 71, 74), bar(72, 71, 69, None)]},
        {"name": "Piano", "midiProgram": 1, "measures": [
            {"notes": [{"pitches": [48, 55, 60], "ticks": 1920}]},
            {"notes": [{"pitches": [50, 57, 62], "ticks": 1920}]}]},
        {"name": "Cello", "midiProgram": 43, "measures": [bar(48, 48, 55, 55), bar(50, 50, 57, 57)]},
    ]}
    binary = Path(os.environ.get("MUSIC_MCP_BINARY", ROOT / "build/windows/musescore-mcp.exe"))
    soundfont = Path(os.environ.get("MUSIC_MCP_SOUNDFONT", ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"))
    child_env = dict(os.environ)
    if os.environ.get("MUSIC_MCP_CLEAN_ENV"):
        child_env = {"SystemRoot": os.environ["SystemRoot"], "PATH": str(Path(os.environ["SystemRoot"]) / "System32"),
                     "TEMP": os.environ["TEMP"], "TMP": os.environ["TMP"]}
    child_args = ["--soundfont", str(soundfont)]
    if os.environ.get("MUSIC_MCP_RESOURCES"):
        child_args += ["--resources", os.environ["MUSIC_MCP_RESOURCES"]]
    params = StdioServerParameters(command=str(binary), args=child_args, env=child_env, cwd=str(OUT))
    report = {"calls": [], "cycles": 0}
    started = time.perf_counter()
    with (OUT / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                report["protocol"] = initialized.protocol_version
                listed = await session.list_tools()
                assert len(listed.tools) == 6
                async def call(name, arguments, expect_error=False):
                    response = await session.call_tool(name, arguments)
                    assert bool(response.is_error) == expect_error, response
                    report["calls"].append({"tool": name, "error": bool(response.is_error)})
                    return response.structured_content
                opened = await call("score_create", {"score": score})
                identity = {"documentId": opened["documentId"]}
                before = await call("score_get", identity)
                assert before["composition"] == score
                (OUT / "before.musicxml").write_text(before["musicxml"], encoding="utf-8")
                bad = {"notes": [{"pitches": [60], "ticks": 480}]}
                await call("score_replace_measures", {**identity, "partIndex": 0, "startMeasure": 1, "measures": [bad]}, True)
                assert (await call("score_get", identity))["composition"] == score
                replacement = bar(76, 74, 72, 71)
                await call("score_replace_measures", {**identity, "partIndex": 0, "startMeasure": 1, "measures": [replacement]})
                expected = copy.deepcopy(score)
                expected["parts"][0]["measures"][1] = replacement
                after = await call("score_get", identity)
                assert after["composition"] == expected
                # Check the actual engine output, not only its stored JSON model.
                parsed = ET.fromstring(after["musicxml"])
                pitches = [n.findtext("pitch/step") + n.findtext("pitch/octave")
                           for n in parsed.findall("part")[0].findall("measure")[1].findall("note")]
                assert pitches == ["E5", "D5", "C5", "B4"], pitches
                for fmt, ext in [("musicxml", "musicxml"), ("mxl", "mxl"), ("metadata", "json"), ("midi", "mid"), ("svg", "svg"),
                                 ("png", "png"), ("pdf", "pdf"), ("mscz", "mscz"), ("wav", "wav")]:
                    exported = await call("score_export", {**identity, "format": fmt, "path": str(OUT / f"score.{ext}")})
                    assert exported["bytes"] == (OUT / f"score.{ext}").stat().st_size > 0
                from verify_exports import canonical_notes
                reference = OUT / "edited.musicxml"
                reference.write_text(after["musicxml"], encoding="utf-8")
                for extension in ["mscz", "mxl"]:
                    reopened = await call("score_open", {"path": str(OUT / f"score.{extension}")})
                    restored = await call("score_get", {"documentId": reopened["documentId"]})
                    restored_path = OUT / f"reopened-{extension}.musicxml"
                    restored_path.write_text(restored["musicxml"], encoding="utf-8")
                    assert canonical_notes(reference) == canonical_notes(restored_path)
                    await call("score_close", {"documentId": reopened["documentId"]})
                # Switching documents must not export the other document's music/audio.
                second = await call("score_open", {"path": str(ROOT / "fixtures/evening.musicxml")})
                assert (await call("score_get", identity))["title"] == before["title"]
                await call("score_close", {"documentId": second["documentId"]})
                await call("score_close", identity)
                await call("score_get", identity, True)
                for _ in range(20):
                    item = await call("score_create", {"score": score})
                    await call("score_close", {"documentId": item["documentId"]})
                    report["cycles"] += 1
    assert not protocol_errors, protocol_errors
    report["seconds"] = round(time.perf_counter() - started, 3)
    assert "ERROR:" not in (OUT / "stderr.log").read_text(encoding="utf-8"), "Engine errors in stderr.log"
    import numpy as np
    import soundfile as sf
    audio, rate = sf.read(OUT / "score.wav", always_2d=True)
    assert rate == 44100 and audio.shape[1] == 2
    assert np.isfinite(audio).all() and np.max(np.abs(audio)) > 0.01, "Audio is silent or invalid"
    report["audio"] = {"rate": rate, "seconds": len(audio) / rate, "peak": float(np.max(np.abs(audio)))}
    report["status"] = "passed"
    (OUT / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "calls"}))


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), timeout=180))
