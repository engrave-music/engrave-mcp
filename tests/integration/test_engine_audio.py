"""Exercise MCP audio exports and capture platform comparisons."""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import math
import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import numpy as np
from engrave_mcp_testing import golden as golden
from engrave_mcp_testing.golden import describe_midi
from engrave_mcp_testing.mcp_client import Client
from engrave_mcp_testing.paths import REPO_ROOT
from engrave_testing import engine_audio as audio

COMPOSITION_FIXTURE = REPO_ROOT / "vendor/engrave/tests/engrave_testing/composition_fixture.py"


_capture = audio.CapturePolicy(golden, Path(__file__), REPO_ROOT / "tests/engrave_mcp_testing/golden.py")
audio_fixture_sources = _capture.audio_fixture_sources
write_capture_manifest = _capture.write_capture_manifest
capture_destination = _capture.capture_destination
test_cross_platform_audio_features = _capture.test_cross_platform_audio_features
test_audio_check_reporting = _capture.test_audio_check_reporting


def find_probe_binary(bin_dir: Path | None) -> Path:
    binary_name = "engine_audio_probe.exe" if os.name == "nt" else "engine_audio_probe"
    candidates = []
    if bin_dir:
        candidates.append(bin_dir / binary_name)
    candidates.append(REPO_ROOT / "build" / "release" / binary_name)
    for c in candidates:
        if c.is_file():
            return c.resolve()
    raise FileNotFoundError(f"engine_audio_probe binary not found in {bin_dir} or standard build dirs")


def git_revision(repo: Path) -> str:
    return subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()


_SLOW_AUDIO_FIRST = (
    "chordline_path.mscx",
    "graceArps.mnx",
    "evening.musicxml",
    "slurs.mscz",
    "layout_elements.mscx",
    "pedal.mscx",
)


def _reusable_golden_audio(
    reuse_golden_actual: Path | None,
    fixtures: dict[str, Path],
    binary: Path,
    resources: Path,
    soundfont: Path,
) -> dict[str, Path] | None:
    if reuse_golden_actual is None:
        return None
    index_path = reuse_golden_actual / "index.json"
    provenance_path = reuse_golden_actual.parent / "provenance.json"
    if not index_path.is_file() or not provenance_path.is_file():
        return None
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    expected_fixtures = {name: audio.file_sha256(path) for name, path in fixtures.items()}
    if (
        not isinstance(provenance, dict)
        or provenance.get("schema") != 1
        or provenance.get("binary_sha256") != audio.file_sha256(binary)
        or provenance.get("soundfont_sha256") != audio.file_sha256(soundfont)
        or provenance.get("resources") != str(resources.resolve())
        or provenance.get("extractor_sha256") != audio.file_sha256(REPO_ROOT / "tests/engrave_mcp_testing/golden.py")
        or provenance.get("fixture_sha256") != expected_fixtures
    ):
        return None
    index = json.loads(index_path.read_text(encoding="utf-8"))
    if not isinstance(index, dict):
        return None
    matched: dict[str, Path] = {}
    for name in fixtures:
        entry = index.get(name)
        audio_path = reuse_golden_actual / name / "audio.json"
        if not isinstance(entry, dict) or not audio_path.is_file():
            return None
        if audio.file_sha256(audio_path) != entry.get("audio.json"):
            return None
        matched[name] = audio_path
    return matched


