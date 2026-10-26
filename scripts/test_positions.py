"""Validate layout coordinates and repeat/tempo playback mappings through MCP."""
from __future__ import annotations
import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import xml.etree.ElementTree as ET

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from test_export_options import REPEAT_FIXTURE_MUSICXML, bar, read_png_dimensions


def timed_fixture():
    root = ET.fromstring(REPEAT_FIXTURE_MUSICXML)
    measures = root.findall('./part/measure')
    for index, measure in enumerate(measures):
        measure.remove(measure.find('note'))
        right_bar = measure.find("barline[@location='right']")
        if right_bar is not None:
            measure.remove(right_bar)
        direction = ET.SubElement(measure, 'direction')
        ET.SubElement(direction, 'sound', tempo=str(120 if index == 0 else 60))
        for pitch in (['C', None, 'E', 'G'] if index == 0 else ['E', 'F']):
            note = ET.SubElement(measure, 'note')
            if pitch is None:
                ET.SubElement(note, 'rest')
            else:
                p = ET.SubElement(note, 'pitch')
                ET.SubElement(p, 'step').text = pitch
                ET.SubElement(p, 'octave').text = '4'
            ET.SubElement(note, 'duration').text = '1' if index == 0 else '2'
            ET.SubElement(note, 'type').text = 'quarter' if index == 0 else 'half'
        if right_bar is not None:
            measure.append(right_bar)
    return ET.tostring(root, encoding='unicode')


