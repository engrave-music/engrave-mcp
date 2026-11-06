"""Golden-output capture and comparison for the native MCP server.

Capture exports a fixed fixture set through the official Python MCP client,
normalizes known-volatile fields, and stores one directory per fixture plus an
``index.json`` manifest of sha256 values. Compare re-captures the same tree and
diffs it against the stored goldens, honoring an explicit allow-list.

Allow entries name exact fixture/output pairs and platform SHA-256 pins for
``different`` or ``extra`` artifacts. Missing artifacts cannot be waived.
Every entry in the comparison context must be exercised; stale entries fail.
Archive MSCX exceptions are restricted to the engine-gate context. ``pin``
prints skeleton entries from an existing report for review; missing-artifact
skeletons are diagnostic only and are rejected by the policy loader.

Volatile fields normalized (observed by running twice):
  * Every MSCX/MSCZ inner ``<eid>`` and EID-reference value (``linkedTo``,
    ``startElement``, ``endElement``, ``NoteEID``, ``mmRestLast``,
    ``startMeasure``, ``endMeasure``, ``sharedPart``) is mapped to a stable
    token (``e1``, ``e2``, ...) in order of first appearance per document (per
    file, after the whole file is read), preserving element identity and
    endpoint relationships across non-deterministic runtime EID allocations.
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
the documented numeric tolerances. ``audio.json`` also stores exact silence
evidence per 0.25 s window: ``nonzeroSamples`` counts, per channel, the WAV
samples that are not exactly zero. A spectral centroid is compared (2.0 Hz)
in every window except those whose count is zero on *both* sides, i.e. windows
the PCM proves are exactly all-zero; a rounded-zero RMS envelope alone is not
evidence of silence. A baseline without the field is rejected instead of
falling back to a weaker rule.

All current MNX, edit and excerpt artifacts share the platform index.

``semantic.json`` (the exported MSCZ) and ``semantic-source.json`` (the fixture
file) come from the ``semantic_dump`` dev tool next to the server binary. They
record musical content read from the in-memory DOM, independent of the MSCX
format version, and are the rebase oracle of roadmap-engrave A0/A2.

EID normalization preserves target identity through stable tokens.

Usage:
  golden.py capture --binary B --resources R --soundfont SF --golden DIR \\
                    [--allow-errors]
  golden.py compare --binary B --resources R --soundfont SF --golden DIR \\
                    --allow vendor/engrave/tests/data/golden/allow.json --output OUTDIR \\
                    [--platform windows|linux]
  golden.py pin --golden DIR --output OUTDIR [--platform windows|linux]
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import argparse
import asyncio
import base64
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from fractions import Fraction
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from support.composition_fixture import composition_musicxml
from support.musicxml_semantic import musicxml_semantic
from support.paths import REPO_ROOT

ROOT = REPO_ROOT
FIXTURE_DIR = ROOT / "vendor/engrave/tests/data/inputs" / "golden"
EXTRA_FIXTURES = (ROOT / "vendor/engrave/tests/data/inputs" / "evening.musicxml",)
# Fixtures without stored excerpts whose part excerpts are generated (after the master
# outputs are captured) to cover excerpt creation, e.g. ChordLine path copying.
GENERATE_EXCERPTS = {"chordline_path.mscx"}
MNX_FIXTURES = (
    "engine/two-notes.mnx",
    "project_examples/tupletSimple.mnx",
    "project_examples/percussionKit.mnx",
    "project_examples/graceArps.mnx",
)
AUDIO_TOLERANCES = {
    "peak": 1e-4,
    "rms": 1e-4,
    "rmsEnvelope": 1e-4,
    "spectralCentroidHz": 1.0,
    "centroidEnvelope": 2.0,
}
AUDIO_EXACT = {"sampleRate", "channels", "frames"}
# Per-window exact silence evidence (count of nonzero PCM samples per channel). It is
# written by describe_audio, gates the centroid comparison and is not a tolerance.
SILENCE_EVIDENCE = "nonzeroSamples"
ALLOW_KINDS = ("different", "extra")
DEFAULT_PLATFORM = "windows" if sys.platform.startswith("win") else "linux"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class GoldenFormatError(ValueError):
    """A golden file lacks evidence this golden.py requires; recapture it."""


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def write_json(path: Path, value, indent: int = 1) -> None:
    write_text(
        path,
        json.dumps(value, indent=indent, sort_keys=True, ensure_ascii=False, allow_nan=False) + "\n",
    )


EID_TAGS = (
    "eid",
    "linkedTo",
    "startElement",
    "endElement",
    "NoteEID",
    "mmRestLast",
    "startMeasure",
    "endMeasure",
    "sharedPart",
)


def normalize_mscx(text: str) -> str:
    eid_map: dict[str, str] = {}

    def replace_eid(match: re.Match) -> str:
        tag = match.group(1)
        val = match.group(2).strip()
        if not val:
            return f"<{tag}/>"
        if val not in eid_map:
            eid_map[val] = f"e{len(eid_map) + 1}"
        return f"<{tag}>{eid_map[val]}</{tag}>"

    tag_pattern = rf"<({'|'.join(EID_TAGS)})>([^<]+)</\1>"
    text = re.sub(tag_pattern, replace_eid, text)

    for tag in ("programVersion", "programRevision"):
        text = re.sub(rf"(<{tag}>)[^<]*(</{tag}>)", r"\1\2", text)
    for name in ("creationDate", "platform"):
        text = re.sub(rf'(<metaTag name="{name}">)[^<]*(</metaTag>)', r"\1\2", text)
    return text


def normalize_musicxml(text: str) -> str:
    for tag in ("encoding-date", "software"):
        text = re.sub(rf"(<{tag}>)[^<]*(</{tag}>)", r"\1\2", text)
    for name in ("creationDate", "platform"):
        text = re.sub(
            rf'(<miscellaneous-field name="{name}">)[^<]*(</miscellaneous-field>)',
            r"\1\2",
            text,
        )
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
        return {
            "width": image.width,
            "height": image.height,
            "mode": image.mode,
            "sha256": sha256(rgba.tobytes()),
        }


def describe_pdf(path: Path) -> dict:
    import pymupdf  # pinned in pixi.toml; cairo hides the volatile metadata in object streams

    document = pymupdf.open(stream=path.read_bytes(), filetype="pdf")
    try:
        pages = document.page_count
        document.set_metadata(
            {
                key: ""
                for key in (
                    "title",
                    "author",
                    "subject",
                    "keywords",
                    "creator",
                    "producer",
                    "creationDate",
                    "modDate",
                )
            }
        )
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
    envelope, centroids, nonzero = [], [], []
    for start in range(0, frames, window):
        chunk = audio[start : start + window]
        envelope.append([round(float(np.sqrt(np.mean(chunk[:, channel] ** 2))), 5) for channel in range(channels)])
        # Exact silence evidence: the stored RMS is rounded, so only a zero sample count
        # proves a window is all-zero PCM (and hence that its centroid means nothing).
        nonzero.append([int(np.count_nonzero(chunk[:, channel])) for channel in range(channels)])
        segment = mono[start : start + window]
        spectrum = np.abs(np.fft.rfft(segment))
        frequencies = np.fft.rfftfreq(len(segment), 1.0 / rate)
        weight = float(spectrum.sum())
        centroids.append(round(float(np.dot(frequencies, spectrum)) / weight, 1) if weight else 0.0)
    block = 1 << 20
    weighted = 0.0
    weight = 0.0
    for start in range(0, len(mono), block):
        chunk = mono[start : start + block]
        spectrum = np.abs(np.fft.rfft(chunk))
        frequencies = np.fft.rfftfreq(len(chunk), 1.0 / rate)
        weighted += float(np.dot(frequencies, spectrum))
        weight += float(spectrum.sum())
    return {
        "sampleRate": int(rate),
        "channels": int(channels),
        "frames": int(frames),
        "peak": peak,
        "rms": rms,
        "rmsEnvelope": envelope,
        "centroidEnvelope": centroids,
        "nonzeroSamples": nonzero,
        "spectralCentroidHz": round(weighted / weight, 1) if weight else 0.0,
    }


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
        inner = next(
            (
                name
                for name in names
                if name.lower().endswith((".xml", ".musicxml")) and not name.upper().startswith("META-INF/")
            ),
            None,
        )
        document = {"entries": names}
        if inner:
            normalized = normalize_musicxml(archive.read(inner).decode("utf-8"))
            document["musicxml"] = {
                "name": inner,
                "sha256": sha256(normalized.encode("utf-8")),
            }
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
        if data[position : position + 4] != b"MTrk":
            raise ValueError(f"expected MTrk at byte {position}")
        length = int.from_bytes(data[position + 4 : position + 8], "big")
        track = data[position + 8 : position + 8 + length]
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
                payload = track[cursor : cursor + size]
                cursor += size
                events.append(
                    [
                        tick,
                        {"type": "meta", "metaType": meta_type, "data": payload.hex()},
                    ]
                )
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
                events.append(
                    [
                        tick,
                        {"type": "sysex", "data": track[cursor : cursor + size].hex()},
                    ]
                )
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
                    events.append(
                        [
                            tick,
                            {
                                "type": "note_off",
                                "ch": channel,
                                "pitch": first,
                                "vel": second,
                            },
                        ]
                    )
                elif command == 0x90:
                    events.append(
                        [
                            tick,
                            {
                                "type": "note_on",
                                "ch": channel,
                                "pitch": first,
                                "vel": second,
                            },
                        ]
                    )
                elif command == 0xA0:
                    events.append(
                        [
                            tick,
                            {
                                "type": "poly_aftertouch",
                                "ch": channel,
                                "pitch": first,
                                "pressure": second,
                            },
                        ]
                    )
                elif command == 0xB0:
                    events.append(
                        [
                            tick,
                            {"type": "cc", "ch": channel, "num": first, "val": second},
                        ]
                    )
                else:
                    events.append(
                        [
                            tick,
                            {
                                "type": "pitchbend",
                                "ch": channel,
                                "value": first | (second << 7),
                            },
                        ]
                    )
            elif command in (0xC0, 0xD0):
                first = track[cursor]
                cursor += 1
                if command == 0xC0:
                    events.append([tick, {"type": "program", "ch": channel, "program": first}])
                else:
                    events.append([tick, {"type": "aftertouch", "ch": channel, "pressure": first}])
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
    """Export one v2 artifact to the stable path used by the comparison harness."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    selected = options.pop("excerpt", None)
    page = options.pop("page", 0)
    if options:
        raise ValueError(f"unsupported capture options: {options}")
    score = {"excerpt": selected} if selected is not None else None
    if fmt in ("measurePositions", "segmentPositions"):
        kind, export_options = (
            "positions",
            {"position_kind": "measure" if fmt.startswith("measure") else "segment"},
        )
    elif fmt in ("svg", "png", "pdf"):
        kind, export_options = "render", {"format": fmt}
        if fmt != "pdf":
            export_options.update(first_page=page, last_page=page)
    elif fmt == "wav":
        kind, export_options = "audio", {"format": fmt}
    else:
        kind, export_options = "score", {"format": fmt}
    if score is not None:
        export_options["score"] = score
    directory = fmt in ("svg", "png")
    with tempfile.TemporaryDirectory(prefix="render-", dir=dest.parent) as tmp:
        location = Path(tmp) if directory else dest
        payload, error = await call_tool(
            session,
            "score_export",
            {
                **identity,
                "kind": kind,
                "options": export_options,
                "destination": {
                    "kind": "directory" if directory else "file",
                    "path": str(location),
                },
            },
        )
        if error:
            return error
        artifacts = payload.get("artifacts", [])
        if len(artifacts) != 1:
            return f"expected one {fmt} artifact, got {len(artifacts)}"
        artifact = Path(artifacts[0]["path"])
        if not artifact.is_file() or artifact.stat().st_size != int(artifacts[0]["size"]):
            return f"invalid {fmt} artifact: {artifact}"
        if directory:
            shutil.copyfile(artifact, dest)
    return None


