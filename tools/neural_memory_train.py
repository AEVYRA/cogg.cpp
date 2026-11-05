#!/usr/bin/env python3
"""Prepare, fit/freeze, then examine a small evidence-applicability network.

Uses a pinned locally cached encoder, never downloads by default. Native C++
inference consumes the exported numeric model and host-provided token features.
Ridge-informed initialization and anchored short training are inspired by Emma's
Noon WSP work; these are supervised fits, not data-free or gradient-free learning.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import numpy as np
import torch
from torch import nn
from transformers import AutoModel, AutoTokenizer
from neural_memory_data import build, canonical, digest

ENCODER = "sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2"
REVISION = "e8f8c211226b894fcb81acc59f3b34ba3efd5f42"
ENCODER_ID = ENCODER + "@" + REVISION + ":last_hidden_state:128:v1"
ROOT = Path(__file__).resolve().parents[1]

def write(path, x):
    path.write_text(canonical(x) + "\n")

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def norm(x):
    return x / np.maximum(np.linalg.norm(x, axis=-1, keepdims=True), 1e-12)

def features(pt, ct):
    p, c = norm(pt.mean(0)), norm(ct.mean(0))
    pn, cn = norm(pt), norm(ct)
    sim = np.clip(cn @ pn.T, -1, 1)
    mp, mc = sim.max(0), sim.max(1)
    wp, wc = (1-mp)**3 + 1e-5, (1-mc)**3 + 1e-5
    rp, rc = norm(wp @ pn), norm(wc @ cn)
    a = np.exp(25 * (sim-mc[:,None])); a /= a.sum(1,keepdims=True)
    pl, cl = np.concatenate((pn[:1],pn[:-1])), np.concatenate((cn[:1],cn[:-1]))
    match = np.maximum(mc,0)**4 + 1e-5
    left = norm(match @ (a @ pl-cl))
    stats = [mp.mean(),mp.min(),mp.max(),mc.mean(),mc.min(),mc.max(),wp.mean(),wc.mean()]
    return np.concatenate((p,c,p-c,p*c,[p@c],rp,rc,left,stats)).astype(np.float32)

class Encoder:
    def __init__(self, cache):
        kw = dict(revision=REVISION, cache_dir=str(cache), local_files_only=True, trust_remote_code=False)
        self.tokenizer = AutoTokenizer.from_pretrained(ENCODER, **kw)
        self.model = AutoModel.from_pretrained(ENCODER, use_safetensors=True, **kw).eval()
        self.model.requires_grad_(False)

    def encode(self, texts):
        result = {}; times = []
        for start in range(0,len(texts),16):
            batch = texts[start:start+16]; t0=time.perf_counter()
            x=self.tokenizer(batch,padding=True,truncation=False,return_tensors="pt")
            if int(x["attention_mask"].sum(-1).max())>128: raise ValueError("explicit chunking required")
            with torch.no_grad(): y=self.model(**x).last_hidden_state
            for i,text in enumerate(batch): result[text]=y[i,x["attention_mask"][i].bool()].numpy().copy()
            times.append(time.perf_counter()-t0)
        return result, dict(seconds=sum(times),texts=len(texts),batch_size=16)

def pairs(part, cache, full=False):
    xs, ys, refs = [], [], []
    memories=part["memories"]
    for qi,q in enumerate(part["queries"]):
        # Same-family hard negatives plus four deterministic unrelated records.
        pool=memories if full else [m for m in memories if m["family"]==q["family"]]
        if not full:
            extra=[m for m in memories if m["family"]!=q["family"]]
            pool=pool+[extra[(qi*7+i*13)%len(extra)] for i in range(4)]
        for m in pool:
            xs.append(features(cache[m["text"]],cache[q["text"]]))
            ys.append(int(m["key"] in q["expected"]))
            refs.append((qi,m["key"]))
    return np.stack(xs),np.array(ys,dtype=np.int64),refs

def network(dim, seed):
    torch.manual_seed(seed)
    return nn.Sequential(nn.Linear(dim,64),nn.Tanh(),nn.Linear(64,2))

def closed_fit(x,y):
    """Supervised ridge direction + training-only PCA, then binary Newton fit."""
    dim=x.shape[1]; model=network(dim,43)
    scale=np.ones(dim); scale[768:1152]=3; scale[1152:1536]=12; scale[1536]=4; scale[-8:]=2.5
    mean=x.mean(0).astype(np.float64); z=(x-mean)*scale
    weights=np.where(y==1,0.5/(y==1).mean(),0.5/(y==0).mean())
    target=2*y-1
    direction=np.linalg.solve(z.T@(z*weights[:,None])+6*np.eye(dim),z.T@(target*weights))
    torch.manual_seed(43)
    _,_,v=torch.pca_lowrank(torch.from_numpy(z).float(),q=63,center=False,niter=2)
    basis=np.column_stack([direction,v.numpy()]); basis=np.linalg.qr(basis)[0].T
    sd=np.maximum((z@basis.T).std(0),1e-2)
    w=(0.5*basis/sd[:,None])*scale
    b=-w@mean; h=np.tanh(x@w.T+b)
    a=np.column_stack([h,np.ones(len(h))]); reg=np.eye(65)*2; reg[-1,-1]=1e-4
    beta=np.linalg.solve(a.T@(a*weights[:,None])+reg,a.T@(target*weights))
    for _ in range(20):
        prob=1/(1+np.exp(-np.clip(a@beta,-40,40)))
        grad=a.T@((prob-y)*weights)+reg@beta
        curvature=np.maximum(prob*(1-prob),1e-5)*weights
        beta-=np.linalg.solve(a.T@(a*curvature[:,None])+reg,grad)
    with torch.no_grad():
        model[0].weight.copy_(torch.tensor(w,dtype=torch.float32)); model[0].bias.copy_(torch.tensor(b,dtype=torch.float32))
        model[2].weight.copy_(torch.tensor(np.stack([-beta[:-1]/2,beta[:-1]/2]),dtype=torch.float32))
        model[2].bias.copy_(torch.tensor([-beta[-1]/2,beta[-1]/2],dtype=torch.float32))
    return model

def train(model,x,y,seed,steps,anchor=False):
    torch.manual_seed(seed); rng=np.random.default_rng(seed)
    init=[p.detach().clone() for p in model.parameters()]
    opt=torch.optim.AdamW(model.parameters(),lr=0.001,weight_decay=0.001)
    xx=torch.from_numpy(x); yy=torch.from_numpy(y)
    weights=torch.tensor([0.5/(y==0).mean(),0.5/(y==1).mean()],dtype=torch.float32)
    for _ in range(steps):
        ix=rng.integers(0,len(y),size=128); opt.zero_grad()
        loss=nn.functional.cross_entropy(model(xx[ix]),yy[ix],weight=weights)
        if anchor: loss=loss+0.05*sum((p-p0).square().sum() for p,p0 in zip(model.parameters(),init))
        loss.backward(); opt.step()
    return model.eval()

def probabilities(model,x):
    with torch.no_grad(): return model(torch.from_numpy(x)).softmax(-1)[:,1].numpy()

def metrics(p,y,t):
    pred=p>=t
    return dict(recall=float(pred[y==1].mean()),false_accept=float(pred[y==0].mean()),
                balanced_accuracy=float((pred[y==1].mean()+(~pred[y==0]).mean())/2))

def calibrate(p,y):
    choices=[]
    for t in np.linspace(0.05,0.99,95):
        m=metrics(p,y,float(t)); choices.append((m["balanced_accuracy"],-m["false_accept"],float(t)))
    return max(choices)[2]

def export(model,threshold):
    return dict(schema="cogg:neural-memory/v1",features="token-alignment/v1",encoder=ENCODER_ID,
                dimension=384,threshold=float(threshold),layers=[dict(weight=l.weight.detach().numpy().tolist(),
                bias=l.bias.detach().numpy().tolist()) for l in model if isinstance(l,nn.Linear)])

def prepare(out):
    out.mkdir(parents=True,exist_ok=False)
    for split,part in build().items(): write(out/(split+".json"),part)
    write(out/"protocol.json",dict(schema="cogg:neural-memory-study/v1",seeds=[17,29,43],
        encoder=ENCODER_ID,methods=["random-300","ridge-pca-newton","ridge-pca-newton-plus-50"],
        selection="highest dev pair balanced accuracy, ties lower false accept; threshold dev only",
        budget=dict(max_items=4,max_bytes=3200,candidate_limit=64),
        scope="Authored controlled bilingual evidence retrieval; held-out families/sites/query surfaces; not independent human data or deployment."))
    write(out/"inputs.json",{p.name:sha(p) for p in out.glob("*.json")})

def fit(out,cache):
    if (out/"freeze.json").exists(): raise ValueError("already frozen; use a new run directory")
    train_part=json.loads((out/"train.json").read_text()); dev=json.loads((out/"dev.json").read_text())
    texts=sorted({m["text"] for d in [train_part,dev] for m in d["memories"]+d["queries"]})
    enc=Encoder(cache); encodings,encoding_time=enc.encode(texts)
    x,y,_=pairs(train_part,encodings); xd,yd,_=pairs(dev,encodings)
    models=[]; t0=time.perf_counter(); direct=closed_fit(x,y); direct_s=time.perf_counter()-t0
    models.append(("ridge-pca-newton",direct,direct_s))
    for seed in (17,29,43):
        t0=time.perf_counter(); m=train(network(x.shape[1],seed),x,y,seed,300)
        models.append((f"random-300-{seed}",m,time.perf_counter()-t0))
        t0=time.perf_counter(); m=train(copy.deepcopy(direct),x,y,seed,50,True)
        models.append((f"ridge-plus-50-{seed}",m,direct_s+time.perf_counter()-t0))
    rows=[]
    for name,m,seconds in models:
        p=probabilities(m,xd); threshold=calibrate(p,yd); met=metrics(p,yd,threshold)
        path=out/(name+".model.json");write(path,export(m,threshold))
        rows.append(dict(name=name,seconds=seconds,threshold=threshold,dev=met,sha256=sha(path)))
    winner=max(rows,key=lambda r:(r["dev"]["balanced_accuracy"],-r["dev"]["false_accept"]))
    cosine=xd[:,1536]; cosine_t=calibrate(cosine,yd)
    write(out/"fit.json",dict(models=rows,selected=winner["name"],train_pairs=len(y),dev_pairs=len(yd),
         encoder_time=encoding_time,cosine_threshold=cosine_t,cosine_dev=metrics(cosine,yd,cosine_t)))
    write(out/"freeze.json",dict(files={p.name:sha(p) for p in out.glob("*.json")},
         code={str(p.relative_to(ROOT)):sha(p) for p in [Path(__file__),ROOT/"tools/neural_memory_data.py"]}))
    print(json.dumps(dict(selected=winner,cosine_dev=metrics(cosine,yd,cosine_t),encoder_time=encoding_time)))

def exam(out,cache,binary):
    if (out/"exam.json").exists(): raise ValueError("exam already opened")
    frozen=json.loads((out/"freeze.json").read_text())
    for name,h in frozen["files"].items():
        if sha(out/name)!=h: raise ValueError("frozen input changed: "+name)
    for name,h in frozen["code"].items():
        if sha(ROOT/name)!=h: raise ValueError("frozen study code changed: "+name)
    part=json.loads((out/"test.json").read_text()); fitted=json.loads((out/"fit.json").read_text())
    noise="Routine weather observation: light clouds and a mild breeze."
    texts=sorted({m["text"] for m in part["memories"]+part["queries"]}|{noise})
    enc=Encoder(cache); encodings,encoding_time=enc.encode(texts)
    # Separate warm batch-one timings include tokenizer+transformer+feature extraction.
    timings=[]
    for q in part["queries"][:12]:
        t0=time.perf_counter();enc.encode([q["text"]]);timings.append(time.perf_counter()-t0)
    x,y,refs=pairs(part,encodings,full=True); results=[]
    for row in fitted["models"]:
        spec=json.loads((out/(row["name"]+".model.json")).read_text());m=network(x.shape[1],0)
        with torch.no_grad():
            for layer,values in zip([m[0],m[2]],spec["layers"]):
                layer.weight.copy_(torch.tensor(values["weight"]));layer.bias.copy_(torch.tensor(values["bias"]))
        results.append(dict(name=row["name"],pair_metrics=metrics(probabilities(m,x),y,row["threshold"])))
    selected=json.loads((out/(fitted["selected"]+".model.json")).read_text())
    native_input=dict(schema="cogg:neural-memory-replay/v1",encoder=ENCODER_ID,model=selected,
        encodings={k:v.tolist() for k,v in encodings.items()},dataset=part,noise=noise,
        cosine_threshold=fitted["cosine_threshold"],budgets=json.loads((out/"protocol.json").read_text())["budget"])
    write(out/"native-input.json",native_input)
    p=subprocess.run([str(binary),str(out/"native-input.json")],text=True,capture_output=True,check=True)
    native=json.loads(p.stdout);write(out/"native-report.json",native)
    chosen_model=network(x.shape[1],0)
    with torch.no_grad():
        for layer,v in zip([chosen_model[0],chosen_model[2]],selected["layers"]):
            layer.weight.copy_(torch.tensor(v["weight"]));layer.bias.copy_(torch.tensor(v["bias"]))
    expected=probabilities(chosen_model,x)
    parity=max(abs(float(expected[i])-native["probabilities"][i]) for i in range(len(expected)))
    if parity>0.0003: raise ValueError("native numeric mismatch: "+str(parity))
    write(out/"exam.json",dict(models=results,native=native,parity_max_abs=parity,encoder_time=encoding_time,
        query_encoding_ms=dict(median=float(np.median(timings)*1000),p95=float(np.percentile(timings,95)*1000)),
        sources=dict(freeze_sha256=sha(out/"freeze.json"),binary_sha256=sha(binary)),
        limitation="One authored synthetic curriculum; no inference of real-world general reliability."))
    print(json.dumps(dict(models=results,parity_max_abs=parity,strategies=native["strategies"])))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("phase",choices=["prepare","fit","exam"])
    p.add_argument("--output",type=Path,required=True);p.add_argument("--model-cache",type=Path)
    p.add_argument("--binary",type=Path);a=p.parse_args();torch.set_num_threads(2)
    if a.phase=="prepare":prepare(a.output)
    elif a.phase=="fit":fit(a.output,a.model_cache)
    else:exam(a.output,a.model_cache,a.binary)

if __name__=="__main__":main()
