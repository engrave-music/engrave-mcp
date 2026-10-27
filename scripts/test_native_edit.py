"""Exercise native DOM edits, stable IDs and same-base concurrent edit semantics via MCP."""
from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import xml.etree.ElementTree as ET

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def fixture(tied: bool = False) -> str:
    root = ET.Element('score-partwise', version='4.0')
    listing = ET.SubElement(root, 'part-list')
    for pi in range(2):
        score_part = ET.SubElement(listing, 'score-part', id=f'P{pi+1}')
        ET.SubElement(score_part, 'part-name').text = f'Part {pi+1}'
    for pi in range(2):
        part = ET.SubElement(root, 'part', id=f'P{pi+1}')
        for mi in range(2):
            measure = ET.SubElement(part, 'measure', number=str(mi+1))
            if mi == 0:
                attrs = ET.SubElement(measure, 'attributes')
                ET.SubElement(attrs, 'divisions').text = '1'
                time = ET.SubElement(attrs, 'time')
                ET.SubElement(time, 'beats').text = '4'
                ET.SubElement(time, 'beat-type').text = '4'
                clef = ET.SubElement(attrs, 'clef')
                ET.SubElement(clef, 'sign').text = 'G'
                ET.SubElement(clef, 'line').text = '2'
            for ni, step in enumerate(['C','D','E','F']):
                note = ET.SubElement(measure, 'note')
                p = ET.SubElement(note, 'pitch')
                ET.SubElement(p, 'step').text = 'C' if tied and pi == 0 and ni < 2 else step
                ET.SubElement(p, 'octave').text = str(4-pi)
                ET.SubElement(note, 'duration').text = '1'
                if tied and pi == 0 and mi == 0 and ni < 2:
                    kind = 'start' if ni == 0 else 'stop'
                    ET.SubElement(note, 'tie', type=kind)
                ET.SubElement(note, 'voice').text = '1'
                ET.SubElement(note, 'type').text = 'quarter'
                if tied and pi == 0 and mi == 0 and ni < 2:
                    ET.SubElement(ET.SubElement(note, 'notations'), 'tied', type=kind)
    return ET.tostring(root, encoding='unicode')


