"""Measure live export RSS after warmup, with consecutive equal-size blocks."""
import argparse
import json
from pathlib import Path
from bench_export_latency import Client, export_args, rss_bytes, summary


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--resources',type=Path,required=True)
    p.add_argument('--fixture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--operations',type=int,default=1000)
    a=p.parse_args()
    assert a.operations>=1000 and a.operations%100==0
    a.output=a.output.resolve()
    c=Client(a.binary,a.resources,a.output,'live')
    doc=c.tool('score_open',{'path':str(a.fixture.resolve())})['document']
    args=export_args(doc,'pdf',a.output/'plateau.pdf')
    for _ in range(100): c.tool('score_export',args)
    rss=[]
    for _ in range(a.operations):
        c.tool('score_export',args)
        rss.append(rss_bytes(c.p.pid))
    c.close()
    report=dict(warmup=100,operations=a.operations,rss_bytes=rss,
        blocks=[summary(rss[i:i+100]) for i in range(0,len(rss),100)])
    (a.output/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report['blocks']))


if __name__=='__main__': main()
