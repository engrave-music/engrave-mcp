"""In-memory score_open input: base64 data parity with path and rejection cases.

The lane exports MSCZ, MXL and MIDI references from fixtures/evening.musicxml,
then opens each file once via `path` and once via `data` (standard base64) and
requires identical outlines and MusicXML exports. Pinned Guitar Pro and MNX fixtures
is checked the same way; malformed argument combinations are rejected; and a
decoded payload above the 8 MiB input limit cannot be opened.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import io
import zipfile
import os
from pathlib import Path
import shutil
import subprocess

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
MAX_INPUT = 8 * 1024 * 1024


async def call(client, name, values, error=False, label="", expect=None):
    response = await client.call_tool(name, values)
    if bool(response.is_error) != error:
        raise AssertionError(f"{label or name}: unexpected result {response.content!r}")
    if error:
        text = " ".join(
            item.text for item in response.content if getattr(item, "text", None)
        )
        if expect is not None and expect not in text:
            raise AssertionError(f"{label or name}: expected {expect!r} in {text!r}")
        return text
    if response.structured_content is not None:
        return response.structured_content
    return json.loads(response.content[0].text)


def summary(document):
    metadata = document["outline"]
    return {
        "title": metadata["title"],
        "pages": metadata["pages"],
        "parts": len(metadata["parts"]),
        "metadataTitle": metadata["title"],
        "measures": metadata["measure_count"],
        "partNames": [part["name"] for part in metadata["parts"]],
    }


async def open_path(client, path):
    return await call(client, "score_open", {"path": str(path)}, label=f"open {path.name}")


def oversized_probe(binary, resources, cwd):
    """The stdio transport caps a request line at 4 MiB, so a decoded payload
    above the score limit cannot be transmitted; the server must reject it."""
    payload = base64.b64encode(b"\0" * (MAX_INPUT + 1)).decode("ascii")
    lines = [
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "open-data-oversized", "version": "1"},
                },
            }
        ),
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}),
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "score_open",
                    "arguments": {"data": payload, "format": "mscz"},
                },
            }
        ),
    ]
    command = [str(binary)]
    if resources is not None:
        command += ["--resources", str(resources)]
    completed = subprocess.run(
        command,
        input=("\n".join(lines) + "\n").encode("utf-8"),
        capture_output=True,
        timeout=120,
        cwd=str(cwd),
    )
    responses = [
        json.loads(line)
        for line in completed.stdout.decode("utf-8", "replace").splitlines()
        if line.strip()
    ]
    assert completed.returncode == 0, (completed.returncode, completed.stderr[-2000:])
    assert any("error" in response for response in responses), responses
    assert not any(
        (response.get("result") or {}).get("structuredContent", {}).get("document")
        for response in responses
        if "result" in response and isinstance(response.get("result"), dict)
    ), responses
    return {
        "encodedBytes": len(payload),
        "decodedBytes": MAX_INPUT + 1,
        "requestLineBytes": len(lines[-1].encode("utf-8")),
        "responses": responses,
    }


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ["--resources", str(args.resources.resolve())] if args.resources else []
    params = StdioServerParameters(
        command=str(args.binary.resolve()),
        args=child_args,
        env=dict(os.environ),
        cwd=str(out),
    )
    report = {"status": "running", "cases": [], "rejections": []}
    with (out / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()

                async def score_export(document, fmt, target):
                    return await call(client, 'score_export', {'document': document, 'kind': 'score',
                        'options': {'format': fmt}, 'destination': {'kind': 'file', 'path': str(target)}})

                async def read_document(document, target):
                    info = await call(client, 'score_outline', {'document': document})
                    await score_export(document, 'musicxml', target)
                    info['musicxml'] = target.read_text(encoding='utf-8')
                    return info

                source = ROOT / "fixtures/evening.musicxml"
                reference = await open_path(client, source)
                reference_id = reference["document"]
                exports = {}
                try:
                    for output_format, name in (
                        ("mscz", "reference.mscz"),
                        ("mxl", "reference.mxl"),
                        ("midi", "reference.mid"),
                    ):
                        target = out / name
                        await score_export(reference_id, output_format, target)
                        assert target.stat().st_size > 0, target
                        exports[output_format] = target
                finally:
                    await call(
                        client,
                        "score_close",
                        {"document": reference_id},
                        label="close reference",
                    )

                fixtures = (args.upstream / "src/importexport/guitarpro/tests/data").resolve()
                for source_path, alias in ((exports['midi'], out / 'alias.midi'),
                                           (source, out / 'alias.xml'),
                                           (fixtures / 'tempo.gp3', out / 'alias.gtp')):
                    shutil.copyfile(source_path, alias)
                cases = [
                    ("mscz", exports["mscz"], "mscz"),
                    ("mxl", exports["mxl"], "mxl"),
                    ("midi", exports["midi"], "midi"),
                    ("midi-extension", out / 'alias.midi', "midi"),
                    ("musicxml", source, "musicxml"),
                    ("xml-extension", out / 'alias.xml', "musicxml"),
                    ("gp3", fixtures / "tempo.gp3", "guitarpro"),
                    ("gtp-extension", out / 'alias.gtp', "guitarpro"),
                    ("mnx", args.upstream / "src/importexport/mnx/tests/data/engine/two-notes.mnx", "mnx"),
                ]
                for name, path, input_format in cases:
                    path = path.resolve()
                    assert path.is_file(), path
                    path_document = await call(
                        client,
                        "score_open",
                        {"path": str(path)},
                        label=f"{name} path open",
                    )
                    try:
                        path_info = await read_document(path_document['document'], out / f'{name}-path.musicxml')
                    finally:
                        await call(
                            client,
                            "score_close",
                            {"document": path_document["document"]},
                            label=f"{name} path close",
                        )

                    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
                    data_document = await call(
                        client,
                        "score_open",
                        {"data": encoded, "format": input_format, "filename_hint": path.name},
                        label=f"{name} data open",
                    )
                    try:
                        data_info = await read_document(data_document['document'], out / f'{name}-data.musicxml')
                    finally:
                        await call(
                            client,
                            "score_close",
                            {"document": data_document["document"]},
                            label=f"{name} data close",
                        )

                    assert summary(path_info) == summary(data_info), (
                        name,
                        summary(path_info),
                        summary(data_info),
                    )
                    assert path_info["musicxml"] == data_info["musicxml"], name
                    if name == 'mscz':
                        mscz_summary = summary(path_info)
                        mscz_xml = path_info['musicxml']
                    report["cases"].append(
                        {
                            "name": name,
                            "format": input_format,
                            "bytes": path.stat().st_size,
                            "encodedBytes": len(encoded),
                            **summary(data_info),
                        }
                    )

                valid = base64.b64encode(exports["mscz"].read_bytes()).decode("ascii")
                legacy_zip = io.BytesIO()
                with zipfile.ZipFile(legacy_zip, "w") as archive:
                    archive.writestr("score.mscx", (ROOT / "tests/negative/legacy-3.mscx").read_bytes())
                rejections = [
                    ("pre-4.6 archive", {"data": base64.b64encode(legacy_zip.getvalue()).decode("ascii"), "format": "mscz"},
                     "scores from before MuseScore 4.6 are not supported"),
                    ("pre-MuseScore-4 path", {"path": str(ROOT / "tests/negative/legacy-3.mscx")},
                     "written by MuseScore 3.02; scores from before MuseScore 4.6 are not supported"),
                    ("pre-MuseScore-4 data",
                     {"data": base64.b64encode((ROOT / "tests/negative/legacy-3.mscx").read_bytes()).decode("ascii"), "format": "mscx"},
                     "scores from before MuseScore 4.6 are not supported"),
                    ("invalid base64 characters",
                     {"data": "U2Nvcm$=", "format": "mscz"}, None),
                    ("unpadded base64", {"data": "U2NvcmU", "format": "mscz"}, None),
                    ("excess base64 padding",
                     {"data": "U2NvcmU===", "format": "mscz"}, None),
                    ("misplaced base64 padding",
                     {"data": "U2Nvcm=A=", "format": "mscz"}, None),
                    ("unknown format",
                     {"data": valid, "format": "aac"}, None),
                    ("mismatched format", {"data": valid, "format": "midi"}, None),
                    (
                        "data and path",
                        {"path": str(exports["mscz"]), "data": valid, "format": "mscz"},
                        None,
                    ),
                    (
                        "data and musicxml",
                        {
                            "musicxml": "<score-partwise/>",
                            "data": valid,
                            "format": "musicxml",
                        },
                        None,
                    ),
                ]
                # V2 format names are canonical; filename aliases remain path-detected.
                rejections.extend((f'retired format {alias}', {'data': valid, 'format': alias}, None)
                                  for alias in ('mid', 'xml', 'gp3', 'gtp'))
                for label, arguments, expect in rejections:
                    await call(
                        client,
                        "score_open",
                        arguments,
                        error=True,
                        label=label,
                        expect=expect,
                    )
                    report["rejections"].append(label)

                # The v2 engine can identify an MSCZ byte stream without an explicit format.
                detected = await call(client, 'score_open', {'data': valid})
                detected_info = await read_document(detected['document'], out / 'detected.musicxml')
                assert summary(detected_info) == mscz_summary
                assert detected_info['musicxml'] == mscz_xml
                await call(client, 'score_close', {'document': detected['document']})
                explicit = await call(client, 'score_open', {'path': str(exports['mscz']), 'format': 'mscz'})
                await call(client, 'score_close', {'document': explicit['document']})
                recovered = await open_path(client, fixtures / "tempo.gp3")
                await call(
                    client,
                    "score_close",
                    {"document": recovered["document"]},
                    label="recovery close",
                )
                report["recovered"] = True

    report["oversized"] = oversized_probe(
        args.binary.resolve(), args.resources.resolve() if args.resources else None, out
    )
    report["status"] = "passed"
    (out / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "oversized"}))
    print(json.dumps({"oversized": report["oversized"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--upstream", "--engine-source", dest="upstream", type=Path,
                        default=ROOT / "vendor/engrave",
                        help="Path to engine source root")
    asyncio.run(asyncio.wait_for(run(parser.parse_args()), timeout=600))
