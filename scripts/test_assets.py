"""Validate dynamic native fonts, cache invalidation and immutable retained data."""
import argparse
import json
from pathlib import Path
import subprocess
import zlib

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--assets',type=Path,required=True)
p.add_argument('--font',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--soundfont',type=Path)
a=p.parse_args()
out=a.output.resolve()
out.mkdir(parents=True,exist_ok=True)
source=a.font.read_bytes()
command=[str(a.binary.resolve()),str(a.assets.resolve()),str(a.font.resolve()),str(out)]
sound_source=a.soundfont.read_bytes() if a.soundfont else None
if a.soundfont:
    command.append(str(a.soundfont.resolve()))
r=subprocess.run(command,
                 capture_output=True,text=True,encoding='utf-8',timeout=60)
(out/'native.stdout.log').write_text(r.stdout,encoding='utf-8')
(out/'native.stderr.log').write_text(r.stderr,encoding='utf-8')
assert r.returncode==0,r.stderr
report=json.loads((out/'evidence.json').read_text('utf-8'))
assert report['contentHash']==f'crc32:{zlib.crc32(source):08x}:{len(source)}'
assert report['sourceBytes']==len(source)
assert a.font.read_bytes()==source, 'Test modified the original host font'
assert (out/'mutable-input.ttf').read_bytes()==b'invalid-font'
# The cached path has process lifetime and is expected to be removed on exit.
assert not Path(report['cachedPath']).exists(), 'Process-owned font cache leaked'
if sound_source is not None:
    sound=report['soundfont']
    assert sound['contentHash']==f'crc32:{zlib.crc32(sound_source):08x}:{len(sound_source)}'
    assert sound['sourceBytes']==len(sound_source)
    assert a.soundfont.read_bytes()==sound_source, 'Test modified the original SoundFont'
    assert (out/'mutable-input.sf2').read_bytes()==b'invalid-soundfont'
    assert not Path(sound['cachedPath']).exists(), 'Process-owned SoundFont cache leaked'
print(json.dumps(report,ensure_ascii=False))