async def required(session, name, arguments):
    result, error = await call_tool(session, name, arguments)
    if error:
        raise RuntimeError(f"{name}: {error}")
    return result


async def open_fixture(session, source):
    if source.name == "created.json":
        xml = composition_musicxml(json.loads(read_text(source)))
        return await required(
            session,
            "score_open",
            {
                "data": base64.b64encode(xml.encode("utf-8")).decode("ascii"),
                "format": "musicxml",
            },
        )
    return await required(session, "score_open", {"path": str(source.resolve())})


async def generate_excerpts(session, identity):
    outline = await required(session, "score_outline", identity)
    parts = [{"kind": "id", "id": part["id"]} for part in outline["outline"]["parts"]]
    await required(
        session,
        "score_edit",
        {**identity, "ops": [{"op": "generate_excerpts", "parts": parts}]},
    )


def positions(path):
    result = load_rounded(path)
    result["pageSize"] = result.pop("page_size")
    if path.name.startswith("segment"):
        for item in result["elements"]:
            for key in ("x", "y"):
                if not float(item[key]).is_integer():
                    raise ValueError(f"non-integral segment {key}: {item[key]}")
                item[key] = int(item[key])
    return result


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
        write_text(
            out_dir / f"{name}.error.txt",
            f"transform failed: {type(exc).__name__}: {sanitize(str(exc), roots)}\n",
        )
        return None


