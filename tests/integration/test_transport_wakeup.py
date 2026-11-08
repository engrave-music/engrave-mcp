"""Exercise blocked-stdin teardown and measure initialized idle process CPU."""

import argparse
import ctypes
import json
import os
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--binary", type=Path, required=True)
p.add_argument("--resources", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()


def start():
    child = subprocess.Popen(
        [str(a.binary.resolve()), "--resources", str(a.resources.resolve())],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    send(
        child,
        "initialize",
        {
            "protocolVersion": "2025-11-25",
            "capabilities": {},
            "clientInfo": {"name": "transport-wakeup", "version": "1"},
        },
    )
    assert json.loads(child.stdout.readline())["id"] == 1
    return child


def send(child, method, params):
    child.stdin.write((json.dumps(dict(jsonrpc="2.0", id=1, method=method, params=params)) + "\n").encode())
    child.stdin.flush()


def cpu(child):
    if os.name == "nt":
        from ctypes import wintypes

        values = [wintypes.FILETIME() for _ in range(4)]
        fn = ctypes.windll.kernel32.GetProcessTimes
        fn.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        assert fn(int(child._handle), *(ctypes.byref(v) for v in values))
        return sum((v.dwHighDateTime << 32) + v.dwLowDateTime for v in values[2:]) / 1e7
    fields = Path(f"/proc/{child.pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


child = start()
before = cpu(child)
started = time.perf_counter()
time.sleep(10)
seconds = time.perf_counter() - started
used = cpu(child) - before
child.stdin.close()
assert child.wait(timeout=5) == 0
child.stdout.close()
# Leave stdin open while broken stdout takes the documented terminal path.
# Exit 74 must not depend on waking a reader blocked inside the OS read.
for _ in range(30):
    child = start()
    child.stdout.close()
    send(child, "tools/list", {})
    assert child.wait(timeout=5) == 74
    child.stdin.close()
report = dict(
    idle_wall_seconds=seconds,
    idle_cpu_seconds=used,
    idle_cpu_percent=used / seconds * 100,
    broken_stdout_open_stdin_sessions=30,
)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
