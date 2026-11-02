"""Exercise edits to both ends of a linked tie and their MXL round trip."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from v2_client import Client


ROOT = Path(__file__).resolve().parents[1]


def first_tie(path: Path):
    notes = ET.parse(path).getroot().findall('./part')[0].findall('./measure')[0].findall('note')
    return [(note.findtext('pitch/step'), [t.get('type') for t in note.findall('tie')],
             [t.get('type') for t in note.findall('notations/tied')]) for note in notes[:2]]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.soundfont = None
    fixture = ROOT / 'fixtures/golden/linked_parts.mscx'
    observed = {}
    async with Client(args, out) as client:
        for label, both in [('partial', False), ('complete', True)]:
            doc = (await client.tool('score_open', {'path': str(fixture)}))['document']
            before = await client.tool('score_query', {'document': doc, 'mode': 'elements',
                'query': {'types': ['Note'], 'props': ['pitch']}})
            notes = {item['id']: item for item in before['elements']}
            assert {'I_I', 'L_L'} <= notes.keys(), notes.keys()
            assert all(notes[n]['props'][0]['value']['value'] == '71' for n in ('I_I', 'L_L'))
            assert len(notes['I_I']['links']) > 1, notes['I_I']
            ids = ['I_I', 'L_L'] if both else ['I_I']
            edited = await client.tool('score_edit', {'document': doc, 'expected_revision': before['revision'],
                'ops': [{'op': 'set_pitch', 'note': {'kind': 'id', 'id': n}, 'pitch': 74} for n in ids]})
            assert edited['diagnostics'] == []
            after = await client.tool('score_query', {'document': doc, 'mode': 'elements',
                'query': {'types': ['Note'], 'props': ['pitch']}})
            pitches = {item['id']: item['props'][0]['value']['value'] for item in after['elements']}
            assert pitches['I_I'] == '74' and pitches['L_L'] == ('74' if both else '71')
            for linked in notes['I_I']['links']:
                prop = await client.tool('score_query', {'document': doc, 'mode': 'properties',
                    'target': {'kind': 'id', 'id': linked}, 'props': ['pitch']})
                assert prop['properties'][0]['value']['value'] == '74'
            for fmt in ('musicxml', 'mxl'):
                await client.tool('score_export', {'document': doc, 'kind': 'score',
                    'options': {'format': fmt}, 'destination': {'kind': 'file', 'path': str(out / f'{label}.{fmt}')}})
            original = first_tie(out / f'{label}.musicxml')
            assert original == [('D', ['start'], ['start']), (('D' if both else 'B'), ['stop'], ['stop'])], original
            restored = (await client.tool('score_open', {'path': str(out / f'{label}.mxl')}))['document']
            result = out / f'{label}-reopened.musicxml'
            await client.tool('score_export', {'document': restored, 'kind': 'score',
                'options': {'format': 'musicxml'}, 'destination': {'kind': 'file', 'path': str(result)}})
            reimported = first_tie(result)
            if both:
                assert reimported == original, reimported
            else:
                assert reimported == [('D', [], ['let-ring']), ('B', [], [])], reimported
            observed[label] = {'export': original, 'reimport': reimported, 'diagnostics': edited['diagnostics']}
            await client.tool('score_close', {'document': restored})
            await client.tool('score_close', {'document': doc})
    (out / 'evidence.json').write_text(json.dumps(observed, indent=2), encoding='utf-8')
    print(json.dumps(observed))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
