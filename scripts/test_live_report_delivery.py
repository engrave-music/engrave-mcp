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


def scenario(a, partial, dead, during_flush=False, wrapper=False, cleanup=False, registration=False):
    out = a.output / (('partial' if partial else 'complete') +
                      ('-dead-flush' if during_flush else '-dead' if dead else '-resume') + ('-wrapper' if wrapper else '') + ('-cleanup' if cleanup else '') + ('-registration' if registration else ''))
    out.mkdir(parents=True)
    pages = out / 'pages'; pages.mkdir()
    blocked = out / '.test-live-noncooperative'; blocked.mkdir()
    log_path = out / 'stderr.log'
    env = dict(os.environ, ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS='8000' if dead else '60000')
    barrier = out / 'write-release'
    env['ENGRAVE_MCP_TEST_WRITE_BARRIER'] = str(barrier)
    if dead: env['ENGRAVE_MCP_TEST_OWNER_UNTIL_WRITE_FAILURE'] = '1'
    if cleanup: env['ENGRAVE_MCP_TEST_FAIL_CLEANUP'] = '1'
    if wrapper: env['ENGRAVE_MCP_TEST_THROW_WRAPPER'] = '1'
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
            if registration:
                Path(str(barrier)+'.arm').touch()
                send('tools/list', {}, 10)
                wait(lambda: 'WRITE_REGISTERED' in log_path.read_text(), 'registered writer')
                send('tools/call', dict(name='score_export', arguments=export_args(doc, 'svg', pages, True)), 30)
                wait(lambda: 'PUBLICATION_WAITING_FOR_WRITE' in log_path.read_text(), 'publication barred by writer')
                assert not list(pages.glob('*.svg'))
                send('notifications/cancelled', dict(requestId=30))
                wait(lambda: 'WATCHDOG_WRITE_DEFERRED' in log_path.read_text(), 'watchdog barred by writer without publisher')
                assert proc.poll() is None
                proc.stdout.close(); barrier.touch()
                assert proc.wait(timeout=25) == 74
                assert not list(pages.glob('*.svg'))
                assert not list(pages.glob('.engrave-stage-*'))
                return dict(registration=True, exit=74)
            if partial: (pages/'page-0002.svg').mkdir()
            if not during_flush: Path(str(barrier)+'.arm').touch()
            send('tools/call', dict(name='score_export', arguments=export_args(doc, 'svg', pages, True)), 30)
            wait(lambda: (pages/'page-0001.svg').is_file(), 'visible artifact')
            if during_flush:
                wait(lambda: 'PUBLICATION_SECOND_FLUSH' in log_path.read_text(), 'active publication')
                assert list(pages.glob('.engrave-stage-*'))
                Path(str(barrier)+'.arm').touch()
                send('tools/list', {}, 10)
            else:
                if not cleanup: wait(lambda: not list(pages.glob('.engrave-stage-*')), 'publication cleanup')
                send('tools/call', dict(name='score_export', arguments=export_args(doc, 'svg', blocked, True)), 31)
                wait(lambda: 'LIVE_NONCOOPERATIVE' in log_path.read_text(), 'later blocked owner')
                send('notifications/cancelled', dict(requestId=31))
                wait(lambda: 'WATCHDOG_REPORT_DEFERRED' in log_path.read_text(), 'locked watchdog recheck')
                assert proc.poll() is None
            wait(lambda: 'WRITE_REGISTERED' in log_path.read_text(), 'writer registered under gate')
            report = None
            if dead:
                proc.stdout.close()
                barrier.touch()
                assert proc.wait(timeout=25) == 74
                assert 'exit 74 after publication cleanup' in log_path.read_text()
                assert ('FLUSH_OBSERVED_WRITE_FAILURE' if during_flush else 'OWNER_OBSERVED_WRITE_FAILURE') in log_path.read_text()
            else:
                barrier.touch()
                while report is None:
                    response = read_one()
                    if response['id'] == 30: report = response['result']['structuredContent']
                if partial or wrapper:
                    assert report['error']['code'] == ('EngineFailure' if wrapper else 'Io'), report
                    reported = report['error']['published_artifacts']
                else:
                    assert report['ok'], report
                    reported = [v['path'] for v in report['artifacts']]
                visible = sorted(p.as_posix() for p in pages.glob('*.svg') if p.is_file())
                assert visible and visible == sorted(reported), report
                assert proc.wait(timeout=10) == 70
            if cleanup:
                assert list(pages.glob('.engrave-stage-*'))
                assert 'staging cleanup failed:' in log_path.read_text()
                assert 'terminal staging cleanup failures:' in log_path.read_text()
            else:
                assert not list(pages.glob('.engrave-stage-*'))
                if dead: assert not list(out.rglob('.engrave-stage-*'))
            return dict(partial=partial, dead=dead, during_flush=during_flush,
                        exit=proc.returncode, report=report)
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=5)
            proc.stdin.close()
            if not proc.stdout.closed: proc.stdout.close()


def full_stderr(a):
    env = dict(os.environ, ENGRAVE_MCP_TEST_FILL_TERMINAL_STDERR='1')
    proc = subprocess.Popen([str(a.binary.resolve()), '--resources', str(a.resources.resolve())],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        def send(method, params):
            proc.stdin.write((json.dumps(dict(jsonrpc='2.0', id=1, method=method, params=params))+'\n').encode())
            proc.stdin.flush()
        send('initialize', dict(protocolVersion='2025-11-25', capabilities={},
             clientInfo=dict(name='full-stderr', version='1')))
        assert json.loads(proc.stdout.readline())['id'] == 1
        proc.stdout.close()
        send('tools/list', {})
        # Never drain stderr until exit. The injected 1 MiB diagnostic exceeds
        # anonymous pipe capacity on both hosts; its writer cannot complete.
        assert proc.wait(timeout=10) == 74
        data = proc.stderr.read()
        # Windows may buffer the entire pending large write until it completes.
        # Exit 75 would mean the diagnostic completed rather than timing out.
        assert len(data) < 1024*1024, len(data)
        return dict(full_stderr=True, exit=74, diagnostic_bytes=len(data))
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        proc.stdin.close(); proc.stderr.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('binary', 'resources', 'output'): p.add_argument('--'+key, type=Path, required=True)
    a = p.parse_args(); a.output = a.output.resolve()
    results = [scenario(a, partial, dead) for partial in (False, True) for dead in (False, True)]
    results += [scenario(a, partial, True, True) for partial in (False, True)]
    results += [scenario(a, partial, False, wrapper=True) for partial in (False, True)]
    results += [scenario(a, False, True, cleanup=True), scenario(a, False, True, registration=True), full_stderr(a)]
    (a.output/'results.json').write_text(json.dumps(results, indent=2))
    print('PASS: complete/partial report before watchdog exit; dead stdout exit 74 after cleanup (11 cases)')


if __name__ == '__main__': main()
