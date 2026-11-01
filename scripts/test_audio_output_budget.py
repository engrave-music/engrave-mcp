"""Measure audio worker cache growth with tiny and non-default budgets."""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from v2_client import Client, ROOT


async def make_snapshot(args):
    async with Client(args, args.output) as client:
        doc = (await client.tool("score_open", {"path": str(ROOT / "fixtures/evening.musicxml")}))["document"]
        duration = (await client.tool("score_outline", {"document": doc}))["outline"]["duration"]
        path = (args.output / "source.mscz").resolve()
        await client.tool("score_export", {"document": doc, "action": "run", "kind": "score",
            "options": {"format": "mscz"}, "destination": {"kind": "file", "path": str(path)}})
        await client.tool("score_close", {"document": doc})
    return path, duration


def worker(args, source, audio_options, limit, expected):
    with tempfile.TemporaryDirectory(prefix="audio-budget-", dir=args.output) as temporary:
        job = Path(temporary).resolve()
        stage, cache = job / "stage", job / "cache"
        stage.mkdir()
        shutil.copyfile(source, job / "payload")
        request = {"version": 2, "parentPid": os.getpid(),
                   "export": {"document": "doc-A-1", "action": "start", "kind": "audio",
                              "options": audio_options,
                              "destination": {"kind": "file", "path": str(job / "destination")}},
                   "payload": str(job / "payload"), "result": str(job / "result.json"), "stage": str(stage),
                   "outputLimit": limit, "resources": str(args.resources.resolve()), "fonts": [],
                   "soundfonts": [str(args.soundfont.resolve())], "sampleRate": 44100, "deterministicIds": False}
        (job / "request.json").write_text(json.dumps(request), encoding="utf-8")
        peak = 0
        with (job / "worker.log").open("wb") as log:
            process = subprocess.Popen([str(args.binary.resolve()), "--worker", str(job / "request.json")],
                                       cwd=ROOT, stdout=log, stderr=log)
            until = time.monotonic() + 60
            while process.poll() is None and time.monotonic() < until:
                for path in cache.glob("*"):
                    try:
                        if path.is_file():
                            peak = max(peak, path.stat().st_size)
                    except FileNotFoundError:
                        pass
                time.sleep(.002)
            if process.poll() is None:
                process.kill()
                process.wait()
                raise AssertionError("worker timed out")
            exit_code = process.wait()
        result = json.loads((job / "result.json").read_text(encoding="utf-8"))
        if expected == "success":
            assert exit_code == 0 and result["ok"], result
            produced = (stage / "output").stat().st_size
            assert 0 < produced <= limit
            digest = hashlib.sha256((stage / "output").read_bytes()).hexdigest()
        else:
            assert exit_code != 0 and not result["ok"] and result["error"]["code"] == expected, result
            assert not list(stage.iterdir()) and not (job / "destination").exists()
            produced = 0
            digest = None
        assert not list(cache.glob("*")), "temporary audio survived worker exit"
        return {"limit": limit, "observed_cache_peak": peak, "exit": exit_code,
                "result": "success" if result["ok"] else result["error"]["code"], "output_bytes": produced,
                "sha256": digest, "message": "" if result["ok"] else result["error"]["message"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--soundfont", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source, duration = asyncio.run(make_snapshot(args))
    seconds = duration + 3.5  # nearest-second outline plus half-second and 3-second tail
    frames = math.ceil(seconds * 44100)
    wav16, wav24 = frames * 4 + 4096, frames * 6 + 4096
    wav_budget = 2 * ((wav16 + wav24) // 2)
    mp3_128 = math.ceil(seconds * 128000 / 8 + 1024 * 1024)
    mp3_512 = math.ceil(seconds * 512000 / 8 + 1024 * 1024)
    mp3_budget = 2 * ((mp3_128 + mp3_512) // 2)
    evidence = {"duration_seconds_rounded": duration,
        "tiny_wav": worker(args, source, {"format": "wav"}, 64, "ResourceLimit"),
        "wav16": worker(args, source, {"format": "wav", "bit_depth": 16}, wav_budget, "success"),
        "wav16_large_budget": worker(args, source, {"format": "wav", "bit_depth": 16}, 2 * wav_budget, "success"),
        "wav24": worker(args, source, {"format": "wav", "bit_depth": 24}, wav_budget, "ResourceLimit"),
        "mp3_128": worker(args, source, {"format": "mp3", "bitrate_kbps": 128}, mp3_budget, "success"),
        "mp3_512": worker(args, source, {"format": "mp3", "bitrate_kbps": 512}, mp3_budget, "ResourceLimit")}
    assert evidence["tiny_wav"]["observed_cache_peak"] <= 64
    assert evidence["wav16"]["sha256"] == evidence["wav16_large_budget"]["sha256"]
    (args.output / "audio-budget-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
