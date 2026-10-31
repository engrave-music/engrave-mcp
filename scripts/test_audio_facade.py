#!/usr/bin/env python3
"""Integration tests for audio export options and audio facade MCP endpoints."""

from __future__ import annotations

import argparse
import asyncio
import copy
import json
import math
from pathlib import Path
import struct
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_async_session import Client
from test_export_options import bar

# A hard-panned track still leaks through the Muse Reverb aux return (the
# engine adds a 0.30 send per track, engrave/src/audio/engine_audio.cpp). Measured with
# the reverb enabled on Windows and Linux 2026-10-28, three identical runs per
# side: hard left -> right/left RMS 0.213, hard right -> left/right RMS 0.272.
# The older 0.35 limit was loosened without this measurement. 0.30 is just
# above the measured maximum (~10% headroom) and still rejects a pan that does
# not attenuate the opposite channel.
TRACK_BALANCE_LEAKAGE_LIMIT = 0.30


def analyze_wav(path: Path):
    import soundfile as sf
    import numpy as np

    audio, rate = sf.read(str(path), always_2d=True)
    channels = audio.shape[1]
    rate = int(rate)
    frames = len(audio)
    info = sf.info(str(path))
    width = 4 if "FLOAT" in info.subtype else 2

    assert (channels, rate) == (2, 44100)
    assert frames > 0
    rms = float(np.sqrt(np.mean(audio ** 2)))
    max_abs = float(np.max(np.abs(audio)))
    left = audio[:, 0]
    right = audio[:, 1]
    left_rms = float(np.sqrt(np.mean(left ** 2)))
    right_rms = float(np.sqrt(np.mean(right ** 2)))
    return channels, width, rate, frames, rms, max_abs, left_rms, right_rms


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
    if fmt == "aac":
        return len(data) > 4 and data[0] == 0xFF and (data[1] & 0xF0) == 0xF0
    return False