async def value_export(session, identity, fmt, raw, out_dir, name, transform, roots, **options):
    error = await export(session, identity, fmt, raw, **options)
    if error:
        write_text(out_dir / f"{name}.error.txt", sanitize(error, roots) + "\n")
        return None
    try:
        value = transform(raw)
    except Exception as exc:  # noqa: BLE001
        write_text(
            out_dir / f"{name}.error.txt",
            f"transform failed: {type(exc).__name__}: {sanitize(str(exc), roots)}\n",
        )
        return None
    write_json(out_dir / name, value)
    return value


async def capture_master(session, identity, raw, out_dir, roots):
    normalized_mscx = normalize_mscx
    await text_export(
        session,
        identity,
        "mscx",
        raw / "score.mscx",
        out_dir,
        "score.mscx",
        normalized_mscx,
        roots,
    )
    await text_export(
        session,
        identity,
        "musicxml",
        raw / "score.musicxml",
        out_dir,
        "score.musicxml",
        normalize_musicxml,
        roots,
    )
    await value_export(
        session,
        identity,
        "mscz",
        raw / "score.mscz",
        out_dir,
        "mscz.json",
        describe_mscz,
        roots,
    )
    await value_export(
        session,
        identity,
        "mxl",
        raw / "score.mxl",
        out_dir,
        "mxl.json",
        describe_mxl,
        roots,
    )
    await value_export(
        session,
        identity,
        "midi",
        raw / "score.mid",
        out_dir,
        "midi.json",
        lambda path: describe_midi(path.read_bytes()),
        roots,
    )
    await value_export(
        session,
        identity,
        "measurePositions",
        raw / "measurePositions.json",
        out_dir,
        "measurePositions.json",
        positions,
        roots,
    )
    await value_export(
        session,
        identity,
        "segmentPositions",
        raw / "segmentPositions.json",
        out_dir,
        "segmentPositions.json",
        positions,
        roots,
    )
    info = await required(session, "score_outline", identity)
    pages = info["outline"]["pages"]
    for page in range(int(pages)):
        await text_export(
            session,
            identity,
            "svg",
            raw / f"page-{page}.svg",
            out_dir,
            f"page-{page}.svg",
            normalize_svg,
            roots,
            page=page,
        )
    await value_export(
        session,
        identity,
        "png",
        raw / "page-0.png",
        out_dir,
        "page-0.png.json",
        describe_png,
        roots,
        page=0,
    )
    await value_export(
        session,
        identity,
        "pdf",
        raw / "score.pdf",
        out_dir,
        "score.pdf.json",
        describe_pdf,
        roots,
    )
    await value_export(
        session,
        identity,
        "wav",
        raw / "score.wav",
        out_dir,
        "audio.json",
        describe_audio,
        roots,
    )
    # Format-independent content of the saved file (roadmap-engrave A0).
    semantic_dump(raw / "score.mscz", out_dir / "semantic.json", roots)


