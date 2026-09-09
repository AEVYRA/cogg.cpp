#!/usr/bin/env python3
"""Offline image -> observation -> actor -> durable recall and recovery checks."""
import base64
import hashlib
import http.server
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time

HOST,TUI,CLI=sys.argv[1:4]
PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jN1sAAAAASUVORK5CYII=')
calls=[]; children=[]; entered=threading.Event(); release=threading.Event()
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_): pass
    def do_POST(self):
        req=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        model=req['model'];calls.append((model,req))
        if model.startswith('vision'):
            assert self.headers.get('X-Cogg-Observation') and not self.headers.get('X-Cogg-Attempt')
            content=req['messages'][-1]['content']
            assert isinstance(content,list)
            uri=content[1]['image_url']['url'];assert uri.startswith('data:image/png;base64,')
            assert base64.b64decode(uri.split(',',1)[1])==PNG
            result={'description':'Fixture observation: a white test pixel.'}
            if model=='vision-abstain': result={'kind':'abstain','reason':'unsupported','detail':'fixture unavailable'}
        else:
            assert self.headers.get('X-Cogg-Attempt')
            present=json.loads(req['messages'][-1]['content']);assert 'base64' not in json.dumps(present)
            result={'kind':'null','text':'','memory':[],'wake_after_ms':None}
            if present['occasion']['kind']=='external':
                result['kind']='speech'
                observations=[p['body']['content'] for p in present['inputs'] if p['body']['producer']=='cogg:vision/v1']
                if observations:
                    observation=observations[0];note=observation['memory_note']
                    assert observation['image']['sha256']==hashlib.sha256(PNG).hexdigest()
                    result['text']=observation['description']
                    if model!='actor-missing-note': result['notes']=[dict(note,type='episode',status='active',sources=[],covers=[])]
                    if model=='actor-silent':result['kind']='null';result['text']=''
                    if model=='actor-hold':entered.set();release.wait(10)
                else:
                    notes=present['deposits'];assert notes and 'white test pixel' in json.dumps(notes)
                    assert not observations
                    result['text']='I remember the supplied observation of a white test pixel.'
        response={'model':model,'choices':[{'finish_reason':'stop','message':{'role':'assistant','content':json.dumps(result)}}], 'usage':{'prompt_tokens':100,'completion_tokens':30}}
        self.send_response(200);self.end_headers()
        try:self.wfile.write(json.dumps(response).encode())
        except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
def run(*args,code=0):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=15)
    assert p.returncode==code,(p.returncode,p.stdout,p.stderr);return p.stdout
def rpc(sock,op,ok=True,**fields):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(3);s.connect(str(sock));s.sendall((json.dumps(dict(protocol='cogg:host/v1',op=op,**fields))+'\n').encode());b=b''
        while b'\n' not in b:
            part=s.recv(65536);assert part;b+=part
    response=json.loads(b);assert response['ok']==ok,response
    return response['data'] if ok else response
def eventually(fn):
    end=time.monotonic()+12
    while time.monotonic()<end:
        try:
            r=fn()
            if r:return r
        except (OSError,AssertionError):pass
        time.sleep(.04)
    raise AssertionError('condition timeout')
def stop(p):
    p.terminate();p.wait(timeout=8)
def start(root,actor='actor',vision='vision',existing=False):
    db=root/'s.db';sock=root/'host.sock';cfg=root/'config.json'
    cfg.write_text(json.dumps({'executors':{key:{'kind':'chat_completions','base_url':f'http://127.0.0.1:{server.server_port}','model':model} for key,model in [('actor',actor),('vision',vision)]}}))
    if not existing:
        seed=root/'seed.json';seed.write_text(json.dumps({'self.profile':{'schema':'cogg:self/v1','revision':0,'data':{'name':'image-test'}}}))
        run(CLI,'init',db,'s',1,100,60000,86400000,seed)
    log=open(root/f'host-{len(children)}.log','wb')
    p=subprocess.Popen([HOST,'--db',str(db),'--subject','s','--socket',str(sock),'--config',str(cfg),'--executor','actor','--vision-executor','vision'],stdout=log,stderr=log);children.append((p,log))
    eventually(lambda:rpc(sock,'snapshot'))
    return p,db,sock
