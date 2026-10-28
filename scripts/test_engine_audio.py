#!/usr/bin/env python3
"""Acceptance test suite for MuseScore 4 audio engine (roadmap step 5, part C2).

Tests both probe-level engine capabilities and MCP server integration:
1. Cross-platform golden features:
   - Compares tests/golden/windows/*/audio.json with tests/golden/linux/*/audio.json
     using explicit per-feature tolerances (frames/sample rate/channels exact)
2. Pure sine wave SoundFont generation and pitch/onset/level accuracy:
   - Renders known scores with tests/engine_audio/sine.sf2
   - Asserts note onset times are within 5 ms of tempo-derived times
   - Asserts fundamental frequencies are within 1 cent of equal temperament (A4 = 440 Hz)
   - Asserts the explicit mf dynamic (velocity 80) produces an RMS within 2% of
     the measured 0.1751
3. MIDI event goldens:
   - Captures handleEvent MIDI events (NoteOn/Off, CC, PitchBend) via --midi-events,
     including each event's render-frame offset
   - Compares events including offsets exactly against
     tests/engine_audio/midi/<fixture>.json and asserts the exact event total
4. Determinism and codec decoding across formats:
   - Repeated exports produce byte-identical files per format (wav, flac, ogg, mp3, aac)
   - Decoded durations (not the probe's own duration field) equal
     repeat-expanded play time + 3.0 s within the codec's stated tolerance
   - Exports every format to a non-ASCII output path and checks uriRoundTrip for a
     non-ASCII SoundFont copy
5. Performance and memory budget on a real dense workload:
   - Generates scripts/make_dense_score.py's deterministic 24-part, 300 s score
     (33,600 notes with chords and dynamics) in a temporary directory
   - Renders wav and ogg, measures render seconds per audio minute and peak memory
   - Budget: < 12.0 s per audio minute and < 1.0 GiB peak memory. Measurements on
     2026-10-28: Windows wav 7.6-7.8 s/min, ogg 8.7-8.9 s/min, ~462 MiB; Linux
     wav 2.1-2.2 s/min, ogg 2.5-2.6 s/min, ~468 MiB. The older "~1 s/min / 240 MiB"
     numbers came from a silent orchestral template with no chords and do not
     describe this workload.
6. Excerpt rendering parity:
   - Asserts excerpt render is sample-identical to the master render with the other
     parts muted; this is identity by construction (muting), not an independent
     excerpt rendering path
7. MCP-level mix and sync/async integration:
   - Solo/mute silence (energy ratio < -80 dB)
   - Volume gain within +/- 0.2 dB
   - Pan balance via L/R energy separation
   - Sync vs async byte identity, with async outputs written to a non-ASCII path
   - .mid export parity against the platform's golden midi.json
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
import subprocess
import sys
import tempfile
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
        ROOT / "build" / "xmake-linux" / "engine_audio_probe",
        ROOT / "build" / "xmake-linux" / "engine_audio_probe.exe",
        ROOT / "build" / "windows" / "engine_audio_probe.exe",
        ROOT / "build" / "linux" / "engine_audio_probe",
    ])
    for c in candidates:
        if c.is_file():
            return c.resolve()
    raise FileNotFoundError(f"engine_audio_probe binary not found in {bin_dir} or standard build dirs")


def probe_env() -> dict[str, str]:
    env = os.environ.copy()
    native_env = ROOT / ".pixi" / "envs" / "native"
    for extra in (native_env / "Library" / "bin", native_env / "bin"):
        if extra.is_dir():
            env["PATH"] = str(extra.resolve()) + os.pathsep + env.get("PATH", "")
    for lib in (native_env / "Library" / "lib", native_env / "lib"):
        if lib.is_dir():
            env["LD_LIBRARY_PATH"] = str(lib.resolve()) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


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

    proc = subprocess.run(
        cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=probe_env())
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


def golden_platform() -> str:
    return "windows" if sys.platform.startswith("win") else "linux"


def probe_peak_bytes(result: dict[str, Any]) -> int:
    """Peak process memory reported by the probe.

    Windows reports PeakWorkingSetSize; Linux reports peak resident set size
    (VmHWM) through the same field so budgets are comparable.
    """
    for key in ("peakWorkingSetBytes", "peakMemoryBytes", "peakVmHWMBytes"):
        value = result.get(key)
        if isinstance(value, (int, float)) and value > 0:
            return int(value)
    raise AssertionError(f"probe did not report peak memory: {result.get('peakWorkingSetBytes')}")


def numeric_values(values: list[Any]) -> list[float]:
    """Flatten scalar lists and per-channel lists (rmsEnvelope is nested)."""
    if values and isinstance(values[0], list):
        return [float(item) for pair in values for item in pair]
    return [float(item) for item in values]


def decode_audio(path: Path) -> tuple[np.ndarray, int]:
    audio, rate = sf.read(str(path), always_2d=True)
    assert audio.size > 0, f"{path} decoded to no audio"
    assert np.isfinite(audio).all(), f"{path} decoded audio contains NaN or Inf"
    assert audio.shape[1] == 2, f"{path} decoded channels != 2: {audio.shape[1]}"
    return audio, int(rate)


def decode_with_ffmpeg(source: Path, destination: Path) -> tuple[np.ndarray, int] | None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return None
    subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-i", str(source), str(destination)],
        capture_output=True,
        check=True,
    )
    return decode_audio(destination)


# --- Cross-platform golden feature tolerances --------------------------------
# Maximum absolute differences measured across the 21 fixture audio.json files
# (2026-10-28, Windows vs Linux): peak 0.019408, rms 0.001524,
# spectralCentroidHz 44.5, rmsEnvelope 0.02438, centroidEnvelope 982.5. The
# largest values all come from slurs.mscz, a bend-heavy GP import whose
# per-0.25 s centroid is sensitive at low levels; the total centroid is stable
# within 44.5 Hz. Frames/sampleRate/channels must match exactly because they
# are structural. Tolerances sit just above the measured maxima.
CROSS_PLATFORM_TOLERANCES = {
    "peak": 0.025,
    "rms": 0.002,
    "spectralCentroidHz": 50.0,
    "rmsEnvelope": 0.03,
    "centroidEnvelope": 1100.0,
}
CROSS_PLATFORM_EXACT = ("sampleRate", "channels", "frames")

# Sine SoundFont level check: explicit mf in tests/engine_audio/level_test.musicxml.
# The MusicXML importer maps mf (with <sound dynamics="80"/>) to note velocity 80
# for this instrument; the measured steady-state RMS is the reference below.
SINE_LEVEL_VELOCITY = 80
SINE_LEVEL_RMS = 0.1751
SINE_LEVEL_RMS_RELATIVE_TOLERANCE = 0.02

# Dense benchmark budget. The score has 24 parts, 150 4/4 measures, 300 s of
# music and 33,600 notes (see scripts/make_dense_score.py). Headroom is ~35%
# over the slowest measured format (ogg 8.68-8.86 s/min on Windows and 2.54-2.57
# on Linux, 2026-10-28) and ~2.2x over the measured peak working set
# (462-468 MiB).
BUDGET_SECONDS_PER_AUDIO_MINUTE = 12.0
BUDGET_PEAK_MEMORY_MIB = 1024.0
DENSE_TRACKS = 24
DENSE_PLAY_TIME_SECS = 300.0
DENSE_FORMATS = ("wav", "ogg")

# Decoded-duration tolerance (seconds) around repeat-expanded play time + 3 s,
# and the expected decoded sample rate. WAV/FLAC are sample-exact; MP3/AAC add
# codec delay and padding (+50..76 ms measured); Ogg is resampled to 48 kHz and
# decodes sample-exact to the engine span.
DECODED_RATE = {"wav": 44100, "flac": 44100, "ogg": 48000, "mp3": 44100, "aac": 44100}
DECODED_DURATION_TOLERANCE = {"wav": 0.001, "flac": 0.001, "ogg": 0.05,
                              "mp3": 0.15, "aac": 0.15}


def test_cross_platform_audio_features(evidence: dict[str, Any]) -> None:
    print("[1/6] Comparing Windows and Linux golden audio features with explicit tolerances...")
    trees = {platform: ROOT / "tests" / "golden" / platform for platform in ("windows", "linux")}
    for platform, tree in trees.items():
        assert tree.is_dir(), f"Golden tree missing for {platform}: {tree}"

    def fixtures(tree: Path) -> dict[str, Path]:
        return {path.parent.name: path for path in sorted(tree.glob("*/audio.json"))}

    windows = fixtures(trees["windows"])
    linux = fixtures(trees["linux"])
    assert windows, "tests/golden/windows contains no audio.json files"
    assert linux, "tests/golden/linux contains no audio.json files"
    assert set(windows) == set(linux), (
        f"Golden fixture sets differ: windows-only {sorted(set(windows) - set(linux))}, "
        f"linux-only {sorted(set(linux) - set(windows))}"
    )

    maxima = {key: 0.0 for key in CROSS_PLATFORM_TOLERANCES}
    worst = {key: "" for key in CROSS_PLATFORM_TOLERANCES}
    for name in sorted(windows):
        left = json.loads(windows[name].read_text(encoding="utf-8"))
        right = json.loads(linux[name].read_text(encoding="utf-8"))
        for key in CROSS_PLATFORM_EXACT:
            assert left[key] == right[key], (
                f"{name}: {key} differs ({left[key]} vs {right[key]})")
        assert len(left["rmsEnvelope"]) == len(right["rmsEnvelope"]), (
            f"{name}: rmsEnvelope lengths differ")
        assert len(left["centroidEnvelope"]) == len(right["centroidEnvelope"]), (
            f"{name}: centroidEnvelope lengths differ")
        for key, tolerance in CROSS_PLATFORM_TOLERANCES.items():
            left_values = left[key]
            right_values = right[key]
            if isinstance(left_values, list):
                delta = max(
                    (abs(a - b) for a, b in zip(numeric_values(left_values),
                                                numeric_values(right_values))),
                    default=0.0,
                )
            else:
                delta = abs(left_values - right_values)
            if delta > maxima[key]:
                maxima[key] = delta
                worst[key] = name
            assert delta <= tolerance, (
                f"{name}: {key} differs by {delta:.6g} (tolerance {tolerance})")
        assert left["channels"] == 2 and left["sampleRate"] in (44100, 48000), name

    evidence["cross_platform_audio"] = {
        "status": "passed",
        "fixtures": len(windows),
        "tolerances": CROSS_PLATFORM_TOLERANCES,
        "max_abs_diff": {key: round(value, 6) for key, value in maxima.items()},
        "worst_fixture": worst,
    }
    summary = ", ".join(f"{key} {value:.6g}" for key, value in maxima.items())
    print(f"  {len(windows)} fixtures agree across Windows/Linux (max deltas: {summary})")


def test_pitch_onset_and_level(
    probe_bin: Path,
    assets: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[2/6] Testing pitch, onset and sine SoundFont output level...")
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

    audio, sr = decode_audio(out_wav)
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
            f"Pitch {midi_pitch} onset error {diff_ms:.2f} ms exceeds 5 ms tolerance "
            f"(detected {detected_t:.4f}s, expected {expected_t:.4f}s)"
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

    # Output level: one explicit mf note at C4. The captured events must show
    # the documented dynamic mapping, then the steady-state RMS must match.
    level_score = ROOT / "tests" / "engine_audio" / "level_test.musicxml"
    assert level_score.is_file(), f"Missing level test score: {level_score}"
    level_wav = out_dir / "level_test.wav"
    if level_wav.exists():
        level_wav.unlink()
    level_result = run_probe(probe_bin, assets, sine_sf2, level_score, level_wav, ["--midi-events"])
    assert level_result.get("ok") is True, f"Level probe render failed: {level_result}"

    level_events = level_result.get("midiEvents", [])
    note_ons = [event for event in level_events if event.get("opcode") == "Opcode::NoteOn"]
    assert len(note_ons) == 1, f"Expected exactly one NoteOn in level_test, got {note_ons}"
    assert note_ons[0].get("note") == 60, note_ons[0]
    assert note_ons[0].get("velocity") == SINE_LEVEL_VELOCITY, (
        f"mf should map to velocity {SINE_LEVEL_VELOCITY}, got {note_ons[0]}")

    level_audio, level_sr = decode_audio(level_wav)
    level_window = level_audio[int(0.10 * level_sr):int(0.40 * level_sr)]
    level_rms = float(np.sqrt(np.mean(level_window ** 2)))
    level_relative = abs(level_rms - SINE_LEVEL_RMS) / SINE_LEVEL_RMS
    assert level_relative <= SINE_LEVEL_RMS_RELATIVE_TOLERANCE, (
        f"Sine mf RMS {level_rms:.5f} differs from measured {SINE_LEVEL_RMS:.5f} by "
        f"{level_relative * 100:.2f}% (tolerance {SINE_LEVEL_RMS_RELATIVE_TOLERANCE * 100:.1f}%)"
    )

    evidence["pitch_and_onset"] = {
        "status": "passed",
        "onsets": onset_results,
        "pitches": pitch_results,
    }
    evidence["sine_level"] = {
        "status": "passed",
        "dynamic": "mf",
        "velocity": SINE_LEVEL_VELOCITY,
        "measured_rms": round(level_rms, 5),
        "expected_rms": SINE_LEVEL_RMS,
        "relative_tolerance": SINE_LEVEL_RMS_RELATIVE_TOLERANCE,
    }
    print(f"  Pitch and onset tests PASSED: max onset diff = "
          f"{max(r['diff_ms'] for r in onset_results):.2f} ms (< 5 ms), max pitch diff = "
          f"{max(r['cents_diff'] for r in pitch_results):.3f} cents (< 1 cent)")
    print(f"  Sine level PASSED: mf RMS {level_rms:.5f} vs {SINE_LEVEL_RMS:.5f} "
          f"({level_relative * 100:.2f}% <= {SINE_LEVEL_RMS_RELATIVE_TOLERANCE * 100:.1f}%)")


def midi_fixtures() -> list[tuple[str, Path]]:
    return [
        ("evening", ROOT / "fixtures" / "evening.musicxml"),
        ("hairpins_and_repeats", ROOT / "fixtures" / "golden" / "hairpins_and_repeats.mscx"),
        ("pedal", ROOT / "fixtures" / "golden" / "pedal.mscx"),
        ("bend.gp", ROOT / "fixtures" / "golden" / "bend.gp"),
        ("repeat70", ROOT / "fixtures" / "golden" / "repeat70.mscx"),
    ]


def midi_golden_file(name: str, golden_dir: Path) -> Path:
    platform_file = golden_dir / golden_platform() / f"{name}.json"
    if platform_file.is_file():
        return platform_file
    return golden_dir / f"{name}.json"


def write_midi_goldens(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    golden_dir: Path,
    platform_subdir: bool,
) -> None:
    """Regenerate the MIDI event goldens from the current probe binary."""
    target = golden_dir / golden_platform() if platform_subdir else golden_dir
    target.mkdir(parents=True, exist_ok=True)
    for name, fixture in midi_fixtures():
        assert fixture.is_file(), f"Fixture missing: {fixture}"
        tmp_wav = out_dir / f"midi_{name}.wav"
        result = run_probe(probe_bin, assets, soundfont, fixture, tmp_wav, ["--midi-events"])
        assert result.get("ok") is True, f"Probe failed for {name}: {result}"
        if tmp_wav.exists():
            tmp_wav.unlink()
        events = result.get("midiEvents", [])
        total = result.get("midiEventTotal")
        assert isinstance(total, int) and total == len(events), (name, total, len(events))
        assert result.get("midiEventsTruncated") is False, f"Probe truncated {name} events"
        payload = {
            "fixture": name,
            "platform": golden_platform() if platform_subdir else "shared",
            "eventTotal": total,
            "events": events,
        }
        path = target / f"{name}.json"
        path.write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8", newline="\n")
        try:
            display = path.resolve().relative_to(ROOT)
        except ValueError:
            display = path
        print(f"  wrote {display}: {total} events")


def test_midi_event_goldens(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[3/6] Testing MIDI event goldens (events plus render-frame offsets and totals)...")
    fixtures = midi_fixtures()
    golden_dir = ROOT / "tests" / "engine_audio" / "midi"
    golden_results = {}

    for name, fix_path in fixtures:
        assert fix_path.is_file(), f"Fixture missing: {fix_path}"
        golden_file = midi_golden_file(name, golden_dir)
        assert golden_file.is_file(), f"MIDI golden file missing: {golden_file}"

        tmp_wav = out_dir / f"midi_{name}.wav"
        if tmp_wav.exists():
            tmp_wav.unlink()

        res = run_probe(probe_bin, assets, soundfont, fix_path, tmp_wav, ["--midi-events"])
        if tmp_wav.exists():
            tmp_wav.unlink()

        events = res.get("midiEvents", [])
        total = res.get("midiEventTotal")
        golden = json.loads(golden_file.read_text(encoding="utf-8"))
        assert isinstance(golden, dict) and "events" in golden and "eventTotal" in golden, (
            f"{golden_file} is not a regenerated MIDI golden with an exact total; "
            "run --write-midi-goldens against the current probe"
        )
        expected_events = golden["events"]
        expected_total = golden["eventTotal"]

        assert isinstance(total, int), f"Probe did not report midiEventTotal for {name}: {res}"
        assert total == expected_total, (
            f"MIDI event total mismatch for {name}: got {total}, expected {expected_total}"
        )
        assert total > 0, f"MIDI golden for {name} has no events"
        assert res.get("midiEventsTruncated") is False, f"Probe truncated {name} events"
        assert total == len(events), (
            f"MIDI event count mismatch for {name}: total {total}, printed {len(events)}")
        assert len(expected_events) == expected_total, (
            f"Golden for {name} stores {len(expected_events)} events but total {expected_total}")
        assert len(events) == len(expected_events), (
            f"MIDI event count mismatch for {name}: got {len(events)}, "
            f"expected {len(expected_events)}"
        )
        assert events == expected_events, (
            f"MIDI events (including render-frame offsets) mismatch for {name}")
        golden_results[name] = {
            "event_total": total,
            "printed_events": len(events),
            "matches_golden": True,
            "golden": str(golden_file.relative_to(ROOT)),
            "platform": golden.get("platform", "shared"),
        }

    evidence["midi_goldens"] = {
        "status": "passed",
        "fixtures": golden_results,
    }
    print(f"  MIDI event goldens PASSED across {len(fixtures)} fixtures "
          f"({sum(item['event_total'] for item in golden_results.values())} total events)")


def test_determinism_and_decoding(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[4/6] Testing determinism, decoded durations and non-ASCII paths "
          "across wav, flac, ogg, mp3, aac...")
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
        assert res1.get("frames") == res2.get("frames"), f"Format {fmt} frame counts differ"

        expected_dur = res1.get("playTime", 0.0) + 3.0
        dec_info: dict[str, Any] = {
            "byte_identical": True,
            "size": len(data1),
            "sha256": hashlib.sha256(data1).hexdigest(),
            "expected_duration": round(expected_dur, 6),
        }

        if fmt == "aac":
            dec_wav = out_dir / "aac_decoded.wav"
            decoded = decode_with_ffmpeg(out1, dec_wav)
            if dec_wav.exists():
                dec_wav.unlink()
            if decoded is None:
                # ADTS syncword check: 12 bits 0xFFF (0xFF followed by 0xF0..0xFF)
                assert len(data1) > 7, "AAC file too small for ADTS header"
                assert data1[0] == 0xFF and (data1[1] & 0xF0) == 0xF0, "Invalid ADTS header in AAC export"
                dec_info["decoder"] = "adts_header_validated"
                det_results[fmt] = dec_info
                continue
            audio, sr = decoded
            dec_info["decoder"] = "ffmpeg"
        else:
            audio, sr = decode_audio(out1)

        dec_info["decoded_sr"] = sr
        assert sr == DECODED_RATE[fmt], f"Format {fmt} decoded sample rate {sr} != {DECODED_RATE[fmt]}"
        decoded_dur = len(audio) / sr
        tolerance = DECODED_DURATION_TOLERANCE[fmt]
        assert abs(decoded_dur - expected_dur) <= tolerance, (
            f"Decoded {fmt} duration {decoded_dur:.6f} != play time {res1.get('playTime')} + 3 s "
            f"(tolerance {tolerance} s)"
        )
        if fmt in ("wav", "flac"):
            assert len(audio) == res1.get("frames"), (
                f"Decoded {fmt} frames {len(audio)} != probe frames {res1.get('frames')}")
        dec_info["decoded_frames"] = len(audio)
        dec_info["decoded_duration"] = round(decoded_dur, 6)
        dec_info["duration_tolerance"] = tolerance
        det_results[fmt] = dec_info

    # Non-ASCII SoundFont copy and output paths for every audio format.
    sine_sf2 = ROOT / "tests" / "engine_audio" / "sine.sf2"
    if not sine_sf2.is_file():
        generate_sine_soundfont(sine_sf2)
    nonascii_sf = out_dir / "非ASCII 音色" / "正弦 音色.sf2"
    nonascii_sf.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(sine_sf2, nonascii_sf)
    pitch_score = ROOT / "tests" / "engine_audio" / "pitch_test.musicxml"
    nonascii_dir = out_dir / "输出 音频"
    nonascii_dir.mkdir(parents=True, exist_ok=True)
    nonascii_results = {}
    for fmt in formats:
        out_path = nonascii_dir / f"输出 音频.{fmt}"
        if out_path.exists():
            out_path.unlink()
        res = run_probe(probe_bin, assets, nonascii_sf, pitch_score, out_path)
        assert res.get("ok") is True, f"Non-ASCII {fmt} export failed: {res}"
        assert res.get("soundfont", {}).get("uriRoundTrip") is True, (
            f"Non-ASCII SoundFont URI did not round-trip for {fmt}: {res.get('soundfont')}")
        assert out_path.is_file() and out_path.stat().st_size > 0, f"Missing non-ASCII {fmt} output"
        expected_dur = res.get("playTime", 0.0) + 3.0
        if fmt == "aac":
            dec_wav = out_dir / f"nonascii_aac_decoded.wav"
            decoded = decode_with_ffmpeg(out_path, dec_wav)
            if dec_wav.exists():
                dec_wav.unlink()
            if decoded is None:
                data = out_path.read_bytes()
                assert len(data) > 7 and data[0] == 0xFF and (data[1] & 0xF0) == 0xF0
                nonascii_results[fmt] = {"bytes": len(data), "decoder": "adts_header_validated"}
                continue
            audio, sr = decoded
        else:
            audio, sr = decode_audio(out_path)
        decoded_dur = len(audio) / sr
        assert abs(decoded_dur - expected_dur) <= DECODED_DURATION_TOLERANCE[fmt], (
            f"Non-ASCII {fmt} decoded duration {decoded_dur:.6f} != {expected_dur:.6f}")
        nonascii_results[fmt] = {
            "bytes": out_path.stat().st_size,
            "decoded_sr": sr,
            "decoded_duration": round(decoded_dur, 6),
            "soundfont_uri_round_trip": True,
        }

    evidence["determinism_and_codecs"] = {
        "status": "passed",
        "formats": det_results,
    }
    evidence["non_ascii_paths"] = {
        "status": "passed",
        "soundfont": str(nonascii_sf.relative_to(out_dir)),
        "soundfont_uri_round_trip": True,
        "formats": nonascii_results,
    }
    print(f"  Determinism and decoded-duration checks PASSED for {', '.join(formats)}")
    print(f"  Non-ASCII SoundFont and output paths PASSED for {', '.join(formats)}")


def test_performance_and_memory(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[5/6] Testing performance and memory on the generated dense orchestral score...")
    from make_dense_score import generate_dense_score

    budget_bytes = int(BUDGET_PEAK_MEMORY_MIB * 1024 * 1024)
    measurements: dict[str, Any] = {}
    with tempfile.TemporaryDirectory(prefix="musescore-dense-score-") as temp_dir:
        temp = Path(temp_dir)
        score_info = generate_dense_score(temp / "dense_orchestral.musicxml")
        score_path = Path(score_info["path"])
        assert score_info["parts"] == DENSE_TRACKS, score_info
        assert score_info["play_time_secs"] == DENSE_PLAY_TIME_SECS, score_info

        for fmt in DENSE_FORMATS:
            out_path = temp / f"dense.{fmt}"
            if out_path.exists():
                out_path.unlink()
            res = run_probe(probe_bin, assets, soundfont, score_path, out_path)
            assert res.get("ok") is True, f"Dense {fmt} render failed: {res}"
            assert res.get("trackCount") == DENSE_TRACKS, (
                f"Dense score should expose {DENSE_TRACKS} tracks, got {res.get('trackCount')}")
            play_time = float(res.get("playTime", 0.0))
            assert abs(play_time - DENSE_PLAY_TIME_SECS) <= 0.01, (
                f"Dense score play time {play_time} != {DENSE_PLAY_TIME_SECS}")
            export_secs = float(res.get("exportSeconds", 0.0))
            sec_per_audio_min = export_secs / (play_time / 60.0)
            peak_bytes = probe_peak_bytes(res)
            peak_mib = peak_bytes / (1024.0 * 1024.0)
            output_bytes = out_path.stat().st_size
            assert output_bytes > 0, f"Dense {fmt} output is empty"
            assert output_bytes < 1024 * 1024 * 1024, (
                f"Dense {fmt} output {output_bytes} exceeds the 1 GiB audio cap")
            assert sec_per_audio_min < BUDGET_SECONDS_PER_AUDIO_MINUTE, (
                f"Dense {fmt} export {sec_per_audio_min:.2f} s/audio-min exceeds budget "
                f"{BUDGET_SECONDS_PER_AUDIO_MINUTE}")
            assert peak_bytes < budget_bytes, (
                f"Dense {fmt} peak memory {peak_mib:.1f} MiB exceeds budget "
                f"{BUDGET_PEAK_MEMORY_MIB} MiB")
            measurements[fmt] = {
                "duration_seconds": round(float(res.get("duration", 0.0)), 3),
                "play_time_seconds": round(play_time, 3),
                "export_seconds": round(export_secs, 3),
                "seconds_per_audio_minute": round(sec_per_audio_min, 3),
                "peak_memory_mib": round(peak_mib, 1),
                "output_bytes": output_bytes,
                "track_count": res.get("trackCount"),
            }
            if out_path.exists():
                out_path.unlink()

        perf_info = {
            "score": "generated by scripts/make_dense_score.py",
            "score_bytes": score_info["bytes"],
            "parts": score_info["parts"],
            "measures": score_info["measures"],
            "notes": score_info["notes"],
            "play_time_seconds": score_info["play_time_secs"],
            "formats": measurements,
            "budget_seconds_per_audio_minute": BUDGET_SECONDS_PER_AUDIO_MINUTE,
            "budget_peak_memory_mib": BUDGET_PEAK_MEMORY_MIB,
        }
        evidence["performance_budget"] = {
            "status": "passed",
            "measurements": perf_info,
        }

    slowest = max(item["seconds_per_audio_minute"] for item in measurements.values())
    highest = max(item["peak_memory_mib"] for item in measurements.values())
    detail = ", ".join(f"{fmt} {item['seconds_per_audio_minute']:.2f} s/min"
                       for fmt, item in measurements.items())
    print(f"  Performance budget PASSED: {detail} (budget "
          f"{BUDGET_SECONDS_PER_AUDIO_MINUTE} s/min), {highest:.1f} MiB "
          f"(budget {BUDGET_PEAK_MEMORY_MIB} MiB); slowest {slowest:.2f} s/min")


def test_excerpt_rendering_parity(
    probe_bin: Path,
    assets: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    print("[6/6] Testing excerpt render identity against master with other parts muted...")
    score_path = ROOT / "fixtures" / "evening.musicxml"
    out_exc = out_dir / "evening_excerpt0.wav"
    out_muted = out_dir / "evening_master_muted12.wav"

    for out in (out_exc, out_muted):
        if out.exists():
            out.unlink()

    # Excerpt 0 in evening.musicxml corresponds to part 0; parts 1 and 2 are absent/invisible.
    # The engine implements excerpt rendering by muting those parts on the master score
    # (masternotation.cpp:748-770), so this assertion proves the muting path is applied
    # consistently; it is not an independent excerpt renderer.
    run_probe(probe_bin, assets, soundfont, score_path, out_exc, ["--excerpt", "0"])
    run_probe(probe_bin, assets, soundfont, score_path, out_muted,
              ["--mute-part", "1", "--mute-part", "2"])

    audio_exc, sr_exc = decode_audio(out_exc)
    audio_muted, sr_muted = decode_audio(out_muted)

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
        "identity_by_construction": True,
    }
    print(f"  Excerpt render sample identity PASSED: max diff = {max_diff} "
          "(identity by construction: excerpt = master with other parts muted)")


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
        audio_ref, sr = decode_audio(ref_wav)
        rms_ref = float(np.sqrt(np.mean(audio_ref ** 2)))
        assert rms_ref > 0.01, f"Reference audio RMS too low: {rms_ref}"

        # 2. Solo / mute silence (energy ratio < -80 dB)
        all_muted = [{"trackId": tid, "muted": True} for tid in track_ids]
        muted_wav = out_dir / "mcp_all_muted.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(muted_wav.resolve()), "mix": {"tracks": all_muted}},
        )
        audio_muted, _ = decode_audio(muted_wav)
        rms_muted = float(np.sqrt(np.mean(audio_muted ** 2)))
        energy_ratio_db = 20.0 * math.log10(max(rms_muted, 1e-12) / rms_ref)
        assert energy_ratio_db < -80.0, f"Muted energy ratio {energy_ratio_db:.1f} dB not < -80 dB"

        # 3. VolumeDb accuracy (+/- 0.2 dB)
        vol_neg6_wav = out_dir / "mcp_vol_neg6.wav"
        await client.tool(
            "score_export",
            {"documentId": doc_id, "format": "wav", "path": str(vol_neg6_wav.resolve()), "mix": {"master": {"volumeDb": -6.0}}},
        )
        audio_neg6, _ = decode_audio(vol_neg6_wav)
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
        audio_left, _ = decode_audio(left_wav)
        audio_right, _ = decode_audio(right_wav)
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
        audio_exc, _ = decode_audio(exc_wav)
        audio_muted_parts, _ = decode_audio(mute_parts_wav)
        assert np.max(np.abs(audio_exc - audio_muted_parts)) == 0.0, (
            "MCP excerpt export not sample-identical to muted master export")

        # 6. Sync vs async byte identity across formats; async outputs use a
        # non-ASCII directory and file name.
        nonascii_dir = out_dir / "输出 音频"
        nonascii_dir.mkdir(parents=True, exist_ok=True)
        nonascii_async = {}
        for fmt in ("wav", "flac", "ogg", "mp3", "aac"):
            sync_path = out_dir / f"mcp_sync.{fmt}"
            async_path = nonascii_dir / f"异步 音频.{fmt}"
            for p in (sync_path, async_path):
                if p.exists():
                    p.unlink()
            await client.tool("score_export", {"documentId": doc_id, "format": fmt, "path": str(sync_path.resolve())})
            job = await client.tool("score_export_start", {"documentId": doc_id, "format": fmt, "path": str(async_path.resolve())})
            done = await client.terminal(job)
            assert done["state"] == "completed", f"Async export {fmt} failed: {done}"
            assert sync_path.read_bytes() == async_path.read_bytes(), f"Sync and async outputs differ for {fmt}"
            nonascii_async[fmt] = str(async_path.relative_to(out_dir))

        # 7. MIDI export unchanged vs the platform's tests/golden tree
        mcp_midi_path = out_dir / "mcp_evening.mid"
        await client.tool("score_export", {"documentId": doc_id, "format": "midi", "path": str(mcp_midi_path.resolve())})
        golden_midi_json = ROOT / "tests" / "golden" / golden_platform() / "evening.musicxml" / "midi.json"
        assert golden_midi_json.is_file(), f"Golden MIDI description missing: {golden_midi_json}"
        exported_midi_desc = describe_midi(mcp_midi_path.read_bytes())
        expected_midi_desc = json.loads(golden_midi_json.read_text(encoding="utf-8"))
        assert exported_midi_desc == expected_midi_desc, "MCP .mid export changed compared to tests/golden!"

        await client.tool("score_close", {"documentId": doc_id})

    mcp_evidence["status"] = "passed"
    mcp_evidence["energy_ratio_db"] = round(energy_ratio_db, 2)
    mcp_evidence["measured_volume_gain_db"] = round(measured_gain, 3)
    mcp_evidence["non_ascii_async_exports"] = nonascii_async
    mcp_evidence["midi_golden"] = str(golden_midi_json.relative_to(ROOT))
    evidence["mcp_integration"] = mcp_evidence
    print("  MCP-level integration tests PASSED")


def main() -> None:
    parser = argparse.ArgumentParser(description="Acceptance test suite for MuseScore 4 audio engine")
    parser.add_argument("--bin-dir", type=Path, default=None, help="Directory containing build executables")
    parser.add_argument("--binary", type=Path, default=None, help="Path to musescore-mcp executable")
    parser.add_argument("--resources", type=Path, default=ROOT / ".runtime" / "assets", help="Path to assets")
    parser.add_argument("--soundfont", type=Path, default=ROOT / ".runtime" / "soundfonts" / "FluidR3Mono_GM.sf3", help="Path to soundfont")
    parser.add_argument("--output", type=Path, required=True, help="Output directory for test artifacts")
    parser.add_argument("--write-midi-goldens", action="store_true",
                        help="Regenerate tests/engine_audio/midi goldens from the current probe")
    parser.add_argument("--midi-golden-dir", type=Path, default=ROOT / "tests" / "engine_audio" / "midi",
                        help="Directory for --write-midi-goldens")
    parser.add_argument("--platform-subdir", action="store_true",
                        help="Write MIDI goldens under <dir>/<platform>/ when hosts differ")
    args = parser.parse_args()

    out_dir = args.output.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    probe_bin = find_probe_binary(args.bin_dir)
    print(f"Using probe binary: {probe_bin}")

    if args.write_midi_goldens:
        write_midi_goldens(probe_bin, args.resources, args.soundfont, out_dir,
                           args.midi_golden_dir, args.platform_subdir)
        return

    evidence: dict[str, Any] = {"status": "passed"}

    test_cross_platform_audio_features(evidence)
    test_pitch_onset_and_level(probe_bin, args.resources, out_dir, evidence)
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
