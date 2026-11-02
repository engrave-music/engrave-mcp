"""Destination metadata errors retain their filesystem classification."""
import argparse
import errno
import json
import os
from pathlib import Path
import subprocess

from bench_export_latency import Client, ROOT, export_args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    a.output = a.output.resolve()
    c = Client(a.binary, a.resources, a.output, None)
    results = {}
    try:
        doc = c.tool('score_open', {'path': str(ROOT/'fixtures/golden/repeat70.mscx')})['document']

        def check(label, target, code):
            errors = {}
            for fmt, dest in [('svg', target), ('pdf', target/'out.pdf')]:
                args = export_args(doc, fmt, a.output, True)
                args['destination']['path'] = str(dest)  # Never resolve away a failing probe.
                error = c.tool('score_export', args, False)['error']
                assert error['code'] == code, error
                assert not error.get('published_artifacts'), error
                if code == 'InvalidArgument':
                    assert str(dest) in error['message'], error
                errors[fmt] = error
            results[label] = errors

        check('missing', a.output/'missing', 'InvalidArgument')
        regular = a.output/'regular'
        regular.write_text('not a directory')
        check('non_directory', regular/'child', 'InvalidArgument')
        blocked = a.output/'blocked'
        target = blocked/'existing'
        target.mkdir(parents=True)
        if os.name == 'posix':
            blocked.chmod(0o600)
            try:
                try:
                    target.stat()
                except OSError as e:
                    assert e.errno == errno.EACCES, e
                else:
                    raise AssertionError('No-search setup must actually deny stat (run unprivileged)')
                check('no_search_existing', target, 'Io')
            finally:
                blocked.chmod(0o700)
            assert list(target.iterdir()) == []
            target.chmod(0o500)
            try:
                check('unwritable_staging', target, 'Io')
            finally:
                target.chmod(0o700)
            assert list(target.iterdir()) == []
            loop = a.output/'loop'
            loop.symlink_to(loop.name, target_is_directory=True)
            check('symlink_loop', loop, 'Io')
            check('name_too_long', a.output/('x'*300), 'Io')
        elif os.name == 'nt':
            # Deny metadata access only on our disposable subtree, then restore it.
            import csv
            row = next(csv.reader(subprocess.check_output(
                ['whoami', '/user', '/fo', 'csv', '/nh'], text=True).splitlines()))
            sid = row[1]
            def acl(*args):
                subprocess.run(['icacls', str(blocked), *args], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                acl('/deny', f'*{sid}:(OI)(CI)(RA)')
                try:
                    target.stat()
                except PermissionError:
                    check('access_denied_existing', target, 'Io')
                else:
                    results['access_denied_existing'] = 'not reproducible: stat succeeds with inherited deny READ_ATTRIBUTES; Windows traverse privilege can bypass search denial'
            finally:
                acl('/remove:d', f'*{sid}', '/T')
            assert list(target.iterdir()) == []

        # A real rename failure, without an injected fault or metadata refusal.
        conflict = a.output/'conflict'
        conflict.mkdir()
        (conflict/'page-0001.svg').mkdir()
        args = export_args(doc, 'svg', conflict, True)
        error = c.tool('score_export', args, False)['error']
        assert error['code'] == 'Io' and not error.get('published_artifacts'), error
        assert sorted(p.name for p in conflict.iterdir()) == ['page-0001.svg']
        results['publication_conflict'] = error
        assert 'tools' in c.rpc('tools/list', {})['result']
    finally:
        c.close()
    (a.output/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(results, ensure_ascii=True))


if __name__ == '__main__':
    main()
