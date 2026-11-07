"""Exercise the MCP wire with the independent official Python MCP client."""

import asyncio
import json
import logging
import os
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
import soundfile as sf
from jsonschema import Draft202012Validator
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from support.paths import REPO_ROOT

OUT = Path(os.environ.get("ENGRAVE_MCP_OUTPUT", REPO_ROOT / "artifacts/mcp-smoke")).resolve()
TOOLS = {tool["name"]: tool for tool in json.loads((REPO_ROOT / "engrave_mcp/tools.json").read_text(encoding="utf-8"))}
VALIDATORS = {name: Draft202012Validator(tool["outputSchema"]) for name, tool in TOOLS.items()}


async def main():
    protocol_errors = []

    class ProtocolLog(logging.Handler):
        def emit(self, record):
            if record.levelno >= logging.ERROR:
                protocol_errors.append(record.getMessage())

    logging.getLogger("mcp.client.stdio").addHandler(ProtocolLog())
    OUT.mkdir(parents=True, exist_ok=True)
    default_binary = REPO_ROOT / (
        "build/xmake-windows/engrave-mcp.exe" if os.name == "nt" else "build/xmake-linux/engrave-mcp"
    )
    binary = Path(os.environ.get("ENGRAVE_MCP_BINARY", default_binary)).resolve()
    soundfont = Path(
        os.environ.get("ENGRAVE_MCP_SOUNDFONT", REPO_ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3")
    ).resolve()
    child_env = dict(os.environ)
    if os.environ.get("ENGRAVE_MCP_CLEAN_ENV"):
        if os.name == "nt":
            child_env = {
                "SystemRoot": os.environ["SystemRoot"],
                "PATH": str(Path(os.environ["SystemRoot"]) / "System32"),
                "TEMP": os.environ["TEMP"],
                "TMP": os.environ["TMP"],
            }
        else:
            child_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
    child_args = ["--soundfont", str(soundfont)]
    if os.environ.get("ENGRAVE_MCP_RESOURCES"):
        child_args += ["--resources", os.environ["ENGRAVE_MCP_RESOURCES"]]
    params = StdioServerParameters(command=str(binary), args=child_args, env=child_env, cwd=str(OUT))
    report = {
        "calls": [],
        "cycles": 0,
        "binary": str(binary),
        "soundfont": str(soundfont),
        "resources": os.environ.get("ENGRAVE_MCP_RESOURCES"),
        "output": str(OUT),
    }
    started = time.perf_counter()
    with (OUT / "stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                assert initialized.server_info.name == "engrave-mcp"
                report["server_name"] = initialized.server_info.name
                report["protocol"] = initialized.protocol_version
                listed = await session.list_tools()
                assert {tool.name for tool in listed.tools} == set(TOOLS)

                async def call(name, arguments, error=False):
                    response = await session.call_tool(name, arguments)
                    assert bool(response.is_error) == error, response
                    payload = response.structured_content
                    VALIDATORS[name].validate(payload)
                    assert payload["ok"] != error, payload
                    assert len(response.content[0].text.encode()) <= 4 * 1024 * 1024
                    report["calls"].append({"tool": name, "error": error})
                    return payload

                info = await call("runtime_info", {})
                assert info["tool_revision"] == "2" and info["limits"]["transport_request_bytes"] == 4194304
                opened = await call(
                    "score_open", {"path": str(REPO_ROOT / "vendor/engrave/tests/data/inputs/evening.musicxml")}
                )
                doc, revision = opened["document"], opened["revision"]
                outline = await call("score_outline", {"document": doc})
                assert outline["outline"]["measure_count"] > 0 and outline["revision"] == revision
                notes = await call(
                    "score_query",
                    {"document": doc, "mode": "elements", "query": {"types": ["Note"]}},
                )
                note = notes["elements"][0]["id"]
                await call("score_describe", {"scope": "type", "type": "Note"})
                bad = await call("score_edit", {"document": doc, "ops": [{"op": "unknown"}]}, True)
                assert bad["error"]["code"] == "InvalidArgument"
                edited = await call(
                    "score_edit",
                    {
                        "document": doc,
                        "expected_revision": revision,
                        "ops": [
                            {
                                "op": "set_pitch",
                                "note": {"kind": "id", "id": note},
                                "pitch": 74,
                            }
                        ],
                    },
                )
                properties = await call(
                    "score_query",
                    {
                        "document": doc,
                        "mode": "properties",
                        "target": {"kind": "id", "id": note},
                        "props": ["pitch"],
                    },
                )
                assert properties["properties"][0]["value"] == {
                    "kind": "int",
                    "value": "74",
                }

                async def export(kind, options, destination):
                    value = await call(
                        "score_export",
                        {
                            "document": doc,
                            "kind": kind,
                            "options": options,
                            "destination": destination,
                        },
                    )
                    assert value["source_revision"] == edited["revision"]
                    return value

                for fmt, ext in (
                    ("musicxml", "musicxml"),
                    ("mxl", "mxl"),
                    ("midi", "mid"),
                    ("mscz", "mscz"),
                ):
                    path = OUT / f"score.{ext}"
                    result = await export("score", {"format": fmt}, {"kind": "file", "path": str(path)})
                    assert (
                        len(result["artifacts"]) == 1 and int(result["artifacts"][0]["size"]) == path.stat().st_size > 0
                    )
                for fmt in ("pdf", "png", "svg"):
                    if fmt == "pdf":
                        path = OUT / "score.pdf"
                        destination = {"kind": "file", "path": str(path)}
                    else:
                        path = OUT / f"{fmt}-pages"
                        path.mkdir(exist_ok=True)
                        destination = {"kind": "directory", "path": str(path)}
                    result = await export("render", {"format": fmt}, destination)
                    assert result["artifacts"] and all(
                        Path(a["path"]).stat().st_size == int(a["size"]) > 0 for a in result["artifacts"]
                    )
                positions = OUT / "positions.json"
                await export(
                    "positions",
                    {"position_kind": "measure"},
                    {"kind": "file", "path": str(positions)},
                )
                assert json.loads(positions.read_text(encoding="utf-8"))["elements"]
                audio = OUT / "score.wav"
                await export("audio", {"format": "wav"}, {"kind": "file", "path": str(audio)})
                root = ET.parse(OUT / "score.musicxml").getroot()
                assert root.findall("part") and root.findall(".//note/pitch")

                # Reopen both native and compressed interchange files and compare exported notes.
                from support.musicxml_notes import canonical_notes

                for extension in ("mscz", "mxl"):
                    restored = await call("score_open", {"path": str(OUT / f"score.{extension}")})
                    path = OUT / f"reopened-{extension}.musicxml"
                    await call(
                        "score_export",
                        {
                            "document": restored["document"],
                            "kind": "score",
                            "options": {"format": "musicxml"},
                            "destination": {"kind": "file", "path": str(path)},
                        },
                    )
                    assert canonical_notes(OUT / "score.musicxml") == canonical_notes(path)
                    await call("score_close", {"document": restored["document"]})
                await call("score_close", {"document": doc})
                closed = await call("score_outline", {"document": doc}, True)
                assert closed["error"]["code"] == "NotFound"
                for _ in range(20):
                    item = await call("score_create", {"template_id": "Treble_Clef"})
                    await call("score_close", {"document": item["document"]})
                    report["cycles"] += 1
    assert not protocol_errors, protocol_errors
    report["seconds"] = round(time.perf_counter() - started, 3)
    assert "ERROR:" not in (OUT / "stderr.log").read_text(encoding="utf-8")

    samples, rate = sf.read(OUT / "score.wav", always_2d=True)
    assert rate == 44100 and samples.shape[1] == 2 and np.isfinite(samples).all() and np.max(np.abs(samples)) > 0.01
    report["audio"] = {
        "rate": rate,
        "seconds": len(samples) / rate,
        "peak": float(np.max(np.abs(samples))),
    }
    report["status"] = "passed"
    (OUT / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "calls"}))


if __name__ == "__main__":
    asyncio.run(asyncio.wait_for(main(), timeout=180))
