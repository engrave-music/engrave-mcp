#!/usr/bin/env python3
"""Integration tests for the audio facade MCP endpoints."""

import argparse
import asyncio
import copy
import json
import math
from pathlib import Path
import struct
import sys
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_async_session import Client


def analyze_wav(path: Path):
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        width = wf.getsampwidth()
        rate = wf.getframerate()
        frames = wf.getnframes()
        data = wf.readframes(frames)

    assert (channels, width, rate) == (2, 2, 44100)
    assert frames > 0 and len(data) == frames * channels * width
    samples = struct.unpack(f"<{len(data) // 2}h", data)

    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    max_abs = max(abs(s) for s in samples)
    return channels, width, rate, frames, rms, max_abs


def check_audio_header(fmt: str, data: bytes) -> bool:
    if not data:
        return False
    if fmt == "flac":
        return data.startswith(b"fLaC")
    if fmt == "ogg":
        return data.startswith(b"OggS")
    if fmt == "mp3":
        if data.startswith(b"ID3"):
            return True
        for i in range(min(1024, len(data) - 1)):
            if data[i] == 0xFF and (data[i + 1] & 0xE0) == 0xE0:
                return True
        return False
    return False


async def test_audio_facade(args):
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    checks = []

    score_payload = {
        "score": {
            "title": "Mix validation",
            "tempo": 120,
            "parts": [
                {
                    "name": "Piano",
                    "midiProgram": 1,
                    "measures": [
                        {
                            "notes": [
                                {"pitches": [60], "ticks": 480},
                                {"pitches": [62], "ticks": 480},
                                {"pitches": [64], "ticks": 480},
                                {"pitches": [65], "ticks": 480},
                            ]
                        }
                    ],
                },
                {
                    # "Bass" matches MuseScore's vocal template while program 43 is
                    # cello: regression for playback-channel sync after MusicXML import.
                    "name": "Bass",
                    "midiProgram": 43,
                    "measures": [
                        {
                            "notes": [
                                {"pitches": [48], "ticks": 480},
                                {"pitches": [50], "ticks": 480},
                                {"pitches": [52], "ticks": 480},
                                {"pitches": [53], "ticks": 480},
                            ]
                        }
                    ],
                },
            ],
        }
    }

    async with Client(
        binary=args.binary,
        resources=args.resources,
        soundfont=args.soundfont,
        directory=output_dir,
    ) as c:
        created = await c.tool("score_create", score_payload)
        doc_id = created["documentId"]
        rev = created["revision"]
        checks.append("score_created")

        ref_wav = output_dir / "reference.wav"
        if args.soundfont:
            await c.tool(
                "score_export",
                {"documentId": doc_id, "format": "wav", "path": str(ref_wav.resolve())},
            )
            checks.append("reference_wav_exported")

        tracks_res = await c.tool("score_audio_tracks", {"documentId": doc_id})
        tracks = tracks_res["tracks"]
        assert len(tracks) == 2, f"Expected 2 tracks, got {len(tracks)}"
        track_ids = [t["trackId"] for t in tracks]
        assert len(set(track_ids)) == 2, "Track IDs must be distinct"
        for t in tracks:
            assert t["name"].strip() != "", "Track name must not be empty"
        checks.append("tracks_verified")

        mix_res = await c.tool("score_audio_get_mix", {"documentId": doc_id})
        default_mix = copy.deepcopy(mix_res["mix"])
        rev = mix_res["revision"]
        mix_track_ids = [t["trackId"] for t in default_mix["tracks"]]
        assert set(mix_track_ids) == set(track_ids), "Mix track IDs must match score tracks"
        checks.append("default_mix_verified")

        doc_get_init = await c.tool("score_get", {"documentId": doc_id})
        meta_init = doc_get_init["metadata"]
        assert "tracks" in meta_init and len(meta_init["tracks"]) == len(tracks), (
            f"Expected {len(tracks)} metadata tracks, got {len(meta_init.get('tracks', []))}"
        )
        for meta_tr in meta_init["tracks"]:
            matching_audio = next(t for t in tracks if t["trackId"] == meta_tr["trackId"])
            matching_mix = next(t for t in default_mix["tracks"] if t["trackId"] == meta_tr["trackId"])
            assert meta_tr["partId"] == matching_audio["partId"]
            assert meta_tr["instrumentId"] == matching_audio["instrumentId"]
            assert meta_tr["name"] == matching_audio["name"]
            assert meta_tr["volumeDb"] == matching_mix["output"]["volumeDb"]
            assert meta_tr["balance"] == matching_mix["output"]["balance"]
            assert meta_tr["solo"] == matching_mix["output"]["solo"]
            assert meta_tr["muted"] == matching_mix["output"]["muted"]
            assert meta_tr["forceMute"] == matching_mix["output"]["forceMute"]
        checks.append("metadata_tracks_consistent")

        noop_res = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": copy.deepcopy(default_mix)},
        )
        assert noop_res["revision"] == rev, "No-op set_mix must not increment revision"
        checks.append("noop_set_mix_verified")

        reordered = copy.deepcopy(default_mix)
        reordered["tracks"] = list(reversed(reordered["tracks"]))
        reordered_res = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": reordered},
        )
        assert reordered_res["revision"] == rev, "Reordered tracks must not increment revision"
        checks.append("reordered_set_mix_verified")

        mod_mix = copy.deepcopy(default_mix)
        mod_mix["master"]["volumeDb"] = -6.0
        mod_res = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": mod_mix},
        )
        assert mod_res["revision"] == rev + 1, "Valid mix modification must increment revision by 1"
        rev = mod_res["revision"]
        checks.append("valid_mod_verified")

        mod_track_mix = copy.deepcopy(mod_res["mix"])
        mod_track_mix["tracks"][0]["output"]["volumeDb"] = -12.0
        mod_track_mix["tracks"][0]["output"]["muted"] = True
        mod_track_res = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": mod_track_mix},
        )
        assert mod_track_res["revision"] == rev + 1
        rev = mod_track_res["revision"]

        doc_get_track_mod = await c.tool("score_get", {"documentId": doc_id})
        meta_track_mod = doc_get_track_mod["metadata"]["tracks"]
        tr0 = next(t for t in meta_track_mod if t["trackId"] == mod_track_mix["tracks"][0]["trackId"])
        assert tr0["volumeDb"] == -12.0
        assert tr0["muted"] is True
        checks.append("metadata_tracks_updated_after_mix_change")

        # Partial patches (webmscore setAudioOutputParams): only named fields change.
        before_patch = copy.deepcopy(mod_track_res["mix"])
        second_id = before_patch["tracks"][1]["trackId"]
        patched = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": {"tracks": [{"trackId": second_id, "output": {"balance": 0.5}}]}},
        )
        assert patched["revision"] == rev + 1, "Partial patch must advance revision once"
        expected = copy.deepcopy(before_patch)
        expected["tracks"][1]["output"]["balance"] = 0.5
        assert patched["mix"] == expected, f"Partial patch changed unnamed fields: {patched['mix']}"
        rev = patched["revision"]
        noop_patch = await c.tool(
            "score_audio_set_mix", {"documentId": doc_id, "mix": {"master": {"volumeDb": -6.0}}}
        )
        assert noop_patch["revision"] == rev, "Partial patch with current values must be a no-op"
        reverted = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": {"tracks": [{"trackId": second_id, "output": {"balance": 0.0}}]}},
        )
        assert reverted["mix"] == before_patch
        rev = reverted["revision"]
        mod_track_res = reverted
        checks.append("partial_patch_verified")

        invalid_cases = []

        m = {"master": {"volumeDb": 3.0}, "tracks": [{"trackId": second_id, "output": {"muted": True}},
                                                     {"trackId": 999999, "output": {"muted": True}}]}
        invalid_cases.append(("partial_with_unknown_id", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["tracks"][1]["trackId"] = m["tracks"][0]["trackId"]
        invalid_cases.append(("duplicate_ids", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["tracks"][0]["trackId"] = 999999
        invalid_cases.append(("unknown_id", {"documentId": doc_id, "mix": m}))

        m = {"tracks": [{"trackId": second_id}]}
        invalid_cases.append(("track_without_output", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["master"]["unexpectedField"] = True
        invalid_cases.append(("extra_field", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["master"]["muted"] = "invalid_bool"
        invalid_cases.append(("wrong_type_bool", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["master"]["volumeDb"] = "loud"
        invalid_cases.append(("wrong_type_number", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["master"]["volumeDb"] = 25.0
        invalid_cases.append(("volumeDb_25", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        m["master"]["balance"] = 2.0
        invalid_cases.append(("balance_2", {"documentId": doc_id, "mix": m}))

        m = copy.deepcopy(default_mix)
        invalid_cases.append(("unknown_arg_reject", {"documentId": doc_id, "mix": m, "unknown": "value"}))

        for case_name, bad_args in invalid_cases:
            await c.tool("score_audio_set_mix", bad_args, error=True)
            verify = await c.tool("score_audio_get_mix", {"documentId": doc_id})
            assert verify["revision"] == rev, f"Revision changed on invalid case {case_name}"
            assert verify["mix"] == mod_track_res["mix"], f"Mix changed on invalid case {case_name}"
        checks.append("invalid_mixes_verified")

        exc_res = await c.tool("score_generate_excerpts", {"documentId": doc_id})
        excerpts = exc_res["excerpts"]
        assert len(excerpts) > 0, "Expected at least one excerpt"
        excerpt_id = excerpts[0]["id"]
        rev = exc_res["revision"]

        exc_get = await c.tool(
            "score_audio_get_mix",
            {"documentId": doc_id, "excerptId": excerpt_id},
        )
        excerpt_mix = copy.deepcopy(exc_get["mix"])
        assert not excerpt_mix["master"]["muted"], "Default excerpt master should not be muted"

        excerpt_mix["master"]["muted"] = True
        exc_set = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "excerptId": excerpt_id, "mix": excerpt_mix},
        )
        assert exc_set["mix"]["master"]["muted"] is True
        assert exc_set["revision"] == rev + 1
        rev = exc_set["revision"]

        master_check = await c.tool("score_audio_get_mix", {"documentId": doc_id})
        assert master_check["mix"]["master"]["volumeDb"] == -6.0
        assert not master_check["mix"]["master"]["muted"]

        reset_res = await c.tool(
            "score_audio_set_mix",
            {"documentId": doc_id, "mix": copy.deepcopy(default_mix)},
        )
        assert reset_res["revision"] == rev + 1, "Master reset must advance revision once"
        rev = reset_res["revision"]

        exc_recheck = await c.tool(
            "score_audio_get_mix",
            {"documentId": doc_id, "excerptId": excerpt_id},
        )
        assert exc_recheck["mix"]["master"]["muted"] is True, "Excerpt mute must persist across master reset"
        checks.append("excerpts_verified")

        doc_get = await c.tool("score_get", {"documentId": doc_id})
        assert doc_get["revision"] == rev, f"score_get revision mismatch: {doc_get['revision']} != {rev}"
        checks.append("revision_coherent")

        if args.soundfont:
            reset_wav = output_dir / "reset.wav"
            await c.tool(
                "score_export",
                {"documentId": doc_id, "format": "wav", "path": str(reset_wav.resolve())},
            )
            ref_c, ref_w, ref_r, ref_n, ref_rms, _ = analyze_wav(ref_wav)
            rst_c, rst_w, rst_r, rst_n, rst_rms, _ = analyze_wav(reset_wav)
            assert ref_rms > 0 and rst_rms > 0, "Reference and reset must contain audible PCM"
            assert (ref_c, ref_w, ref_r, ref_n) == (rst_c, rst_w, rst_r, rst_n), "WAV properties mismatch"
            max_rms = max(ref_rms, rst_rms, 1e-9)
            rms_diff = abs(ref_rms - rst_rms) / max_rms
            assert rms_diff <= 0.01, f"RMS difference {rms_diff:.4f} exceeds 1%"
            checks.append("wav_comparison_passed")

            muted_wav = output_dir / "mutedexcerpt.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "excerptId": excerpt_id,
                    "format": "wav",
                    "path": str(muted_wav.resolve()),
                },
            )
            _, _, _, _, _, muted_max = analyze_wav(muted_wav)
            assert muted_max <= 1, f"Muted excerpt max sample {muted_max} > 1 LSB"
            checks.append("muted_excerpt_passed")

            for fmt in ["flac", "ogg", "mp3"]:
                comp_path = output_dir / f"master.{fmt}"
                await c.tool(
                    "score_export",
                    {"documentId": doc_id, "format": fmt, "path": str(comp_path.resolve())},
                )
                assert comp_path.exists() and comp_path.stat().st_size > 0, f"{fmt} file empty"
                data = comp_path.read_bytes()
                assert check_audio_header(fmt, data), f"Invalid {fmt} header"
            checks.append("compressed_exports_passed")

        await c.tool("score_close", {"documentId": doc_id})
        checks.append("score_closed")

    evidence = {
        "status": "passed",
        "checks": checks,
        "revision": rev,
    }
    evidence_file = output_dir / "evidence.json"
    evidence_file.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(json.dumps(evidence))


def main():
    parser = argparse.ArgumentParser(description="Test audio facade MCP tools")
    parser.add_argument("--binary", type=Path, required=True, help="Path to MCP binary")
    parser.add_argument("--resources", type=Path, default=None, help="Path to resources")
    parser.add_argument("--soundfont", type=Path, default=None, help="Path to soundfont")
    parser.add_argument("--output", type=Path, required=True, help="Output directory")
    args = parser.parse_args()

    asyncio.run(test_audio_facade(args))


if __name__ == "__main__":
    main()
