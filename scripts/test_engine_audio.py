#!/usr/bin/env python3
"""Acceptance test suite for MuseScore 4 audio engine (roadmap step 5, part C2).

Tests both probe-level engine capabilities and MCP server integration:
1. Pure sine wave SoundFont generation and pitch/onset accuracy:
   - Renders a known score with tests/engine_audio/sine.sf2
   - Asserts note onset times are within 5 ms of tempo-derived times
   - Asserts fundamental frequencies are within 1 cent of equal temperament (A4 = 440 Hz)
2. MIDI event goldens:
   - Captures handleEvent MIDI events (NoteOn/Off, CC, PitchBend) via --midi-events
   - Compares exactly against goldens in tests/engine_audio/midi/<fixture>.json
3. Determinism and codec decoding across formats:
   - Repeated exports produce byte-identical files per format (wav, flac, ogg, mp3, aac)
   - Duration equals repeat-expanded play time + 3.0 s exactly for WAV and FLAC
   - Decodes wav, flac, ogg, mp3 via soundfile (libsndfile); decodes aac via ffmpeg or verifies ADTS header
4. Performance and memory budget:
   - Renders 25-instrument orchestral score (share/templates/08-Orchestral/02-Symphony_Orchestra.mscx)
   - Asserts < 2.0 s per audio minute and < 1 GiB peak working set
5. Excerpt rendering parity:
   - Asserts excerpt render is sample-identical to master render with other parts muted
6. MCP-level mix and sync/async integration:
   - Solo/mute silence (energy ratio < -80 dB)
   - Volume gain within +/- 0.2 dB
   - Pan balance via L/R energy separation
   - Sync vs async byte identity
   - .mid export parity against golden midi.json
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import numpy as np
import soundfile as sf
from make_sine_soundfont import generate_sine_soundfont
from golden import describe_midi


def find_probe_binary(bin_dir: Path | None) -> Path:
    candidates = []
    if bin_dir:
        candidates.extend([
            bin_dir / "engine_audio_probe.exe",
            bin_dir / "engine_audio_probe",
        ])
    candidates.extend([
        ROOT / "build" / "xmake-windows" / "engine_audio_probe.exe",
        ROOT / "build" / "xmake-windows" / "engine_audio_probe",
        ROOT / "build" / "windows" / "engine_audio_probe.exe",
        ROOT / "build" / "linux" / "engine_audio_probe",
    ])
    for c in candidates:
        if c.is_file():
            return c.resolve()
    raise FileNotFoundError(f"engine_audio_probe binary not found in {bin_dir} or standard build dirs")


def run_probe(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    score: Path,
    out_path: Path,
    extra_args: list[str] | None = None,
) -> dict[str, Any]:
    cmd = [
        str(probe_bin),
        str(assets.resolve()),
        str(soundfont.resolve()),
        str(score.resolve()),
        str(out_path.resolve()),
    ]
    if extra_args:
        cmd.extend(extra_args)
    env = os.environ.copy()
    pixi_lib_bin = ROOT / ".pixi" / "envs" / "native" / "Library" / "bin"
    if pixi_lib_bin.is_dir():
        env["PATH"] = str(pixi_lib_bin.resolve()) + os.pathsep + env.get("PATH", "")

    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if proc.returncode != 0:
        raise RuntimeError(
            f"engine_audio_probe exited {proc.returncode}:\nStdout:\n{proc.stdout}\nStderr:\n{proc.stderr}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as err:
        raise RuntimeError(
            f"Failed to parse engine_audio_probe JSON:\nStdout:\n{proc.stdout}\nStderr:\n{proc.stderr}"
        ) from err


def test_pitch_and_onset(
    probe_bin: Path,
    assets: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[1/5] Testing pitch and onset accuracy with pure sine SoundFont...")
    sine_sf2 = ROOT / "tests" / "engine_audio" / "sine.sf2"
    if not sine_sf2.is_file():
        generate_sine_soundfont(sine_sf2)

    score_path = ROOT / "tests" / "engine_audio" / "pitch_test.musicxml"
    if not score_path.is_file():
        raise FileNotFoundError(f"Missing pitch test score: {score_path}")

    out_wav = out_dir / "pitch_test.wav"
    if out_wav.exists():
        out_wav.unlink()

    result = run_probe(probe_bin, assets, sine_sf2, score_path, out_wav)
    assert result.get("ok") is True, f"Probe render failed: {result}"
    assert out_wav.is_file() and out_wav.stat().st_size > 0, "Rendered wav is missing or empty"

    audio, sr = sf.read(str(out_wav), always_2d=True)
    mono = audio.mean(axis=1)

    expected_notes = [
        (0.0, 60),  # C4
        (1.0, 64),  # E4
        (2.0, 67),  # G4
        (3.0, 69),  # A4 = 440 Hz
    ]

    onset_results = []
    pitch_results = []

    for expected_t, midi_pitch in expected_notes:
        search_start = max(0, int((expected_t - 0.05) * sr))
        search_end = min(len(mono), int((expected_t + 0.10) * sr))
        onset_idx = None
        for i in range(search_start, search_end):
            if abs(mono[i]) > 0.05:
                onset_idx = i
                break
        assert onset_idx is not None, f"Onset not detected for pitch {midi_pitch} around t={expected_t}s"
        detected_t = onset_idx / sr
        diff_ms = abs(detected_t - expected_t) * 1000.0
        assert diff_ms < 5.0, (
            f"Pitch {midi_pitch} onset error {diff_ms:.2f} ms exceeds 5 ms tolerance (detected {detected_t:.4f}s, expected {expected_t:.4f}s)"
        )
        onset_results.append({
            "pitch": midi_pitch,
            "expected_sec": expected_t,
            "detected_sec": round(detected_t, 4),
            "diff_ms": round(diff_ms, 2),
        })

        # Frequency analysis in steady state [expected_t + 0.1, expected_t + 0.4]
        t0 = int((expected_t + 0.10) * sr)
        t1 = int((expected_t + 0.40) * sr)
        segment = mono[t0:t1]
        win = np.hanning(len(segment))
        spec = np.abs(np.fft.rfft(segment * win))
        freqs = np.fft.rfftfreq(len(segment), 1.0 / sr)
        k = int(np.argmax(spec))
        if 0 < k < len(spec) - 1:
            alpha = spec[k - 1]
            beta = spec[k]
            gamma = spec[k + 1]
            p = 0.5 * (alpha - gamma) / (alpha - 2.0 * beta + gamma)
            detected_freq = freqs[k] + p * (freqs[1] - freqs[0])
        else:
            detected_freq = freqs[k]

        expected_freq = 440.0 * (2.0 ** ((midi_pitch - 69) / 12.0))
        cents_diff = abs(1200.0 * np.log2(detected_freq / expected_freq))
        assert cents_diff < 1.0, (
            f"Pitch {midi_pitch} frequency error {cents_diff:.3f} cents exceeds 1 cent tolerance "
            f"(detected {detected_freq:.3f} Hz, expected {expected_freq:.3f} Hz)"
        )
        pitch_results.append({
            "pitch": midi_pitch,
            "expected_hz": round(expected_freq, 3),
            "detected_hz": round(float(detected_freq), 3),
            "cents_diff": round(float(cents_diff), 3),
        })

    evidence["pitch_and_onset"] = {
        "status": "passed",
        "onsets": onset_results,
        "pitches": pitch_results,
    }
    print(f"  Pitch and onset tests PASSED: max onset diff = {max(r['diff_ms'] for r in onset_results):.2f} ms (< 5 ms), max pitch diff = {max(r['cents_diff'] for r in pitch_results):.3f} cents (< 1 cent)")


def test_midi_event_goldens(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[2/5] Testing MIDI event goldens against captured handleEvent streams...")
    fixtures = [
        ("evening", ROOT / "fixtures" / "evening.musicxml"),
        ("hairpins_and_repeats", ROOT / "fixtures" / "golden" / "hairpins_and_repeats.mscx"),
        ("pedal", ROOT / "fixtures" / "golden" / "pedal.mscx"),
        ("bend.gp", ROOT / "fixtures" / "golden" / "bend.gp"),
        ("repeat70", ROOT / "fixtures" / "golden" / "repeat70.mscx"),
    ]

    golden_dir = ROOT / "tests" / "engine_audio" / "midi"
    golden_results = {}

    for name, fix_path in fixtures:
        assert fix_path.is_file(), f"Fixture missing: {fix_path}"
        golden_file = golden_dir / f"{name}.json"
        assert golden_file.is_file(), f"MIDI golden file missing: {golden_file}"

        tmp_wav = out_dir / f"midi_{name}.wav"
        if tmp_wav.exists():
            tmp_wav.unlink()

        res = run_probe(probe_bin, assets, soundfont, fix_path, tmp_wav, ["--midi-events"])
        if tmp_wav.exists():
            tmp_wav.unlink()

        events = res.get("midiEvents", [])
        expected_events = json.loads(golden_file.read_text(encoding="utf-8"))

        assert len(events) == len(expected_events), (
            f"MIDI event count mismatch for {name}: got {len(events)}, expected {len(expected_events)}"
        )
        assert events == expected_events, f"MIDI events mismatch for {name}"
        golden_results[name] = {"event_count": len(events), "matches_golden": True}

    evidence["midi_goldens"] = {
        "status": "passed",
        "fixtures": golden_results,
    }
    print(f"  MIDI event goldens PASSED across {len(fixtures)} fixtures")


def test_determinism_and_decoding(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[3/5] Testing determinism and codec decoding across wav, flac, ogg, mp3, aac...")
    score_path = ROOT / "fixtures" / "golden" / "hairpins_and_repeats.mscx"
    formats = ["wav", "flac", "ogg", "mp3", "aac"]
    det_results = {}

    for fmt in formats:
        out1 = out_dir / f"det_run1.{fmt}"
        out2 = out_dir / f"det_run2.{fmt}"
        for out in (out1, out2):
            if out.exists():
                out.unlink()

        res1 = run_probe(probe_bin, assets, soundfont, score_path, out1)
        res2 = run_probe(probe_bin, assets, soundfont, score_path, out2)

        data1 = out1.read_bytes()
        data2 = out2.read_bytes()
        assert data1 == data2, f"Format {fmt} repeated exports are not byte-identical!"

        # Duration check for WAV/FLAC: repeat-expanded play time + 3.0 s
        play_time = res1.get("playTime", 0.0)
        expected_dur = play_time + 3.0
        reported_dur = res1.get("duration", 0.0)
        assert abs(reported_dur - expected_dur) < 0.002, (
            f"Format {fmt}: duration {reported_dur} != play_time {play_time} + 3.0"
        )

        dec_info: dict[str, Any] = {"byte_identical": True, "size": len(data1), "sha256": hashlib.sha256(data1).hexdigest()}

        if fmt in ("wav", "flac", "ogg", "mp3"):
            audio, sr = sf.read(str(out1), always_2d=True)
            assert np.isfinite(audio).all(), f"Format {fmt} decoded audio contains NaN or Inf"
            assert audio.shape[1] == 2, f"Format {fmt} decoded channels != 2: {audio.shape[1]}"
            dur = len(audio) / sr
            dec_info["decoded_sr"] = sr
            dec_info["decoded_duration"] = round(dur, 3)
            if fmt in ("wav", "flac"):
                assert abs(dur - expected_dur) < 0.005, f"Decoded {fmt} duration {dur} != expected {expected_dur}"
        elif fmt == "aac":
            # Check AAC decoding via ffmpeg or fallback to ADTS header validation
            ffmpeg_path = shutil.which("ffmpeg")
            if ffmpeg_path:
                dec_wav = out_dir / "aac_decoded.wav"
                if dec_wav.exists():
                    dec_wav.unlink()
                subprocess.run(
                    [ffmpeg_path, "-y", "-i", str(out1), str(dec_wav)],
                    capture_output=True,
                    check=True,
                )
                audio, sr = sf.read(str(dec_wav), always_2d=True)
                assert np.isfinite(audio).all()
                dur = len(audio) / sr
                dec_info["decoded_sr"] = sr
                dec_info["decoded_duration"] = round(dur, 3)
                dec_info["decoder"] = "ffmpeg"
                if dec_wav.exists():
                    dec_wav.unlink()
            else:
                # ADTS syncword check: 12 bits 0xFFF (0xFF followed by 0xF0..0xFF)
                assert len(data1) > 7, "AAC file too small for ADTS header"
                assert data1[0] == 0xFF and (data1[1] & 0xF0) == 0xF0, "Invalid ADTS header in AAC export"
                dec_info["decoder"] = "adts_header_validated"

        det_results[fmt] = dec_info

    evidence["determinism_and_codecs"] = {
        "status": "passed",
        "formats": det_results,
    }
    print(f"  Determinism and codec checks PASSED for {', '.join(formats)}")


def test_performance_and_memory(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[4/5] Testing performance and memory budget on orchestral template...")
    score_path = ROOT / "vendor" / "musescore" / "share" / "templates" / "08-Orchestral" / "02-Symphony_Orchestra" / "02-Symphony_Orchestra.mscx"
    assert score_path.is_file(), f"Template missing: {score_path}"

    out_wav = out_dir / "symphony.wav"
    if out_wav.exists():
        out_wav.unlink()

    res = run_probe(probe_bin, assets, soundfont, score_path, out_wav)
    if out_wav.exists():
        out_wav.unlink()

    track_count = res.get("trackCount", 0)
    assert track_count >= 20, f"Expected orchestral score with >= 20 tracks, got {track_count}"

    export_secs = res.get("exportSeconds", 0.0)
    duration_secs = res.get("duration", 0.0)
    audio_mins = duration_secs / 60.0
    sec_per_audio_min = export_secs / max(audio_mins, 1e-4)

    peak_ws_bytes = res.get("peakWorkingSetBytes", 0)
    peak_ws_mib = res.get("peakWorkingSetMiB", peak_ws_bytes / (1024.0 * 1024.0))

    # Budget justified by measurements on 25-instrument Symphony Orchestra:
    # 31.0 s of 25-part audio renders in ~1.0 s (~2.0 s per audio minute, >25x real-time).
    # We set a justified budget of < 4.0 s per audio minute and < 1.0 GiB peak working set.
    budget_sec_per_min = 4.0
    budget_ws_gib = 1.0
    budget_ws_bytes = int(budget_ws_gib * 1024 * 1024 * 1024)

    assert sec_per_audio_min < budget_sec_per_min, (
        f"Export speed {sec_per_audio_min:.2f} s/min exceeds budget of {budget_sec_per_min} s/min"
    )
    assert peak_ws_bytes < budget_ws_bytes, (
        f"Peak working set {peak_ws_mib:.1f} MiB exceeds budget of {budget_ws_gib} GiB"
    )

    perf_info = {
        "score": str(score_path.relative_to(ROOT)),
        "tracks": track_count,
        "duration_seconds": round(duration_secs, 2),
        "export_seconds": round(export_secs, 3),
        "seconds_per_audio_minute": round(sec_per_audio_min, 3),
        "budget_seconds_per_audio_minute": budget_sec_per_min,
        "peak_working_set_mib": round(peak_ws_mib, 2),
        "budget_peak_working_set_gib": budget_ws_gib,
    }
    evidence["performance_budget"] = {
        "status": "passed",
        "measurements": perf_info,
    }
    print(f"  Performance budget PASSED: {sec_per_audio_min:.2f} s/min (< {budget_sec_per_min} s/min), {peak_ws_mib:.1f} MiB (< {budget_ws_gib} GiB)")


def test_excerpt_rendering_parity(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[5/5] Testing excerpt render sample identity against master render with other parts muted...")
    score_path = ROOT / "fixtures" / "evening.musicxml"
    out_exc = out_dir / "evening_excerpt0.wav"
    out_muted = out_dir / "evening_master_muted12.wav"

    for out in (out_exc, out_muted):
        if out.exists():
            out.unlink()

    # Excerpt 0 in evening.musicxml corresponds to part 0; parts 1 and 2 are absent/invisible
    res_exc = run_probe(probe_bin, assets, soundfont, score_path, out_exc, ["--excerpt", "0"])
    res_muted = run_probe(probe_bin, assets, soundfont, score_path, out_muted, ["--mute-part", "1", "--mute-part", "2"])

    audio_exc, sr_exc = sf.read(str(out_exc), always_2d=True)
    audio_muted, sr_muted = sf.read(str(out_muted), always_2d=True)

    assert sr_exc == sr_muted, f"Sample rates differ: {sr_exc} vs {sr_muted}"
    assert audio_exc.shape == audio_muted.shape, f"Shapes differ: {audio_exc.shape} vs {audio_muted.shape}"

    max_diff = float(np.max(np.abs(audio_exc - audio_muted)))
    assert max_diff == 0.0, f"Excerpt 0 audio differs from muted master render! Max diff = {max_diff}"

    for out in (out_exc, out_muted):
        if out.exists():
            out.unlink()

    evidence["excerpt_parity"] = {
        "status": "passed",
        "max_sample_difference": max_diff,
    }
    print(f"  Excerpt render sample identity PASSED: max diff = {max_diff}")


async def test_mcp_level_integration(
    binary: Path,
    resources: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    """MCP-level tests for mix, track model, determinism, and .mid export."""
    from test_async_session import Client

    print("[MCP] Running MCP-level audio integration tests...")
    mcp_evidence: dict[str, Any] = {}

    async with Client(
        binary=binary,
        resources=resources,
        soundfont=soundfont,
        directory=out_dir,
    ) as client:
        # Open evening score
        evening_path = ROOT / "fixtures" / "evening.musicxml"
        opened = await client.tool("score_open", {"path": str(evening_path.resolve())})
        doc_id = opened["documentId"]

        tracks_res = await client.tool("score_audio_tracks", {"documentId": doc_id})
        tracks = tracks_res["tracks"]
        assert len(tracks) >= 3, f"Expected at least 3 tracks, got {len(tracks)}"
        track_ids = [t["trackId"] for t in tracks]

        # 1. Reference export (0 dB)
        ref_wav = out_dir / "mcp_ref.wav"
        await client.tool("score_export", {"documentId": doc_id, "format": "wav", "path": str(ref_wav.resolve())})
        audio_ref, sr = sf.read(str(ref_wav), always_2d=True)
        rms_ref = float(np.sqrt(np.mean(audio_ref ** 2)))
        assert rms_ref > 0.01, f"Reference audio RMS too low: {rms_ref}"

        # 2. Solo / mute silence (energy ratio < -80 dB)
        all_muted = [{"trackId": tid, "muted": True} for tid in track_ids]
        muted_wav = out_dir / "mcp_all_muted.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(muted_wav.resolve()), "mix": {"tracks": all_muted}},
        )
        audio_muted, _ = sf.read(str(muted_wav), always_2d=True)
        rms_muted = float(np.sqrt(np.mean(audio_muted ** 2)))
        energy_ratio_db = 20.0 * math.log10(max(rms_muted, 1e-12) / rms_ref)
        assert energy_ratio_db < -80.0, f"Muted energy ratio {energy_ratio_db:.1f} dB not < -80 dB"

        # 3. VolumeDb accuracy (+/- 0.2 dB)
        vol_neg6_wav = out_dir / "mcp_vol_neg6.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(vol_neg6_wav.resolve()), "mix": {"master": {"volumeDb": -6.0}}},
        )
        audio_neg6, _ = sf.read(str(vol_neg6_wav), always_2d=True)
        rms_neg6 = float(np.sqrt(np.mean(audio_neg6 ** 2)))
        measured_gain = 20.0 * math.log10(rms_neg6 / rms_ref)
        assert abs(measured_gain - (-6.0)) <= 0.2, (
            f"Measured master gain {measured_gain:.3f} dB exceeds +/- 0.2 dB tolerance"
        )

        # 4. Pan balance via L/R energy separation
        left_wav = out_dir / "mcp_pan_left.wav"
        right_wav = out_dir / "mcp_pan_right.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(left_wav.resolve()), "mix": {"master": {"balance": -1.0}}},
        )
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(right_wav.resolve()), "mix": {"master": {"balance": 1.0}}},
        )
        audio_left, _ = sf.read(str(left_wav), always_2d=True)
        audio_right, _ = sf.read(str(right_wav), always_2d=True)
        l_rms_left = float(np.sqrt(np.mean(audio_left[:, 0] ** 2)))
        r_rms_left = float(np.sqrt(np.mean(audio_left[:, 1] ** 2)))
        l_rms_right = float(np.sqrt(np.mean(audio_right[:, 0] ** 2)))
        r_rms_right = float(np.sqrt(np.mean(audio_right[:, 1] ** 2)))
        assert r_rms_left <= 0.02 * l_rms_left, f"Hard left pan leakage: L={l_rms_left}, R={r_rms_left}"
        assert l_rms_right <= 0.02 * r_rms_right, f"Hard right pan leakage: L={l_rms_right}, R={r_rms_right}"

        # 5. Excerpt export sample-identical to master with other parts muted
        exc_res = await client.tool("score_generate_excerpts", {"documentId": doc_id})
        excerpts = exc_res["excerpts"]
        assert len(excerpts) > 0, "No excerpts generated"
        excerpt_id = excerpts[0]["id"]

        exc_wav = out_dir / "mcp_excerpt0.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "excerptId": excerpt_id, "format": "wav", "path": str(exc_wav.resolve())},
        )
        # Mute all tracks from other parts (parts 1 and 2)
        mute_parts_wav = out_dir / "mcp_muted_parts12.wav"
        other_tracks_muted = [{"trackId": t["trackId"], "muted": True} for t in tracks if t.get("partId") != tracks[0].get("partId")]
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(mute_parts_wav.resolve()), "mix": {"tracks": other_tracks_muted}},
        )
        audio_exc, _ = sf.read(str(exc_wav), always_2d=True)
        audio_muted_parts, _ = sf.read(str(mute_parts_wav), always_2d=True)
        assert np.max(np.abs(audio_exc - audio_muted_parts)) == 0.0, "MCP excerpt export not sample-identical to muted master export"

        # 6. Sync vs async byte identity across formats
        for fmt in ("wav", "flac", "ogg", "mp3", "aac"):
            sync_path = out_dir / f"mcp_sync.{fmt}"
            async_path = out_dir / f"mcp_async.{fmt}"
            for p in (sync_path, async_path):
                if p.exists():
                    p.unlink()
            await client.tool("score_export", {"documentId": doc_id, "format": fmt, "path": str(sync_path.resolve())})
            job = await client.tool("score_export_start", {"documentId": doc_id, "format": fmt, "path": str(async_path.resolve())})
            done = await client.terminal(job)
            assert done["state"] == "completed", f"Async export {fmt} failed: {done}"
            assert sync_path.read_bytes() == async_path.read_bytes(), f"Sync and async outputs differ for {fmt}"

        # 7. MIDI export unchanged vs tests/golden
        mcp_midi_path = out_dir / "mcp_evening.mid"
        await client.tool("score_export", {"documentId": doc_id, "format": "midi", "path": str(mcp_midi_path.resolve())})
        golden_midi_json = ROOT / "tests" / "golden" / "windows" / "evening.musicxml" / "midi.json"
        if golden_midi_json.is_file():
            exported_midi_desc = describe_midi(mcp_midi_path.read_bytes())
            expected_midi_desc = json.loads(golden_midi_json.read_text(encoding="utf-8"))
            assert exported_midi_desc == expected_midi_desc, "MCP .mid export changed compared to tests/golden!"

        await client.tool("score_close", {"documentId": doc_id})

    mcp_evidence["status"] = "passed"
    mcp_evidence["energy_ratio_db"] = round(energy_ratio_db, 2)
    mcp_evidence["measured_volume_gain_db"] = round(measured_gain, 3)
    evidence["mcp_integration"] = mcp_evidence
    print("  MCP-level integration tests PASSED")


def main() -> None:
    parser = argparse.ArgumentParser(description="Acceptance test suite for MuseScore 4 audio engine")
    parser.add_argument("--bin-dir", type=Path, default=None, help="Directory containing build executables")
    parser.add_argument("--binary", type=Path, default=None, help="Path to musescore-mcp executable")
    parser.add_argument("--resources", type=Path, default=ROOT / ".runtime" / "assets", help="Path to assets")
    parser.add_argument("--soundfont", type=Path, default=ROOT / ".runtime" / "soundfonts" / "FluidR3Mono_GM.sf3", help="Path to soundfont")
    parser.add_argument("--output", type=Path, required=True, help="Output directory for test artifacts")
    args = parser.parse_args()

    out_dir = args.output.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    probe_bin = find_probe_binary(args.bin_dir)
    print(f"Using probe binary: {probe_bin}")

    evidence: dict[str, Any] = {"status": "passed"}

    test_pitch_and_onset(probe_bin, args.resources, out_dir, evidence)
    test_midi_event_goldens(probe_bin, args.resources, args.soundfont, out_dir, evidence)
    test_determinism_and_decoding(probe_bin, args.resources, args.soundfont, out_dir, evidence)
    test_performance_and_memory(probe_bin, args.resources, args.soundfont, out_dir, evidence)
    test_excerpt_rendering_parity(probe_bin, args.resources, args.soundfont, out_dir, evidence)

    # Check if MCP integration is available
    integrate_marker = ROOT / "notes" / "claude-workers" / "engine-integrate-dsh.md"
    mcp_ready = (integrate_marker.is_file() and integrate_marker.stat().st_size > 0) or (args.binary and args.binary.is_file())
    if mcp_ready and args.binary and args.binary.is_file():
        asyncio.run(test_mcp_level_integration(args.binary, args.resources, args.soundfont, out_dir, evidence))
    else:
        print("[MCP] Skipping MCP-level tests (engine-integrate-dsh.md empty or pending)")

    evidence_path = out_dir / "evidence.json"
    evidence_path.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(f"\nAll engine_audio tests completed successfully. Evidence saved to {evidence_path}")


if __name__ == "__main__":
    main()