SEMANTIC_DUMP: dict = {}


def semantic_dump(source: Path, dest: Path, roots, *extra: str) -> None:
    """Run the semantic_dump dev tool next to the server binary; errors become .error.txt."""
    if not source.is_file():
        write_text(dest.with_suffix(".error.txt"), "source export missing\n")
        return
    result = subprocess.run(
        [
            str(SEMANTIC_DUMP["binary"]),
            str(SEMANTIC_DUMP["resources"]),
            str(source),
            *extra,
        ],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        write_text(
            dest.with_suffix(".error.txt"),
            sanitize(result.stderr.decode("utf-8", "replace"), roots) + "\n",
        )
        return
    write_json(dest, json.loads(result.stdout.decode("utf-8")))


async def capture_excerpts(session, identity, raw, out_dir, roots):
    payload = await required(session, "score_outline", identity)
    normalized_mscx = normalize_mscx
    for index, excerpt in enumerate(payload["outline"]["excerpts"]):
        excerpt_id = excerpt["id"]
        folder = out_dir / f"excerpt-{index}"
        await text_export(
            session,
            identity,
            "mscx",
            raw / f"excerpt-{index}.mscx",
            folder,
            "score.mscx",
            normalized_mscx,
            roots,
            excerpt=excerpt_id,
        )
        await text_export(
            session,
            identity,
            "musicxml",
            raw / f"excerpt-{index}.musicxml",
            folder,
            "score.musicxml",
            normalize_musicxml,
            roots,
            excerpt=excerpt_id,
        )
        await value_export(
            session,
            identity,
            "midi",
            raw / f"excerpt-{index}.mid",
            folder,
            "midi.json",
            lambda path: describe_midi(path.read_bytes()),
            roots,
            excerpt=excerpt_id,
        )
        await text_export(
            session,
            identity,
            "svg",
            raw / f"excerpt-{index}-page-0.svg",
            folder,
            "page-0.svg",
            normalize_svg,
            roots,
            excerpt=excerpt_id,
            page=0,
        )


async def capture_fixture(
    session,
    fixture_id,
    source,
    raw_root,
    out_dir,
    roots,
):
    raw = raw_root / fixture_id
    raw.mkdir(parents=True, exist_ok=True)
    payload = await open_fixture(session, source)
    identity = {"document": payload["document"]}
    if source.name != "created.json" and source.suffix.lower() != ".mnx":
        # The fixture as the engine reads it, before any save.
        # semantic_dump does not accept MNX; its exported MSCZ is still checked.
        semantic_dump(
            source,
            out_dir / "semantic-source.json",
            roots,
            *(["--generate-excerpts"] if fixture_id in GENERATE_EXCERPTS else []),
        )
    try:
        await capture_master(session, identity, raw, out_dir, roots)
        if fixture_id in GENERATE_EXCERPTS:
            await generate_excerpts(session, identity)
        await capture_excerpts(session, identity, raw, out_dir, roots)
    finally:
        await call_tool(session, "score_close", identity)


# Edit scenarios (roadmap-engrave A0): the edit tools' observable results, which A2 must
# preserve on 5.0. Each opens a source, applies one edit and captures the saved content.
EDIT_SCENARIOS = {
    "edit-replace-measures": {
        "source": "created.json",
        "kind": "replace-measures",
        "start_measure": 1,
        "notes": [
            {"pitches": [60, 64, 67], "ticks": 960},
            {"pitches": [62], "ticks": 480},
            {"pitches": [], "ticks": 240},
            {"pitches": [65], "ticks": 240},
        ],
    },
    "edit-set-pitches-coverage": {
        "source": "coverage.musicxml",
        "kind": "set-pitches",
        "part": 0,
        "measures": 2,
        "notes": 4,
        "delta": 2,
    },
    # Generated part excerpts are linked to the master, so the edit must reach them too
    # (linked_parts.mscx is unusable here: every note in it is tied).
    "edit-set-pitches-linked": {
        "source": "chordline_path.mscx",
        "kind": "set-pitches",
        "generate": True,
        "part": 0,
        "measures": 2,
        "notes": 3,
        "delta": 1,
    },
}


async def capture_edit(session, name, scenario, raw_root, out_dir, roots):
    raw = raw_root / name
    raw.mkdir(parents=True, exist_ok=True)
    source = FIXTURE_DIR / scenario["source"]
    payload = await open_fixture(session, source)
    identity = {"document": payload["document"]}
    try:
        await apply_edit_scenario(session, identity, name, scenario, raw)
        await text_export(
            session,
            identity,
            "mscx",
            raw / "score.mscx",
            out_dir,
            "score.mscx",
            normalize_mscx,
            roots,
        )
        await text_export(
            session,
            identity,
            "musicxml",
            raw / "score.musicxml",
            out_dir,
            "score.musicxml",
            normalize_musicxml,
            roots,
        )
        await value_export(
            session,
            identity,
            "midi",
            raw / "score.mid",
            out_dir,
            "midi.json",
            lambda path: describe_midi(path.read_bytes()),
            roots,
        )
        error = await export(session, identity, "mscz", raw / "score.mscz")
        if error:
            write_text(out_dir / "mscz.error.txt", sanitize(error, roots) + "\n")
        else:
            semantic_dump(raw / "score.mscz", out_dir / "semantic.json", roots)
    finally:
        await call_tool(session, "score_close", identity)


async def apply_edit_scenario(session, identity, name, scenario, raw):
    """Apply the pinned edit scenario through score_apply operations."""
    if scenario.get("generate"):
        await generate_excerpts(session, identity)
    outline = await required(session, "score_outline", identity)
    if scenario["kind"] == "replace-measures":
        measure = outline["outline"]["measures"][scenario["start_measure"]]
        tick = Fraction(int(measure["tick"]["num"]), int(measure["tick"]["den"]))
        ops = []
        for item in scenario["notes"]:
            duration = Fraction(item["ticks"], 1920)
            ops.append(
                {
                    "op": "enter_notes",
                    "at": {
                        "kind": "segment",
                        "measure": measure["id"],
                        "staff": 0,
                        "voice": 0,
                        "tick": {
                            "num": str(tick.numerator),
                            "den": str(tick.denominator),
                        },
                        "segment_kind": "ChordRest",
                    },
                    "duration": {
                        "num": str(duration.numerator),
                        "den": str(duration.denominator),
                    },
                    "pitches": [{"pitch": pitch} for pitch in item["pitches"]],
                }
            )
            tick += duration
        # Each entry can create the segment needed by the next one. V2
        # resolves addresses at batch start, so publish them in sequence.
        revision = outline["revision"]
        for op in ops:
            result = await required(
                session,
                "score_edit",
                {**identity, "expected_revision": revision, "ops": [op]},
            )
            revision = result["revision"]
        return
    else:
        staves = [staff["index"] for staff in outline["outline"]["parts"][scenario["part"]]["staves"]]
        view = await required(
            session,
            "score_query",
            {
                **identity,
                "mode": "elements",
                "query": {
                    "types": ["Note"],
                    "staves": staves,
                    "first_measure": 0,
                    "measure_count": scenario["measures"],
                    "props": ["pitch"],
                    "limit": 100,
                },
            },
        )
        # The old region omitted grace notes. Read the exported fixed fixture to
        # identify their chord IDs, then use the old segment/staff/voice order.
        before = raw / "before.mscx"
        error = await export(session, identity, "mscx", before)
        if error:
            raise RuntimeError(f"pre-edit export: {error}")
        grace = {
            chord.findtext("eid")
            for chord in ET.parse(before).iter("Chord")
            if any(
                chord.find(tag) is not None
                for tag in (
                    "acciaccatura",
                    "appoggiatura",
                    "grace4",
                    "grace16",
                    "grace32",
                )
            )
        }
        candidates = [n for n in view["elements"] if n["parent"] not in grace]
        candidates.sort(
            key=lambda n: (
                Fraction(int(n["tick"]["num"]), int(n["tick"]["den"])),
                n["staff"],
                n["voice"],
            )
        )
        targets = candidates[: scenario["notes"]]
        signatures = [
            (
                int(n["props"][0]["value"]["value"]),
                Fraction(int(n["tick"]["num"]), int(n["tick"]["den"])),
                n["staff"],
            )
            for n in targets
        ]
        expected = {
            "edit-set-pitches-coverage": [
                (72, Fraction(0), 0),
                (74, Fraction(1, 4), 0),
                (53, Fraction(1, 4), 1),
                (76, Fraction(1, 3), 0),
            ],
            "edit-set-pitches-linked": [
                (72, Fraction(0), 0),
                (74, Fraction(1, 4), 0),
                (76, Fraction(1, 2), 0),
            ],
        }[name]
        if signatures != expected:
            raise ValueError(f"{name}: fixed untied region changed: {signatures!r}")
        ops = [
            {
                "op": "set_pitch",
                "note": {"kind": "id", "id": n["id"]},
                "pitch": int(n["props"][0]["value"]["value"]) + scenario["delta"],
            }
            for n in targets
        ]
    await required(
        session,
        "score_edit",
        {**identity, "expected_revision": outline["revision"], "ops": ops},
    )


def fixture_files() -> list[Path]:
    files = sorted(path for path in FIXTURE_DIR.iterdir() if path.is_file() and path.name != "README.md")
    return files + [path for path in EXTRA_FIXTURES if path.is_file()]


def mnx_fixture_files() -> list[Path]:
    """Engine-owned MNX inputs covered by the platform baseline."""
    engine = Path(os.environ.get("MUSESCORE_ENGINE_SOURCE", ROOT / "vendor/engrave"))
    data = engine / "src/importexport/mnx/tests/data"
    files = [data / name for name in MNX_FIXTURES]
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"missing MNX fixtures: {missing}")
    return files


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


