"""Fail-closed, stage-synchronized 4.70 conversion interruption.

Requires engrave-mcp-test (MUSIC_ASYNC_TEST_HOOKS); the release host must fail.
The callback pauses at Converting unit 2 for a real MCP cancel notification,
or expires the real reservation there for the deadline variant.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Client:
    def __init__(self, args, out):
        self.log = (out / 'wire.jsonl').open('w', encoding='utf-8')
        self.errors = (out / 'stderr.log').open('w', encoding='utf-8')
        self.process = subprocess.Popen([str(args.binary.resolve()), '--resources',
            str(args.resources.resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errors, env=dict(os.environ))
        self.responses = queue.Queue()
        self.counter = 0
        def read():
            for line in self.process.stdout:
                self.responses.put(json.loads(line))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, method, params, notification=False):
        self.counter += 1
        request = dict(jsonrpc='2.0', method=method, params=params)
        if not notification:
            request['id'] = self.counter
        self.log.write(json.dumps(dict(time=time.monotonic(), sent=request))+'\n')
        self.log.flush()
        self.process.stdin.write((json.dumps(request)+'\n').encode())
        self.process.stdin.flush()
        return self.counter

    def receive(self, ident):
        deadline = time.monotonic()+150
        while time.monotonic() < deadline:
            response = self.responses.get(timeout=max(.01, deadline-time.monotonic()))
            self.log.write(json.dumps(dict(time=time.monotonic(), received=response))+'\n')
            self.log.flush()
            if response.get('id') == ident:
                return response
        raise TimeoutError(ident)

    def call(self, tool, args):
        response = self.receive(self.send('tools/call', dict(name=tool, arguments=args)))
        assert 'result' in response, response
        return response['result']['structuredContent']

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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--resources', type=Path, required=True)
    p.add_argument('--host-sha256', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    assert sha(args.binary) == args.host_sha256, 'host hash mismatch'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT/'tests/phase_b/notes.mscx'
    preflight = dict(host_sha256=sha(args.binary), source_sha256=sha(source),
        engine_revision=subprocess.check_output(['git', '-C', str(ROOT/'vendor/musescore'),
            'rev-parse', 'HEAD'], text=True).strip(),
        assets={str(f.relative_to(args.resources)):sha(f) for f in sorted(args.resources.rglob('*')) if f.is_file()})
    (out/'preflight.json').write_text(json.dumps(preflight, indent=2))
    client = Client(args, out)
    records = []
    complete = False
    try:
        client.receive(client.send('initialize', dict(protocolVersion='2024-11-05',
            capabilities={}, clientInfo=dict(name='phase-b-conversion-interruption', version='2'))))
        client.send('notifications/initialized', {}, notification=True)
        opened = client.call('score_open', dict(path=str(source)))
        assert opened['ok'], opened
        def arguments(action, fmt, destination):
            return dict(document=opened['document'], kind='score',
                options=dict(format=fmt, native_version='4.70'),
                destination=dict(kind='file', path=str(destination)))
        for action in ('live',):
            for fmt in ('mscx', 'mscz'):
                for reason in ('cancel', 'deadline'):
                    destination = out/f'{action}-{fmt}.test-470-converting-{reason}.{fmt}'
                    offset = (out/'stderr.log').stat().st_size
                    request = client.send('tools/call', dict(name='score_export',
                        arguments=arguments(action, fmt, destination)))
                    record = dict(format=fmt, reason=reason, stage='Converting',
                        observed=False, units=0, destination=str(destination))
                    records.append(record)
                    until = time.monotonic()+15
                    while True:
                        trace = (out/'stderr.log').read_bytes()[offset:].decode(errors='replace')
                        if 'WRITE470_CONVERTING units=2' in trace:
                            record.update(observed=True, units=2)
                            break
                        if time.monotonic() >= until:
                            record.update(trace=trace, failure='No Converting unit 2 marker within 15 seconds')
                            raise AssertionError(f'{record}: requires test host with conversion probes enabled')
                        time.sleep(.002)
                    if reason == 'cancel':
                        client.send('notifications/cancelled', dict(requestId=request), notification=True)
                    response = client.receive(request)
                    trace = (out/'stderr.log').read_bytes()[offset:].decode(errors='replace')
                    record.update(trace=trace, response=response)
                    assert 'WRITE470_RETURN units=2 error=Cancelled' in trace, trace
                    if reason == 'cancel':
                        assert response.get('error', {}).get('code') == -32800, response
                    else:
                        value = response['result']['structuredContent']
                        assert not value['ok'] and value['error']['code'] == 'DeadlineExceeded', value
                        assert not value.get('artifacts') and not value.get('job'), value
                    assert not destination.exists(), 'interrupted conversion published a destination'
                    # Recovery after every abort, on the same document and process.
                    recovery_path = out/f'{action}-{fmt}-{reason}-recovery.mscx'
                    recovery = client.call('score_export', arguments('run', 'mscx', recovery_path))
                    assert recovery['ok'], recovery
                    assert ET.parse(recovery_path).getroot().attrib['version'] == '4.70'
                    record['recovery'] = recovery
        assert len(records) == 4 and all(r['observed'] and r['units'] > 0 for r in records)
        complete = True
    finally:
        client.close()
        (out/'results.json').write_text(json.dumps(dict(preflight=preflight, records=records,
            conversion_stage_observed=complete), indent=2))
    assert complete, 'conversion entry was not observed'
    print(json.dumps(dict(output=str(out), records=len(records), conversion_stage_observed=True)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