async def test_audio_facade(args):
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    checks = []
    balance_leakage = {}

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

        # 1. Inspect score_audio_tracks and metadata tracks defaults
        tracks_res = await c.tool("score_audio_tracks", {"documentId": doc_id})
        tracks = tracks_res["tracks"]
        assert len(tracks) == 2, f"Expected 2 tracks, got {len(tracks)}"
        track_ids = [t["trackId"] for t in tracks]
        assert len(set(track_ids)) == 2, "Track IDs must be distinct"
        for t in tracks:
            assert t["name"].strip() != "", "Track name must not be empty"
            assert "partId" in t and "instrumentId" in t
            # Defaults for created score: 0 dB, balance 0, not solo, not muted, forceMute false
            assert t["volumeDb"] == 0.0, f"Expected default volumeDb 0.0, got {t['volumeDb']}"
            assert t["balance"] == 0.0, f"Expected default balance 0.0, got {t['balance']}"
            assert t["solo"] is False, f"Expected default solo False, got {t['solo']}"
            assert t["muted"] is False, f"Expected default muted False, got {t['muted']}"
            assert t["forceMute"] is False, f"Expected default forceMute False, got {t['forceMute']}"
        checks.append("tracks_defaults_verified")

        doc_get_init = await c.tool("score_get", {"documentId": doc_id})
        meta_init = doc_get_init["metadata"]
        assert "tracks" in meta_init and len(meta_init["tracks"]) == len(tracks)
        for meta_tr in meta_init["tracks"]:
            matching = next(t for t in tracks if t["trackId"] == meta_tr["trackId"])
            assert meta_tr["partId"] == matching["partId"]
            assert meta_tr["instrumentId"] == matching["instrumentId"]
            assert meta_tr["name"] == matching["name"]
            assert meta_tr["volumeDb"] == matching["volumeDb"]
            assert meta_tr["balance"] == matching["balance"]
            assert meta_tr["solo"] == matching["solo"]
            assert meta_tr["muted"] == matching["muted"]
            assert meta_tr["forceMute"] == matching["forceMute"]
        checks.append("metadata_tracks_consistent")

        if args.soundfont:
            # 2. Reference full mix export (explicit 0 dB mix)
            ref_wav = output_dir / "reference.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(ref_wav.resolve()),
                    "mix": {"master": {"volumeDb": 0.0}},
                },
            )
            _, _, _, _, ref_rms, ref_max, _, _ = analyze_wav(ref_wav)
            assert ref_rms > 0 and ref_max > 0, "Reference audio must not be silent"
            checks.append("reference_wav_exported")

            # 3. Muted track silent
            # A: Both tracks muted -> output must be silent
            both_muted_wav = output_dir / "both_muted.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(both_muted_wav.resolve()),
                    "mix": {
                        "tracks": [
                            {"trackId": track_ids[0], "muted": True},
                            {"trackId": track_ids[1], "muted": True},
                        ]
                    },
                },
            )
            _, _, _, _, both_muted_rms, both_muted_max, _, _ = analyze_wav(both_muted_wav)
            assert both_muted_max <= 1e-4, f"Both tracks muted sample peak {both_muted_max} > 1e-4"
            assert both_muted_rms < 1e-4, f"Both tracks muted RMS {both_muted_rms} > 1e-4"
            checks.append("both_tracks_muted_silent")

            # B: Muting track 0 or track 1 reduces energy
            mute0_wav = output_dir / "mute0.wav"
            mute1_wav = output_dir / "mute1.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(mute0_wav.resolve()),
                    "mix": {"tracks": [{"trackId": track_ids[0], "muted": True}]},
                },
            )
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(mute1_wav.resolve()),
                    "mix": {"tracks": [{"trackId": track_ids[1], "muted": True}]},
                },
            )
            _, _, _, _, mute0_rms, mute0_max, _, _ = analyze_wav(mute0_wav)
            _, _, _, _, mute1_rms, mute1_max, _, _ = analyze_wav(mute1_wav)
            assert 0 < mute0_rms < ref_rms, f"Track 0 muted RMS {mute0_rms} not in (0, {ref_rms})"
            assert 0 < mute1_rms < ref_rms, f"Track 1 muted RMS {mute1_rms} not in (0, {ref_rms})"
            checks.append("individual_track_mute_verified")

            # 4. Solo -> other tracks silent (energy ratio)
            solo0_wav = output_dir / "solo0.wav"
            solo1_wav = output_dir / "solo1.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(solo0_wav.resolve()),
                    "mix": {"tracks": [{"trackId": track_ids[0], "solo": True}]},
                },
            )
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(solo1_wav.resolve()),
                    "mix": {"tracks": [{"trackId": track_ids[1], "solo": True}]},
                },
            )
            _, _, _, _, solo0_rms, _, _, _ = analyze_wav(solo0_wav)
            _, _, _, _, solo1_rms, _, _, _ = analyze_wav(solo1_wav)

            # Soloing track 0 mutes track 1, so solo0 should match mute1
            diff0 = abs(solo0_rms - mute1_rms) / max(mute1_rms, 1e-9)
            assert diff0 <= 0.01, f"Solo track 0 RMS ({solo0_rms}) differs from mute track 1 RMS ({mute1_rms}) by {diff0:.4f}"
            diff1 = abs(solo1_rms - mute0_rms) / max(mute0_rms, 1e-9)
            assert diff1 <= 0.01, f"Solo track 1 RMS ({solo1_rms}) differs from mute track 0 RMS ({mute0_rms}) by {diff1:.4f}"
            checks.append("solo_energy_parity_verified")

            # 4b. Solo + mute together on track 0 -> all tracks silent (track 0 muted, track 1 force-muted)
            solo_mute_wav = output_dir / "solo_and_mute.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(solo_mute_wav.resolve()),
                    "mix": {
                        "tracks": [
                            {"trackId": track_ids[0], "solo": True, "muted": True},
                        ]
                    },
                },
            )
            _, _, _, _, sm_rms, sm_max, _, _ = analyze_wav(solo_mute_wav)
            assert sm_max <= 1e-4, f"Solo+mute track peak {sm_max} > 1e-4"
            assert sm_rms < 1e-4, f"Solo+mute track RMS {sm_rms} > 1e-4"
            checks.append("solo_and_mute_silent_verified")

            # 4c. Two solos -> both tracks active (neither force-muted)
            two_solos_wav = output_dir / "two_solos.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(two_solos_wav.resolve()),
                    "mix": {
                        "tracks": [
                            {"trackId": track_ids[0], "solo": True},
                            {"trackId": track_ids[1], "solo": True},
                        ]
                    },
                },
            )
            _, _, _, _, two_solos_rms, _, _, _ = analyze_wav(two_solos_wav)
            two_solos_diff = abs(two_solos_rms - ref_rms) / max(ref_rms, 1e-9)
            assert two_solos_diff <= 0.01, f"Two solos RMS ({two_solos_rms}) differs from ref ({ref_rms}) by {two_solos_diff:.4f}"
            checks.append("two_solos_verified")

            # 5. volumeDb within ±0.2 dB of the expected gain on a single-track score
            single_score = {
                "score": {
                    "title": "Single part gain test",
                    "tempo": 120,
                    "parts": [
                        {
                            "name": "Piano",
                            "midiProgram": 1,
                            "measures": [
                                {
                                    "notes": [
                                        {"pitches": [60], "ticks": 480},
                                        {"pitches": [64], "ticks": 480},
                                        {"pitches": [67], "ticks": 480},
                                        {"pitches": [72], "ticks": 480},
                                    ]
                                }
                            ],
                        }
                    ],
                }
            }
            single_created = await c.tool("score_create", single_score)
            single_doc = single_created["documentId"]
            single_track_id = (await c.tool("score_audio_tracks", {"documentId": single_doc}))["tracks"][0]["trackId"]

            gain_0db_wav = output_dir / "gain_0db.wav"
            gain_neg6db_wav = output_dir / "gain_neg6db.wav"

            await c.tool(
                "score_export",
                {
                    "documentId": single_doc,
                    "format": "wav",
                    "path": str(gain_0db_wav.resolve()),
                    "mix": {"tracks": [{"trackId": single_track_id, "volumeDb": 0.0}]},
                },
            )
            await c.tool(
                "score_export",
                {
                    "documentId": single_doc,
                    "format": "wav",
                    "path": str(gain_neg6db_wav.resolve()),
                    "mix": {"tracks": [{"trackId": single_track_id, "volumeDb": -6.0}]},
                },
            )

            _, _, _, _, rms_0db, _, _, _ = analyze_wav(gain_0db_wav)
            _, _, _, _, rms_neg6db, _, _, _ = analyze_wav(gain_neg6db_wav)
            assert rms_0db > 0 and rms_neg6db > 0
            measured_gain_db = 20.0 * math.log10(rms_neg6db / rms_0db)
            expected_gain_db = -6.0
            gain_error = abs(measured_gain_db - expected_gain_db)
            assert gain_error <= 0.2, (
                f"Gain at -6 dB measured {measured_gain_db:.3f} dB (diff {gain_error:.3f} dB > 0.2 dB tolerance)"
            )
            await c.tool("score_close", {"documentId": single_doc})
            checks.append("volume_gain_accuracy_verified")

            # 6. Balance via L/R energy
            left_wav = output_dir / "panned_left.wav"
            right_wav = output_dir / "panned_right.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(left_wav.resolve()),
                    "mix": {"master": {"balance": -1.0}},
                },
            )
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(right_wav.resolve()),
                    "mix": {"master": {"balance": 1.0}},
                },
            )
            _, _, _, _, _, _, l_rms_left, r_rms_left = analyze_wav(left_wav)
            _, _, _, _, _, _, l_rms_right, r_rms_right = analyze_wav(right_wav)
            assert l_rms_left > 0 and r_rms_left <= 0.02 * l_rms_left, f"Hard left pan leaked into right channel: L={l_rms_left}, R={r_rms_left}"
            assert r_rms_right > 0 and l_rms_right <= 0.02 * r_rms_right, f"Hard right pan leaked into left channel: L={l_rms_right}, R={r_rms_right}"
            checks.append("balance_lr_energy_verified")

            # 6b. Per-track balance via L/R energy (isolate track 0 by muting track 1)
            tr_left_wav = output_dir / "track0_panned_left.wav"
            tr_right_wav = output_dir / "track0_panned_right.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(tr_left_wav.resolve()),
                    "mix": {
                        "tracks": [
                            {"trackId": track_ids[0], "balance": -1.0},
                            {"trackId": track_ids[1], "muted": True},
                        ]
                    },
                },
            )
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(tr_right_wav.resolve()),
                    "mix": {
                        "tracks": [
                            {"trackId": track_ids[0], "balance": 1.0},
                            {"trackId": track_ids[1], "muted": True},
                        ]
                    },
                },
            )
            _, _, _, _, _, _, tr_l_left, tr_r_left = analyze_wav(tr_left_wav)
            _, _, _, _, _, _, tr_l_right, tr_r_right = analyze_wav(tr_right_wav)
            left_leak = tr_r_left / tr_l_left
            right_leak = tr_l_right / tr_r_right
            assert tr_l_left > 0 and tr_r_left <= TRACK_BALANCE_LEAKAGE_LIMIT * tr_l_left, (
                f"Track hard left leaked into R: L={tr_l_left}, R={tr_r_left} "
                f"(ratio {left_leak:.4f} > {TRACK_BALANCE_LEAKAGE_LIMIT})")
            assert tr_r_right > 0 and tr_l_right <= TRACK_BALANCE_LEAKAGE_LIMIT * tr_r_right, (
                f"Track hard right leaked into L: L={tr_l_right}, R={tr_r_right} "
                f"(ratio {right_leak:.4f} > {TRACK_BALANCE_LEAKAGE_LIMIT})")
            balance_leakage = {
                "measured_hard_left_into_right": round(left_leak, 4),
                "measured_hard_right_into_left": round(right_leak, 4),
                "limit": TRACK_BALANCE_LEAKAGE_LIMIT,
                "reverb_aux_send": 0.30,
            }
            checks.append("track_balance_lr_energy_verified")

            # 7. Master volumeDb
            master_neg12_wav = output_dir / "master_neg12db.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(master_neg12_wav.resolve()),
                    "mix": {"master": {"volumeDb": -12.0}},
                },
            )
            _, _, _, _, master_neg12_rms, _, _, _ = analyze_wav(master_neg12_wav)
            measured_master_db = 20.0 * math.log10(master_neg12_rms / ref_rms)
            master_error = abs(measured_master_db - (-12.0))
            assert master_error <= 0.2, f"Master volume -12 dB measured {measured_master_db:.3f} dB (diff {master_error:.3f} dB > 0.2 dB)"
            checks.append("master_volume_gain_verified")

            # 8. Compressed exports with mix
            for fmt in ["flac", "ogg", "mp3", "aac"]:
                comp_path = output_dir / f"mix_export.{fmt}"
                await c.tool(
                    "score_export",
                    {
                        "documentId": doc_id,
                        "format": fmt,
                        "path": str(comp_path.resolve()),
                        "mix": {"master": {"volumeDb": -3.0}},
                    },
                )
                assert comp_path.exists() and comp_path.stat().st_size > 0, f"{fmt} file empty"
                data = comp_path.read_bytes()
                assert check_audio_header(fmt, data), f"Invalid {fmt} header"
            checks.append("compressed_exports_passed")

        # 9. Excerpts with mix
        exc_res = await c.tool("score_generate_excerpts", {"documentId": doc_id})
        excerpts = exc_res["excerpts"]
        assert len(excerpts) > 0, "Expected at least one excerpt"
        excerpt_id = excerpts[0]["id"]
        rev = exc_res["revision"]

        exc_tracks = (await c.tool("score_audio_tracks", {"documentId": doc_id, "excerptId": excerpt_id}))["tracks"]
        assert len(exc_tracks) > 0, "Excerpt must have audio tracks"
        if args.soundfont:
            exc_wav = output_dir / "excerpt_mix.wav"
            await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "excerptId": excerpt_id,
                    "format": "wav",
                    "path": str(exc_wav.resolve()),
                    "mix": {"master": {"volumeDb": -2.0}},
                },
            )
            _, _, _, _, exc_rms, _, _, _ = analyze_wav(exc_wav)
            assert exc_rms > 0, "Excerpt export must produce audio"
            checks.append("excerpt_mix_export_passed")

        # 10. Rejection cases for mix option
        invalid_cases = [
            ("unknown_trackId", {"tracks": [{"trackId": 999999, "muted": True}]}),
            ("duplicate_trackId", {"tracks": [{"trackId": track_ids[0]}, {"trackId": track_ids[0]}]}),
            ("volumeDb_too_high", {"master": {"volumeDb": 25.0}}),
            ("volumeDb_too_low", {"master": {"volumeDb": -121.0}}),
            ("balance_too_high", {"master": {"balance": 1.5}}),
            ("balance_too_low", {"master": {"balance": -1.5}}),
            ("forceMute_key_not_accepted", {"tracks": [{"trackId": track_ids[0], "forceMute": True}]}),
            ("unexpected_key_master", {"master": {"unexpected": True}}),
            ("unexpected_key_track", {"tracks": [{"trackId": track_ids[0], "unexpected": True}]}),
        ]

        for case_name, bad_mix in invalid_cases:
            bad_path = output_dir / f"bad_{case_name}.wav"
            if bad_path.exists():
                bad_path.unlink()
            res = await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": "wav",
                    "path": str(bad_path.resolve()),
                    "mix": bad_mix,
                },
                error=True,
            )
            assert res.get("isError") is True, f"Expected error for {case_name}, got {res}"
            assert not bad_path.exists(), f"File should not be created for invalid case {case_name}"
        checks.append("invalid_mix_options_rejected")

        # Mix on non-audio formats must be rejected
        for bad_fmt in ["svg", "midi", "musicxml", "png", "pdf"]:
            non_audio_path = output_dir / f"bad_fmt_{bad_fmt}.{bad_fmt}"
            if non_audio_path.exists():
                non_audio_path.unlink()
            res = await c.tool(
                "score_export",
                {
                    "documentId": doc_id,
                    "format": bad_fmt,
                    "path": str(non_audio_path.resolve()),
                    "mix": {"master": {"volumeDb": 0.0}},
                },
                error=True,
            )
            assert res.get("isError") is True, f"Expected mix rejection for format {bad_fmt}"
            assert not non_audio_path.exists()
        checks.append("mix_on_non_audio_rejected")

        # Live async rejection via score_export_start: unknown trackId
        async_track_path = output_dir / "async_bad_track.wav"
        if async_track_path.exists():
            async_track_path.unlink()
        async_bad_track = await c.tool(
            "score_export_start",
            {
                "documentId": doc_id,
                "format": "wav",
                "path": str(async_track_path.resolve()),
                "mix": {"tracks": [{"trackId": 999999, "muted": True}]},
            },
            error=True,
        )
        assert async_bad_track.get("isError") is True, f"Expected async rejection for unknown trackId: {async_bad_track}"
        assert not async_track_path.exists()
        checks.append("async_unknown_track_rejected")

        # Live async rejection via score_export_start: mix on non-audio
        async_fmt_path = output_dir / "async_bad_fmt.musicxml"
        if async_fmt_path.exists():
            async_fmt_path.unlink()
        async_bad_fmt = await c.tool(
            "score_export_start",
            {
                "documentId": doc_id,
                "format": "musicxml",
                "path": str(async_fmt_path.resolve()),
                "mix": {"master": {"volumeDb": 0.0}},
            },
            error=True,
        )
        assert async_bad_fmt.get("isError") is True, f"Expected async rejection for mix on musicxml: {async_bad_fmt}"
        assert not async_fmt_path.exists()
        checks.append("async_mix_on_non_audio_rejected")

        # Live call to removed tool name returns unknown tool
        for removed_name in ["score_audio_get_mix", "score_audio_set_mix", "score_audio_start", "score_audio_read", "score_audio_seek"]:
            removed_res = await c.call("tools/call", {"name": removed_name, "arguments": {}})
            assert "error" in removed_res, f"Expected RPC error for removed tool {removed_name}, got {removed_res}"
            err_msg = removed_res["error"].get("message", "").lower()
            assert "unknown tool" in err_msg, f"Expected 'unknown tool' in error message for {removed_name}, got {err_msg}"
        checks.append("removed_tools_rejected")

        # Exports never change the document revision
        doc_get_after = await c.tool("score_get", {"documentId": doc_id})
        assert doc_get_after["revision"] == rev, f"Document revision changed from {rev} to {doc_get_after['revision']}"
        checks.append("export_never_changes_revision")

        # 11. Default mix from MSCZ (fixtures/audio/mix.mscz)
        mix_fixture_path = ROOT / "fixtures/audio/mix.mscz"
        if mix_fixture_path.is_file():
            opened_mscz = await c.tool("score_open", {"path": str(mix_fixture_path.resolve())})
            mscz_doc = opened_mscz["documentId"]

            # Inspect tracks: Cello should be muted by default
            mscz_tracks_res = await c.tool("score_audio_tracks", {"documentId": mscz_doc})
            mscz_tracks = mscz_tracks_res["tracks"]
            assert len(mscz_tracks) == 3, f"Expected 3 tracks in mix.mscz, got {len(mscz_tracks)}"

            cello_track = next((t for t in mscz_tracks if t["name"] == "Cello"), None)
            assert cello_track is not None, f"Cello track not found in {mscz_tracks}"
            assert cello_track["muted"] is True, f"Cello must be muted by default from audiosettings.json: {cello_track}"

            violin_track = next(t for t in mscz_tracks if t["name"] == "Violin")
            assert violin_track["muted"] is False, f"Violin must not be muted: {violin_track}"

            # Inspect metadata.tracks
            mscz_get = await c.tool("score_get", {"documentId": mscz_doc})
            meta_cello = next(t for t in mscz_get["metadata"]["tracks"] if t["name"] == "Cello")
            assert meta_cello["muted"] is True, f"Metadata must report Cello muted: {meta_cello}"
            checks.append("mscz_default_mix_metadata_verified")

            if args.soundfont:
                # Plain WAV export: Cello is silent by default
                default_mscz_wav = output_dir / "mscz_default.wav"
                await c.tool(
                    "score_export",
                    {"documentId": mscz_doc, "format": "wav", "path": str(default_mscz_wav.resolve())},
                )
                _, _, _, _, def_mscz_rms, _, _, _ = analyze_wav(default_mscz_wav)

                # Export with mix explicitly unmuting Cello: brings Cello back, higher energy
                unmute_mscz_wav = output_dir / "mscz_unmuted_cello.wav"
                await c.tool(
                    "score_export",
                    {
                        "documentId": mscz_doc,
                        "format": "wav",
                        "path": str(unmute_mscz_wav.resolve()),
                        "mix": {"tracks": [{"trackId": cello_track["trackId"], "muted": False}]},
                    },
                )
                _, _, _, _, unmute_mscz_rms, _, _, _ = analyze_wav(unmute_mscz_wav)
                assert unmute_mscz_rms > def_mscz_rms + 0.001, (
                    f"Unmuting Cello must increase RMS energy: {unmute_mscz_rms} vs default {def_mscz_rms}"
                )

                # Export with explicit mix keeping Cello muted: matches default export
                explicit_mute_wav = output_dir / "mscz_explicit_mute.wav"
                await c.tool(
                    "score_export",
                    {
                        "documentId": mscz_doc,
                        "format": "wav",
                        "path": str(explicit_mute_wav.resolve()),
                        "mix": {"tracks": [{"trackId": cello_track["trackId"], "muted": True}]},
                    },
                )
                _, _, _, _, exp_mute_rms, _, _, _ = analyze_wav(explicit_mute_wav)
                mute_diff = abs(exp_mute_rms - def_mscz_rms) / max(def_mscz_rms, 1e-9)
                assert mute_diff <= 0.01, f"Explicit mute RMS differs from default mix RMS by {mute_diff:.4f}"
                checks.append("mscz_default_mix_audio_verified")

            await c.tool("score_close", {"documentId": mscz_doc})

        # 11b. Excerpt-specific mixer settings from MSCZ (fixtures/audio/mix_parts.mscz)
        mix_parts_path = ROOT / "fixtures/audio/mix_parts.mscz"
        if mix_parts_path.is_file():
            parts_mscz = await c.tool("score_open", {"path": str(mix_parts_path.resolve())})
            parts_doc = parts_mscz["documentId"]

            # Excerpt 0 (Violin): muted in root audiosettings.json, but unmuted in Excerpts/0_Violin/audiosettings.json
            violin_part_tracks = (await c.tool("score_audio_tracks", {"documentId": parts_doc, "excerptId": 0}))["tracks"]
            violin_part_tr = next(t for t in violin_part_tracks if t["name"] == "Violin")
            assert violin_part_tr["muted"] is False, f"Violin excerpt must not inherit root mute: {violin_part_tr}"

            # Excerpt 2 (Cello): unmuted in root audiosettings.json, but muted in Excerpts/2_Cello/audiosettings.json
            cello_part_tracks = (await c.tool("score_audio_tracks", {"documentId": parts_doc, "excerptId": 2}))["tracks"]
            cello_part_tr = next(t for t in cello_part_tracks if t["name"] == "Cello")
            assert cello_part_tr["muted"] is True, f"Cello excerpt must apply its own mute: {cello_part_tr}"

            if args.soundfont:
                # Export Violin excerpt: audible (ignores root mute)
                violin_part_wav = output_dir / "excerpt_violin.wav"
                await c.tool(
                    "score_export",
                    {"documentId": parts_doc, "excerptId": 0, "format": "wav", "path": str(violin_part_wav.resolve())},
                )
                _, _, _, _, v_rms, v_max, _, _ = analyze_wav(violin_part_wav)
                assert v_rms > 0 and v_max > 0, "Violin part export must be audible"

                # Export Cello excerpt: silent (part's own mute applies)
                cello_part_wav = output_dir / "excerpt_cello_muted.wav"
                await c.tool(
                    "score_export",
                    {"documentId": parts_doc, "excerptId": 2, "format": "wav", "path": str(cello_part_wav.resolve())},
                )
                _, _, _, _, c_rms, c_max, _, _ = analyze_wav(cello_part_wav)
                assert c_max <= 1e-4, f"Cello part export peak {c_max} > 1e-4"
                assert c_rms < 1e-4, f"Cello part export RMS {c_rms} > 1e-4"
                checks.append("mscz_parts_audio_settings_verified")

            await c.tool("score_close", {"documentId": parts_doc})

        # 12. Score with multi-instrument parts (instrument change score)
        change_score_path = ROOT / "vendor/musescore/src/engraving/tests/instrumentchange_data/change-ref.mscx"
        if change_score_path.is_file():
            change_opened = await c.tool("score_open", {"path": str(change_score_path.resolve())})
            ch_doc = change_opened["documentId"]

            # Sync & async metadata report tracks for all instrument tracks
            ch_get = await c.tool("score_get", {"documentId": ch_doc})
            ch_tracks_meta = ch_get["metadata"]["tracks"]
            assert len(ch_tracks_meta) == 3, f"Expected 3 tracks for instrument change score, got {ch_tracks_meta}"

            ch_tracks_res = await c.tool("score_audio_tracks", {"documentId": ch_doc})
            ch_tracks = ch_tracks_res["tracks"]
            assert len(ch_tracks) == 3, f"Expected 3 audio tracks, got {len(ch_tracks)}"
            ch_track_ids = [t["trackId"] for t in ch_tracks]
            assert len(set(ch_track_ids)) == 3, "Track IDs must be unique"

            # Verify partId "1" has two tracks (flute and oboe)
            p1_tracks = [t for t in ch_tracks if t["partId"] == "1"]
            assert len(p1_tracks) == 2, f"Expected 2 tracks for part 1, got {p1_tracks}"

            # Async metadata export via score_export_start
            meta_path = output_dir / "change_meta.json"
            meta_job = await c.tool(
                "score_export_start",
                {"documentId": ch_doc, "format": "metadata", "path": str(meta_path.resolve())},
            )
            meta_done = await c.terminal(meta_job)
            assert meta_done["state"] == "completed", meta_done
            meta_data = json.loads(meta_path.read_text(encoding="utf-8"))
            assert len(meta_data["tracks"]) == 3, f"Async metadata tracks must contain 3 tracks: {meta_data['tracks']}"

            if args.soundfont:
                # Sync plain WAV export succeeds via engine
                ch_sync_wav = output_dir / "change_sync.wav"
                await c.tool(
                    "score_export",
                    {"documentId": ch_doc, "format": "wav", "path": str(ch_sync_wav.resolve())},
                )
                _, _, _, _, ch_s_rms, ch_s_max, _, _ = analyze_wav(ch_sync_wav)
                assert ch_s_rms > 0 and ch_s_max > 0, "Sync WAV export must produce audio"

                # Async plain WAV export succeeds via engine
                ch_async_wav = output_dir / "change_async.wav"
                ch_job = await c.tool(
                    "score_export_start",
                    {"documentId": ch_doc, "format": "wav", "path": str(ch_async_wav.resolve())},
                )
                ch_done = await c.terminal(ch_job)
                assert ch_done["state"] == "completed", ch_done
                assert ch_async_wav.read_bytes() == ch_sync_wav.read_bytes(), "Sync and async WAV must be byte-identical"

                # Mix request succeeds on multi-instrument score
                mix_sync_wav = output_dir / "change_mix_sync.wav"
                await c.tool(
                    "score_export",
                    {
                        "documentId": ch_doc,
                        "format": "wav",
                        "path": str(mix_sync_wav.resolve()),
                        "mix": {"tracks": [{"trackId": ch_track_ids[0], "muted": True}]},
                    },
                )
                mix_async_wav = output_dir / "change_mix_async.wav"
                mix_job = await c.tool(
                    "score_export_start",
                    {
                        "documentId": ch_doc,
                        "format": "wav",
                        "path": str(mix_async_wav.resolve()),
                        "mix": {"tracks": [{"trackId": ch_track_ids[0], "muted": True}]},
                    },
                )
                mix_done = await c.terminal(mix_job)
                assert mix_done["state"] == "completed", mix_done
                assert mix_sync_wav.read_bytes() == mix_async_wav.read_bytes(), "Sync and async mixed WAV must be byte-identical"
                _, _, _, _, ch_mix_rms, _, _, _ = analyze_wav(mix_sync_wav)
                assert ch_mix_rms < ch_s_rms, f"Muting track must reduce energy: {ch_mix_rms} vs {ch_s_rms}"
                checks.append("multi_instrument_score_sync_async_verified")

            await c.tool("score_close", {"documentId": ch_doc})

        await c.tool("score_close", {"documentId": doc_id})
        checks.append("score_closed")

    evidence = {
        "status": "passed",
        "checks": checks,
        "revision": rev,
    }
    if balance_leakage:
        evidence["track_balance_leakage"] = balance_leakage
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
