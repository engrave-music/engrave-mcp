"""Exercise live asset replacement and captured worker leases through v2 MCP."""
from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import shutil

import numpy as np
import soundfile as sf

from test_async_session import Client

ROOT = Path(__file__).resolve().parents[1]


async def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    font_source = args.resources / 'fonts/petaluma/PetalumaScript.otf'
    font_input, soundfont_input = out / 'mutable-font.otf', out / 'mutable-soundfont.sf2'
    shutil.copyfile(font_source, font_input)
    shutil.copyfile(ROOT / 'tests/engine_audio/sine.sf2', soundfont_input)

    score = (ROOT / 'fixtures/golden/linked_parts.mscx').read_text(encoding='utf-8')
    marker = '<height>10</height>'
    assert marker in score
    score = score.replace(marker, '<Text><style>title</style><text><font face="Petaluma Script"/>'
                          'MMMWWWiii012345</text></Text>' + marker, 1)
    score_path = out / 'font-score.mscx'
    score_path.write_text(score, encoding='utf-8')

    async with Client(args, out) as client:
        async def export(doc, kind, fmt, path, action='run'):
            directory = kind == 'render' and fmt in ('svg', 'png')
            if directory:
                path.mkdir(exist_ok=True)
            return await client.tool('score_export', {'document': doc, 'action': action, 'kind': kind,
                'options': {'format': fmt}, 'destination': {'kind': 'directory' if directory else 'file', 'path': str(path)}})

        doc = (await client.tool('score_open', {'path': str(score_path)}))['document']
        revision = (await client.tool('score_outline', {'document': doc}))['revision']
        control, before, after = out / 'control', out / 'before.test-slow', out / 'after'
        await export(doc, 'render', 'svg', control)
        captured = await export(doc, 'render', 'svg', before, 'start')
        assert (await client.tool('runtime_job', {'action': 'get', 'job': captured['job']}))['status']['state'] in ('queued', 'running')
        font = await client.tool('runtime_info', {'action': 'add_font', 'path': str(font_input)})
        font_path = Path(font['fonts'][-1])
        assert font_path != font_input and font_path.read_bytes() == font_input.read_bytes()
        again = await client.tool('runtime_info', {'action': 'add_font', 'path': str(font_input)})
        assert again['fonts'] == font['fonts']
        font_input.write_bytes(b'invalid font')
        await export(doc, 'render', 'svg', after)
        assert (await client.terminal(captured['job']))['state'] == 'succeeded'
        control_svg, before_svg, after_svg = (p / 'page-0001.svg' for p in (control, before, after))
        assert before_svg.read_bytes() == control_svg.read_bytes(), 'job lost captured fonts'
        assert before_svg.read_bytes() != after_svg.read_bytes(), 'font did not change rendered text'
        assert font_path.read_bytes() == font_source.read_bytes()
        assert (await client.tool('score_outline', {'document': doc}))['revision'] == revision
        await client.tool('score_close', {'document': doc})

        audio_doc = (await client.tool('score_open', {'path': str(ROOT / 'fixtures/evening.musicxml')}))['document']
        revision = (await client.tool('score_outline', {'document': audio_doc}))['revision']
        fluid, captured_path, sine, again_path = (out / p for p in
            ('fluid.wav', 'captured.test-slow.wav', 'sine.wav', 'sine-again.wav'))
        await export(audio_doc, 'audio', 'wav', fluid)
        captured = await export(audio_doc, 'audio', 'wav', captured_path, 'start')
        assert (await client.tool('runtime_job', {'action': 'get', 'job': captured['job']}))['status']['state'] in ('queued', 'running')
        selected = await client.tool('runtime_info', {'action': 'set_soundfonts', 'files': [str(soundfont_input)]})
        retained_sf = Path(selected['capabilities']['soundfonts'][0])
        assert retained_sf != soundfont_input and retained_sf.read_bytes() == soundfont_input.read_bytes()
        soundfont_input.write_bytes(b'invalid soundfont')
        await export(audio_doc, 'audio', 'wav', sine)
        assert (await client.terminal(captured['job']))['state'] == 'succeeded'
        assert captured_path.read_bytes() == fluid.read_bytes(), 'job lost captured SoundFont'
        assert sine.read_bytes() != fluid.read_bytes(), 'SoundFont change did not affect audio'
        for path in (fluid, sine):
            audio, rate = sf.read(path, always_2d=True)
            assert rate == 44100 and np.isfinite(audio).all() and np.max(np.abs(audio)) > 0.001
        await client.tool('runtime_info', {'action': 'set_soundfonts', 'files': [str(soundfont_input)]}, error=True)
        await export(audio_doc, 'audio', 'wav', again_path)
        assert again_path.read_bytes() == sine.read_bytes(), 'invalid SoundFont changed selection'
        assert (await client.tool('score_outline', {'document': audio_doc}))['revision'] == revision
        await client.tool('score_close', {'document': audio_doc})

    assert not font_path.exists(), 'retained font survived host shutdown'
    assert not retained_sf.exists(), 'retained SoundFont survived host shutdown'
    print(json.dumps({'status': 'passed', 'font_worker_relayout': True, 'captured_font': True,
        'soundfont_worker_selection': True, 'captured_soundfont': True,
        'invalid_soundfont_rollback': True, 'revision_unchanged': True, 'retained_files_cleaned': True}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True, help='test-hook host binary')
    parser.add_argument('--resources', type=Path, required=True)
    parser.add_argument('--soundfont', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
