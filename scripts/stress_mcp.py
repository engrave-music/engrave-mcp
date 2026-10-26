"""Bounded repeated-document and stream-recovery checks on the native executable."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("MUSIC_MCP_STRESS_OUTPUT", ROOT / "artifacts/stress"))


class Counters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ["PeakWorkingSetSize", "WorkingSetSize",
        "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
        "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage"]]


def memory(process):
    if os.name != "nt":
        # Private resident pages; Linux and Windows absolute values are not comparable.
        fields = (Path(f"/proc/{process.pid}/smaps_rollup").read_text()).splitlines()
        return sum(int(line.split()[1]) * 1024 for line in fields
                   if line.startswith(("Private_Clean:", "Private_Dirty:", "Private_Hugetlb:")))
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    fn = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    fn.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    fn.restype = wintypes.BOOL
    if not fn(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return counters.PrivateUsage


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    reports = []
    for soundfont in [False, True]:
        command = [os.environ.get("MUSIC_MCP_BINARY", str(ROOT / "build/windows/musescore-mcp.exe"))]
        if os.environ.get("MUSIC_MCP_RESOURCES"):
            command += ["--resources", os.environ["MUSIC_MCP_RESOURCES"]]
        if soundfont: command += ["--soundfont", str(ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3")]
        with (OUT / f"stderr-{soundfont}.log").open("wb") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, cwd=OUT)
            lines = queue.Queue()
            def receive():
                for line in process.stdout: lines.put(line)
                lines.put(None)
            threading.Thread(target=receive, daemon=True).start()
            counter = 0
            def exchange(method, params=None):
                nonlocal counter
                counter += 1
                request = {"jsonrpc": "2.0", "id": counter, "method": method}
                if params is not None: request["params"] = params
                process.stdin.write(json.dumps(request).encode() + b"\n")
                process.stdin.flush()
                raw = lines.get(timeout=30)
                assert raw, "Server exited"
                response = json.loads(raw)
                assert response["id"] == counter and "error" not in response, response
                return response["result"]
            def call(name, args, error=False):
                result = exchange("tools/call", {"name": name, "arguments": args})
                assert result["isError"] == error, result
                return result.get("structuredContent")
            try:
                exchange("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "stress", "version": "1"}})
                process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
                process.stdin.flush()
                # JSON nesting and Windows control-Z must fail without truncating the stream.
                process.stdin.write(b'{"jsonrpc":"2.0","id":-1,"method":"x","params":' + b'[' * 2000 + b'0' + b']' * 2000 + b'}\n')
                process.stdin.write(b'\x1a\n')
                process.stdin.flush()
                assert json.loads(lines.get(timeout=10))["error"]["code"] == -32600
                assert json.loads(lines.get(timeout=10))["error"]["code"] == -32700
                exchange("ping")
                score = {"parts": [{"name": "Piano", "midiProgram": 1, "measures": [
                    {"notes": [{"pitches": [60, 64, 67], "ticks": 1920}]}]}]}
                samples = []
                for iteration in range(120):
                    doc = call("score_create", {"score": score})
                    if iteration % 10 == 9:
                        fmt = "wav" if soundfont else "pdf"
                        call("score_export", {"documentId": doc["documentId"], "format": fmt,
                                              "path": str(OUT / f"cycle.{fmt}")})
                    call("score_close", {"documentId": doc["documentId"]})
                    if iteration % 10 == 9: samples.append({"cycle": iteration + 1, "private_bytes": memory(process)})
                growth = samples[-1]["private_bytes"] - samples[3]["private_bytes"]
                (OUT / f"memory-{soundfont}.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
                assert growth < 16 * 1024 * 1024, f"Memory grew after warmup: {growth}"
                call("score_create", {"score": {"title": "\ufffe", **score}}, error=True)
                call("score_open", {"path": "missing-♯-乐谱.musicxml"}, error=True)
                exchange("ping")
                process.stdin.close()
                assert process.wait(timeout=20) == 0
                reports.append({"soundfont": soundfont, "cycles": 120, "growth_after_40": growth, "samples": samples})
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    (OUT / "validation.json").write_text(json.dumps(reports, indent=2), encoding="utf-8")
    print(json.dumps([{k: v for k, v in r.items() if k != "samples"} for r in reports]))


if __name__ == "__main__":
    main()
