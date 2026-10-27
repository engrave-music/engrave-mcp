"""Golden-output capture and comparison for the native MCP server (roadmap step 0).

Capture exports a fixed fixture set through the official Python MCP client,
normalizes known-volatile fields, and stores one directory per fixture plus an
``index.json`` manifest of sha256 values. Compare re-captures the same tree and
diffs it against the stored goldens, honoring an explicit allow-list.

Volatile fields removed by normalization (observed by running twice):
  * MSCX/MSCZ inner ``<eid>`` values: random per created document. Each unique
    value is replaced by ``eid-<n>`` in first-occurrence order so equalities
    between references survive.
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
the documented numeric tolerances.

Usage:
  golden.py capture --binary B --resources R --soundfont SF --golden DIR
  golden.py compare --binary B --resources R --soundfont SF --golden DIR \\
                    --allow tests/golden/allow.json --output OUTDIR
"""

from __future__ import annotations

import argparse
import asyncio
import difflib
import hashlib
import json
import re
import shutil
import sys
import tempfile
import zipfile
from fnmatch import fnmatch
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "fixtures" / "golden"
EXTRA_FIXTURES = (ROOT / "fixtures" / "evening.musicxml",)
AUDIO_TOLERANCES = {"peak": 1e-4, "rms": 1e-4, "rmsEnvelope": 1e-4, "spectralCentroidHz": 1.0}
AUDIO_EXACT = {"sampleRate", "channels", "frames"}


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
    seen: dict[str, str] = {}

    def eid(match: re.Match) -> str:
        value = match.group(1)
        if value not in seen:
            seen[value] = f"eid-{len(seen)}"
        return f"<eid>{seen[value]}</eid>"

    text = re.sub(r"<eid>([^<]*)</eid>", eid, text)
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
    envelope = []
    for start in range(0, frames, window):
        chunk = audio[start:start + window]
        envelope.append([round(float(np.sqrt(np.mean(chunk[:, channel] ** 2))), 5)
                         for channel in range(channels)])
    mono = audio.mean(axis=1)
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
        return write_text(out_dir / name, normalize(read_text(raw)))
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
    try:
        await capture_master(session, identity, raw, out_dir, roots)
        await capture_excerpts(session, identity, raw, out_dir, roots)
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
    return build_index(target)


def load_allow(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    document = json.loads(read_text(path))
    entries = document.get("entries", [])
    for entry in entries:
        missing = [key for key in ("fixture", "output", "step", "reason") if key not in entry]
        if missing:
            raise ValueError(f"allow entry {entry!r} is missing {', '.join(missing)}")
    return entries


def allow_match(entries, fixture, output):
    for position, entry in enumerate(entries):
        if fnmatch(fixture, entry["fixture"]) and fnmatch(output, entry["output"]):
            return position, entry
    return None, None


def audio_equal(golden, actual, key=None) -> bool:
    if isinstance(golden, dict) and isinstance(actual, dict):
        return golden.keys() == actual.keys() and all(
            audio_equal(golden[name], actual[name], name) for name in golden)
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


def compare_trees(golden_root: Path, actual_root: Path, allow: list[dict], output: Path) -> int:
    golden_index = json.loads(read_text(golden_root / "index.json"))
    report = {"golden": str(golden_root), "actual": str(actual_root), "fixtures": {},
              "stale": [], "counts": {}, "status": "pass"}
    counts = {"identical": 0, "allowed": 0, "different": 0, "missing": 0, "extra": 0}
    matched_allow: set[int] = set()
    diff_root = output / "diff"
    if diff_root.exists():
        shutil.rmtree(diff_root)

    for fixture in sorted(golden_index):
        expected = golden_index[fixture]
        golden_dir = golden_root / fixture
        actual_dir = actual_root / fixture
        actual_files = set()
        if actual_dir.is_dir():
            actual_files = {path.relative_to(actual_dir).as_posix()
                            for path in actual_dir.rglob("*") if path.is_file()}
        results = {}
        for name in sorted(set(expected) | actual_files):
            golden_file, actual_file = golden_dir / name, actual_dir / name
            if name not in expected:
                position, entry = allow_match(allow, fixture, name)
                if entry:
                    matched_allow.add(position)
                    counts["allowed"] += 1
                    results[name] = {"status": "allowed", "step": entry["step"],
                                     "reason": entry["reason"]}
                    print(f"allowed  {fixture}/{name} ({entry['step']}: {entry['reason']})")
                else:
                    counts["extra"] += 1
                    results[name] = {"status": "extra"}
                    print(f"extra    {fixture}/{name}")
            elif not actual_file.is_file():
                position, entry = allow_match(allow, fixture, name)
                if entry:
                    matched_allow.add(position)
                    counts["allowed"] += 1
                    results[name] = {"status": "allowed", "step": entry["step"],
                                     "reason": entry["reason"]}
                    print(f"allowed  {fixture}/{name} (missing; {entry['step']}: {entry['reason']})")
                else:
                    counts["missing"] += 1
                    results[name] = {"status": "missing"}
                    print(f"missing  {fixture}/{name}")
            elif files_equal(golden_file, actual_file, name):
                counts["identical"] += 1
                results[name] = {"status": "identical",
                                 "sha256": sha256(actual_file.read_bytes())}
            else:
                position, entry = allow_match(allow, fixture, name)
                if entry:
                    matched_allow.add(position)
                    counts["allowed"] += 1
                    results[name] = {"status": "allowed", "step": entry["step"],
                                     "reason": entry["reason"],
                                     "goldenSha256": sha256(golden_file.read_bytes()),
                                     "actualSha256": sha256(actual_file.read_bytes())}
                    print(f"allowed  {fixture}/{name} ({entry['step']}: {entry['reason']})")
                else:
                    counts["different"] += 1
                    results[name] = {"status": "different",
                                     "goldenSha256": sha256(golden_file.read_bytes()),
                                     "actualSha256": sha256(actual_file.read_bytes())}
                    print(f"DIFF     {fixture}/{name}")
                    write_diff(diff_root, fixture, name, golden_file, actual_file)
        if results:
            report["fixtures"][fixture] = results

    for position, entry in enumerate(allow):
        if position not in matched_allow:
            report["stale"].append(entry)
            print(f"stale    allow entry fixture={entry['fixture']} output={entry['output']} "
                  f"({entry['step']}: {entry['reason']})")

    report["counts"] = counts
    failing = counts["different"] + counts["missing"] + counts["extra"]
    report["status"] = "pass" if failing == 0 else "fail"
    write_json(output / "report.json", report, indent=2)
    total = sum(counts.values())
    print(f"golden compare: {len(golden_index)} fixtures, {total} outputs")
    for key in ("identical", "allowed", "different", "missing", "extra"):
        print(f"{key}: {counts[key]}")
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
    return compare_trees(args.golden, actual, allow, args.output)


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
        if name == "compare":
            sub.add_argument("--allow", type=Path, required=True)
            sub.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
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

