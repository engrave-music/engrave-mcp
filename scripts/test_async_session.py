"""Native async host acceptance: concurrent edits, revision snapshots, jobs and cancellation."""
from __future__ import annotations
import argparse
import asyncio
import json
import os
from pathlib import Path
import time
from test_export_options import canonical_notes, bar

class Client:
    def __init__(self, binary, resources, soundfont, directory):
        self.binary, self.resources, self.soundfont, self.directory = binary, resources, soundfont, directory
        self.serial = 0
        self.pending = {}
    async def __aenter__(self):
        args = [str(self.binary.resolve())]
        if self.resources: args += ['--resources',str(self.resources.resolve())]
        if self.soundfont: args += ['--soundfont',str(self.soundfont.resolve())]
        self.log = (self.directory/'stderr.log').open('wb')
        self.proc = await asyncio.create_subprocess_exec(*args,stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,stderr=self.log,limit=16*1024*1024)
        self.reader = asyncio.create_task(self.read())
        await self.call('initialize', {'protocolVersion':'2025-11-25','capabilities':{},
                                      'clientInfo':{'name':'async-acceptance','version':'1'}})
        await self.notify('notifications/initialized', {})
        return self
    async def read(self):
        try:
            while line := await self.proc.stdout.readline():
                message = json.loads(line)
                identity = message.get('id')
                if identity not in self.pending:
                    raise AssertionError(f'Unmatched response id: {identity}')
                future = self.pending.pop(identity)
                if not future.done(): future.set_result(message)
            if self.pending: raise RuntimeError('Server exited with pending requests')
        except BaseException as exc:
            for future in self.pending.values():
                if not future.done(): future.set_exception(exc)
            self.pending.clear()
            raise
    async def notify(self,name,params):
        self.proc.stdin.write((json.dumps({'jsonrpc':'2.0','method':name,'params':params})+'\n').encode())
        await self.proc.stdin.drain()
    async def call(self,name,params):
        self.serial += 1
        identity = self.serial
        future = asyncio.get_running_loop().create_future()
        self.pending[identity] = future
        self.proc.stdin.write((json.dumps({'jsonrpc':'2.0','id':identity,'method':name,'params':params})+'\n').encode())
        await self.proc.stdin.drain()
        return await asyncio.wait_for(future,60)
    async def tool(self,name,args,error=False):
        message = await self.call('tools/call',{'name':name,'arguments':args})
        assert 'error' not in message, message
        result = message['result']
        assert bool(result.get('isError')) == error, result
        if error: return result
        return result['structuredContent']
    async def terminal(self,job):
        deadline = time.monotonic()+60
        while time.monotonic() < deadline:
            value = await self.tool('score_job_get',{'jobId':job['jobId']})
            if value['state'] in ('completed','failed','cancelled'): return value
            await asyncio.sleep(.02)
        raise AssertionError('Job did not terminate')
    async def __aexit__(self,*exc):
        self.proc.stdin.close()
        try:
            await asyncio.wait_for(self.proc.wait(),10)
            await self.reader
            if exc[0] is None: assert self.proc.returncode == 0
        finally:
            if self.proc.returncode is None:
                self.proc.kill()
                await self.proc.wait()
            self.log.close()

