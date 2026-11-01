"""Malformed/archive input must fail without killing or poisoning the server."""
import asyncio
import base64
import json
import os
from pathlib import Path
import zipfile

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts/native-input-errors"


async def main():
    OUT.mkdir(parents=True, exist_ok=True)
    fixture = (ROOT / "fixtures/evening.musicxml").read_text(encoding="utf-8")
    cases = {
        "oversized-root": ("<container><rootfiles><rootfile full-path='score.xml'/></rootfiles></container>", fixture + "<!--" + "x" * (9 * 1024 * 1024) + "-->"),
        "missing-root": ("<container><rootfiles><rootfile full-path='missing.xml'/></rootfiles></container>", fixture),
        "malformed-container": ("<container><rootfiles><rootfile full-path='score.xml'/></rootfiles>", fixture),
    }
    for name, (container, score) in cases.items():
        with zipfile.ZipFile(OUT / f"{name}.mxl", "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("META-INF/container.xml", container)
            archive.writestr("score.xml", score)
    binary = os.environ.get("ENGRAVE_MCP_BINARY", str(ROOT / ("build/xmake-windows/engrave-mcp.exe" if os.name == "nt" else "build/xmake-linux/engrave-mcp")))
    resources = os.environ.get("ENGRAVE_MCP_RESOURCES", str(ROOT / ".runtime/assets"))
    params = StdioServerParameters(command=binary, args=["--resources", resources], env=dict(os.environ))
    with (OUT / "stderr.log").open("w", encoding="utf-8") as log:
        async with stdio_client(params, errlog=log) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                for name in cases:
                    response = await session.call_tool("score_open", {"path": str(OUT / f"{name}.mxl")})
                    assert response.is_error, name
                response = await session.call_tool("score_open", {
                    "data": base64.b64encode(b"<score-partwise><part>").decode('ascii'), "format": "musicxml"})
                assert response.is_error
                valid = await session.call_tool("score_open", {"path": str(ROOT / "fixtures/evening.musicxml")})
                assert not valid.is_error
                closed = await session.call_tool("score_close", {"document": valid.structured_content["document"]})
                assert not closed.is_error
    result = {"rejected": list(cases) + ["malformed-musicxml"], "valid_score_after_errors": True}
    (OUT / "validation.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    asyncio.run(main())
