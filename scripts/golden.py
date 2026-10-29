"""Golden-output capture and comparison for the native MCP server (roadmap step 0).

Capture exports a fixed fixture set through the official Python MCP client,
normalizes known-volatile fields, and stores one directory per fixture plus an
``index.json`` manifest of sha256 values. Compare re-captures the same tree and
diffs it against the stored goldens, honoring an explicit allow-list.

Allow entries match a fixture/output glob pair and only accept the expected
``kind`` (``different``, ``missing`` or ``extra``). Differences and extras must
also pin the actual file's normalized sha256 for the current platform, so an
entry cannot silently cover future changes. ``pin`` prints skeleton entries for
the un-allowed differences in an existing compare report.

Volatile fields removed by normalization (observed by running twice):
  * Every MSCX/MSCZ inner ``<eid>`` and EID-reference value (``linkedTo``,
    ``startElement``, ``endElement``, ``NoteEID``, ``mmRestLast``) is replaced
    with a self-closing empty tag (e.g. ``<eid/>``); the values are random per
    created document and non-deterministic across runs.
  * MSCX ``programVersion``/``programRevision`` and metaTag ``creationDate``/
    ``platform``; MusicXML ``encoding-date``/``software`` and the mirrored
    ``miscellaneous-field`` creationDate/platform values.
  * PDF: cairo writes CreationDate/Producer inside a compressed object stream.
    The PDF is opened with PyMuPDF, metadata is cleared, and the re-serialized
    bytes are hashed (garbage=3, deflate, no new id). ``bytes`` is the size of
    that normalized PDF because the raw export size varies by one byte.
    MusicXML also mirrors the MSCX creationDate/platform metaTags as
    ``miscellaneous-field`` values; those are blanked too.
Everything else is compared byte for byte, except ``audio.json`` which uses
the documented numeric tolerances. For spectral centroid envelope comparisons,
the metric is mathematically undefined in near-silent windows where RMS in both
golden and actual rounds to zero (e.g. low-energy FFT/quantization noise); in
such windows, centroid differences are ignored while preserving strict
tolerances for non-silent audio.

``semantic.json`` (the exported MSCZ) and ``semantic-source.json`` (the fixture
file) come from the ``semantic_dump`` dev tool next to the server binary. They
record musical content read from the in-memory DOM, independent of the MSCX
format version, and are the rebase oracle of roadmap-engrave A0/A2.

Usage:
  golden.py capture --binary B --resources R --soundfont SF --golden DIR \\
                    [--allow-errors]
  golden.py compare --binary B --resources R --soundfont SF --golden DIR \\
                    --allow tests/golden/allow.json --output OUTDIR \\
                    [--platform windows|linux]
  golden.py pin --golden DIR --output OUTDIR [--platform windows|linux]
"""

from __future__ import annotations

import argparse
import asyncio
import difflib
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from fnmatch import fnmatch
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from musicxml_semantic import musicxml_semantic

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "fixtures" / "golden"
EXTRA_FIXTURES = (ROOT / "fixtures" / "evening.musicxml",)
# Fixtures without stored excerpts whose part excerpts are generated (after the master
# outputs are captured) to cover excerpt creation, e.g. ChordLine path copying.
GENERATE_EXCERPTS = {"chordline_path.mscx"}
AUDIO_TOLERANCES = {"peak": 1e-4, "rms": 1e-4, "rmsEnvelope": 1e-4,
                    "spectralCentroidHz": 1.0, "centroidEnvelope": 2.0}
AUDIO_EXACT = {"sampleRate", "channels", "frames"}
ALLOW_KINDS = ("different", "missing", "extra")
DEFAULT_PLATFORM = "windows" if sys.platform.startswith("win") else "linux"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def write_json(path: Path, value, indent: int = 1) -> None:
    write_text(path, json.dumps(value, indent=indent, sort_keys=True, ensure_ascii=False,
                                allow_nan=False) + "\n")


def normalize_mscx(text: str) -> str:
    for tag in ("eid", "linkedTo", "startElement", "endElement", "NoteEID", "mmRestLast"):
        text = re.sub(rf"<{tag}>[^<]*</{tag}>", rf"<{tag}/>", text)
    for tag in ("programVersion", "programRevision"):
        text = re.sub(rf"(<{tag}>)[^<]*(</{tag}>)", rf"\1\2", text)
    for name in ("creationDate", "platform"):
        text = re.sub(rf'(<metaTag name="{name}">)[^<]*(</metaTag>)', r"\1\2", text)
    return text


def normalize_musicxml(text: str) -> str:
    for tag in ("encoding-date", "software"):
        text = re.sub(rf"(<{tag}>)[^<]*(</{tag}>)", rf"\1\2", text)
    for name in ("creationDate", "platform"):
        text = re.sub(
            rf'(<miscellaneous-field name="{name}">)[^<]*(</miscellaneous-field>)', r"\1\2", text)
    return text


