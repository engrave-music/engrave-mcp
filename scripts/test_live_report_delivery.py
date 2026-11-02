"""Publication must exclude watchdog exit through the final stdout write."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
from bench_export_latency import ROOT, export_args


def scenario(a, partial, dead, during_flush=False):
    out = a.output / (('partial' if partial else 'complete') +
                      ('-dead-flush' if during_flush else '-dead' if dead else '-resume'))
    out.mkdir(parents=True)
    pages = out / 'pages'; pages.mkdir()
    blocked = out / '.test-live-noncooperative'; blocked.mkdir()
    log_path = out / 'stderr.log'
    env = dict(os.environ, ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS='8000' if dead else '60000')
    if during_flush: env['ENGRAVE_MCP_TEST_SLOW_SECOND_FLUSH'] = '1'
    with log_path.open('wb') as log:
        proc = subprocess.Popen([str(a.binary.resolve()), '--resources', str(a.resources.resolve())],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=env, bufsize=0)
        def send(method, params, ident=None):
            msg = dict(jsonrpc='2.0', method=method, params=params)
            if ident is not None: msg['id'] = ident
            proc.stdin.write((json.dumps(msg)+'\n').encode()); proc.stdin.flush()

        def wait(predicate, label, timeout=25):
            end = time.monotonic() + timeout
            while not predicate():
                assert proc.poll() is None, (label, proc.returncode, log_path.read_text())
                assert time.monotonic() < end, label
                time.sleep(.005)

        # A reader is started only for explicit reads; stdout remains undrained
        # during the watchdog barrier. Queue timeouts bound every response read.
        def read_one():
            q = queue.Queue()
            def read():
                q.put(proc.stdout.readline())
            threading.Thread(target=read, daemon=True).start()
            return json.loads(q.get(timeout=25))

        try:
            send('initialize', dict(protocolVersion='2025-11-25', capabilities={},
                                   clientInfo=dict(name='report-delivery', version='1')), 1)
            assert read_one()['id'] == 1
            send('notifications/initialized', {})
            send('tools/call', dict(name='score_open', arguments=dict(path=str(ROOT/'fixtures/golden/repeat70.mscx'))), 2)
            doc = read_one()['result']['structuredContent']['document']
            send('tools/call', dict(name='score_outline', arguments=dict(document=doc)), 3)
            measure = read_one()['result']['structuredContent']['outline']['measures'][0]['id']
            send('tools/call', dict(name='score_edit', arguments=dict(document=doc, ops=[dict(
                op='set_layout_break', measure=dict(kind='id', id=measure), kind='page', on=True)])), 4)
            assert read_one()['result']['structuredContent']['ok']
            if partial: (pages/'page-0002.svg').mkdir()
            for ident in range(10, 20): send('tools/list', {}, ident)
            send('tools/call', dict(name='score_export', arguments=export_args(doc, 'svg', pages, True)), 30)
            wait(lambda: (pages/'page-0001.svg').is_file(), 'visible artifact')
            if during_flush:
                wait(lambda: 'PUBLICATION_SECOND_FLUSH' in log_path.read_text(), 'active publication')
                assert list(pages.glob('.engrave-stage-*'))
            else:
                wait(lambda: not list(pages.glob('.engrave-stage-*')), 'publication cleanup')
                send('tools/call', dict(name='score_export', arguments=export_args(doc, 'svg', blocked, True)), 31)
                wait(lambda: 'LIVE_NONCOOPERATIVE' in log_path.read_text(), 'later blocked owner')
                send('notifications/cancelled', dict(requestId=31))
                wait(lambda: 'WATCHDOG_REPORT_DEFERRED' in log_path.read_text(), 'locked watchdog recheck')
                assert proc.poll() is None
            report = None
            if dead:
                proc.stdout.close()
                assert proc.wait(timeout=25) == 74
                assert 'exit 74 after publication cleanup' in log_path.read_text()
            else:
                while report is None:
                    response = read_one()
                    if response['id'] == 30: report = response['result']['structuredContent']
                if partial:
                    assert report['error']['code'] == 'Io', report
                    reported = report['error']['published_artifacts']
                else:
                    assert report['ok'], report
                    reported = [v['path'] for v in report['artifacts']]
                visible = sorted(p.as_posix() for p in pages.glob('*.svg') if p.is_file())
                assert visible and visible == sorted(reported), report
                assert proc.wait(timeout=10) == 70
            assert not list(pages.glob('.engrave-stage-*'))
            if dead: assert not list(out.rglob('.engrave-stage-*'))
            return dict(partial=partial, dead=dead, during_flush=during_flush,
                        exit=proc.returncode, report=report)
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=5)
            proc.stdin.close()
            if not proc.stdout.closed: proc.stdout.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('binary', 'resources', 'output'): p.add_argument('--'+key, type=Path, required=True)
    a = p.parse_args(); a.output = a.output.resolve()
    results = [scenario(a, partial, dead) for partial in (False, True) for dead in (False, True)]
    results += [scenario(a, partial, True, True) for partial in (False, True)]
    (a.output/'results.json').write_text(json.dumps(results, indent=2))
    print('PASS: complete/partial report before watchdog exit; dead stdout exit 74 after cleanup (6 cases)')


if __name__ == '__main__': main()
