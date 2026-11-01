"""M1 routing, revisions, cancellation, atomic publication, multi-page exports.

Use --binary engrave-mcp-test for the deterministic owner-queue cancellation seam.
Direct engine byte comparisons additionally run in export_job_test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import zipfile
import re
from bench_export_latency import Client, ROOT, export_args

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary",type=Path,required=True)
    p.add_argument("--resources",type=Path,required=True)
    p.add_argument("--output",type=Path,required=True)
    a=p.parse_args()
    a.output=a.output.resolve()
    results={}
    for mode in (None,"isolated","live"):
        out=a.output/(mode or "default")
        c=Client(a.binary,a.resources,out,mode)
        doc=c.tool("score_open",{"path":str(ROOT/"fixtures/golden/repeat70.mscx")})["document"]
        hashes={}
        for fmt in ("mscz","mscx","musicxml","mxl","midi","pdf","svg","png"):
            path=out/fmt
            if fmt in ("svg","png"): path.mkdir(exist_ok=True)
            args=export_args(doc,"svg" if fmt=="png" else fmt,path)
            args["options"]["format"]=fmt
            if fmt in ("mscx","musicxml","mxl","midi"):
                args["kind"]="score"; args["options"]={"format":fmt}
            r=c.tool("score_export",args)
            artifact=Path(r["artifacts"][0]["path"])
            content=artifact.read_bytes()
            if fmt in ("mscz","mxl"):
                with zipfile.ZipFile(artifact) as archive:
                    content=b"".join(name.encode()+archive.read(name) for name in sorted(archive.namelist()))
            if fmt=="pdf":
                content=re.sub(rb"/(CreationDate|ModDate) \(D:[^)]*\)",rb"/\1 (D:normalized)",content)
            hashes[fmt]=hashlib.sha256(content).hexdigest()
            args["expected_revision"]="999"
            assert c.tool("score_export",args,False)["error"]["code"]=="RevisionConflict"
        positions=out/"positions.json"
        c.tool("score_export",dict(document=doc,action="run",kind="positions",options={"position_kind":"measure"},destination={"kind":"file","path":str(positions)}))
        hashes["positions"]=hashlib.sha256(positions.read_bytes()).hexdigest()
        outline=c.tool("score_outline",{"document":doc})["outline"]
        measure=outline["measures"][0]["id"]
        c.tool("score_edit",{"document":doc,"ops":[{"op":"set_layout_break","measure":{"kind":"id","id":measure},"kind":"page","on":True}]})
        pages=out/"pages"; pages.mkdir(exist_ok=True)
        r=c.tool("score_export",export_args(doc,"svg",pages,True))
        assert len(r["artifacts"])>=2
        # Failure on page 2 preserves page 1 and reports it, no staged leftovers.
        partial=out/"partial"; partial.mkdir(exist_ok=True)
        (partial/"page-0002.svg").mkdir(exist_ok=True)
        r=c.tool("score_export",export_args(doc,"svg",partial,True),False)
        assert r["error"]["published_artifacts"]==[str((partial/"page-0001.svg").resolve()).replace("\\","/")] or len(r["error"]["published_artifacts"])==1
        assert not list(partial.glob(".musescore-job-*"))
        c.tool("score_export",export_args(doc,"pdf",out/"multi.pdf",True))
        # Queue an export behind a known running owner task, cancel before engine use.
        sleeper=c.send("tools/call",{"name":"test_owner_sleep","arguments":{}})
        time.sleep(.05)
        dest=out/"cancel.pdf"
        ident=c.send("tools/call",{"name":"score_export","arguments":export_args(doc,"pdf",dest)})
        c.send("notifications/cancelled",{"requestId":ident},True)
        c.receive(sleeper)
        assert c.receive(ident)["error"]["code"]==-32800
        assert not dest.exists()
        phases=c.close()
        modes={x["mode"] for x in phases}
        assert ("live" in modes)==(mode=="live"),modes
        results[mode or "default"]=hashes
    (a.output/"comparison.json").write_text(json.dumps(results,indent=2))
    for fmt in results["default"]:
        if fmt != "pdf": assert results["default"][fmt]==results["isolated"][fmt],fmt
    for fmt in ("mscz","mscx","musicxml","mxl","midi","positions"):
        assert results["live"][fmt]==results["isolated"][fmt],fmt
    (a.output/"comparison.json").write_text(json.dumps(results,indent=2))
    print("live export passed; render hash differences retained in comparison.json")

if __name__=="__main__": main()