async def capture_tree(
    binary,
    resources,
    soundfont,
    target,
    log_path,
):
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
            cwd=str(raw_root),
            env=dict(os.environ),
        )
        with log_path.open("w", encoding="utf-8") as log:
            async with stdio_client(params, errlog=log) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    for source in fixture_files() + mnx_fixture_files():
                        try:
                            await capture_fixture(
                                session,
                                source.name,
                                source,
                                raw_root,
                                target / source.name,
                                [raw_root, target],
                            )
                        except Exception as exc:  # noqa: BLE001 - keep other fixtures going
                            write_text(
                                target / source.name / "capture.error.txt",
                                f"{type(exc).__name__}: {sanitize(str(exc), [raw_root, target])}\n",
                            )
                    for name, scenario in EDIT_SCENARIOS.items():
                        try:
                            await capture_edit(
                                session,
                                name,
                                scenario,
                                raw_root,
                                target / name,
                                [raw_root, target],
                            )
                        except Exception as exc:  # noqa: BLE001
                            write_text(
                                target / name / "capture.error.txt",
                                f"{type(exc).__name__}: {sanitize(str(exc), [raw_root, target])}\n",
                            )
    return build_index(target)


def load_allow(path: Path, context: str = "host") -> list[dict]:
    if not path.is_file():
        return []
    entries = json.loads(read_text(path)).get("entries", [])
    for entry in entries:
        missing = [key for key in ("fixture", "output", "step", "reason") if key not in entry]
        if missing:
            raise ValueError(f"allow entry {entry!r} is missing {', '.join(missing)}")
        if entry.setdefault("kind", "different") not in ALLOW_KINDS:
            raise ValueError(f"allow entry has invalid kind: {entry['kind']!r}")
        for key in ("fixture", "output", "step", "reason"):
            if not isinstance(entry[key], str) or not entry[key].strip():
                raise ValueError(f"invalid allow {key}")
        if any(c in entry[key] for key in ("fixture", "output") for c in "*?["):
            raise ValueError("allow entries must name exact artifacts")
        if entry.get("context") not in ("host", "engine-gate-archive"):
            raise ValueError("allow entries require an explicit context: host or engine-gate-archive")
        pins = entry.get("sha256")
        if (
            not isinstance(pins, dict)
            or not pins
            or any(p not in ("windows", "linux") or not re.fullmatch(r"[0-9a-f]{64}", str(h)) for p, h in pins.items())
        ):
            raise ValueError("allow entries require explicit platform SHA-256 pins")
    return [e for e in entries if e["context"] == context]