def check_geometry(data, pages):
    assert set(data) == {'pageSize', 'elements', 'events'}
    size = data['pageSize']
    assert size['width'] > 0 and size['height'] > 0
    elements = data['elements']
    assert [x['id'] for x in elements] == list(range(len(elements)))
    for item in elements:
        assert set(item) == {'id', 'x', 'y', 'sx', 'sy', 'page'}
        assert 0 <= item['page'] < pages
        assert all(math.isfinite(item[k]) for k in ('x', 'y', 'sx', 'sy'))
        assert 0 <= item['x'] < size['width'] and 0 <= item['y'] < size['height']
        assert item['sx'] > 0 and item['sy'] > 0
    assert all(0 <= event['elid'] < len(elements) for event in data['events'])
    times = [event['position'] for event in data['events']]
    assert times == sorted(times) and times[0] == 0


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    child_args = ['--resources', str(args.resources.resolve())] if args.resources else []
    child_env = dict(os.environ)
    if os.environ.get('MUSIC_MCP_CLEAN_ENV'):
        child_env = ({'SystemRoot': os.environ['SystemRoot'], 'PATH': str(Path(os.environ['SystemRoot'])/'System32'),
                      'TEMP': os.environ['TEMP'], 'TMP': os.environ['TMP']} if os.name == 'nt'
                     else {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8'})
    params = StdioServerParameters(command=str(args.binary.resolve()), args=child_args,
                                  env=child_env, cwd=str(out))
    evidence = {'binary': str(args.binary.resolve())}
    with (out/'stderr.log').open('w', encoding='utf-8') as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()
                async def call(name, arguments, error=False):
                    r = await client.call_tool(name, arguments)
                    assert bool(r.is_error) == error, r
                    if error:
                        return None
                    return r.structured_content if r.structured_content is not None else json.loads(r.content[0].text)
                async def export(doc, fmt, name, **kw):
                    path = out/name
                    r = await call('score_export', {'documentId': doc, 'format': fmt, 'path': str(path), **kw})
                    assert path.stat().st_size == r['bytes']
                    return path
                tools = await client.list_tools()
                schema = next(t.input_schema for t in tools.tools if t.name == 'score_export')
                assert {'measurePositions', 'segmentPositions'} <= set(schema['properties']['format']['enum'])
                doc = (await call('score_open', {'musicxml': timed_fixture()}))['documentId']
                before = await export(doc, 'midi', 'before.mid', expandRepeats=False)
                mappings = {}
                for fmt, ids, times in [
                    ('measurePositions', [0,1,0,1], [0,2000,6000,8000]),
                    ('segmentPositions', list(range(6))*2, [0,500,1000,1500,2000,4000,6000,6500,7000,7500,8000,10000]),
                ]:
                    path = await export(doc, fmt, fmt+'.json')
                    data = json.loads(path.read_text('utf-8'))
                    check_geometry(data, 1)
                    assert len(data['elements']) == (2 if fmt == 'measurePositions' else 6)
                    if fmt == 'segmentPositions':
                        assert all(type(e['x']) is int and type(e['y']) is int for e in data['elements'])
                    assert [e['elid'] for e in data['events']] == ids
                    assert [e['position'] for e in data['events']] == times
                    again = await export(doc, fmt, fmt+'-again.json')
                    assert json.loads(again.read_text('utf-8')) == data
                    mappings[fmt] = data
                for i, segment in enumerate(mappings['segmentPositions']['elements']):
                    measure = mappings['measurePositions']['elements'][0 if i < 4 else 1]
                    assert segment['page'] == measure['page']
                    assert measure['x'] - 1 <= segment['x'] <= measure['x'] + measure['sx']
                after = await export(doc, 'midi', 'after.mid', expandRepeats=False)
                assert before.read_bytes() == after.read_bytes()
                png = await export(doc, 'png', 'page.png', pngDpi=144)
                w,h = read_png_dimensions(png)
                size = mappings['measurePositions']['pageSize']
                assert (w,h) == (math.ceil(size['width']*144/1200), math.ceil(size['height']*144/1200))
                sentinel = out/'sentinel.json'
                for fmt in ('measurePositions', 'segmentPositions'):
                    for option in [{'page':0}, {'expandRepeats':False}, {'pngDpi':144}]:
                        sentinel.write_bytes(b'unchanged')
                        await call('score_export', {'documentId':doc,'format':fmt,'path':str(sentinel), **option}, True)
                        assert sentinel.read_bytes() == b'unchanged'
                await call('score_close', {'documentId': doc})
                score = {'title':'Position pages', 'tempo':120, 'parts':[
                    {'name':'Violin','midiProgram':41,'measures':[bar(60,64,67,72) for _ in range(60)]},
                    {'name':'Cello','midiProgram':43,'measures':[bar(48,52,55,60) for _ in range(60)]}]}
                opened = await call('score_create', {'score':score})
                assert opened['pages'] >= 2
                for fmt, count, interval in [('measurePositions',60,2000),('segmentPositions',240,500)]:
                    path = await export(opened['documentId'],fmt,'pages-'+fmt+'.json')
                    data = json.loads(path.read_text('utf-8'))
                    check_geometry(data, opened['pages'])
                    assert len(data['elements']) == count and len(data['events']) == count
                    assert set(x['page'] for x in data['elements']) == set(range(opened['pages']))
                    assert [e['position'] for e in data['events']] == [i*interval for i in range(count)]
                await call('score_close', {'documentId': opened['documentId']})
                evidence.update(status='passed', tempo_repeat_events=mappings,
                                multipage={'pages':opened['pages'],'measures':60,'segments':240},
                                midi_unchanged=True, png_dimensions=[w,h], invalid_options=6)
                # Sequences from pinned upstream engraving/tests/repeat_tests.cpp.
                # Test data remains in the pinned checkout rather than being copied into this repo.
                cases = {
                    'repeat06': [1,2,3,2,4,5,6],  # simple volta
                    'repeat07': [1,2,3,4,5,6,1,2,3],  # D.C. al fine
                    'repeat08': [1,2,3,4,5,6,2,3,4,7,8,9,10,11],  # D.S. al coda
                    'repeat14': [1,2,3,4,5,6,7,8,9,10,2,3,4,5,6,7,8,11,12,2,3,4,5,6,7,8,
                                 13,14,15,16,17,18,16,17,18,19,20,21,22,23,5,6,7,24,25,26],
                }
                for name, expected in cases.items():
                    fixture = args.upstream/'src/engraving/tests/repeat_data'/f'{name}.mscx'
                    opened = await call('score_open', {'path':str(fixture.resolve())})
                    for fmt in ('measurePositions', 'segmentPositions'):
                        path = await export(opened['documentId'],fmt,f'{name}-{fmt}.json')
                        data = json.loads(path.read_text('utf-8'))
                        check_geometry(data,opened['pages'])
                        if fmt == 'measurePositions':
                            assert [e['elid']+1 for e in data['events']] == expected, name
                    await call('score_close', {'documentId':opened['documentId']})
                evidence['upstream_repeat_fixtures'] = list(cases)
    (out/'evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in evidence.items() if k != 'tempo_repeat_events'},ensure_ascii=False))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--resources',type=Path)
    p.add_argument('--upstream',type=Path,default=Path(__file__).resolve().parents[1]/'.upstream/webmscore')
    p.add_argument('--output',type=Path,required=True)
    asyncio.run(run(p.parse_args()))
