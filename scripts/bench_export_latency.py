"""Native JSON-RPC latency benchmark; raw samples and server phases are retained.

Engine call includes encoding (the public API returns encoded bytes). Response
phase ends on session resume; client total includes transport serialization.
"""
import argparse
import json
import math
import os
from pathlib import Path
import queue
import statistics
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]

class Client:
    def __init__(self, binary, resources, output, mode=None):
        output.mkdir(parents=True, exist_ok=True)
        self.log_path = output / "stderr.log"
        self.log = self.log_path.open("w", encoding="utf-8")
        args = [str(binary.resolve()), "--resources", str(resources.resolve())]
        if mode: args.append("--export-mode=" + mode)
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.log, env=dict(os.environ, ENGRAVE_MCP_PROFILE_EXPORT="1"))
        self.q = queue.Queue()
        self.ident = 0
        def read():
            for line in self.p.stdout:
                self.q.put(json.loads(line))
            self.q.put({"error":"server exited", "exit":self.p.wait()})
        threading.Thread(target=read, daemon=True).start()
        self.rpc("initialize", {"protocolVersion":"2025-11-25", "capabilities":{},
            "clientInfo":{"name":"latency", "version":"1"}})
        self.send("notifications/initialized", {}, True)

    def send(self, method, params, notification=False):
        self.ident += 1
        msg = dict(jsonrpc="2.0", method=method, params=params)
        if not notification: msg["id"] = self.ident
        self.p.stdin.write((json.dumps(msg)+"\n").encode())
        self.p.stdin.flush()
        return self.ident

    def receive(self, ident):
        while True:
            msg = self.q.get(timeout=150)
            if msg.get("error") == "server exited": raise RuntimeError(msg)
            if msg.get("id") == ident: return msg

    def rpc(self, method, params):
        return self.receive(self.send(method, params))

    def tool(self, name, args, ok=True):
        msg = self.rpc("tools/call", {"name":name, "arguments":args})
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
                phases.append(json.loads(line[len("EXPORT_TIMING "):]))
        return phases


def summary(samples):
    values = sorted(samples)
    return dict(median=statistics.median(values), p95=values[math.ceil(.95*len(values))-1],
        max=max(values), n=len(values))


def export_args(doc, fmt, path, all_pages=False):
    directory = fmt == "svg"
    options = {"format":fmt}
    if fmt != "mscz": options.update(first_page=0, last_page=-1 if all_pages else 0)
    return dict(document=doc, action="run", kind="score" if fmt=="mscz" else "render",
        options=options, destination=dict(kind="directory" if directory else "file", path=str(path.resolve())))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--resources", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--warm", type=int, default=100)
    p.add_argument("--cold", type=int, default=30)
    a = p.parse_args()
    assert a.warm >= 50 and a.cold >= 10
    a.output = a.output.resolve()
    report = {"warm":[], "cold":[], "phase_semantics":"engine_call includes encoding; encode_write is host staging; response ends at session resume"}
    for mode in ("live", "isolated"):
        for fixture in ("repeat70.mscx", "lyrics.mscx"):
            out = a.output/mode/fixture
            c = Client(a.binary, a.resources, out, mode)
            doc = c.tool("score_open", {"path":str(ROOT/"fixtures/golden"/fixture)})["document"]
            for fmt, pages in (("svg",False), ("pdf",False), ("mscz",False), ("pdf",True)):
                if pages:
                    measure=c.tool("score_outline",{"document":doc})["outline"]["measures"][0]["id"]
                    c.tool("score_edit",{"document":doc,"ops":[{"op":"set_layout_break",
                        "measure":{"kind":"id","id":measure},"kind":"page","on":True}]})
                path = out/(fmt + ("-all" if pages else ""))
                if fmt=="svg": path.mkdir(exist_ok=True)
                args = export_args(doc, fmt, path, pages)
                c.tool("score_export", args) # warmup excluded
                samples=[]
                for _ in range(a.warm):
                    start=time.perf_counter()
                    c.tool("score_export", args)
                    samples.append((time.perf_counter()-start)*1000)
                report["warm"].append(dict(mode=mode,fixture=fixture,format=fmt,all_pages=pages,
                    total_ms=summary(samples),samples_ms=samples))
            # Reapply a real edit before each render; export timing excludes the edit's layout.
            post=[]
            for _ in range(a.warm):
                c.tool("score_edit",{"document":doc,"ops":[{"op":"set_layout_break",
                    "measure":{"kind":"id","id":measure},"kind":"page","on":bool(_ % 2)}]})
                start=time.perf_counter()
                c.tool("score_export",export_args(doc,"pdf",out/"post-edit.pdf"))
                post.append((time.perf_counter()-start)*1000)
            report["warm"].append(dict(mode=mode,fixture=fixture,format="post-edit.pdf",all_pages=False,
                total_ms=summary(post),samples_ms=post))
            phases=c.close()
            for row in report["warm"]:
                if row["mode"]==mode and row["fixture"]==fixture:
                    key=str((out/(row["format"]+("-all" if row["all_pages"] else ""))).resolve()).replace("\\","/")
                    matches=[x for x in phases if x["destination"]==key]
                    row["server_samples"]=matches
            samples=[]
            cold_phases=[]
            for i in range(a.cold):
                start=time.perf_counter()
                c=Client(a.binary,a.resources,out/("cold-"+str(i)),mode)
                doc=c.tool("score_open", {"path":str(ROOT/"fixtures/golden"/fixture)})["document"]
                c.tool("score_export",export_args(doc,"pdf",out/("cold-"+str(i)+".pdf")))
                samples.append((time.perf_counter()-start)*1000)
                cold_phases.extend(c.close())
            report["cold"].append(dict(mode=mode,fixture=fixture,total_ms=summary(samples),samples_ms=samples,server_samples=cold_phases))
    (a.output/"results.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    for row in report["warm"]+report["cold"]:
        print(json.dumps({k:v for k,v in row.items() if k not in ("samples_ms","server_samples")}))

if __name__=="__main__": main()