def allow_accept(entries, fixture, output, kind, actual_sha, platform):
    """Return (position, entry, problem) for the first matching allow entry."""
    matches = [
        (position, entry)
        for position, entry in enumerate(entries)
        if fixture == entry["fixture"] and output == entry["output"]
    ]
    problems = []
    for position, entry in matches:
        if entry["kind"] != kind:
            problems.append(f"entry {position}: kind {entry['kind']} != {kind}")
            continue
        if kind not in ALLOW_KINDS:
            continue
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
        present = {path.relative_to(root / name).as_posix() for path in (root / name).rglob("*") if path.is_file()}
        issues += [f"missing indexed file {name}/{entry}" for entry in sorted(set(index[name]) - present)]
        issues += [f"unindexed file {name}/{entry}" for entry in sorted(present - set(index[name]))]
        for entry in sorted(present & set(index[name])):
            actual = sha256((root / name / entry).read_bytes())
            if actual != index[name][entry]:
                issues.append(f"indexed file hash mismatch {name}/{entry}: expected {index[name][entry]}, got {actual}")
    return issues


def silence_windows(audio: dict) -> list[bool]:
    """Per-window exact-silence flags from the stored PCM evidence."""
    counts = audio.get(SILENCE_EVIDENCE)
    if counts is None:
        raise GoldenFormatError(
            f"audio.json has no {SILENCE_EVIDENCE!r} silence evidence, so exact silence (and "
            "therefore whether a centroid window may be skipped) cannot be established; "
            "recapture the baseline with the current tests/support/golden.py"
        )
    windows = []
    for window in counts:
        if not isinstance(window, (list, tuple)):
            raise GoldenFormatError(f"audio.json {SILENCE_EVIDENCE!r} values must be per-channel lists")
        try:
            windows.append(all(int(count) == 0 for count in window))
        except (TypeError, ValueError):
            raise GoldenFormatError(f"audio.json {SILENCE_EVIDENCE!r} values must be sample counts") from None
    return windows