def revision(data):
    return data['revision'] if 'revision' in data else data['score']['revision']


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ['--resources',str(args.resources.resolve())] if args.resources else []
    env = dict(os.environ)
    if os.environ.get('MUSIC_MCP_CLEAN_ENV'):
        env = ({'SystemRoot':os.environ['SystemRoot'], 'PATH':str(Path(os.environ['SystemRoot'])/'System32'),
                'TEMP':os.environ['TEMP'], 'TMP':os.environ['TMP']} if os.name == 'nt'
               else {'PATH':'/usr/bin:/bin','LANG':'C.UTF-8'})
    params = StdioServerParameters(command=str(args.binary.resolve()), args=child_args, env=env, cwd=str(out))
    report = {'binary':str(args.binary.resolve()), 'calls':[]}
    with (out/'stderr.log').open('w', encoding='utf-8') as errors:
        async with stdio_client(params,errlog=errors) as (read,write):
            async with ClientSession(read,write) as client:
                await client.initialize()
                async def raw(name, arguments):
                    response = await client.call_tool(name,arguments)
                    report['calls'].append({'tool':name,'error':bool(response.is_error)})
                    if response.structured_content is not None:
                        payload = response.structured_content
                    elif response.content and hasattr(response.content[0],'text'):
                        try: payload = json.loads(response.content[0].text)
                        except json.JSONDecodeError: payload = {'message':response.content[0].text}
                    else: payload = {}
                    return bool(response.is_error),payload
                async def call(name,arguments):
                    error,payload = await raw(name,arguments)
                    if error: raise AssertionError((name,payload))
                    return payload
                opened = await call('score_open',{'musicxml':fixture()})
                doc = opened['documentId']
                async def query(part=0,document=doc):
                    return await call('score_query_region',{'documentId':document,'partIndex':part,'firstMeasure':0,'measureCount':2})
                before = [await query(0),await query(1)]
                assert all(len(q['notes']) == 8 for q in before)
                assert before[0]['notes'] == (await query(0))['notes'], 'Repeated query changed IDs or musical data'
                base = revision(before[0])
                targets = [q['notes'][i] for q in before for i in (0,4)]
                changes = [{'noteId':n['id'],'pitch':n['pitch']+1} for n in targets]
                requests = [{'documentId':doc,'baseRevision':base,'operationId':f'independent-{i}', 'changes':[change]}
                            for i,change in enumerate(changes)]
                results = await asyncio.gather(*(raw('score_set_pitches',r) for r in requests))
                assert not any(error for error,_ in results), results
                latest = [await query(0),await query(1)]
                expected = {c['noteId']:c['pitch'] for c in changes}
                old_notes = {n['id']:n for q in before for n in q['notes']}
                actual = {n['id']:n for q in latest for n in q['notes']}
                assert set(actual) == set(old_notes), 'Pitch editing replaced note identity'
                for id,n in actual.items():
                    assert n['pitch'] == expected.get(id,old_notes[id]['pitch'])
                    for field in ('measureId','staffIndex','measureIndex','voice','tick','durationTicks'):
                        assert n[field] == old_notes[id][field], (id,field)
                current_revision = revision(latest[0])
                assert current_revision == base+4
                replayed = await call('score_set_pitches',requests[0])
                assert revision(replayed) == current_revision or replayed.get('replayed') is True
                assert (await query(0))['notes'] == latest[0]['notes']
                conflict_base = revision(await query(0))
                conflict_note = latest[0]['notes'][1]
                overlap = [{'documentId':doc,'baseRevision':conflict_base,'operationId':f'overlap-{i}',
                            'changes':[{'noteId':conflict_note['id'],'pitch':conflict_note['pitch']+i+1}]} for i in range(2)]
                conflicts = await asyncio.gather(*(raw('score_set_pitches',r) for r in overlap))
                assert sorted(error for error,_ in conflicts) == [False,True], conflicts
                clean = await query(0)
                bad = {'documentId':doc,'baseRevision':revision(clean),'operationId':'invalid-batch','changes':[
                    {'noteId':clean['notes'][0]['id'],'pitch':80},{'noteId':clean['notes'][1]['id'],'pitch':128}]}
                assert (await raw('score_set_pitches',bad))[0]
                after_bad = await query(0)
                assert after_bad['notes'] == clean['notes'] and revision(after_bad) == revision(clean)
                assert (await raw('score_query_region',{'documentId':doc,'partIndex':99,'firstMeasure':0,'measureCount':1}))[0]
                assert (await raw('score_query_region',{'documentId':doc,'partIndex':0,'firstMeasure':0,'measureCount':129}))[0]
                duplicate = dict(requests[0])
                duplicate['changes'] = [{'noteId':targets[0]['id'],'pitch':90}]
                assert (await raw('score_set_pitches',duplicate))[0], 'Operation ID reused with different payload'
                view = await call('score_get',{'documentId':doc})
                (out/'edited.musicxml').write_text(view['musicxml'],encoding='utf-8')
                root = ET.fromstring(view['musicxml'])
                assert len(root.findall('./part')) == 2 and len(root.findall('./part/measure/note')) == 16
                await call('score_close',{'documentId':doc})
                tied_doc = (await call('score_open',{'musicxml':fixture(True)}))['documentId']
                tied_view = await query(0,tied_doc)
                tied_note = next(n for n in tied_view['notes'] if n['tied'])
                assert (await raw('score_set_pitches',{'documentId':tied_doc,'baseRevision':revision(tied_view),
                    'operationId':'partial-tie','changes':[{'noteId':tied_note['id'],'pitch':tied_note['pitch']+1}]}))[0]
                assert (await query(0,tied_doc))['notes'] == tied_view['notes']
                await call('score_close',{'documentId':tied_doc})
                report.update(status='passed', concurrent_independent_edits=4,overlap_successes=1,
                              stable_note_ids=16, invalid_batch_unchanged=True, partial_tie_rejected=True,
                              operation_replay_deduplicated=True)
    (out/'evidence.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k != 'calls'},ensure_ascii=False))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--resources',type=Path)
    p.add_argument('--output',type=Path,required=True)
    asyncio.run(run(p.parse_args()))
