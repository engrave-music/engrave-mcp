"""Shared raw stdio client and schema validators for the v2 test lanes."""
from __future__ import annotations

import asyncio
import json
from pathlib import Path
import time

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
TOOLS = {x["name"]: x for x in json.loads((ROOT / "native/tools.json").read_text(encoding="utf-8"))}
VALIDATORS = {name: Draft202012Validator(tool["outputSchema"]) for name, tool in TOOLS.items()}

class Client:
    def __init__(self, args, out):
        self.args, self.out, self.serial, self.pending = args, out, 0, {}

    async def __aenter__(self):
        command = [str(self.args.binary.resolve())]
        if self.args.resources: command += ["--resources", str(self.args.resources.resolve())]
        if self.args.soundfont: command += ["--soundfont", str(self.args.soundfont.resolve())]
        self.log = (self.out / "stderr.log").open("wb")
        self.proc = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=self.log, limit=16 * 1024 * 1024)
        self.reader = asyncio.create_task(self.read())
        result = await self.call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                                 "clientInfo": {"name": "v2-async-test", "version": "1"}})
        assert result["result"]["serverInfo"]["name"] == "engrave-mcp", result
        await self.notify("notifications/initialized", {})
        return self

    async def read(self):
        try:
            while line := await self.proc.stdout.readline():
                response = json.loads(line)
                identity = response.get("id")
                if identity not in self.pending: raise AssertionError(f"unexpected response: {response}")
                future = self.pending.pop(identity)
                if not future.done(): future.set_result(response)
            if self.pending: raise RuntimeError("server exited with pending requests")
        except BaseException as exc:
            for future in self.pending.values():
                if not future.done(): future.set_exception(exc)
            self.pending.clear()
            raise

    async def send(self, message):
        self.proc.stdin.write((json.dumps(message, separators=(",", ":")) + "\n").encode())
        await self.proc.stdin.drain()

    async def notify(self, method, params):
        await self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def begin(self, method, params):
        self.serial += 1
        identity = self.serial
        future = asyncio.get_running_loop().create_future()
        self.pending[identity] = future
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": identity, "method": method,
                                           "params": params}, separators=(",", ":")) + "\n").encode())
        return identity, future

    async def call(self, method, params):
        _, future = self.begin(method, params)
        await self.proc.stdin.drain()
        return await asyncio.wait_for(future, 60)

    async def tool(self, name, args, error=False):
        response = await self.call("tools/call", {"name": name, "arguments": args})
        assert "error" not in response, response
        result = response["result"]
        assert bool(result["isError"]) == error, result
        payload = result["structuredContent"]
        assert payload["ok"] != error, payload
        if name in VALIDATORS: VALIDATORS[name].validate(payload)
        assert len(result["content"][0]["text"].encode()) <= 4 * 1024 * 1024
        return payload

    async def terminal(self, job):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = (await self.tool("runtime_job", {"action": "get", "job": job}))["status"]
            if status["state"] in ("succeeded", "failed", "cancelled"): return status
            await asyncio.sleep(.02)
        raise AssertionError("job did not terminate: " + job)

    async def close(self):
        self.proc.stdin.close()
        await asyncio.wait_for(self.proc.wait(), 15)
        await asyncio.wait_for(self.reader, 5)
        assert self.proc.returncode == 0, self.proc.returncode

    async def __aexit__(self, kind, value, tb):
        try:
            if self.proc.returncode is None:
                if kind is None: await self.close()
                else:
                    self.proc.kill()
                    await self.proc.wait()
        finally:
            self.log.close()