def audio_equal(golden, actual, key=None) -> bool:
    if isinstance(golden, dict) and isinstance(actual, dict):
        if "centroidEnvelope" in golden or "centroidEnvelope" in actual:
            # Exact silence evidence is required on both sides before any window is skipped;
            # a baseline without it is rejected, not compared under a weaker rule.
            g_silent = silence_windows(golden)
            a_silent = silence_windows(actual)
        if golden.keys() != actual.keys():
            return False
        has_centroids = "centroidEnvelope" in golden and "centroidEnvelope" in actual
        has_rms = "rmsEnvelope" in golden and "rmsEnvelope" in actual
        for name in golden:
            if name == SILENCE_EVIDENCE:
                # Evidence for the centroid gate below, not a compared tolerance: the count of
                # nonzero samples legitimately differs in reverb/release tails.
                continue
            if name == "centroidEnvelope" and has_centroids and has_rms:
                g_cents = golden["centroidEnvelope"]
                a_cents = actual["centroidEnvelope"]
                if len(g_cents) != len(a_cents) or len(g_cents) != len(g_silent) or len(a_cents) != len(a_silent):
                    return False
                tol = AUDIO_TOLERANCES.get("centroidEnvelope", 2.0)
                for i, (g_c, a_c) in enumerate(zip(g_cents, a_cents)):
                    # Only a window whose PCM samples are exactly all-zero on BOTH sides has no
                    # meaningful spectral centroid. A rounded-zero RMS is not such proof, and
                    # quiet nonzero windows (e.g. RMS 0.00010) are compared like any other.
                    if g_silent[i] and a_silent[i]:
                        continue
                    if abs(g_c - a_c) > tol:
                        return False
            else:
                if not audio_equal(golden[name], actual[name], name):
                    return False
        return True
    if isinstance(golden, list) and isinstance(actual, list):
        return len(golden) == len(actual) and all(audio_equal(left, right, key) for left, right in zip(golden, actual))
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
        except (json.JSONDecodeError, KeyError):
            return False
    return golden.read_bytes() == actual.read_bytes()


def write_diff(root: Path, fixture: str, output: str, golden: Path, actual: Path) -> None:
    lines = list(
        difflib.unified_diff(
            read_text(golden).splitlines(keepends=True),
            read_text(actual).splitlines(keepends=True),
            fromfile=f"golden/{fixture}/{output}",
            tofile=f"actual/{fixture}/{output}",
        )
    )
    if len(lines) > 2000:
        lines = lines[:2000] + ["... truncated after 2000 diff lines ...\n"]
    write_text(
        root / fixture / f"{output}.diff",
        "".join(line if line.endswith("\n") else line + "\n" for line in lines),
    )


def verify_input_provenance() -> list[str]:
    """Check current inputs against main's reviewed migration pins."""
    try:
        pins = json.loads(read_text(ROOT / "vendor/engrave/tests/data/golden/inputs.json"))["fixtures"]
        sources = {source.name: source for source in fixture_files() + mnx_fixture_files()}
        sources.update({name: FIXTURE_DIR / scenario["source"] for name, scenario in EDIT_SCENARIOS.items()})
        if set(pins) != set(sources):
            return ["input fixture inventory differs from reviewed pins"]
        issues = []
        for name, source in sources.items():
            entry = pins[name]
            if entry["fixture_sha256"] != sha256(source.read_bytes()):
                issues.append(f"input fixture changed: {name}")
            if name in EDIT_SCENARIOS:
                scenario = json.dumps(
                    EDIT_SCENARIOS[name], sort_keys=True, ensure_ascii=False, separators=(",", ":")
                ).encode("utf-8")
                if entry.get("scenario_sha256") != sha256(scenario):
                    issues.append(f"edit scenario changed: {name}")
            if source.name == "created.json":
                xml = composition_musicxml(json.loads(read_text(source))).encode("utf-8")
                if entry.get("input_sha256") != sha256(xml) or entry.get("converter_sha256") != sha256(
                    (ROOT / "vendor/engrave/tests/support/composition_fixture.py").read_bytes()
                ):
                    issues.append(f"created input or converter changed: {name}")
        return issues
    except (OSError, ValueError, KeyError, TypeError) as exc:
        return [f"invalid input provenance: {exc}"]


