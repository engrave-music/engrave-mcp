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
format version, and provide an independent musical-content oracle.

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

import argparse
import asyncio
import base64
import hashlib
import inspect
import json
import os
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET
from fractions import Fraction
from pathlib import Path

from engrave_testing import engine_audio as _engine_audio
from engrave_testing import golden as _engine_golden
from engrave_testing.musicxml_semantic import musicxml_semantic
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from engrave_mcp_testing.paths import REPO_ROOT

GoldenFormatError = _engine_golden.GoldenFormatError
# Use the fork's converter: its bytes are what the input provenance check authenticates.
composition_musicxml = _engine_golden.composition_musicxml
allow_accept = _engine_golden.allow_accept
audio_equal = _engine_golden.audio_equal
describe_audio = _engine_golden.describe_audio
describe_midi = _engine_golden.describe_midi
describe_mscz = _engine_golden.describe_mscz
describe_mxl = _engine_golden.describe_mxl
describe_pdf = _engine_golden.describe_pdf
describe_png = _engine_golden.describe_png
files_equal = _engine_golden.files_equal
image_size = _engine_golden.image_size
load_allow = _engine_golden.load_allow
load_rounded = _engine_golden.load_rounded
normalize_mscx = _engine_golden.normalize_mscx
normalize_musicxml = _engine_golden.normalize_musicxml
normalize_svg = _engine_golden.normalize_svg
read_text = _engine_golden.read_text
round_json = _engine_golden.round_json
sha256 = _engine_golden.sha256
silence_windows = _engine_golden.silence_windows
verify_golden_index = _engine_golden.verify_golden_index
write_json = _engine_golden.write_json
write_text = _engine_golden.write_text

positions = _engine_golden.positions
sanitize = _engine_golden.sanitize
reset_root = _engine_golden.reset_root
build_index = _engine_golden.build_index
write_diff = _engine_golden.write_diff
command_pin = _engine_golden.command_pin


FIXTURE_DIR = REPO_ROOT / "vendor/engrave/tests/data/inputs" / "golden"
EXTRA_FIXTURES = (REPO_ROOT / "vendor/engrave/tests/data/inputs" / "evening.musicxml",)
# Fixtures without stored excerpts whose part excerpts are generated (after the master
# outputs are captured) to cover excerpt creation, e.g. ChordLine path copying.
GENERATE_EXCERPTS = _engine_golden.GENERATE_EXCERPTS
MNX_FIXTURES = _engine_golden.MNX_FIXTURES
AUDIO_TOLERANCES = _engine_golden.AUDIO_TOLERANCES
AUDIO_EXACT = _engine_golden.AUDIO_EXACT
# Per-window exact silence evidence (count of nonzero PCM samples per channel). It is
# written by describe_audio, gates the centroid comparison and is not a tolerance.
SILENCE_EVIDENCE = _engine_golden.SILENCE_EVIDENCE
ALLOW_KINDS = ("different", "extra")
DEFAULT_PLATFORM = "windows" if sys.platform.startswith("win") else "linux"


EID_TAGS = _engine_golden.EID_TAGS


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
    """Export one artifact to the stable path used by the comparison harness."""
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


def created_input_pin() -> str:
    """Reviewed hash of the MusicXML sent for created.json, from the authenticated manifest."""
    manifest = _engine_golden.REPO_ROOT / "tests/data/golden/inputs.json"
    data = manifest.read_bytes()
    if sha256(data) != _engine_golden.INPUT_MANIFEST_SHA256:
        raise RuntimeError("input provenance manifest hash mismatch")
    return json.loads(data)["fixtures"]["created.json"]["input_sha256"]


