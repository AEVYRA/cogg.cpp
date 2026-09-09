#!/usr/bin/env python3
"""Explicit live image smoke: fresh subject, vision + actor, restart and recall.

Only run on an image you authorize these configured providers to receive.
No automatic retries, service deployment or access to other subjects.
"""
import argparse
import hashlib
import json
import pathlib
import socket
import subprocess
import time

def main():
    parser=argparse.ArgumentParser()
    for flag in ('host','cli','config','image','output'):parser.add_argument('--'+flag,required=True,type=pathlib.Path)
    parser.add_argument('--actor',default='deepseek');parser.add_argument('--vision',default='kimi')
    parser.add_argument('--allow-remote',action='store_true',required=True)
    a=parser.parse_args();root=a.output.resolve();root.mkdir(mode=0o700,parents=True,exist_ok=False)
    db=root/'subject.db';sock=root/'host.sock';seed=root/'seed.json'
    seed.write_text(json.dumps({'self.profile':{'schema':'cogg:self/v1','revision':0,'data':{'name':'perception-test'}}}))
    def cli(*args):return subprocess.check_output([str(a.cli.resolve()),*map(str,args)],text=True,timeout=30)
    def rpc(op,**fields):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5);s.connect(str(sock));s.sendall((json.dumps(dict(protocol='cogg:host/v1',op=op,**fields))+'\n').encode());data=b''
            while b'\n' not in data:
                part=s.recv(65536)
                if not part:raise RuntimeError('disconnected')
                data+=part
        reply=json.loads(data)
        if not reply['ok']:raise RuntimeError(reply.get('error','host rejected request'))
        return reply['data']
    def wait(tick):
        until=time.monotonic()+150
        while time.monotonic()<until:
            s=rpc('snapshot')
            if s['tick']==tick:return s
            if s['host']['paused']:raise RuntimeError('host paused: '+json.dumps(s['host']))
            time.sleep(.25)
        raise RuntimeError('deadline expired')
    log=open(root/'host.log','wb');p=None
    def start():
        nonlocal p
        p=subprocess.Popen([str(a.host.resolve()),'--db',str(db),'--subject','s','--socket',str(sock),
            '--config',str(a.config.resolve()),'--executor',a.actor,'--vision-executor',a.vision,'--allow-remote'],stdout=log,stderr=log)
        for _ in range(100):
            try:rpc('snapshot');return
            except OSError:
                if p.poll() is not None:raise RuntimeError('host startup failed')
                time.sleep(.05)
        raise RuntimeError('socket missing')
    def stop():
        nonlocal p
        if p is not None:
            p.terminate();p.wait(timeout=15);p=None
    try:
        cli('init',db,'s',1,100,3600000,86400000,seed)
        start();rpc('resume');wait(1)
        imported=rpc('image',path=str(a.image.resolve()),key='image-1',text='Describe the visible scene in this image and remember the observation for later.')
        image_answer=wait(2)
        stop();cli('verify',db,'s')
        first=json.loads(cli('inspect',db,'s'));head=first['head']
        observations=list(pathlib.Path(str(db)+'.media').glob('*.observation'))
        assert len(observations)==1
        observed_before=observations[0].read_bytes()
        # Existing pixels remain stored, but the new question carries no image.
        start();assert rpc('snapshot')['head']==head
        rpc('send',key='recall-1',text='From the image I showed you earlier, what animal was visible and what was on the ground? Use your saved observation.')
        rpc('resume');recall=wait(3);stop();cli('verify',db,'s')
        assert observations[0].read_bytes()==observed_before
        trace=json.loads(cli('inspect',db,'s'))
        image_commit=trace['commits'][2]['body'];later=trace['commits'][3]['body']
        attempts={x['id']:x['body'] for x in trace['attempts']}
        image_attempt=attempts[image_commit['attempt']];recall_attempt=attempts[later['attempt']]
        image_inputs=[x for x in image_attempt['inputs'] if x['body']['producer']=='cogg:vision/v1']
        assert len(image_inputs)==1 and not any(x['body']['producer']=='cogg:vision/v1' for x in recall_attempt['inputs'])
        assert len(image_commit['proposal']['notes'])>=1
        assert image_commit['proposal']['kind']=='speech' and image_commit['proposal']['text'].strip(), 'initial image request produced no visible answer'
        assert later['proposal']['kind']=='speech' and later['proposal']['text'].strip(), 'recall produced no visible answer'
        result={'schema':'cogg:perception-smoke/v1','status':'passed','image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
            'import':imported,'observation':image_inputs[0],'image_proposal':image_commit['proposal'],'recall_proposal':later['proposal'],
            'image_admission':image_attempt,'recall_admission':recall_attempt,'head_before_restart':head,'final_tick':trace['tick'],
            'chain_verified':True,'receipt_unchanged_after_recall':True,
            'scope':'One authorized image and one fresh subject; provider/semantic quality remains scenario-specific.'}
        (root/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
        print(json.dumps({k:result[k] for k in ('status','image_sha256','image_proposal','recall_proposal','final_tick')},ensure_ascii=False))
    except Exception as error:
        (root/'failure.json').write_text(json.dumps({'error':str(error)},ensure_ascii=False,indent=2)+'\n');raise
    finally:stop();log.close()
if __name__=='__main__':main()
