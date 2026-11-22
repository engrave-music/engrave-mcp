"""MCP dense-score cancellation and recovery evidence.

The optional observer is a TEMPORARY engine instrumentation overlay, never a pin
change. Rebuild after installing/removing it. Phase trials pause at an existing
poll; they measure notification-to-response including real cleanup, NOT latency
to the next poll. Delay trials use an unmodified release binary.
"""

import argparse
import hashlib
import json
import os
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from engrave_mcp_testing.process_client import Client, summary


def observer(engine, remove):
    source = engine / "engrave/src/audio/engine_audio.cpp"
    backup = source.with_suffix(".cpp.m2-observer-backup")
    if remove:
        source.write_bytes(backup.read_bytes())
        backup.unlink()
        return
    assert not backup.exists(), "observer already installed"
    original = source.read_bytes()
    text = original.decode()
    needle = "    bool cancelled = false;\n    std::exception_ptr callbackError;"
    replacement = """    bool evidenceSeen = false;
    const char* evidencePhase = std::getenv("ENGRAVE_M2_PHASE_OBSERVER");
    const char* evidenceArm = std::getenv("ENGRAVE_M2_PHASE_ARM");
    const bool evidenceArmed = evidenceArm && std::filesystem::exists(evidenceArm);
    bool cancelled = false;
    std::exception_ptr callbackError;"""
    assert text.count(needle) == 1
    text = text.replace(needle, replacement)
    needle = "            try { cancelled = request.cancelled(); }"
    replacement = """            try {
                if (evidenceArmed && evidencePhase && !evidenceSeen &&
                    static_cast<int>(OfflineCancellation::phase) == std::atoi(evidencePhase)) {
                    evidenceSeen = true;
                    std::fprintf(stderr, "M2_PHASE_READY %s\\n", evidencePhase);
                    std::fflush(stderr);
                    while (!request.cancelled())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                cancelled = request.cancelled();
            }"""
    assert text.count(needle) == 1
    backup.write_bytes(original)
    source.write_text(text.replace(needle, replacement), encoding="utf-8", newline="\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--observer-engine", type=Path)
    p.add_argument("--remove-observer", action="store_true")
    for name in ("binary", "resources", "soundfont", "score", "output"):
        p.add_argument("--" + name, type=Path)
    p.add_argument("--delay-ms", type=float)
    p.add_argument("--trials", type=int, default=50)
    a = p.parse_args()
    if a.observer_engine:
        observer(a.observer_engine, a.remove_observer)
        return
    if any(getattr(a, name) is None for name in ("binary", "resources", "soundfont", "score", "output")):
        p.error("--binary, --resources, --soundfont, --score and --output are required for trials")
    assert a.trials >= 50
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    score = a.score.resolve()

    assert len(ET.parse(score).findall(".//note")) == 49152
    result = dict(
        binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
        score_sha256=hashlib.sha256(score.read_bytes()).hexdigest(),
        score_bytes=score.stat().st_size,
        notes=49152,
        rows=[],
    )
    phases = (
        [("delay", None)]
        if a.delay_ms is not None
        else [
            ("before-synthesis", 0),
            ("playback-model", 1),
            ("synthesis", 3),
            ("encode", 4),
        ]
    )

    def save():
        (out / "results.json").write_text(json.dumps(result, indent=2))

    for label, phase in phases:
        arm = out / (label + ".armed")
        arm.touch()
        os.environ["ENGRAVE_M2_PHASE_ARM"] = str(arm)
        if phase is None:
            os.environ.pop("ENGRAVE_M2_PHASE_OBSERVER", None)
        else:
            os.environ["ENGRAVE_M2_PHASE_OBSERVER"] = str(phase)
        c = Client(a.binary, a.resources, out / label, "live", a.soundfont)
        row = dict(
            phase=label,
            samples_ms=[],
            responses=[],
            method=(
                f"unheld release, {a.delay_ms} ms after submission"
                if phase is None
                else "temporary observer holds at existing engine phase poll; excludes time to next poll"
            ),
        )
        result["rows"].append(row)
        try:
            doc = c.tool("score_open", {"path": str(score)})["document"]

            def args(path):
                return {"document": doc, "format": "wav", "path": str(path)}

            # Environment changes apply only to newly launched clients.
            os.environ.pop("ENGRAVE_M2_PHASE_OBSERVER", None)
            ref = Client(a.binary, a.resources, out / (label + "-reference"), "live", a.soundfont)
            try:
                rd = ref.tool("score_open", {"path": str(score)})["document"]
                reference = args(out / (label + "-reference.wav"))
                reference["document"] = rd
                ref.tool("score_export", reference)
            finally:
                ref.close()
            expected = hashlib.sha256((out / (label + "-reference.wav")).read_bytes()).hexdigest()
            for i in range(a.trials):
                path = out / f"{label}-{i}.wav"
                offset = c.log_path.stat().st_size
                ident = c.send("tools/call", dict(name="score_export", arguments=args(path)))
                if phase is None:
                    time.sleep(a.delay_ms / 1000)
                else:
                    until = time.monotonic() + 60
                    while True:
                        with c.log_path.open("rb") as log:
                            log.seek(offset)
                            ready = b"M2_PHASE_READY" in log.read()
                        if ready:
                            break
                        assert time.monotonic() < until, "phase never reached"
                        time.sleep(0.001)
                start = time.perf_counter()
                c.send("notifications/cancelled", {"requestId": ident}, True)
                c.cancelled(ident, doc)
                reply = None
                row["samples_ms"].append((time.perf_counter() - start) * 1000)
                row["responses"].append(reply)

                assert not path.exists()
                assert not list(out.rglob(".engrave-stage-*"))
                save()
            arm.unlink()
            recovery = out / (label + "-recovery.wav")
            c.tool("score_export", args(recovery))
            assert hashlib.sha256(recovery.read_bytes()).hexdigest() == expected
            row["same_session_recovery_identical"] = True
            row["summary_ms"] = summary(row["samples_ms"])
            row["no_cancelled_artifact"] = True
        finally:
            c.close()
            save()
        row["reference_sha256"] = expected
    save()
    print(
        json.dumps(
            [{k: v for k, v in r.items() if k not in ("samples_ms", "responses")} for r in result["rows"]],
            indent=2,
        )
    )
    assert all(r["summary_ms"]["p95"] <= 50 and r["summary_ms"]["max"] <= 100 for r in result["rows"])


if __name__ == "__main__":
    main()
