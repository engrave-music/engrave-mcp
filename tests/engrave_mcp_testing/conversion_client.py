"""Shared conversion client helpers."""

from __future__ import annotations

import hashlib
import json
import os
import queue
import subprocess
import threading
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Client:
    def __init__(self, args, out):
        self.log = (out / "wire.jsonl").open("w", encoding="utf-8")
        self.errors = (out / "stderr.log").open("w", encoding="utf-8")
        self.process = subprocess.Popen(
            [str(args.binary.resolve()), "--resources", str(args.resources.resolve())],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.errors,
            env=dict(os.environ),
        )
        self.responses = queue.Queue()
        self.counter = 0

        def read():
            for line in self.process.stdout:
                self.responses.put(json.loads(line))

        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, method, params, notification=False):
        self.counter += 1
        request = dict(jsonrpc="2.0", method=method, params=params)
        if not notification:
            request["id"] = self.counter
        self.log.write(json.dumps(dict(time=time.monotonic(), sent=request)) + "\n")
        self.log.flush()
        self.process.stdin.write((json.dumps(request) + "\n").encode())
        self.process.stdin.flush()
        return self.counter

    def receive(self, ident):
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            response = self.responses.get(timeout=max(0.01, deadline - time.monotonic()))
            self.log.write(json.dumps(dict(time=time.monotonic(), received=response)) + "\n")
            self.log.flush()
            if response.get("id") == ident:
                return response
        raise TimeoutError(ident)

    def call(self, tool, args):
        response = self.receive(self.send("tools/call", dict(name=tool, arguments=args)))
        assert "result" in response, response
        return response["result"]["structuredContent"]

    def close(self):
        self.process.stdin.close()
        try:
            assert self.process.wait(timeout=15) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.reader.join(timeout=2)
            self.log.close()
            self.errors.close()
