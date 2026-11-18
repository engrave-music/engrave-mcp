"""Direct ZIP decompression checks plus optional valid MXL/MSCZ/GP MCP imports."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import zipfile
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_testing.archives import run_probe


async def valid_score_imports(binary, resources, out, upstream):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    xml = (upstream / "tests/data/inputs/evening.musicxml").read_bytes()
    with zipfile.ZipFile(out / "valid.mxl", "w", compression=zipfile.ZIP_DEFLATED) as target:
        target.writestr(
            "META-INF/container.xml",
            "<container><rootfiles><rootfile full-path='score.xml'/></rootfiles></container>",
        )
        target.writestr("score.xml", xml)
    params = StdioServerParameters(command=str(binary), args=["--resources", str(resources)], env=dict(os.environ))
    result = []
    with (out / "mcp.stderr.log").open("w", encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                opened = await session.call_tool("score_open", {"path": str(out / "valid.mxl")})
                assert not opened.is_error, opened.content
                document = opened.structured_content["document"]
                exported = await session.call_tool(
                    "score_export",
                    {
                        "document": document,
                        "kind": "score",
                        "options": {"format": "mscz"},
                        "destination": {
                            "kind": "file",
                            "path": str(out / "valid.mscz"),
                        },
                    },
                )
                assert not exported.is_error, exported.content
                assert exported.structured_content["artifacts"] and (out / "valid.mscz").stat().st_size > 0
                closed = await session.call_tool("score_close", {"document": document})
                assert not closed.is_error, closed.content
                result.append("mxl")
                for path, label in [
                    (out / "valid.mscz", "mscz"),
                    (
                        upstream / "src/importexport/guitarpro/tests/data/sforzato.gp",
                        "gp",
                    ),
                ]:
                    assert path.is_file(), path
                    opened = await session.call_tool("score_open", {"path": str(path)})
                    assert not opened.is_error, (label, opened.content)
                    closed = await session.call_tool(
                        "score_close",
                        {"document": opened.structured_content["document"]},
                    )
                    assert not closed.is_error, closed.content
                    result.append(label)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="archive_test native probe")
    parser.add_argument("--server", type=Path, help="Optional MCP executable for valid score formats")
    parser.add_argument("--resources", type=Path, default=REPO_ROOT / ".runtime/assets")
    parser.add_argument("--upstream", type=Path, default=REPO_ROOT / "vendor/engrave")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "artifacts/archive-bounds")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = run_probe(args.binary.resolve(), out)
    if args.server:
        report["valid_score_imports"] = asyncio.run(
            valid_score_imports(args.server.resolve(), args.resources.resolve(), out, args.upstream.resolve())
        )
    (out / "evidence.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
