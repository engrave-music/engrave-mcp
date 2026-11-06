"""Deterministic publication lock, visible-artifact and terminal resource regressions."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import psutil
from support.process_client import ROOT, export_args


class Host:
    def __init__(self, a, out, env=None, drain=True, regular=False):
        self.out = out
        out.mkdir(parents=True)
        self.regular = open(out / "regular-stderr.log", "wb") if regular else None
        self.p = subprocess.Popen(
            [str(a.binary.resolve()), "--resources", str(a.resources.resolve())],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.regular or subprocess.PIPE,
            env=dict(os.environ, **(env or {})),
            bufsize=0,
        )
        self.lines = queue.Queue()
        self.log = bytearray()
        if drain and not regular:

            def read_log():
                while data := self.p.stderr.readline():
                    self.log.extend(data)

            self.reader = threading.Thread(target=read_log, daemon=True)
            self.reader.start()
        self.send(
            "initialize",
            dict(
                protocolVersion="2025-11-25",
                capabilities={},
                clientInfo=dict(name="terminal-contract", version="1"),
            ),
            1,
        )
        assert self.read()["id"] == 1
        self.send("notifications/initialized", {})
        self.send(
            "tools/call",
            dict(
                name="score_open",
                arguments=dict(path=str(ROOT / "tests/data/inputs/golden/repeat70.mscx")),
            ),
            2,
        )
        self.doc = self.read()["result"]["structuredContent"]["document"]

    def send(self, method, params, ident=None):
        value = dict(jsonrpc="2.0", method=method, params=params)
        if ident is not None:
            value["id"] = ident
        self.p.stdin.write((json.dumps(value) + "\n").encode())
        self.p.stdin.flush()

    def read(self):
        q = queue.Queue()
        threading.Thread(target=lambda: q.put(self.p.stdout.readline()), daemon=True).start()
        return json.loads(q.get(timeout=20))

    def wait(self, predicate, label):
        end = time.monotonic() + 20
        while not predicate():
            assert self.p.poll() is None, (label, self.p.returncode, bytes(self.log))
            assert time.monotonic() < end, (label, bytes(self.log))
            time.sleep(0.005)

    def marker(self, label):
        self.wait(lambda: label.encode() in self.log, label)

    def pages(self):
        self.send(
            "tools/call",
            dict(name="score_outline", arguments=dict(document=self.doc)),
            3,
        )
        measure = self.read()["result"]["structuredContent"]["outline"]["measures"][0]["id"]
        self.send(
            "tools/call",
            dict(
                name="score_edit",
                arguments=dict(
                    document=self.doc,
                    ops=[
                        dict(
                            op="set_layout_break",
                            measure=dict(kind="id", id=measure),
                            kind="page",
                            on=True,
                        )
                    ],
                ),
            ),
            4,
        )
        assert self.read()["result"]["structuredContent"]["ok"]
        pages = self.out / "pages"
        pages.mkdir()
        return pages

    def export(self, pages, ident=30):
        self.send(
            "tools/call",
            dict(name="score_export", arguments=export_args(self.doc, "svg", pages, True)),
            ident,
        )

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:
                assert self.p.wait(timeout=10) == 0
            finally:
                if self.p.poll() is None:
                    self.p.kill()
                    self.p.wait()
        if hasattr(self, "reader"):
            self.reader.join(timeout=2)
        elif self.regular:
            self.regular.close()
        else:
            self.log.extend(self.p.stderr.read())
        (self.out / "stderr.log").write_bytes(self.log)
        self.p.stdout.close()
        if self.p.stderr:
            self.p.stderr.close()


def artifact_fault(a, after=None, partial=False, wrapper=False, boundary=None):
    env = {}
    if after:
        env["ENGRAVE_MCP_TEST_THROW_AFTER_RENAME"] = str(after)
    if wrapper:
        env["ENGRAVE_MCP_TEST_THROW_WRAPPER"] = "1"
    if boundary:
        env["ENGRAVE_MCP_TEST_THROW_" + boundary] = "1"
    h = Host(a, a.output / f"fault-{after}-{partial}-{wrapper}-{boundary}", env)
    try:
        pages = h.pages()
        if partial:
            (pages / "page-0002.svg").mkdir()
        h.export(pages)
        result = h.read()["result"]["structuredContent"]
        assert result["error"]["code"] == ("Io" if after and not boundary else "EngineFailure"), result
        visible = sorted(p.as_posix() for p in pages.glob("*.svg") if p.is_file())
        assert visible == sorted(result["error"]["published_artifacts"]) and visible, result
        assert len(visible) == (after if after else (1 if partial else 2))
        assert not list(pages.glob(".engrave-stage-*"))
        return dict(after=after, partial=partial, wrapper=wrapper, visible=visible)
    finally:
        h.close()


def barrier(a, dead, report=True):
    out = a.output / f"barrier-{dead}-{report}"
    release = out / "release"
    h = Host(a, out, dict(ENGRAVE_MCP_TEST_WRITE_BARRIER=str(release)))
    try:
        pages = h.pages()
        queued = out / "queued"
        queued.mkdir()
        Path(str(release) + ".arm").touch()
        if report:
            h.export(pages)
        else:
            h.send("tools/list", {}, 10)
        h.marker("WRITE_REGISTERED")
        # The next owner cannot start while the report write holds the mutex.
        enqueued = h.log.count(b"OWNER_ENQUEUED")
        h.export(queued, 31)
        h.wait(lambda: h.log.count(b"OWNER_ENQUEUED") > enqueued, "queued owner admitted")
        if not dead:
            h.send("notifications/cancelled", dict(requestId=31))
        h.send("tools/list", {}, 12)
        assert not list(queued.iterdir())
        if dead:
            h.p.stdout.close()
        release.touch()
        if dead:
            assert h.p.wait(timeout=5) == 74
            h.reader.join(timeout=2)
            assert b"exit 74" in h.log, bytes(h.log)
            assert not list(queued.iterdir())
            assert not list(out.rglob(".engrave-stage-*"))
        else:
            seen = {}
            while not {30, 31, 12} <= seen.keys():
                r = h.read()
                seen[r["id"]] = r
            assert seen[30]["result"]["structuredContent"]["ok"]
            assert seen[31]["error"]["code"] == -32800
        return dict(dead=dead, report=report, exit=h.p.returncode)
    finally:
        h.close()


def watchdog_barrier(a):
    out = a.output / "watchdog-barrier"
    release = out / "release"
    h = Host(
        a,
        out,
        dict(
            ENGRAVE_MCP_TEST_WRITE_BARRIER=str(release),
            ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS="60000",
            ENGRAVE_MCP_TEST_OWNER_UNTIL_WRITE_FAILURE="1",
        ),
    )
    try:
        pages = out / ".test-live-noncooperative"
        pages.mkdir()
        h.export(pages)
        h.marker("LIVE_NONCOOPERATIVE")
        Path(str(release) + ".arm").touch()
        h.send("tools/list", {}, 10)
        h.marker("WRITE_REGISTERED")
        h.send("notifications/cancelled", dict(requestId=30))
        h.marker("WATCHDOG_CLAIMED")
        h.marker("WATCHDOG_LOCK_WAITING")
        assert h.p.poll() is None
        h.p.stdout.close()
        release.touch()
        assert h.p.wait(timeout=5) == 74
        assert not list(out.rglob(".engrave-stage-*"))
        return dict(watchdog_barrier=True, exit=74)
    finally:
        h.close()


def diagnostic_cycles(a):
    h = Host(
        a,
        a.output / "diagnostics",
        dict(ENGRAVE_MCP_TEST_FAIL_CLEANUP="1"),
        drain=False,
    )
    try:
        if os.name == "posix":
            import fcntl

            fcntl.fcntl(h.p.stderr, fcntl.F_SETPIPE_SZ, 4096)
        pages = h.out / ("p" * 90)
        pages.mkdir()
        proc = psutil.Process(h.p.pid)
        base = proc.num_threads()
        samples = []
        for i in range(80):
            args = dict(
                document=h.doc,
                kind="render",
                options=dict(format="svg", first_page=1000, last_page=1000),
                destination=dict(kind="directory", path=str(pages)),
            )
            h.send("tools/call", dict(name="score_export", arguments=args), i + 100)
            result = h.read()["result"]["structuredContent"]
            assert result["error"]["code"] == "InvalidArgument", result
            samples.append(proc.num_threads())
        if os.name == "posix":
            import array
            import termios

            pending = array.array("i", [0])
            fcntl.ioctl(h.p.stderr, termios.FIONREAD, pending)
            capacity = fcntl.fcntl(h.p.stderr, fcntl.F_GETPIPE_SZ)
            assert pending[0] > capacity - 1024, (pending[0], capacity)
        assert max(samples) <= base, (base, samples)
        assert len(list(pages.glob(".engrave-stage-*"))) == 80
        h.p.stdout.close()
        h.send("tools/list", {}, 999)
        assert h.p.wait(timeout=5) == 74
        return dict(
            cycles=80,
            base_threads=base,
            max_threads=max(samples),
            exit=74,
            pipe_bytes=pending[0] if os.name == "posix" else None,
            pipe_capacity=capacity if os.name == "posix" else None,
        )
    finally:
        h.close()


def after_closing(a, timeout=False):
    out = a.output / f"after-closing-{timeout}"
    release = out / "release"
    h = Host(
        a,
        out,
        dict(
            ENGRAVE_MCP_TEST_CLOSING_BARRIER=str(release),
            ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS="60000",
        ),
    )
    try:
        queued = out / "queued"
        queued.mkdir()
        if timeout:
            blocked = out / ".test-live-noncooperative"
            blocked.mkdir()
            h.export(blocked, 20)
            h.marker("LIVE_NONCOOPERATIVE")
            h.export(queued, 21)
        h.p.stdout.close()
        h.send("tools/list", {}, 10)
        h.marker("CLOSING_REGISTERED")
        h.export(queued, 22)
        h.marker("ADMISSION_REFUSED_CLOSING")
        start = time.monotonic()
        release.touch()
        assert h.p.wait(timeout=2) == 74
        elapsed = time.monotonic() - start
        assert elapsed < 1.5, elapsed
        if timeout:
            assert elapsed >= 0.45, elapsed
        h.reader.join(timeout=2)
        assert not list(queued.iterdir())
        if timeout:
            assert b"cleanup timed out; staging may remain" in h.log, bytes(h.log)
            assert list(blocked.glob(".engrave-stage-*"))
        else:
            assert not list(out.rglob(".engrave-stage-*"))
        return dict(after_closing=True, cleanup_timeout=timeout, exit=74, elapsed=elapsed)
    finally:
        h.close()


def after_report(a):
    h = Host(a, a.output / "after-report", dict(ENGRAVE_MCP_TEST_THROW_AFTER_REPORT="1"))
    try:
        h.export(h.pages())
        assert h.read()["result"]["structuredContent"]["ok"]
        h.send("tools/list", {}, 99)
        assert h.read()["id"] == 99  # a duplicate response would precede this
        return dict(after_report=True, responses=1)
    finally:
        h.close()


def refused_cleanup(a, watchdog=False):
    out = a.output / f"refused-cleanup-{watchdog}"
    admission, terminal = out / "admit", out / "terminal"
    h = Host(
        a,
        out,
        dict(
            ENGRAVE_MCP_TEST_ADMISSION_BARRIER=str(admission),
            ENGRAVE_MCP_TEST_CLOSING_BARRIER=str(terminal),
            ENGRAVE_MCP_TEST_CLEANUP_BARRIER=str(out / "never-release"),
        ),
    )
    try:
        pages = h.pages()
        h.export(pages)
        h.marker("PUBLICATION_READY")
        if watchdog:
            h.send("notifications/cancelled", dict(requestId=30))
            h.marker("WATCHDOG_CLAIMED")
            admission.touch()
            h.marker("CLEANUP_BLOCKED")
            # The watchdog must acquire the fence despite blocked cleanup.
            assert h.p.wait(timeout=2) == 70
        else:
            queued = out / "queued"
            queued.mkdir()
            enqueued = h.log.count(b"OWNER_ENQUEUED")
            h.export(queued, 31)
            h.wait(
                lambda: h.log.count(b"OWNER_ENQUEUED") > enqueued,
                "queued owner admitted",
            )
            h.p.stdout.close()
            h.send("tools/list", {}, 90)
            h.marker("CLOSING_REGISTERED")
            admission.touch()
            h.marker("CLEANUP_BLOCKED")
            start = time.monotonic()
            terminal.touch()
            assert h.p.wait(timeout=2) == 74
            elapsed = time.monotonic() - start
            assert 0.45 <= elapsed < 1.5, elapsed
            assert not list(queued.iterdir())
        assert list(pages.glob(".engrave-stage-*"))
        assert not list(pages.glob("*.svg"))
        return dict(
            refused_cleanup=True,
            watchdog=watchdog,
            exit=h.p.returncode,
            elapsed=elapsed if not watchdog else None,
        )
    finally:
        h.close()


def regular_diagnostic(a):
    if os.name != "posix":
        return dict(regular_stderr="POSIX only")
    # Interpose the exact POSIX write used by terminal_diagnostic: any stderr
    # write would stall for a minute. Validate the shim before testing the host.
    probe = a.output / "diagnostic-probe"
    probe.mkdir()
    source = probe / "stall.c"
    library = probe / "stall.so"
    source.write_text(
        "#define _GNU_SOURCE\n#include <unistd.h>\n#include <dlfcn.h>\n"
        "ssize_t write(int fd,const void* p,size_t n) {\n"
        "if(fd==2) sleep(60);\n"
        'return ((ssize_t(*)(int,const void*,size_t))dlsym(RTLD_NEXT,"write"))(fd,p,n); }\n'
    )
    subprocess.run(
        [
            shutil.which("cc"),
            "-shared",
            "-fPIC",
            str(source),
            "-o",
            str(library),
            "-ldl",
        ],
        check=True,
    )
    env = dict(os.environ, LD_PRELOAD=str(library))
    with open(probe / "probe.log", "wb") as dest:
        try:
            subprocess.run(
                [sys.executable, "-c", 'import os; os.write(2,b"blocked")'],
                env=env,
                stderr=dest,
                timeout=0.25,
                check=True,
            )
            raise AssertionError("stderr write shim did not stall")
        except subprocess.TimeoutExpired:
            pass
    h = Host(a, a.output / "regular-stderr", dict(LD_PRELOAD=str(library)), regular=True)
    try:
        initial = (h.out / "regular-stderr.log").stat().st_size
        h.p.stdout.close()
        start = time.monotonic()
        h.send("tools/list", {}, 90)
        assert h.p.wait(timeout=2) == 74
        elapsed = time.monotonic() - start
        assert elapsed < 1.5, elapsed
        assert (h.out / "regular-stderr.log").stat().st_size == initial
        return dict(regular_stderr="skipped", exit=74, elapsed=elapsed)
    finally:
        h.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ("binary", "resources", "output"):
        p.add_argument("--" + k, type=Path, required=True)
    a = p.parse_args()
    a.output = a.output.resolve()
    results = [artifact_fault(a, after=i) for i in (1, 2)]
    results += [artifact_fault(a, partial=partial, wrapper=True) for partial in (False, True)]
    results += [
        artifact_fault(a, after=1, boundary="ERROR_REPORT"),
        artifact_fault(a, boundary="DOCUMENT_CALLBACK"),
        after_report(a),
        refused_cleanup(a),
        refused_cleanup(a, True),
        regular_diagnostic(a),
    ]
    results += [barrier(a, dead, report) for dead, report in ((False, True), (True, True), (True, False))]
    results += [
        watchdog_barrier(a),
        diagnostic_cycles(a),
        after_closing(a),
        after_closing(a, True),
    ]
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
    print("PASS: visible paths, real wrapper faults, queued fencing, publication/watchdog lock, 80 bounded diagnostics")


if __name__ == "__main__":
    main()