def compare_trees(
    golden_root: Path,
    actual_root: Path,
    allow: list[dict],
    output: Path,
    platform: str,
) -> int:
    golden_index = json.loads(read_text(golden_root / "index.json"))
    counts = {
        key: 0
        for key in (
            "identical",
            "allowed",
            "different",
            "missing",
            "extra",
            "goldenError",
            "extraFixture",
            "index",
        )
    }
    report = {
        "golden": str(golden_root),
        "actual": str(actual_root),
        "fixtures": {},
        "stale": [],
        "extraFixtures": [],
        "indexIssues": [],
        "counts": counts,
        "status": "pass",
    }
    matched_allow: set[int] = set()
    diff_root = output / "diff"
    if diff_root.exists():
        shutil.rmtree(diff_root)

    report["indexIssues"].extend(verify_golden_index(golden_root, golden_index))
    report["indexIssues"].extend(verify_input_provenance())
    counts["index"] = len(report["indexIssues"])
    for issue in report["indexIssues"]:
        print(f"index    {issue}")
    actual_fixtures = {path.name for path in actual_root.iterdir() if path.is_dir()}
    for fixture in sorted(actual_fixtures - set(golden_index)):
        report["extraFixtures"].append(fixture)
        counts["extraFixture"] += 1
        print(f"extra fixture  {fixture}")

    for fixture in sorted(golden_index):
        expected = golden_index[fixture]
        golden_dir, actual_dir = golden_root / fixture, actual_root / fixture
        actual_files = (
            {path.relative_to(actual_dir).as_posix() for path in actual_dir.rglob("*") if path.is_file()}
            if actual_dir.is_dir()
            else set()
        )
        results = report["fixtures"].setdefault(fixture, {})
        for name in sorted(set(expected) | actual_files):
            golden_file = golden_dir / name
            actual_file = actual_dir / name
            golden_sha = sha256(golden_file.read_bytes()) if golden_file.is_file() else None
            actual_sha = sha256(actual_file.read_bytes()) if actual_file.is_file() else None
            golden_error = golden_sha is not None and name.endswith(".error.txt")
            golden_format = None
            if actual_sha is None:
                kind = "missing"
            elif name not in expected:
                kind = "extra"
            elif golden_sha is None:
                kind = "different"
            else:
                try:
                    same = files_equal(golden_file, actual_file, name)
                except GoldenFormatError as exc:
                    # A baseline missing required evidence is an error, not a difference an
                    # allow entry may cover: it must be recaptured.
                    same, golden_format = False, str(exc)
                kind = None if same and not golden_error else "different"
            if kind is None:
                counts["identical"] += 1
                results[name] = {"status": "identical", "sha256": actual_sha}
                continue
            if golden_format:
                counts["goldenError"] += 1
                results[name] = {
                    "status": "golden-format",
                    "kind": kind,
                    "goldenSha256": golden_sha,
                    "actualSha256": actual_sha,
                    "error": golden_format,
                }
                print(f"golden-format {fixture}/{name}: {golden_format}")
                continue

            position, entry, problem = allow_accept(allow, fixture, name, kind, actual_sha, platform)
            if entry:
                matched_allow.add(position)
                counts["allowed"] += 1
                results[name] = {
                    "status": "allowed",
                    "kind": kind,
                    "step": entry["step"],
                    "reason": entry["reason"],
                    "actualSha256": actual_sha,
                    "goldenSha256": golden_sha,
                }
                print(f"allowed  {fixture}/{name} ({kind}; {entry['step']}: {entry['reason']})")
            else:
                status = "golden-error" if golden_error else kind
                counts["goldenError" if golden_error else kind] += 1
                results[name] = {
                    "status": status,
                    "kind": kind,
                    "goldenSha256": golden_sha,
                    "actualSha256": actual_sha,
                }
                suffix = f" (allow mismatch: {problem})" if problem else ""
                label = "missing" if kind == "missing" else status
                print(f"{label:<12} {fixture}/{name} actual sha256={actual_sha or 'none'}{suffix}")
            if kind == "different" and golden_file.is_file() and actual_sha != golden_sha:
                write_diff(diff_root, fixture, name, golden_file, actual_file)

    for position, entry in enumerate(allow):
        # An entry is only expected to match on the platforms it pins.
        if position not in matched_allow and platform in entry["sha256"]:
            report["stale"].append(entry)
            print(
                f"stale    allow entry fixture={entry['fixture']} output={entry['output']} "
                f"({entry['step']}: {entry['reason']})"
            )

    failing = sum(
        counts[key]
        for key in (
            "different",
            "missing",
            "extra",
            "goldenError",
            "extraFixture",
            "index",
        )
    )
    failing += len(report["stale"])
    report["status"] = "pass" if failing == 0 else "fail"
    write_json(output / "report.json", report, indent=2)
    total = sum(
        counts[key]
        for key in (
            "identical",
            "allowed",
            "different",
            "missing",
            "extra",
            "goldenError",
        )
    )
    print(f"golden compare: {len(golden_index)} fixtures, {total} outputs")
    for key in counts:
        print(f"{key}: {counts[key]}")
    print(f"result: {'PASS' if failing == 0 else 'FAIL'}")
    return 0 if failing == 0 else 1


def command_capture(args) -> int:
    args.golden.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="golden-log-") as temporary:
        index = asyncio.run(
            asyncio.wait_for(
                capture_tree(
                    args.binary,
                    args.resources,
                    args.soundfont,
                    args.golden,
                    Path(temporary) / "server-stderr.log",
                ),
                timeout=1800,
            )
        )
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
        asyncio.run(
            asyncio.wait_for(
                capture_tree(
                    args.binary,
                    args.resources,
                    args.soundfont,
                    actual,
                    Path(temporary) / "server-stderr.log",
                ),
                timeout=1800,
            )
        )
        # Keep a copy of the server log for debugging (outside the compared tree).
        shutil.copyfile(Path(temporary) / "server-stderr.log", args.output / "server-stderr.log")
    return compare_trees(args.golden, actual, allow, args.output, args.platform)


def command_pin(args) -> int:
    report_path = args.output / "report.json"
    if not report_path.is_file():
        raise SystemExit(f"report does not exist: {report_path}")
    report = json.loads(read_text(report_path))
    if not isinstance(report.get("fixtures"), dict):
        raise SystemExit("invalid golden report: expected fixtures mapping")
    entries = []
    for fixture, results in sorted(report["fixtures"].items()):
        for name, result in sorted(results.items()):
            if result.get("status") not in (
                "different",
                "missing",
                "extra",
                "golden-error",
            ):
                continue
            kind = result.get("kind") or result["status"]
            sha = {} if kind == "missing" else {"sha256": {args.platform: result["actualSha256"]}}
            entries.append(
                {
                    "fixture": fixture,
                    "output": name,
                    "step": "TODO",
                    "reason": "TODO",
                    "kind": kind,
                    **sha,
                }
            )
    print(json.dumps(entries, indent=1, sort_keys=True))
    return int(bool(entries))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
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
    for label, path in (
        ("binary", args.binary),
        ("resources", args.resources),
        ("soundfont", args.soundfont),
    ):
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