async def open_fixture(session, source):
    if source.name == "created.json":
        data = composition_musicxml(json.loads(read_text(source))).encode("utf-8")
        if sha256(data) != created_input_pin():
            raise RuntimeError("created.json converted to unreviewed MusicXML bytes")
        return await required(
            session,
            "score_open",
            {
                "data": base64.b64encode(data).decode("ascii"),
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


async def text_export(session, identity, fmt, raw, out_dir, name, normalize, roots, **options):
    error = await export(session, identity, fmt, raw, **options)
    if error:
        write_text(out_dir / f"{name}.error.txt", sanitize(error, roots) + "\n")
        return None
    try:
        text = normalize(read_text(raw))
        if normalize is normalize_musicxml:
            # Engine-independent content, the semantic oracle for MusicXML (musical content).
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
    info = await required(session, "score_query", {**identity, "mode": "page_count"})
    pages = info["pages"]
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
    # Format-independent content of the saved file (musical content).
    await semantic_dump_async(raw / "score.mscz", out_dir / "semantic.json", roots)


SEMANTIC_DUMP: dict = {}

_SLOW_GOLDEN_FIRST = (
    "chordline_path.mscx",
    "graceArps.mnx",
    "evening.musicxml",
    "slurs.mscz",
    "layout_elements.mscx",
    "pedal.mscx",
    "repeat_with_2_voltas.mscx",
    "bends.mid",
)


def semantic_dump(source: Path, dest: Path, roots, *extra: str) -> None:
    _engine_golden.semantic_dump(source, dest, roots, *extra, config=SEMANTIC_DUMP)


async def semantic_dump_async(source: Path, dest: Path, roots, *extra: str) -> None:
    if not source.is_file():
        write_text(dest.with_suffix(".error.txt"), "source export missing\n")
        return
    config = SEMANTIC_DUMP
    proc = await asyncio.create_subprocess_exec(
        str(config["binary"]),
        str(config["resources"]),
        str(source),
        *extra,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=120)
    except TimeoutError:
        proc.kill()
        await proc.communicate()
        write_text(dest.with_suffix(".error.txt"), "semantic_dump timed out after 120s\n")
        return
    except BaseException:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        raise
    if proc.returncode != 0:
        write_text(
            dest.with_suffix(".error.txt"),
            sanitize(stderr.decode("utf-8", "replace"), roots) + "\n",
        )
        return
    write_json(dest, json.loads(stdout.decode("utf-8")))


def verify_input_provenance(*, root=None, sources=None, fixture_dir=None) -> list[str]:
    return _engine_golden.verify_input_provenance(
        root=REPO_ROOT / "vendor/engrave" if root is None else root,
        sources=fixture_files() + mnx_fixture_files() if sources is None else sources,
        fixture_dir=FIXTURE_DIR if fixture_dir is None else fixture_dir,
        scenarios=EDIT_SCENARIOS,
    )


def compare_trees(golden_root, actual_root, allow, output, platform) -> int:
    return _engine_golden.compare_trees(
        golden_root,
        actual_root,
        allow,
        output,
        platform,
        provenance_check=verify_input_provenance,
    )


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
        await semantic_dump_async(
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


# Edit scenarios (musical content): the edit tools' observable results, which the host must
# preserve on 5.0. Each opens a source, applies one edit and captures the saved content.
EDIT_SCENARIOS = _engine_golden.EDIT_SCENARIOS


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
            await semantic_dump_async(raw / "score.mscz", out_dir / "semantic.json", roots)
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
    engine = Path(os.environ.get("MUSESCORE_ENGINE_SOURCE", REPO_ROOT / "vendor/engrave"))
    data = engine / "src/importexport/mnx/tests/data"
    files = [data / name for name in MNX_FIXTURES]
    missing = [str(path) for path in files if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"missing MNX fixtures: {missing}")
    return files


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
    priority = {name: idx for idx, name in enumerate(_SLOW_GOLDEN_FIRST)}
    fixture_tasks = sorted(
        (("fixture", source) for source in fixture_files() + mnx_fixture_files()),
        key=lambda item: (priority.get(item[1].name, len(priority)), -item[1].stat().st_size, item[1].name),
    )
    edit_tasks = [("edit", (name, scenario)) for name, scenario in EDIT_SCENARIOS.items()]
    work_items: list[tuple[str, object]] = [*fixture_tasks, *edit_tasks]
    queue: asyncio.Queue[tuple[str, object]] = asyncio.Queue()
    for item in work_items:
        queue.put_nowait(item)
    worker_count = min(4, max(1, (os.cpu_count() or 2) // 2), len(work_items))
    with (
        tempfile.TemporaryDirectory(prefix="golden-raw-") as temporary,
        tempfile.TemporaryDirectory(prefix="golden-logs-") as log_temporary,
    ):
        raw_root = Path(temporary)
        log_root = Path(log_temporary)
        params = StdioServerParameters(
            command=str(binary),
            args=["--resources", str(resources), "--soundfont", str(soundfont)],
            cwd=str(raw_root),
            env=dict(os.environ),
        )

        async def run_worker(idx: int) -> None:
            worker_log_path = log_root / f"worker-{idx}-stderr.log"
            with worker_log_path.open("w", encoding="utf-8") as worker_log:
                async with stdio_client(params, errlog=worker_log) as (read, write):
                    async with ClientSession(read, write) as session:
                        await session.initialize()
                        while True:
                            try:
                                kind, payload = queue.get_nowait()
                            except asyncio.QueueEmpty:
                                break
                            item_name = payload.name if kind == "fixture" else payload[0]
                            try:
                                if kind == "fixture":
                                    await capture_fixture(
                                        session,
                                        payload.name,
                                        payload,
                                        raw_root,
                                        target / payload.name,
                                        [raw_root, target],
                                    )
                                else:
                                    name, scenario = payload
                                    await capture_edit(
                                        session,
                                        name,
                                        scenario,
                                        raw_root,
                                        target / name,
                                        [raw_root, target],
                                    )
                            except Exception as exc:  # noqa: BLE001 - keep other fixtures going
                                write_text(
                                    target / item_name / "capture.error.txt",
                                    f"{type(exc).__name__}: {sanitize(str(exc), [raw_root, target])}\n",
                                )
                            finally:
                                queue.task_done()

        await asyncio.gather(*(run_worker(idx) for idx in range(worker_count)))
        with log_path.open("w", encoding="utf-8") as combined:
            for idx in range(worker_count):
                worker_log_path = log_root / f"worker-{idx}-stderr.log"
                if worker_log_path.is_file():
                    combined.write(worker_log_path.read_text(encoding="utf-8", errors="replace"))
    return build_index(target)


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


def _file_sha256(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _resources_sha256(resources: Path) -> str:
    def fail(error: OSError) -> None:
        raise error

    files = (folder / name for folder, _, names in resources.walk(on_error=fail) for name in names)
    entries = sorted((path.relative_to(resources).as_posix(), _file_sha256(path)) for path in files)
    return sha256(json.dumps(entries).encode("utf-8"))


def _extractor_sha256() -> str:
    # Fixed roles and order keep the digest independent of checkout paths.
    sources = (
        Path(__file__),
        Path(inspect.getfile(describe_audio)),
        Path(inspect.getfile(composition_musicxml)),
        Path(inspect.getfile(_engine_audio.CapturePolicy)),
    )
    return sha256("".join(_file_sha256(path) for path in sources).encode("ascii"))


def audio_capture_provenance(binary: Path, resources: Path, soundfont: Path, fixtures: dict[str, Path]) -> dict:
    return {
        "schema": 2,
        "binary_sha256": _file_sha256(binary),
        "soundfont_sha256": _file_sha256(soundfont),
        "resourcesSha256": _resources_sha256(resources),
        "extractorSha256": _extractor_sha256(),
        "fixture_sha256": {name: _file_sha256(path) for name, path in fixtures.items()},
    }


def reusable_golden_audio(
    actual: Path | None,
    fixtures: dict[str, Path],
    binary: Path,
    resources: Path,
    soundfont: Path,
) -> dict[str, Path] | None:
    if actual is None:
        return None
    try:
        provenance = json.loads((actual.parent / "provenance.json").read_text(encoding="utf-8"))
        index_bytes = (actual / "index.json").read_bytes()
        if not isinstance(provenance, dict) or provenance.get("indexSha256") != sha256(index_bytes):
            return None
        index = json.loads(index_bytes)
        if not isinstance(index, dict):
            return None
        expected = audio_capture_provenance(binary, resources, soundfont, fixtures)
        if any(provenance.get(key) != value for key, value in expected.items()):
            return None
        matched: dict[str, Path] = {}
        for name in fixtures:
            entry = index[name]
            audio_path = actual / name / "audio.json"
            if not isinstance(entry, dict) or _file_sha256(audio_path) != entry["audio.json"]:
                return None
            matched[name] = audio_path
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError):
        return None
    return matched


def command_compare(args) -> int:
    allow = load_allow(args.allow)
    args.output.mkdir(parents=True, exist_ok=True)
    provenance_path = args.output / "provenance.json"
    provenance_path.unlink(missing_ok=True)
    actual = args.output / "actual"
    fixtures = {path.name: path for path in fixture_files() + mnx_fixture_files()}
    provenance = audio_capture_provenance(args.binary, args.resources, args.soundfont, fixtures)
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
    provenance["indexSha256"] = _file_sha256(actual / "index.json")
    write_json(provenance_path, provenance)
    return compare_trees(args.golden, actual, allow, args.output, args.platform)


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