def normalize_svg(text: str) -> str:
    # SVG exports carry no timestamp in the observed engine build; keep as-is.
    return text


def round_json(value):
    if isinstance(value, float):
        return round(value, 4)
    if isinstance(value, dict):
        return {key: round_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [round_json(item) for item in value]
    return value


def image_size(data: bytes) -> dict:
    import io
    from PIL import Image
    with Image.open(io.BytesIO(data)) as image:
        return {"width": image.width, "height": image.height}


def load_rounded(path: Path):
    return round_json(json.loads(read_text(path)))


def describe_png(path: Path) -> dict:
    from PIL import Image
    with Image.open(path) as image:
        rgba = image.convert("RGBA")
        return {"width": image.width, "height": image.height, "mode": image.mode,
                "sha256": sha256(rgba.tobytes())}


def describe_pdf(path: Path) -> dict:
    import pymupdf  # pinned in pixi.toml; cairo hides the volatile metadata in object streams
    document = pymupdf.open(stream=path.read_bytes(), filetype="pdf")
    try:
        pages = document.page_count
        document.set_metadata({key: "" for key in
                               ("title", "author", "subject", "keywords", "creator",
                                "producer", "creationDate", "modDate")})
        canonical = document.tobytes(garbage=3, deflate=True, no_new_id=True)
    finally:
        document.close()
    canonical = re.sub(rb"/(CreationDate|ModDate|Producer)\s*\([^)]*\)", rb"/\1 ()", canonical)
    # Raw export bytes vary by one with compressed cairo metadata; report the
    # normalized size so the golden is stable.
    return {"pages": pages, "bytes": len(canonical), "sha256": sha256(canonical)}


def describe_audio(path: Path) -> dict:
    import numpy as np
    import soundfile as sf

    audio, rate = sf.read(path, always_2d=True)
    frames, channels = audio.shape
    peak = [round(float(np.max(np.abs(audio[:, channel]))), 6) for channel in range(channels)]
    rms = [round(float(np.sqrt(np.mean(audio[:, channel] ** 2))), 6) for channel in range(channels)]
    window = max(1, int(round(rate * 0.25)))
    mono = audio.mean(axis=1)
    envelope, centroids = [], []
    for start in range(0, frames, window):
        chunk = audio[start:start + window]
        envelope.append([round(float(np.sqrt(np.mean(chunk[:, channel] ** 2))), 5)
                         for channel in range(channels)])
        segment = mono[start:start + window]
        spectrum = np.abs(np.fft.rfft(segment))
        frequencies = np.fft.rfftfreq(len(segment), 1.0 / rate)
        weight = float(spectrum.sum())
        centroids.append(round(float(np.dot(frequencies, spectrum)) / weight, 1)
                         if weight else 0.0)
    block = 1 << 20
    weighted = 0.0
    weight = 0.0
    for start in range(0, len(mono), block):
        chunk = mono[start:start + block]
        spectrum = np.abs(np.fft.rfft(chunk))
        frequencies = np.fft.rfftfreq(len(chunk), 1.0 / rate)
        weighted += float(np.dot(frequencies, spectrum))
        weight += float(spectrum.sum())
    return {"sampleRate": int(rate), "channels": int(channels), "frames": int(frames),
            "peak": peak, "rms": rms, "rmsEnvelope": envelope,
            "centroidEnvelope": centroids,
            "spectralCentroidHz": round(weighted / weight, 1) if weight else 0.0}


def describe_mscz(path: Path) -> dict:
    with zipfile.ZipFile(path) as archive:
        names = sorted(archive.namelist())
        entries, texts = [], {}
        for name in names:
            data = archive.read(name)
            base = name.rsplit("/", 1)[-1].lower()
            if base.endswith(".mscx"):
                normalized = normalize_mscx(data.decode("utf-8"))
                entries.append({"name": name, "sha256": sha256(normalized.encode("utf-8"))})
            elif base in ("audiosettings.json", "viewsettings.json"):
                entries.append({"name": name, "size": len(data)})
                texts[name] = data.decode("utf-8")
            elif base.startswith("thumbnail") and base.endswith((".png", ".jpg", ".jpeg")):
                entries.append({"name": name, **image_size(data)})
            else:
                entries.append({"name": name, "size": len(data)})
    return {"entries": entries, "texts": texts}


def describe_mxl(path: Path) -> dict:
    with zipfile.ZipFile(path) as archive:
        names = sorted(archive.namelist())
        inner = next((name for name in names
                      if name.lower().endswith((".xml", ".musicxml"))
                      and not name.upper().startswith("META-INF/")), None)
        document = {"entries": names}
        if inner:
            normalized = normalize_musicxml(archive.read(inner).decode("utf-8"))
            document["musicxml"] = {"name": inner, "sha256": sha256(normalized.encode("utf-8"))}
    return document


def describe_midi(data: bytes) -> dict:
    if not data.startswith(b"MThd"):
        raise ValueError("invalid MIDI header")
    header_len = int.from_bytes(data[4:8], "big")
    midi_format = int.from_bytes(data[8:10], "big")
    track_count = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    position = 8 + header_len
    tracks = []
    for _ in range(track_count):
        if data[position:position + 4] != b"MTrk":
            raise ValueError(f"expected MTrk at byte {position}")
        length = int.from_bytes(data[position + 4:position + 8], "big")
        track = data[position + 8:position + 8 + length]
        position += 8 + length
        events, cursor, tick, running = [], 0, 0, None
        while cursor < len(track):
            delta = 0
            while cursor < len(track):
                byte = track[cursor]
                cursor += 1
                delta = (delta << 7) | (byte & 0x7F)
                if not byte & 0x80:
                    break
            tick += delta
            if cursor >= len(track):
                break
            lead = track[cursor]
            if lead == 0xFF:
                meta_type = track[cursor + 1]
                cursor += 2
                size = 0
                while cursor < len(track):
                    byte = track[cursor]
                    cursor += 1
                    size = (size << 7) | (byte & 0x7F)
                    if not byte & 0x80:
                        break
                payload = track[cursor:cursor + size]
                cursor += size
                events.append([tick, {"type": "meta", "metaType": meta_type,
                                      "data": payload.hex()}])
                running = None
                if meta_type == 0x2F:
                    break
                continue
            if lead in (0xF0, 0xF7):
                cursor += 1
                size = 0
                while cursor < len(track):
                    byte = track[cursor]
                    cursor += 1
                    size = (size << 7) | (byte & 0x7F)
                    if not byte & 0x80:
                        break
                events.append([tick, {"type": "sysex", "data": track[cursor:cursor + size].hex()}])
                cursor += size
                running = None
                continue
            status = lead if lead & 0x80 else running
            if lead & 0x80:
                cursor += 1
            running = status
            command, channel = status & 0xF0, status & 0x0F
            if command in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                first, second = track[cursor], track[cursor + 1]
                cursor += 2
                if command in (0x80, 0x90) and not (command == 0x90 and second > 0):
                    events.append([tick, {"type": "note_off", "ch": channel,
                                          "pitch": first, "vel": second}])
                elif command == 0x90:
                    events.append([tick, {"type": "note_on", "ch": channel,
                                          "pitch": first, "vel": second}])
                elif command == 0xA0:
                    events.append([tick, {"type": "poly_aftertouch", "ch": channel,
                                          "pitch": first, "pressure": second}])
                elif command == 0xB0:
                    events.append([tick, {"type": "cc", "ch": channel,
                                          "num": first, "val": second}])
                else:
                    events.append([tick, {"type": "pitchbend", "ch": channel,
                                          "value": first | (second << 7)}])
            elif command in (0xC0, 0xD0):
                first = track[cursor]
                cursor += 1
                if command == 0xC0:
                    events.append([tick, {"type": "program", "ch": channel, "program": first}])
                else:
                    events.append([tick, {"type": "aftertouch", "ch": channel,
                                          "pressure": first}])
            else:
                raise ValueError(f"unsupported MIDI status {status}")
        tracks.append(events)
    return {"format": midi_format, "division": division, "tracks": tracks}


async def call_tool(session: ClientSession, name: str, arguments: dict):
    response = await session.call_tool(name, arguments)
    if response.is_error:
        texts = [item.text for item in response.content if getattr(item, "text", None)]
        return None, " ".join(texts).strip() or "tool error"
    if response.structured_content is not None:
        return response.structured_content, None
    if response.content and getattr(response.content[0], "text", None):
        return json.loads(response.content[0].text), None
    return None, "empty tool response"


async def export(session, identity, fmt, dest, **options):
    _, error = await call_tool(session, "score_export",
                               {**identity, "format": fmt, "path": str(dest), **options})
    return error


def sanitize(text: str, roots: list[Path]) -> str:
    for root in roots:
        text = text.replace(str(root), "<tmp>")
    return text


async def text_export(session, identity, fmt, raw, out_dir, name, normalize, roots, **options):
    error = await export(session, identity, fmt, raw, **options)
    if error:
        write_text(out_dir / f"{name}.error.txt", sanitize(error, roots) + "\n")
        return None
    try:
        text = normalize(read_text(raw))
        if normalize is normalize_musicxml:
            # Engine-independent content, the A2 oracle for MusicXML (roadmap-engrave A0).
            write_json(out_dir / "musicxml-semantic.json", musicxml_semantic(text))
        return write_text(out_dir / name, text)
    except Exception as exc:  # noqa: BLE001 - record instead of aborting the fixture
        write_text(out_dir / f"{name}.error.txt",
                   f"transform failed: {type(exc).__name__}: {sanitize(str(exc), roots)}\n")
        return None


async def value_export(session, identity, fmt, raw, out_dir, name, transform, roots, **options):
    error = await export(session, identity, fmt, raw, **options)
    if error:
        write_text(out_dir / f"{name}.error.txt", sanitize(error, roots) + "\n")
        return None
    try:
        value = transform(raw)
    except Exception as exc:  # noqa: BLE001
        write_text(out_dir / f"{name}.error.txt",
                   f"transform failed: {type(exc).__name__}: {sanitize(str(exc), roots)}\n")
        return None
    write_json(out_dir / name, value)
    return value


async def capture_master(session, identity, raw, out_dir, roots):
    await text_export(session, identity, "mscx", raw / "score.mscx", out_dir,
                      "score.mscx", normalize_mscx, roots)
    await text_export(session, identity, "musicxml", raw / "score.musicxml", out_dir,
                      "score.musicxml", normalize_musicxml, roots)
    await value_export(session, identity, "mscz", raw / "score.mscz", out_dir,
                       "mscz.json", describe_mscz, roots)
    await value_export(session, identity, "mxl", raw / "score.mxl", out_dir,
                       "mxl.json", describe_mxl, roots)
    await value_export(session, identity, "midi", raw / "score.mid", out_dir,
                       "midi.json", lambda path: describe_midi(path.read_bytes()), roots)
    metadata = await value_export(session, identity, "metadata", raw / "metadata.json",
                                  out_dir, "metadata.json", load_rounded, roots)
    await value_export(session, identity, "measurePositions", raw / "measurePositions.json",
                       out_dir, "measurePositions.json", load_rounded, roots)
    await value_export(session, identity, "segmentPositions", raw / "segmentPositions.json",
                       out_dir, "segmentPositions.json", load_rounded, roots)
    pages = metadata.get("pages") if isinstance(metadata, dict) else None
    if pages is None:
        info, _ = await call_tool(session, "score_get", identity)
        pages = info.get("pages", 0) if isinstance(info, dict) else 0
    for page in range(int(pages)):
        await text_export(session, identity, "svg", raw / f"page-{page}.svg", out_dir,
                          f"page-{page}.svg", normalize_svg, roots, page=page)
    await value_export(session, identity, "png", raw / "page-0.png", out_dir,
                       "page-0.png.json", describe_png, roots, page=0)
    await value_export(session, identity, "pdf", raw / "score.pdf", out_dir,
                       "score.pdf.json", describe_pdf, roots)
    await value_export(session, identity, "wav", raw / "score.wav", out_dir,
                       "audio.json", describe_audio, roots)
    # Format-independent content of the saved file (roadmap-engrave A0).
    semantic_dump(raw / "score.mscz", out_dir / "semantic.json", roots)


SEMANTIC_DUMP: dict = {}


def semantic_dump(source: Path, dest: Path, roots, *extra: str) -> None:
    """Run the semantic_dump dev tool next to the server binary; errors become .error.txt."""
    if not source.is_file():
        write_text(dest.with_suffix(".error.txt"), "source export missing\n")
        return
    result = subprocess.run(
        [str(SEMANTIC_DUMP["binary"]), str(SEMANTIC_DUMP["resources"]), str(source), *extra],
        capture_output=True, check=False)
    if result.returncode != 0:
        write_text(dest.with_suffix(".error.txt"),
                   sanitize(result.stderr.decode("utf-8", "replace"), roots) + "\n")
        return
    write_json(dest, json.loads(result.stdout.decode("utf-8")))


async def capture_excerpts(session, identity, raw, out_dir, roots):
    payload, error = await call_tool(session, "score_list_excerpts", identity)
    if error:
        write_text(out_dir / "excerpts.error.txt", sanitize(error, roots) + "\n")
        return
    for excerpt in payload.get("excerpts", []):
        index = excerpt["id"]
        folder = out_dir / f"excerpt-{index}"
        await text_export(session, identity, "mscx", raw / f"excerpt-{index}.mscx", folder,
                          "score.mscx", normalize_mscx, roots, excerptId=index)
        await text_export(session, identity, "musicxml", raw / f"excerpt-{index}.musicxml",
                          folder, "score.musicxml", normalize_musicxml, roots, excerptId=index)
        await value_export(session, identity, "midi", raw / f"excerpt-{index}.mid", folder,
                           "midi.json", lambda path: describe_midi(path.read_bytes()), roots,
                           excerptId=index)
        await value_export(session, identity, "metadata", raw / f"excerpt-{index}-metadata.json",
                           folder, "metadata.json", load_rounded, roots, excerptId=index)
        await text_export(session, identity, "svg", raw / f"excerpt-{index}-page-0.svg",
                          folder, "page-0.svg", normalize_svg, roots, excerptId=index, page=0)


async def capture_fixture(session, fixture_id, source, raw_root, out_dir, roots):
    raw = raw_root / fixture_id
    raw.mkdir(parents=True, exist_ok=True)
    if source.name == "created.json":
        payload, error = await call_tool(
            session, "score_create", {"score": json.loads(read_text(source))})
    else:
        payload, error = await call_tool(session, "score_open", {"path": str(source)})
    if error:
        write_text(out_dir / "open.error.txt", sanitize(error, roots) + "\n")
        return
    identity = {"documentId": payload["documentId"]}
    if source.name != "created.json":
        # The fixture as the engine reads it, before any save.
        semantic_dump(source, out_dir / "semantic-source.json", roots,
                      *(["--generate-excerpts"] if fixture_id in GENERATE_EXCERPTS else []))
    try:
        await capture_master(session, identity, raw, out_dir, roots)
        if fixture_id in GENERATE_EXCERPTS:
            _, error = await call_tool(session, "score_generate_excerpts", identity)
            if error:
                write_text(out_dir / "generate.error.txt", sanitize(error, roots) + "\n")
        await capture_excerpts(session, identity, raw, out_dir, roots)
    finally:
        await call_tool(session, "score_close", identity)


# Edit scenarios (roadmap-engrave A0): the edit tools' observable results, which A2 must
# preserve on 5.0. Each opens a source, applies one edit and captures the saved content.
EDIT_SCENARIOS = {
    "edit-replace-measures": {
        "source": "created.json", "tool": "score_replace_measures",
        "arguments": {"partIndex": 0, "startMeasure": 1, "measures": [{"notes": [
            {"pitches": [60, 64, 67], "ticks": 960}, {"pitches": [62], "ticks": 480},
            {"pitches": [], "ticks": 240}, {"pitches": [65], "ticks": 240}]}]},
    },
    "edit-set-pitches-coverage": {"source": "coverage.musicxml", "tool": "score_set_pitches",
                                  "part": 0, "measures": 2, "notes": 4, "delta": 2},
    # Generated part excerpts are linked to the master, so the edit must reach them too
    # (linked_parts.mscx is unusable here: every note in it is tied).
    "edit-set-pitches-linked": {"source": "chordline_path.mscx", "tool": "score_set_pitches",
                                "generate": True, "part": 0, "measures": 2, "notes": 3,
                                "delta": 1},
}


async def capture_edit(session, name, scenario, raw_root, out_dir, roots):
    raw = raw_root / name
    raw.mkdir(parents=True, exist_ok=True)
    source = FIXTURE_DIR / scenario["source"]
    if source.name == "created.json":
        payload, error = await call_tool(
            session, "score_create", {"score": json.loads(read_text(source))})
    else:
        payload, error = await call_tool(session, "score_open", {"path": str(source)})
    if error:
        write_text(out_dir / "open.error.txt", sanitize(error, roots) + "\n")
        return
    identity = {"documentId": payload["documentId"]}
    try:
        if scenario.get("generate"):
            _, error = await call_tool(session, "score_generate_excerpts", identity)
            if error:
                write_text(out_dir / "generate.error.txt", sanitize(error, roots) + "\n")
                return
        if scenario["tool"] == "score_set_pitches":
            view, error = await call_tool(session, "score_query_region", {
                **identity, "partIndex": scenario["part"], "firstMeasure": 0,
                "measureCount": scenario["measures"]})
            if error:
                write_text(out_dir / "query.error.txt", sanitize(error, roots) + "\n")
                return
            # Tied notes are skipped: a pitch change on one tie end is rejected by design.
            targets = [n for n in view["notes"] if not n["tied"]][:scenario["notes"]]
            base = view["revision"] if "revision" in view else view["score"]["revision"]
            arguments = {**identity, "baseRevision": base, "operationId": name,
                         "changes": [{"noteId": n["id"], "pitch": n["pitch"] + scenario["delta"]}
                                     for n in targets]}
        else:
            arguments = {**identity, **scenario["arguments"]}
        _, error = await call_tool(session, scenario["tool"], arguments)
        if error:
            write_text(out_dir / "edit.error.txt", sanitize(error, roots) + "\n")
            return
        await text_export(session, identity, "mscx", raw / "score.mscx", out_dir,
                          "score.mscx", normalize_mscx, roots)
        await text_export(session, identity, "musicxml", raw / "score.musicxml", out_dir,
                          "score.musicxml", normalize_musicxml, roots)
        await value_export(session, identity, "midi", raw / "score.mid", out_dir,
                           "midi.json", lambda path: describe_midi(path.read_bytes()), roots)
        error = await export(session, identity, "mscz", raw / "score.mscz")
        if error:
            write_text(out_dir / "mscz.error.txt", sanitize(error, roots) + "\n")
        else:
            semantic_dump(raw / "score.mscz", out_dir / "semantic.json", roots)
    finally:
        await call_tool(session, "score_close", identity)


def fixture_files() -> list[Path]:
    files = sorted(path for path in FIXTURE_DIR.iterdir()
                   if path.is_file() and path.name != "README.md")
    return files + [path for path in EXTRA_FIXTURES if path.is_file()]


def reset_root(root: Path) -> None:
    if root.exists():
        for child in root.iterdir():
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()
    root.mkdir(parents=True, exist_ok=True)


def build_index(root: Path) -> dict:
    index = {}
    for folder in sorted(path for path in root.iterdir() if path.is_dir()):
        files = {}
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                files[path.relative_to(folder).as_posix()] = sha256(path.read_bytes())
        index[folder.name] = files
    write_json(root / "index.json", index)
    return index


async def capture_tree(binary, resources, soundfont, target, log_path):
    dump = binary.parent / ("semantic_dump.exe" if sys.platform.startswith("win") else "semantic_dump")
    if not dump.is_file():
        raise SystemExit(f"semantic_dump not found next to the server binary: {dump}")
    SEMANTIC_DUMP.update(binary=dump.resolve(), resources=Path(resources).resolve())
    reset_root(target)
    with tempfile.TemporaryDirectory(prefix="golden-raw-") as temporary:
        raw_root = Path(temporary)
        params = StdioServerParameters(
            command=str(binary),
            args=["--resources", str(resources), "--soundfont", str(soundfont)],
            cwd=str(raw_root))
        with log_path.open("w", encoding="utf-8") as log:
            async with stdio_client(params, errlog=log) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    for source in fixture_files():
                        try:
                            await capture_fixture(session, source.name, source, raw_root,
                                                  target / source.name, [raw_root, target])
                        except Exception as exc:  # noqa: BLE001 - keep other fixtures going
                            write_text(target / source.name / "capture.error.txt",
                                       f"{type(exc).__name__}: "
                                       f"{sanitize(str(exc), [raw_root, target])}\n")
                    for name, scenario in EDIT_SCENARIOS.items():
                        try:
                            await capture_edit(session, name, scenario, raw_root,
                                               target / name, [raw_root, target])
                        except Exception as exc:  # noqa: BLE001
                            write_text(target / name / "capture.error.txt",
                                       f"{type(exc).__name__}: "
                                       f"{sanitize(str(exc), [raw_root, target])}\n")
    return build_index(target)


def load_allow(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    entries = json.loads(read_text(path)).get("entries", [])
    for entry in entries:
        missing = [key for key in ("fixture", "output", "step", "reason") if key not in entry]
        if missing:
            raise ValueError(f"allow entry {entry!r} is missing {', '.join(missing)}")
        if entry.setdefault("kind", "different") not in ALLOW_KINDS:
            raise ValueError(f"allow entry has invalid kind: {entry['kind']!r}")
    return entries


def allow_accept(entries, fixture, output, kind, actual_sha, platform):
    """Return (position, entry, problem) for the first matching allow entry."""
    matches = [(position, entry) for position, entry in enumerate(entries)
               if fnmatch(fixture, entry["fixture"]) and fnmatch(output, entry["output"])]
    problems = []
    for position, entry in matches:
        if entry["kind"] != kind:
            problems.append(f"entry {position}: kind {entry['kind']} != {kind}")
            continue
        if kind == "missing":
            return position, entry, None
        pinned = entry.get("sha256")
        pinned = pinned.get(platform) if isinstance(pinned, dict) else None
        if not pinned:
            problems.append(f"entry {position}: no sha256 for {platform}")
        elif str(pinned).lower() != actual_sha:
            problems.append(f"entry {position}: pinned {pinned} != actual {actual_sha}")
        else:
            return position, entry, None
    return None, None, "; ".join(problems) if problems else None


def verify_golden_index(root: Path, index: dict) -> list[str]:
    on_disk = {path.name for path in root.iterdir() if path.is_dir()}
    issues = [f"unindexed fixture {name}" for name in sorted(on_disk - set(index))]
    issues += [f"missing fixture folder {name}" for name in sorted(set(index) - on_disk)]
    for name in sorted(on_disk & set(index)):
        present = {path.relative_to(root / name).as_posix()
                   for path in (root / name).rglob("*") if path.is_file()}
        issues += [f"missing indexed file {name}/{entry}" for entry in sorted(set(index[name]) - present)]
        issues += [f"unindexed file {name}/{entry}" for entry in sorted(present - set(index[name]))]
    return issues


def audio_equal(golden, actual, key=None) -> bool:
    if isinstance(golden, dict) and isinstance(actual, dict):
        if golden.keys() != actual.keys():
            return False
        has_centroids = "centroidEnvelope" in golden and "centroidEnvelope" in actual
        has_rms = "rmsEnvelope" in golden and "rmsEnvelope" in actual
        for name in golden:
            if name == "centroidEnvelope" and has_centroids and has_rms:
                g_cents = golden["centroidEnvelope"]
                a_cents = actual["centroidEnvelope"]
                g_rms_env = golden["rmsEnvelope"]
                a_rms_env = actual["rmsEnvelope"]
                if len(g_cents) != len(a_cents) or len(g_cents) != len(g_rms_env) or len(a_cents) != len(a_rms_env):
                    return False
                tol = AUDIO_TOLERANCES.get("centroidEnvelope", 2.0)
                rms_tol = AUDIO_TOLERANCES.get("rmsEnvelope", 1e-4)
                for i, (g_c, a_c) in enumerate(zip(g_cents, a_cents)):
                    g_silent = max(g_rms_env[i]) <= rms_tol if isinstance(g_rms_env[i], list) else g_rms_env[i] <= rms_tol
                    a_silent = max(a_rms_env[i]) <= rms_tol if isinstance(a_rms_env[i], list) else a_rms_env[i] <= rms_tol
                    if g_silent and a_silent:
                        continue
                    if abs(g_c - a_c) > tol:
                        return False
            else:
                if not audio_equal(golden[name], actual[name], name):
                    return False
        return True
    if isinstance(golden, list) and isinstance(actual, list):
        return len(golden) == len(actual) and all(
            audio_equal(left, right, key) for left, right in zip(golden, actual))
    if isinstance(golden, bool) or isinstance(actual, bool):
        return golden == actual
    if isinstance(golden, (int, float)) and isinstance(actual, (int, float)):
        if key in AUDIO_EXACT:
            return golden == actual
        return abs(golden - actual) <= AUDIO_TOLERANCES.get(key, 1e-9)
    return golden == actual


def files_equal(golden: Path, actual: Path, output: str) -> bool:
    if output == "audio.json":
        try:
            return audio_equal(json.loads(read_text(golden)), json.loads(read_text(actual)))
        except (ValueError, KeyError):
            return False
    return golden.read_bytes() == actual.read_bytes()


def write_diff(root: Path, fixture: str, output: str, golden: Path, actual: Path) -> None:
    lines = list(difflib.unified_diff(
        read_text(golden).splitlines(keepends=True),
        read_text(actual).splitlines(keepends=True),
        fromfile=f"golden/{fixture}/{output}", tofile=f"actual/{fixture}/{output}"))
    if len(lines) > 2000:
        lines = lines[:2000] + ["... truncated after 2000 diff lines ...\n"]
    write_text(root / fixture / f"{output}.diff",
               "".join(line if line.endswith("\n") else line + "\n" for line in lines))


def compare_trees(golden_root: Path, actual_root: Path, allow: list[dict], output: Path,
                  platform: str) -> int:
    golden_index = json.loads(read_text(golden_root / "index.json"))
    counts = {key: 0 for key in ("identical", "allowed", "different", "missing", "extra",
                                 "goldenError", "extraFixture", "index")}
    report = {"golden": str(golden_root), "actual": str(actual_root), "fixtures": {},
              "stale": [], "extraFixtures": [], "indexIssues": [], "counts": counts,
              "status": "pass"}
    matched_allow: set[int] = set()
    diff_root = output / "diff"
    if diff_root.exists():
        shutil.rmtree(diff_root)

    report["indexIssues"] = verify_golden_index(golden_root, golden_index)
    counts["index"] = len(report["indexIssues"])
    for issue in report["indexIssues"]: print(f"index    {issue}")
    actual_fixtures = {path.name for path in actual_root.iterdir() if path.is_dir()}
    for fixture in sorted(actual_fixtures - set(golden_index)):
        report["extraFixtures"].append(fixture)
        counts["extraFixture"] += 1
        print(f"extra fixture  {fixture}")

    for fixture in sorted(golden_index):
        expected = golden_index[fixture]
        golden_dir, actual_dir = golden_root / fixture, actual_root / fixture
        actual_files = ({path.relative_to(actual_dir).as_posix()
                         for path in actual_dir.rglob("*") if path.is_file()}
                        if actual_dir.is_dir() else set())
        results = report["fixtures"].setdefault(fixture, {})
        for name in sorted(set(expected) | actual_files):
            golden_file, actual_file = golden_dir / name, actual_dir / name
            golden_sha = sha256(golden_file.read_bytes()) if golden_file.is_file() else None
            actual_sha = sha256(actual_file.read_bytes()) if actual_file.is_file() else None
            golden_error = golden_sha is not None and name.endswith(".error.txt")
            if actual_sha is None:
                kind = "missing"
            elif name not in expected:
                kind = "extra"
            elif golden_sha is None:
                kind = "different"
            elif files_equal(golden_file, actual_file, name) and not golden_error:
                kind = None
            else:
                kind = "different"
            if kind is None:
                counts["identical"] += 1
                results[name] = {"status": "identical", "sha256": actual_sha}
                continue

            position, entry, problem = allow_accept(allow, fixture, name, kind, actual_sha, platform)
            if entry:
                matched_allow.add(position)
                counts["allowed"] += 1
                results[name] = {"status": "allowed", "kind": kind, "step": entry["step"],
                                 "reason": entry["reason"], "actualSha256": actual_sha,
                                 "goldenSha256": golden_sha}
                print(f"allowed  {fixture}/{name} ({kind}; {entry['step']}: {entry['reason']})")
            else:
                status = "golden-error" if golden_error else kind
                counts["goldenError" if golden_error else kind] += 1
                results[name] = {"status": status, "kind": kind, "goldenSha256": golden_sha,
                                 "actualSha256": actual_sha}
                suffix = f" (allow mismatch: {problem})" if problem else ""
                label = "missing" if kind == "missing" else status
                print(f"{label:<12} {fixture}/{name} actual sha256={actual_sha or 'none'}{suffix}")
            if kind == "different" and golden_file.is_file() and actual_sha != golden_sha:
                write_diff(diff_root, fixture, name, golden_file, actual_file)

    for position, entry in enumerate(allow):
        if position not in matched_allow:
            report["stale"].append(entry)
            print(f"stale    allow entry fixture={entry['fixture']} output={entry['output']} "
                  f"({entry['step']}: {entry['reason']})")

    failing = sum(counts[key] for key in ("different", "missing", "extra", "goldenError",
                                          "extraFixture", "index"))
    report["status"] = "pass" if failing == 0 else "fail"
    write_json(output / "report.json", report, indent=2)
    total = sum(counts[key] for key in ("identical", "allowed", "different", "missing",
                                        "extra", "goldenError"))
    print(f"golden compare: {len(golden_index)} fixtures, {total} outputs")
    for key in counts: print(f"{key}: {counts[key]}")
    print(f"result: {'PASS' if failing == 0 else 'FAIL'}")
    return 0 if failing == 0 else 1


def command_capture(args) -> int:
    args.golden.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="golden-log-") as temporary:
        index = asyncio.run(asyncio.wait_for(capture_tree(
            args.binary, args.resources, args.soundfont, args.golden,
            Path(temporary) / "server-stderr.log"), timeout=1800))
    count = sum(len(files) for files in index.values())
    size = sum(path.stat().st_size for path in args.golden.rglob("*") if path.is_file())
    print(f"captured {len(index)} fixtures, {count} outputs, {size} bytes into {args.golden}")
    errors = sorted(args.golden.rglob("*.error.txt"))
    for path in errors:
        print(f"error    {path.relative_to(args.golden).as_posix()}: {read_text(path).strip()}")
    if errors and not args.allow_errors:
        print(f"capture failed: {len(errors)} error file(s); pass --allow-errors to accept")
        return 1
    return 0


def command_compare(args) -> int:
    allow = load_allow(args.allow)
    args.output.mkdir(parents=True, exist_ok=True)
    actual = args.output / "actual"
    with tempfile.TemporaryDirectory(prefix="golden-log-") as temporary:
        asyncio.run(asyncio.wait_for(capture_tree(
            args.binary, args.resources, args.soundfont, actual,
            Path(temporary) / "server-stderr.log"), timeout=1800))
        # Keep a copy of the server log for debugging (outside the compared tree).
        shutil.copyfile(Path(temporary) / "server-stderr.log", args.output / "server-stderr.log")
    return compare_trees(args.golden, actual, allow, args.output, args.platform)


def command_pin(args) -> int:
    report_path = args.output / "report.json"
    if not report_path.is_file():
        raise SystemExit(f"report does not exist: {report_path}")
    report = json.loads(read_text(report_path))
    entries = []
    for fixture, results in sorted(report.get("fixtures", {}).items()):
        for name, result in sorted(results.items()):
            if result.get("status") not in ("different", "missing", "extra", "golden-error"):
                continue
            kind = result.get("kind") or result["status"]
            sha = {} if kind == "missing" else {"sha256": {args.platform: result["actualSha256"]}}
            entries.append({"fixture": fixture, "output": name, "step": "TODO",
                            "reason": "TODO", "kind": kind, **sha})
    print(json.dumps(entries, indent=1, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("capture", "compare"):
        sub = subparsers.add_parser(name)
        sub.add_argument("--binary", type=Path, required=True)
        sub.add_argument("--resources", type=Path, required=True)
        sub.add_argument("--soundfont", type=Path, required=True)
        sub.add_argument("--golden", type=Path, required=True)
        if name == "capture":
            sub.add_argument("--allow-errors", action="store_true")
        else:
            sub.add_argument("--allow", type=Path, required=True)
            sub.add_argument("--output", type=Path, required=True)
            sub.add_argument("--platform", choices=("windows", "linux"), default=DEFAULT_PLATFORM)
    pin = subparsers.add_parser("pin")
    pin.add_argument("--golden", type=Path, required=True)
    pin.add_argument("--output", type=Path, required=True)
    pin.add_argument("--platform", choices=("windows", "linux"), default=DEFAULT_PLATFORM)
    args = parser.parse_args()
    if args.command == "pin":
        return command_pin(args)
    for label, path in (("binary", args.binary), ("resources", args.resources),
                        ("soundfont", args.soundfont)):
        if not path.exists():
            parser.error(f"{label} does not exist: {path}")
    if args.command == "compare" and not args.golden.exists():
        parser.error(f"golden does not exist: {args.golden}")
    args.binary = args.binary.resolve()
    args.resources = args.resources.resolve()
    args.soundfont = args.soundfont.resolve()
    args.golden = args.golden.resolve()
    if args.command == "capture":
        return command_capture(args)
    args.allow = args.allow.resolve()
    args.output = args.output.resolve()
    return command_compare(args)


if __name__ == "__main__":
    sys.exit(main())

