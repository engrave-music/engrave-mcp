"""Reject XML-invalid score text without mutating a document; accept valid Unicode."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from v2_client import Client


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.soundfont = None
    async with Client(args, out) as client:
        for field in ('title', 'subtitle', 'composer', 'lyricist'):
            for invalid in ('\ufffe', '\uffff', '\x00', '\x01'):
                result = await client.tool('score_create', {'template_id': 'Treble_Clef', field: 'bad' + invalid}, error=True)
                assert result['error']['code'] == 'InvalidArgument', result
        title = '乐谱 🎵'
        created = await client.tool('score_create', {'template_id': 'Treble_Clef', 'title': title})
        doc = created['document']
        outline = await client.tool('score_outline', {'document': doc})
        assert outline['outline']['title'] == title
        measure = outline['outline']['measures'][0]
        address = {'kind': 'segment', 'measure': measure['id'], 'staff': 0, 'voice': 0,
                   'tick': measure['tick'], 'segment_kind': 'ChordRest'}
        invalid_batch = {'document': doc, 'expected_revision': outline['revision'], 'ops': [
            {'op': 'set_layout_break', 'measure': {'kind': 'id', 'id': measure['id']}, 'kind': 'line', 'on': True},
            {'op': 'add_text', 'at': address, 'style': 'staff', 'text': 'bad\ufffe'}]}
        error = await client.tool('score_edit', invalid_batch, error=True)
        assert error['error']['code'] == 'InvalidArgument' and error['error']['op_index'] == 1
        unchanged = await client.tool('score_outline', {'document': doc})
        assert unchanged['revision'] == outline['revision']
        bad_property = {'document': doc, 'ops': [{'op': 'set_property',
            'target': {'kind': 'id', 'id': measure['id']}, 'property': 'text',
            'value': {'kind': 'string', 'value': 'bad\x01'}}]}
        error = await client.tool('score_edit', bad_property, error=True)
        assert error['error']['code'] == 'InvalidArgument' and error['error']['op_index'] == 0
        await client.tool('score_edit', {'document': doc, 'ops': [
            {'op': 'add_text', 'at': address, 'style': 'staff', 'text': '音乐 🎵\nsecond line'}]})
        for fmt in ('mscx', 'musicxml'):
            path = out / f'valid.{fmt}'
            await client.tool('score_export', {'document': doc, 'action': 'run', 'kind': 'score',
                'options': {'format': fmt}, 'destination': {'kind': 'file', 'path': str(path)}})
            tree = ET.parse(path)
            assert title.encode('utf-8') in path.read_bytes()
            assert tree.getroot() is not None
        await client.tool('score_close', {'document': doc})
    print(json.dumps({'status': 'passed', 'invalid_metadata_cases': 16,
                      'invalid_batch_unchanged': True, 'valid_unicode_exported': True}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
