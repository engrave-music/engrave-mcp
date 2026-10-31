"""In-memory score_open input: base64 data parity with path and rejection cases.

The lane exports MSCZ, MXL and MIDI references from fixtures/evening.musicxml,
then opens each file once via `path` and once via `data` (standard base64) and
requires identical score_get metadata and MusicXML. A pinned Guitar Pro fixture
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
    metadata = document["metadata"]
    return {
        "title": document["title"],
        "pages": document["pages"],
        "parts": document["parts"],
        "metadataTitle": metadata["title"],
        "measures": metadata["measures"],
        "partNames": [part["name"] for part in metadata["parts"]],
    }


async def open_path(client, path):
    return await call(client, "score_open", {"path": str(path)}, label=f"open {path.name}")


def oversized_probe(binary, resources, cwd):
    """The stdio transport caps a request line at 8 MiB, so a decoded payload
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
        (response.get("result") or {}).get("structuredContent", {}).get("documentId")
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

                source = ROOT / "fixtures/evening.musicxml"
                reference = await open_path(client, source)
                reference_id = reference["documentId"]
                exports = {}
                try:
                    for output_format, name in (
                        ("mscz", "reference.mscz"),
                        ("mxl", "reference.mxl"),
                        ("midi", "reference.mid"),
                    ):
                        target = out / name
                        await call(
                            client,
                            "score_export",
                            {
                                "documentId": reference_id,
                                "format": output_format,
                                "path": str(target),
                            },
                            label=f"export {output_format}",
                        )
                        assert target.stat().st_size > 0, target
                        exports[output_format] = target
                finally:
                    await call(
                        client,
                        "score_close",
                        {"documentId": reference_id},
                        label="close reference",
                    )

                fixtures = (args.upstream / "src/importexport/guitarpro/tests/data").resolve()
                cases = [
                    ("mscz", exports["mscz"], "mscz"),
                    ("mxl", exports["mxl"], "mxl"),
                    ("midi", exports["midi"], "midi"),
                    ("mid-alias", exports["midi"], "mid"),
                    ("musicxml", source, "musicxml"),
                    ("xml-alias", source, "xml"),
                    ("gp3", fixtures / "tempo.gp3", "gp3"),
                    ("gtp-alias", fixtures / "tempo.gp3", "gtp"),
                ]
                for name, path, input_format in cases:
                    assert path.is_file(), path
                    path_document = await call(
                        client,
                        "score_open",
                        {"path": str(path)},
                        label=f"{name} path open",
                    )
                    try:
                        path_info = await call(
                            client,
                            "score_get",
                            {"documentId": path_document["documentId"]},
                            label=f"{name} path get",
                        )
                    finally:
                        await call(
                            client,
                            "score_close",
                            {"documentId": path_document["documentId"]},
                            label=f"{name} path close",
                        )

                    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
                    data_document = await call(
                        client,
                        "score_open",
                        {"data": encoded, "format": input_format},
                        label=f"{name} data open",
                    )
                    try:
                        data_info = await call(
                            client,
                            "score_get",
                            {"documentId": data_document["documentId"]},
                            label=f"{name} data get",
                        )
                    finally:
                        await call(
                            client,
                            "score_close",
                            {"documentId": data_document["documentId"]},
                            label=f"{name} data close",
                        )

                    assert summary(path_info) == summary(data_info), (
                        name,
                        summary(path_info),
                        summary(data_info),
                    )
                    assert path_info["musicxml"] == data_info["musicxml"], name
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
                     {"data": "U2Nvcm$=", "format": "mscz"}, "base64"),
                    ("unpadded base64", {"data": "U2NvcmU", "format": "mscz"}, "base64"),
                    ("excess base64 padding",
                     {"data": "U2NvcmU===", "format": "mscz"}, "base64"),
                    ("misplaced base64 padding",
                     {"data": "U2Nvcm=A=", "format": "mscz"}, "base64"),
                    ("missing format", {"data": valid}, "format is required"),
                    ("unknown format",
                     {"data": valid, "format": "aac"}, "Unsupported input format"),
                    ("mismatched format", {"data": valid, "format": "midi"}, None),
                    (
                        "data and path",
                        {"path": str(exports["mscz"]), "data": valid, "format": "mscz"},
                        "Specify exactly one",
                    ),
                    (
                        "data and musicxml",
                        {
                            "musicxml": "<score-partwise/>",
                            "data": valid,
                            "format": "musicxml",
                        },
                        "Specify exactly one",
                    ),
                    (
                        "format without data",
                        {"path": str(exports["mscz"]), "format": "mscz"},
                        "format is required",
                    ),
                ]
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

                recovered = await open_path(client, fixtures / "tempo.gp3")
                await call(
                    client,
                    "score_close",
                    {"documentId": recovered["documentId"]},
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
                        default=ROOT / "vendor/musescore",
                        help="Path to engine source root")
    asyncio.run(asyncio.wait_for(run(parser.parse_args()), timeout=600))