async def capture_current_audio(
    binary: Path,
    resources: Path,
    soundfont: Path,
    target: Path,
    engine_commit: str,
    reuse_golden_actual: Path | None = None,
) -> None:
    """Render the golden fixture inventory and preserve the server's version stamp."""

    composition_musicxml = golden.composition_musicxml

    fixtures = audio_fixture_sources()
    with tempfile.TemporaryDirectory(prefix="musescore-audio-capture-") as temporary:
        raw = Path(temporary)
        params = golden.StdioServerParameters(
            command=str(binary.resolve()),
            args=[
                "--resources",
                str(resources.resolve()),
                "--soundfont",
                str(soundfont.resolve()),
            ],
            cwd=str(raw),
            env=audio.probe_env(),
        )
        proof_log = raw / "proof-stderr.log"
        with proof_log.open("w", encoding="utf-8") as log:
            async with golden.stdio_client(params, errlog=log) as (read, write):
                async with golden.ClientSession(read, write) as session:
                    await session.initialize()
                    created, error = await golden.call_tool(
                        session,
                        "score_create",
                        {
                            "template_id": "Treble_Clef",
                            "title": "Audio capture provenance",
                        },
                    )
                    assert not error, f"create version proof: {error}"
                    proof_document = created["document"]
                    proof = target / "version.mscx"
                    _, error = await golden.call_tool(
                        session,
                        "score_export",
                        {
                            "document": proof_document,
                            "kind": "score",
                            "options": {"format": "mscx"},
                            "destination": {"kind": "file", "path": str(proof)},
                        },
                    )
                    assert not error, f"export version proof: {error}"
                    await golden.call_tool(session, "score_close", {"document": proof_document})
                    _capture.validate_version_proof(proof, audio.golden_platform(), engine_commit)

        reusable = _reusable_golden_audio(reuse_golden_actual, fixtures, binary, resources, soundfont)
        if reusable is not None:
            for name, src in reusable.items():
                dst = target / name / "audio.json"
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
            shutil.copyfile(proof_log, target / "server-stderr.log")
            return

        priority = {name: idx for idx, name in enumerate(_SLOW_AUDIO_FIRST)}
        ordered_fixtures = sorted(
            fixtures.items(),
            key=lambda item: (priority.get(item[0], len(priority)), -item[1].stat().st_size, item[0]),
        )
        queue: asyncio.Queue[tuple[str, Path]] = asyncio.Queue()
        for item in ordered_fixtures:
            queue.put_nowait(item)
        worker_count = min(4, max(1, (os.cpu_count() or 2) // 2), len(ordered_fixtures))
        worker_logs = [raw / f"worker-{idx}-stderr.log" for idx in range(worker_count)]

        async def run_worker(worker_id: int) -> None:
            wav = raw / f"capture-{worker_id}.wav"
            with worker_logs[worker_id].open("w", encoding="utf-8") as log:
                async with golden.stdio_client(params, errlog=log) as (read, write):
                    async with golden.ClientSession(read, write) as session:
                        await session.initialize()
                        while True:
                            try:
                                name, source = queue.get_nowait()
                            except asyncio.QueueEmpty:
                                break
                            try:
                                if name == "created.json":
                                    xml = composition_musicxml(json.loads(source.read_text(encoding="utf-8")))
                                    payload, error = await golden.call_tool(
                                        session,
                                        "score_open",
                                        {
                                            "data": base64.b64encode(xml.encode()).decode(),
                                            "format": "musicxml",
                                        },
                                    )
                                else:
                                    payload, error = await golden.call_tool(
                                        session, "score_open", {"path": str(source.resolve())}
                                    )
                                assert not error, f"{name}: open failed: {error}"
                                document = payload["document"]
                                try:
                                    _, error = await golden.call_tool(
                                        session,
                                        "score_export",
                                        {
                                            "document": document,
                                            "kind": "audio",
                                            "options": {"format": "wav"},
                                            "destination": {"kind": "file", "path": str(wav)},
                                        },
                                    )
                                    assert not error, f"{name}: audio export failed: {error}"
                                    golden.write_json(target / name / "audio.json", golden.describe_audio(wav))
                                    wav.unlink()
                                finally:
                                    await golden.call_tool(session, "score_close", {"document": document})
                            finally:
                                queue.task_done()

        await asyncio.gather(*(run_worker(idx) for idx in range(worker_count)))
        with (target / "server-stderr.log").open("w", encoding="utf-8") as combined:
            for log_file in [proof_log, *worker_logs]:
                if log_file.is_file():
                    combined.write(log_file.read_text(encoding="utf-8", errors="replace"))


def make_current_audio_capture(
    binary: Path,
    resources: Path,
    soundfont: Path,
    engine_source: Path,
    capture_root: Path,
    reuse_golden_actual: Path | None = None,
) -> tuple[str, str]:
    platform = audio.golden_platform()
    destination = capture_destination(capture_root, platform)
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=True,
    ).stdout
    assert not status.strip(), "capture requires a clean superproject checkout"
    source_commit = git_revision(REPO_ROOT)
    engine_commit = git_revision(engine_source)
    binary_sha, soundfont_sha = audio.file_sha256(binary), audio.file_sha256(soundfont)
    fixtures = audio_fixture_sources()
    fixture_hashes = {name: audio.file_sha256(path) for name, path in fixtures.items()}
    extractor_sha = audio.file_sha256(REPO_ROOT / "tests/engrave_mcp_testing/golden.py")
    script_sha = audio.file_sha256(Path(__file__))
    fixture_helper_sha = audio.file_sha256(COMPOSITION_FIXTURE)
    capture_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{platform}-capture-", dir=capture_root) as temporary:
        target = Path(temporary)
        asyncio.run(
            asyncio.wait_for(
                capture_current_audio(
                    binary,
                    resources,
                    soundfont,
                    target,
                    engine_commit,
                    reuse_golden_actual=reuse_golden_actual,
                ),
                timeout=1800,
            )
        )
        assert git_revision(REPO_ROOT) == source_commit, "superproject commit changed during capture"
        assert git_revision(engine_source) == engine_commit, "engine commit changed during capture"
        assert audio.file_sha256(binary) == binary_sha, "binary changed during capture"
        assert audio.file_sha256(soundfont) == soundfont_sha, "soundfont changed during capture"
        assert {name: audio.file_sha256(path) for name, path in audio_fixture_sources().items()} == fixture_hashes, (
            "fixture inventory or inputs changed during capture"
        )
        assert audio.file_sha256(REPO_ROOT / "tests/engrave_mcp_testing/golden.py") == extractor_sha, (
            "extractor changed during capture"
        )
        assert audio.file_sha256(Path(__file__)) == script_sha, "capture script changed during capture"
        assert audio.file_sha256(COMPOSITION_FIXTURE) == fixture_helper_sha, "fixture helper changed during capture"
        write_capture_manifest(
            target,
            platform,
            source_commit,
            engine_commit,
            binary_sha,
            soundfont_sha,
            fixtures,
        )
        destination = capture_destination(capture_root, platform)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(target, destination)
    print(
        f"  Captured {len(fixtures)} fixtures to {destination} "
        f"(host {socket.gethostname()}, commit {source_commit}, binary sha256 {binary_sha})"
    )
    return source_commit, engine_commit


async def test_mcp_level_integration(
    binary: Path,
    resources: Path,
    soundfont: Path,
    out_dir: Path,
    evidence: dict[str, Any],
) -> None:
    """Exercise mix, excerpt, codec and MIDI output against existing evidence."""

    print("[MCP] Running MCP-level audio integration tests...")
    args = SimpleNamespace(binary=binary, resources=resources, soundfont=soundfont)
    async with Client(args, out_dir) as client:

        async def export(doc, fmt, path, options=None, score=None):
            kind = "score" if fmt == "midi" else "audio"
            values = {"format": fmt, **(options or {})}
            if score is not None:
                values["score"] = {"excerpt": score}
            await client.tool(
                "score_export",
                {
                    "document": doc,
                    "kind": kind,
                    "options": values,
                    "destination": {"kind": "file", "path": str(path.resolve())},
                },
            )
            assert path.is_file() and path.stat().st_size > 0
            return path

        doc = (
            await client.tool(
                "score_open",
                {"path": str((REPO_ROOT / "vendor/engrave/tests/data/inputs/evening.musicxml").resolve())},
            )
        )["document"]
        info = await client.tool("score_query", {"document": doc, "mode": "audio_tracks"})
        tracks = info["audio_tracks"]["tracks"]
        assert len(tracks) >= 3 and len({t["track"] for t in tracks}) == len(tracks)
        ref_wav = await export(doc, "wav", out_dir / "mcp_ref.wav")
        audio_ref, _ = audio.decode_audio(ref_wav)
        rms_ref = float(np.sqrt(np.mean(audio_ref**2)))
        assert rms_ref > 0.01
        muted = await export(
            doc,
            "wav",
            out_dir / "mcp_all_muted.wav",
            {"tracks": [{"track": t["track"], "mute": True} for t in tracks]},
        )
        audio_muted, _ = audio.decode_audio(muted)
        rms_muted = float(np.sqrt(np.mean(audio_muted**2)))
        energy_ratio_db = 20 * math.log10(max(rms_muted, 1e-12) / rms_ref)
        assert energy_ratio_db < -80.0
        low = await export(doc, "wav", out_dir / "mcp_vol_neg6.wav", {"master_volume_db": -6.0})
        audio_low, _ = audio.decode_audio(low)
        measured_gain = 20 * math.log10(float(np.sqrt(np.mean(audio_low**2))) / rms_ref)
        assert abs(measured_gain + 6.0) <= 0.2
        left = await export(doc, "wav", out_dir / "mcp_pan_left.wav", {"master_balance": -1.0})
        right = await export(doc, "wav", out_dir / "mcp_pan_right.wav", {"master_balance": 1.0})
        audio_left, _ = audio.decode_audio(left)
        audio_right, _ = audio.decode_audio(right)
        ll, lr = (float(np.sqrt(np.mean(audio_left[:, i] ** 2))) for i in (0, 1))
        rl, rr = (float(np.sqrt(np.mean(audio_right[:, i] ** 2))) for i in (0, 1))
        assert lr <= 0.02 * ll and rl <= 0.02 * rr

        midi = await export(doc, "midi", out_dir / "mcp_evening.mid")
        expected = json.loads(
            (
                REPO_ROOT / "vendor/engrave/tests/data/golden" / audio.golden_platform() / "evening.musicxml/midi.json"
            ).read_text(encoding="utf-8")
        )
        assert describe_midi(midi.read_bytes()) == expected
        await client.tool("score_close", {"document": doc})

        short_doc = (
            await client.tool(
                "score_open",
                {"path": str((REPO_ROOT / "vendor/engrave/tests/data/inputs/golden/ghost_note.mscx").resolve())},
            )
        )["document"]
        nonascii_dir = out_dir / "输出 音频"
        nonascii_dir.mkdir(parents=True, exist_ok=True)
        nonascii_async = {}
        try:
            for fmt in ("wav", "flac", "opus", "mp3", "aac"):
                sync = await export(short_doc, fmt, out_dir / f"mcp_sync.{fmt}")
                async_path = await export(short_doc, fmt, nonascii_dir / f"异步 音频.{fmt}")
                assert sync.read_bytes() == async_path.read_bytes(), fmt
                nonascii_async[fmt] = str(async_path.relative_to(out_dir))
        finally:
            await client.tool("score_close", {"document": short_doc})
    evidence["mcp_integration"] = {
        "status": "passed",
        "energy_ratio_db": round(energy_ratio_db, 2),
        "measured_volume_gain_db": round(measured_gain, 3),
        "non_ascii_async_exports": nonascii_async,
        "midi_matches_golden": True,
    }
    print("  MCP-level integration tests PASSED")


def main() -> int:
    parser = argparse.ArgumentParser(description="Acceptance test suite for MuseScore 4 audio engine")
    parser.add_argument(
        "--engine-only",
        action="store_true",
        help="Run fork audio assertions without the frozen MCP host or dense benchmark",
    )
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=None,
        help="Directory containing build executables",
    )
    parser.add_argument("--binary", type=Path, default=None, help="Path to engrave-mcp executable")
    parser.add_argument(
        "--resources",
        type=Path,
        default=REPO_ROOT / ".runtime" / "assets",
        help="Path to assets",
    )
    parser.add_argument(
        "--soundfont",
        type=Path,
        default=REPO_ROOT / ".runtime" / "soundfonts" / "FluidR3Mono_GM.sf3",
        help="Path to soundfont",
    )
    parser.add_argument("--output", type=Path, required=True, help="Output directory for test artifacts")
    parser.add_argument(
        "--write-midi-goldens",
        action="store_true",
        help="Regenerate vendor/engrave/tests/data/engine_audio/midi goldens from the current probe",
    )
    parser.add_argument(
        "--midi-golden-dir",
        type=Path,
        default=REPO_ROOT / "vendor/engrave/tests/data/engine_audio" / "midi",
        help="Directory for --write-midi-goldens",
    )
    parser.add_argument(
        "--platform-subdir",
        action="store_true",
        help="Write MIDI goldens under <dir>/<platform>/ when hosts differ",
    )
    parser.add_argument(
        "--defer-dense-performance",
        action="store_true",
        help="Leave the dense performance check pending and exit nonzero",
    )
    parser.add_argument(
        "--engine-source",
        type=Path,
        default=Path(os.environ.get("MUSESCORE_ENGINE_SOURCE", REPO_ROOT / "vendor" / "engrave")),
        help="Git checkout used to build the engine",
    )
    parser.add_argument(
        "--cross-platform-captures",
        type=Path,
        default=REPO_ROOT / ".runtime" / "audio-cross-platform",
        help="Directory containing windows/ and linux/ current captures",
    )
    parser.add_argument(
        "--capture-cross-platform-only",
        action="store_true",
        help="Capture this host without running or comparing the other checks",
    )
    parser.add_argument(
        "--reuse-golden-actual",
        type=Path,
        default=None,
        help="Optional golden/actual directory with verified index.json audio hashes to reuse",
    )
    args = parser.parse_args()

    out_dir = args.output.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.capture_cross_platform_only:
        if not args.binary or not args.binary.is_file():
            parser.error("--capture-cross-platform-only requires an existing --binary")
        make_current_audio_capture(
            args.binary,
            args.resources,
            args.soundfont,
            args.engine_source,
            args.cross_platform_captures.resolve(),
            reuse_golden_actual=args.reuse_golden_actual,
        )
        return 0

    probe_bin = find_probe_binary(args.bin_dir)
    print(f"Using probe binary: {probe_bin}")

    if args.write_midi_goldens:
        audio.write_midi_goldens(
            probe_bin,
            args.resources,
            args.soundfont,
            out_dir,
            args.midi_golden_dir,
            args.platform_subdir,
        )
        return 0

    evidence: dict[str, Any] = {}

    def dense_performance() -> None:
        if args.defer_dense_performance:
            raise audio.AudioCheckPending("dense performance must be measured separately")
        audio.test_performance_and_memory(probe_bin, args.resources, args.soundfont, out_dir, evidence)

    def mcp_integration() -> None:
        if not args.binary or not args.binary.is_file():
            raise audio.AudioCheckPending("MCP binary not supplied or missing")
        asyncio.run(test_mcp_level_integration(args.binary, args.resources, args.soundfont, out_dir, evidence))

    def cross_platform() -> None:
        if not args.binary or not args.binary.is_file():
            raise audio.AudioCheckPending("MCP binary not supplied or missing for current capture")
        captures = args.cross_platform_captures.resolve()
        source_commit, engine_commit = make_current_audio_capture(
            args.binary,
            args.resources,
            args.soundfont,
            args.engine_source,
            captures,
            reuse_golden_actual=args.reuse_golden_actual,
        )
        test_cross_platform_audio_features(
            evidence,
            captures,
            source_commit,
            engine_commit,
            audio_fixture_sources(),
            audio.file_sha256(args.soundfont),
        )

    checks = audio.engine_checks(
        probe_bin, args.resources, args.soundfont, out_dir, evidence, test_audio_check_reporting
    )
    if not args.engine_only:
        checks.extend(
            [
                ("dense_performance", dense_performance),
                ("mcp_integration", mcp_integration),
                ("cross_platform", cross_platform),
            ]
        )
    code = audio.run_audio_checks(checks, evidence)

    evidence_path = out_dir / "evidence.json"
    evidence_path.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(f"\nEvidence saved to {evidence_path}")
    return code


if __name__ == "__main__":
    sys.exit(main())