async def run(args):
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    evidence={'binary':str(args.binary.resolve())}
    async with Client(args.binary,args.resources,args.soundfont,out) as client:
        score={'title':'Concurrent test','tempo':120,'parts':[
            {'name':'Melody','midiProgram':1,'measures':[bar(60,62,64,65) for _ in range(8)]},
            {'name':'Bass','midiProgram':43,'measures':[bar(48,50,52,53) for _ in range(8)]}]}
        created=await client.tool('score_create',{'score':score});doc=created['documentId'];base=created['revision']
        edits=[{'documentId':doc,'partIndex':part,'startMeasure':measure,'measures':[bar(pitch,pitch,pitch,pitch)],
                'baseRevision':base,'operationId':f'independent-{i}'}
               for i,(part,measure,pitch) in enumerate([(0,0,67),(1,0,55),(0,2,69),(1,2,57)])]
        results=await asyncio.gather(*(client.tool('score_replace_measures',edit) for edit in edits))
        latest=await client.tool('score_get',{'documentId':doc})
        assert latest['revision']==base+4,latest
        model=latest['composition']
        for part,measure,pitch in [(0,0,67),(1,0,55),(0,2,69),(1,2,57)]:
            assert model['parts'][part]['measures'][measure]['notes'][0]['pitches']==[pitch]
        retry=await client.tool('score_replace_measures',edits[0])
        assert retry['revision']==results[0]['revision']
        assert (await client.tool('score_get',{'documentId':doc}))['revision']==latest['revision']
        overlap=[{**edits[0],'startMeasure':4,'baseRevision':latest['revision'],'operationId':f'overlap-{i}',
                  'measures':[bar(70+i,70+i,70+i,70+i)]} for i in range(2)]
        raw=await asyncio.gather(*(client.call('tools/call',{'name':'score_replace_measures','arguments':e}) for e in overlap))
        assert sorted(bool(r['result'].get('isError')) for r in raw)==[False,True],raw
        conflict=next(r['result'] for r in raw if r['result'].get('isError'))
        assert conflict.get('structuredContent',{}).get('error',{}).get('code')=='conflict',conflict
        latest=await client.tool('score_get',{'documentId':doc})
        snapshot=await client.tool('score_export_start',{'documentId':doc,'format':'musicxml','path':str(out/'snapshot.musicxml'),
                                                       'expectedRevision':latest['revision']})
        await client.tool('score_replace_measures',{**edits[0],'startMeasure':1,'baseRevision':latest['revision'],
                                                    'operationId':'after-snapshot','measures':[bar(72,72,72,72)]})
        done=await client.terminal(snapshot)
        assert done['state']=='completed' and done['revision']==latest['revision'],done
        assert canonical_notes(out/'snapshot.musicxml')==canonical_notes(latest['musicxml'])
        # An invalid page fails in the isolated worker and does not replace the destination.
        sentinel=out/'bad-page.png';sentinel.write_bytes(b'preserved')
        bad=await client.tool('score_export_start',{'documentId':doc,'format':'png','page':999,'path':str(sentinel)})
        assert (await client.terminal(bad))['state']=='failed'
        assert sentinel.read_bytes()==b'preserved'
        if args.test_hooks:
            stall = asyncio.create_task(client.tool('test_owner_sleep', {}))
            for _ in range(100):
                if (await client.tool('test_owner_state', {}))['busy']: break
                await asyncio.sleep(.005)
            else: raise AssertionError('Owner stall did not start')
            started = time.perf_counter()
            region, status, late_cancel = await asyncio.gather(
                client.tool('score_query_region', {'documentId':doc,'partIndex':0,'firstMeasure':0,'measureCount':1}),
                client.tool('score_job_get', {'jobId':snapshot['jobId']}),
                client.tool('score_job_cancel', {'jobId':snapshot['jobId']}))
            elapsed = (time.perf_counter()-started)*1000
            assert elapsed < 500, elapsed
            assert (await client.tool('test_owner_state', {}))['busy']
            assert region['consistency']=='lastPublished' and region['notes']
            assert status['state']==late_cancel['state']=='completed'
            # Queued cancellation must prevent the edit, while running owner work completes truthfully.
            queued_edit = asyncio.create_task(client.call('tools/call', {'name':'score_replace_measures',
                'arguments':{**edits[0],'operationId':'cancel-queued','baseRevision':region['revision']}}))
            await asyncio.sleep(.03)
            await client.notify('notifications/cancelled', {'requestId':client.serial})
            assert (await queued_edit)['error']['code']==-32800
            assert (await stall)['sleptMs']==2000
            evidence['busy_owner_query_status_cancel_ms']=elapsed
            evidence['queued_request_cancel']=True
        # Captured work survives live document close.
        closing=await client.tool('score_export_start',{'documentId':doc,'format':'musicxml','path':str(out/'closed.musicxml')})
        await client.tool('score_close',{'documentId':doc})
        assert (await client.terminal(closing))['state']=='completed'
        evidence.update(disjoint_edits=4,overlap_conflicts=1,dedup=True,snapshot_revision=done['revision'],
                        worker_failure_preserved_output=True,close_preserved_snapshot=True)
        if args.soundfont:
            # Stream data comes from a frozen revision and mix, including after live close.
            audio_doc=(await client.tool('score_create',{'score':score}))['documentId']
            mix=(await client.tool('score_audio_get_mix',{'documentId':audio_doc}))['mix']
            mix['master']['muted']=True
            muted=await client.tool('score_audio_set_mix',{'documentId':audio_doc,'mix':mix})
            stream=await client.tool('score_audio_start',{'documentId':audio_doc,'expectedRevision':muted['revision']})
            mix['master']['muted']=False
            await client.tool('score_audio_set_mix',{'documentId':audio_doc,'mix':mix})
            first_chunks=await client.tool('score_audio_read',{'jobId':stream['jobId'],'batchSize':16})
            assert first_chunks['revision']==muted['revision'] and first_chunks['sampleRate']==44100
            assert 1<=len(first_chunks['chunks'])<=16
            for chunk in first_chunks['chunks']:
                assert len(chunk['left'])==len(chunk['right'])<=512
                assert all(sample==0 for sample in chunk['left']+chunk['right'])
            await client.tool('score_audio_seek',{'jobId':stream['jobId'],'seconds':0})
            again=await client.tool('score_audio_read',{'jobId':stream['jobId']})
            assert again['chunks'][0]['startTime']==0
            await client.tool('score_close',{'documentId':audio_doc})
            assert (await client.tool('score_audio_read',{'jobId':stream['jobId']}))['chunks']
            await client.tool('score_job_cancel',{'jobId':stream['jobId']})
            assert (await client.terminal(stream))['state']=='cancelled'
            ended=await client.tool('score_audio_read',{'jobId':stream['jobId']},error=True)
            assert ended['structuredContent']['error']['code']=='not_found'
            evidence.update(stream_snapshot_mix=True,stream_read_seek=True,stream_survives_close=True)
            large={'title':'Background audio','tempo':60,'parts':[
                {'name':f'Part {p}','midiProgram':1+p,'measures':[bar(48+p,52+p,55+p,60+p) for _ in range(128)]}
                for p in range(8)]}
            opened=await client.tool('score_create',{'score':large}); doc=opened['documentId']
            first=await client.tool('score_export_start',{'documentId':doc,'format':'wav','path':str(out/'cancel-first.wav')})
            second=await client.tool('score_export_start',{'documentId':doc,'format':'wav','path':str(out/'cancel-second.wav')})
            queued_path=out/'queued.musicxml';queued_path.write_bytes(b'preserved')
            queued=await client.tool('score_export_start',{'documentId':doc,'format':'musicxml','path':str(queued_path)})
            cancelled=await client.tool('score_job_cancel',{'jobId':queued['jobId']})
            assert cancelled['state']=='cancelled',cancelled
            assert queued_path.read_bytes()==b'preserved'
            latencies=[]
            for _ in range(20):
                started=time.perf_counter();await client.call('ping',{});latencies.append((time.perf_counter()-started)*1000)
            p95=sorted(latencies)[18]
            assert p95<250,latencies
            for job in (first,second):
                await client.tool('score_job_cancel',{'jobId':job['jobId']})
                assert (await client.terminal(job))['state']=='cancelled'
            assert not (out/'cancel-first.wav').exists() and not (out/'cancel-second.wav').exists()
            evidence.update(queued_cancel=True,running_cancel=True,ping_p95_ms=p95,ping_samples_ms=latencies)
            # Leave a live worker for EOF/shutdown cancellation.
            await client.tool('score_export_start',{'documentId':doc,'format':'wav','path':str(out/'shutdown.wav')})
    assert not (out/'shutdown.wav').exists()
    assert not list(out.glob('.musescore-job-*'))
    evidence['status']='passed'
    (out/'evidence.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in evidence.items() if k!='ping_samples_ms'}))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--resources',type=Path)
    p.add_argument('--test-hooks',action='store_true');p.add_argument('--soundfont',type=Path);p.add_argument('--output',type=Path,required=True)
    asyncio.run(run(p.parse_args()))
