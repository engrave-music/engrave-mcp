"""Subprocess MCP client and export/RSS measurement helpers."""

import json
import math
import os
import queue
import statistics
import subprocess
import threading
from pathlib import Path


class Client:
    def __init__(self, binary, resources, output, mode=None, soundfont=None, *, protocol_version="2025-11-25"):
        output.mkdir(parents=True, exist_ok=True)
        self.log_path = output / "stderr.log"
        self.log = self.log_path.open("w", encoding="utf-8")
        args = [str(binary.resolve()), "--resources", str(resources.resolve())]
        if soundfont:
            args += ["--soundfont", str(soundfont.resolve())]
        # Windows priority is explicit: ordinary children need not inherit High.
        high = os.environ.get("ENGRAVE_MCP_BENCH_HIGH_PRIORITY") == "1"
        if high and os.name != "nt":
            raise RuntimeError("High protocol requires Windows")
        if high:
            import ctypes
            from ctypes import wintypes

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.GetCurrentProcess.restype = wintypes.HANDLE
            kernel.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.GetPriorityClass.argtypes = [wintypes.HANDLE]
            if not kernel.SetPriorityClass(kernel.GetCurrentProcess(), 128):
                raise ctypes.WinError()
        self.p = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            creationflags=subprocess.HIGH_PRIORITY_CLASS if high else 0,
            env=dict(os.environ, ENGRAVE_MCP_PROFILE_EXPORT="1"),
        )
        if high:
            parent = kernel.GetPriorityClass(kernel.GetCurrentProcess())
            child = kernel.GetPriorityClass(wintypes.HANDLE(int(self.p._handle)))
            proof = dict(
                parent_pid=os.getpid(),
                child_pid=self.p.pid,
                parent_priority=parent,
                child_priority=child,
            )
            (output / "priority.json").write_text(json.dumps(proof, indent=2))
            print("PRIORITY " + json.dumps(proof), flush=True)
            if parent != 128 or child != 128:
                self.p.kill()
                self.p.wait()
                raise RuntimeError(f"High priority verification failed: {proof}")
        self.q = queue.Queue()
        self.ident = 0
        self.pending = {}

        def read():
            for line in self.p.stdout:
                self.q.put(json.loads(line))
            self.q.put({"error": "server exited", "exit": self.p.wait()})

        threading.Thread(target=read, daemon=True).start()
        self.rpc(
            "initialize",
            {
                "protocolVersion": protocol_version,
                "capabilities": {},
                "clientInfo": {"name": "latency", "version": "1"},
            },
        )
        self.send("notifications/initialized", {}, True)

    def send(self, method, params, notification=False):
        self.ident += 1
        msg = dict(jsonrpc="2.0", method=method, params=params)
        if not notification:
            msg["id"] = self.ident
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        return self.ident

    def receive(self, ident):
        if ident in self.pending:
            return self.pending.pop(ident)
        while True:
            msg = self.q.get(timeout=150)
            if msg.get("error") == "server exited":
                raise RuntimeError(msg)
            if msg.get("id") == ident:
                return msg
            if "id" in msg:
                self.pending[msg["id"]] = msg

    def cancelled(self, ident, document=None):
        if document is None:
            self.rpc("ping", {})
        else:
            # A revision guard reaches the owner after the cancelled export, without traversing the score.
            probe = self.tool(
                "score_query", {"document": document, "mode": "page_count", "expected_revision": "0"}, False
            )
            assert probe["error"]["code"] == "RevisionConflict"
        assert ident not in self.pending, self.pending.get(ident)

    def rpc(self, method, params):
        return self.receive(self.send(method, params))

    def tool(self, name, args, ok=True):
        msg = self.rpc("tools/call", {"name": name, "arguments": args})
        assert "result" in msg, msg
        result = msg["result"]["structuredContent"]
        assert result["ok"] == ok, result
        return result

    def close(self):
        self.p.stdin.close()
        assert self.p.wait(timeout=30) == 0
        self.log.close()
        phases = []
        for line in self.log_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("EXPORT_TIMING "):
                phases.append(json.loads(line[len("EXPORT_TIMING ") :]))
        return phases


def rss_bytes(pid):
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                (name, ctypes.c_size_t)
                for name in (
                    "PeakWorkingSetSize",
                    "WorkingSetSize",
                    "QuotaPeakPagedPoolUsage",
                    "QuotaPagedPoolUsage",
                    "QuotaPeakNonPagedPoolUsage",
                    "QuotaNonPagedPoolUsage",
                    "PagefileUsage",
                    "PeakPagefileUsage",
                )
            ]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(Counters),
            wintypes.DWORD,
        ]
        handle = kernel.OpenProcess(0x410, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                raise ctypes.WinError(ctypes.get_last_error())
            return counters.WorkingSetSize
        finally:
            kernel.CloseHandle(handle)
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("RSS unavailable")


def summary(samples):
    values = sorted(samples)
    return dict(
        median=statistics.median(values),
        p95=values[math.ceil(0.95 * len(values)) - 1],
        max=max(values),
        n=len(values),
    )


def export_args(doc, fmt, path, all_pages=False):
    options = {}
    if fmt != "mscz":
        options.update(first_page=0, last_page=-1 if all_pages else 0)
    return dict(
        document=doc,
        format=fmt,
        options=options,
        path=str(path.resolve()),
    )
