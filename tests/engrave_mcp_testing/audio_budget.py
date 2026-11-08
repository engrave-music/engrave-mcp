"""Shared audio budget helpers."""

from __future__ import annotations

import asyncio
import hashlib
import os
import tempfile
from pathlib import Path
from types import SimpleNamespace

from engrave_mcp_testing.mcp_client import Client


def live_budget(args, source, audio_options, limit, expected):
    async def execute():
        with tempfile.TemporaryDirectory(prefix="live-budget-", dir=args.output) as temporary:
            out = Path(temporary).resolve()
            configured = dict(vars(args), output=out)

            previous = os.environ.get("ENGRAVE_MCP_TEST_OUTPUT_BYTES")
            os.environ["ENGRAVE_MCP_TEST_OUTPUT_BYTES"] = str(limit)
            try:
                async with Client(SimpleNamespace(**configured), out) as client:
                    doc = (await client.tool("score_open", {"path": str(source)}))["document"]
                    path = out / "output"
                    result = await client.tool(
                        "score_export",
                        {
                            "document": doc,
                            "kind": "audio",
                            "options": audio_options,
                            "destination": {"kind": "file", "path": str(path)},
                        },
                        error=expected != "success",
                    )
                    if expected == "success":
                        produced = path.stat().st_size
                        assert 0 < produced <= limit // 2
                        digest = hashlib.sha256(path.read_bytes()).hexdigest()
                    else:
                        assert result["error"]["code"] == expected, result
                        assert not path.exists() and not list(out.glob(".engrave-stage-*"))
                        produced, digest = 0, None
                    return {
                        "limit": limit,
                        "result": "success" if result["ok"] else result["error"]["code"],
                        "output_bytes": produced,
                        "sha256": digest,
                        "message": "" if result["ok"] else result["error"]["message"],
                    }
            finally:
                if previous is None:
                    os.environ.pop("ENGRAVE_MCP_TEST_OUTPUT_BYTES", None)
                else:
                    os.environ["ENGRAVE_MCP_TEST_OUTPUT_BYTES"] = previous

    return asyncio.run(execute())