def ready(sock):
    rpc(sock,'resume');eventually(lambda:rpc(sock,'snapshot')['tick']==1)
def vision_count():return sum(m.startswith('vision') for m,_ in calls)
try:
    with tempfile.TemporaryDirectory() as tmp:
        root=pathlib.Path(tmp)
        for scenario in ('normal','missing-note','silent','crash','bad-media','abstain'):
            r=root/scenario;r.mkdir(mode=0o700);file=r/'image with spaces.png';file.write_bytes(PNG)
            actor={'missing-note':'actor-missing-note','silent':'actor-silent','crash':'actor-hold'}.get(scenario,'actor')
            vision='vision-abstain' if scenario=='abstain' else 'vision'
            p,db,sock=start(r,actor,vision);ready(sock);rpc(sock,'pause')
            before=vision_count()
            queued=rpc(sock,'image',path=str(file),key='image',text='Describe and remember this image.')
            assert rpc(sock,'image',path=str(file),key='image',text='Describe and remember this image.')['occasion']==queued['occasion']
            media=pathlib.Path(str(db)+'.media')/(queued['image']['sha256']+'.image')
            assert media.read_bytes()==PNG and media.stat().st_mode&0o077==0
            # No observation before explicit resume, and caller file is no longer required.
            file.unlink();time.sleep(.1);assert vision_count()==before
            if scenario=='bad-media':media.write_bytes(PNG[:-1]+b'X')
            rpc(sock,'resume')
            if scenario=='crash':
                assert entered.wait(8);p.kill();p.wait(timeout=3);release.set()
                assert vision_count()==before+1
                p,db,sock=start(r,'actor','vision',existing=True)
                assert rpc(sock,'snapshot')['host']['paused']
                rpc(sock,'resume')
            elif scenario in ('missing-note','silent','bad-media','abstain'):
                state=eventually(lambda:(s if (s:=rpc(sock,'snapshot'))['host']['paused'] else None))
                assert state['tick']==1 and state['schedule']['occasion']['id']==queued['occasion']
                if scenario=='bad-media':
                    assert vision_count()==before
                    media.write_bytes(PNG);rpc(sock,'resume')
                elif scenario in ('missing-note','silent'):
                    assert vision_count()==before+1
                    stop(p);p,db,sock=start(r,'actor','vision',existing=True);rpc(sock,'resume')
                else:
                    assert vision_count()==before+1
                    time.sleep(.2);assert vision_count()==before+1
                    stop(p);run(CLI,'verify',db,'s');continue
            eventually(lambda:rpc(sock,'snapshot')['tick']==2)
            assert vision_count()==before+1
            stop(p);trace=json.loads(run(CLI,'inspect',db,'s'));commit=trace['commits'][-1]
            attempt=next(a['body'] for a in trace['attempts'] if a['id']==commit['body']['attempt'])
            assert attempt['inputs'][0]['body']['producer']=='cogg:vision/v1'
            assert attempt['inputs'][-1]['body']['producer']=='cogg:self/v1'
            assert commit['body']['proposal']['notes'][0]['text']==attempt['inputs'][0]['body']['content']['memory_note']['text']
            media.unlink()  # Recall after restart must not fetch pixels or call vision.
            p,db,sock=start(r,existing=True)
            rpc(sock,'send',key='recall',text='What do you remember about the image observation?');rpc(sock,'resume')
            eventually(lambda:rpc(sock,'snapshot')['tick']==3);assert vision_count()==before+1
            watched=subprocess.run([TUI,'--socket',str(sock),'--watch','--request',json.dumps({'op':'image','path':'unused','key':'watch'})],capture_output=True)
            assert watched.returncode!=0
            stop(p);run(CLI,'verify',db,'s')
        print('PASS perception: bytes/retention, head-bound receipts, retry, crash, self-state, abstention, missing-note rejection, tamper, recall without pixels, watch guard')
finally:
    release.set()
    for p,log in children:
        if p.poll() is None:
            p.terminate()
            try:p.wait(timeout=8)
            except subprocess.TimeoutExpired:p.kill();p.wait()
        log.close()
    server.shutdown();server.server_close()
