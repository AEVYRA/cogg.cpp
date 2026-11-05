#!/usr/bin/env python3
"""Freeze and replay source-grounded retrieval cases. No training or downloads.

Keep private corpora outside the public repository. All excerpts are exact spans
of pinned Git blobs; questions and relevance labels remain human/author judgments.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n")


def load_corpus(path, repo):
    data = json.loads(Path(path).read_text())
    if data.get("schema") != "cogg:source-corpus/v1":
        raise ValueError("unknown source corpus schema")
    blobs = {}
    for source in data["sources"]:
        sid, commit, name = source["id"], source["commit"], source["path"]
        if sid in blobs or not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError("duplicate source or unpinned commit")
        if Path(name).is_absolute() or ".." in Path(name).parts:
            raise ValueError("invalid source path")
        blob = subprocess.run(["git", "-C", str(repo), "show", f"{commit}:{name}"],
                              check=True, capture_output=True).stdout
        if hashlib.sha256(blob).hexdigest() != source["sha256"]:
            raise ValueError("source hash mismatch: " + sid)
        blobs[sid] = blob.decode("utf-8")
    records, current = {}, {}
    for m in data["memories"]:
        key = m["key"]
        if key in records or not re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", key):
            raise ValueError("invalid or duplicate record key")
        text = blobs[m["source"]]
        start, end = m["span"]
        if not (0 <= start < end <= len(text)) or text[start:end] != m["text"]:
            raise ValueError("excerpt does not match pinned source: " + key)
        if m.get("role") not in ("evidence", "counterevidence", "condition", "distractor"):
            raise ValueError("unknown evidence role")
        logical = m.get("logical_key", key)
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", logical):
            raise ValueError("invalid logical key")
        if m.get("status", "active") not in ("active", "retracted"):
            raise ValueError("unsupported source status")
        records[key] = m
        current[logical] = key if m.get("status", "active") == "active" else None
    live = set(current.values()) - {None}
    qids = set()
    for q in data["queries"]:
        if q["id"] in qids or not q["text"].strip():
            raise ValueError("duplicate or empty query")
        qids.add(q["id"])
        expected = q["expected"]
        if len(set(expected)) != len(expected) or not set(expected) <= live:
            raise ValueError("expected evidence is duplicated, missing, or inactive")
        if not set(q.get("forbidden", [])) <= set(records):
            raise ValueError("unknown forbidden record")
        if set(expected) & set(q.get("forbidden", [])):
            raise ValueError("contradictory labels")
    if not 1 <= len(records) <= 200 or not 1 <= len(qids) <= 256:
        raise ValueError("corpus size outside replay bounds")
    return data


def freeze(args):
    from neural_memory_train import Encoder, ENCODER_ID
    data = load_corpus(args.corpus, args.repo)
    model = json.loads(args.model.read_text())
    if model["encoder"] != ENCODER_ID:
        raise ValueError("encoder mismatch")
    # Token lengths only: no scores or labels used to select weights/thresholds.
    enc = Encoder(args.model_cache)
    lengths = {m["text"]: len(enc.tokenizer(m["text"], truncation=False)["input_ids"])
               for m in data["memories"] + data["queries"]}
    if max(lengths.values()) > 128:
        raise ValueError("choose explicit smaller exact source spans before freeze: " +
                         str([(n, t[:100]) for t, n in lengths.items() if n > 128]))
    args.output.mkdir(parents=True, exist_ok=False)
    write(args.output / "corpus.json", data)
    (args.output / "model.json").write_bytes(args.model.read_bytes())
    protocol = dict(schema="cogg:source-replay-protocol/v1", encoder=ENCODER_ID,
                    methods=["balanced", "cosine", "neural", "cosine_topk", "neural_topk"],
                    budgets=dict(max_items=4, max_bytes=3200, candidate_limit=64),
                    cosine_threshold=args.cosine_threshold, noise_count=0,
                    scope="Pinned archival excerpts with authored queries and labels; not independent user traffic.",
                    selection="Existing model and thresholds fixed; top-k is diagnostic, no tuning on this corpus.",
                    acceptance="Neural must beat both frozen baselines in source hits, preserve at least their best counterevidence/condition coverage, have no more no-match errors than either, and return zero forbidden records. This is a transfer gate, not live deployment authorization.",
                    max_tokens=max(lengths.values()))
    write(args.output / "protocol.json", protocol)
    root = Path(__file__).resolve().parents[1]
    write(args.output / "freeze.json", dict(
        files={n: sha(args.output / n) for n in ("corpus.json", "model.json", "protocol.json")},
        code={n: sha(root / n) for n in ("tools/source_memory_replay.py", "tools/neural_memory_train.py",
                                       "tools/neural_memory_eval.cpp")},
        binary_sha256=sha(args.binary)))
    print(json.dumps(dict(memories=len(data["memories"]), queries=len(data["queries"]),
                         max_tokens=max(lengths.values()), freeze=sha(args.output / "freeze.json"))))


def run(args):
    import numpy as np
    import torch
    from neural_memory_train import Encoder, ENCODER_ID, features, network, probabilities
    out = args.output
    frozen = json.loads((out / "freeze.json").read_text())
    root = Path(__file__).resolve().parents[1]
    for group, base in (("files", out), ("code", root)):
        for name, digest in frozen[group].items():
            if sha(base / name) != digest:
                raise ValueError("frozen input changed: " + name)
    if sha(args.binary) != frozen["binary_sha256"]:
        raise ValueError("frozen binary changed")
    # Reserve before opening scores; even a failed run cannot masquerade as unopened.
    with (out / "opened.json").open("x") as f:
        json.dump(dict(freeze_sha256=sha(out / "freeze.json"), time=time.time()), f)
    data = load_corpus(out / "corpus.json", args.repo)
    spec = json.loads((out / "model.json").read_text())
    protocol = json.loads((out / "protocol.json").read_text())
    torch.set_num_threads(2)
    encoder = Encoder(args.model_cache)
    texts = sorted({m["text"] for m in data["memories"] + data["queries"]})
    encodings, encoding_time = encoder.encode(texts)
    timings = []
    for q in data["queries"][:12]:
        start = time.perf_counter(); encoder.encode([q["text"]]); timings.append((time.perf_counter()-start)*1000)
    native_input = dict(schema="cogg:neural-memory-replay/v2", encoder=ENCODER_ID, model=spec,
                        dataset=data, encodings={k: v.tolist() for k, v in encodings.items()},
                        **{k: protocol[k] for k in ("methods", "budgets", "cosine_threshold", "noise_count", "scope")})
    write(out / "native-input.json", native_input)
    result = subprocess.run([str(args.binary), str(out / "native-input.json")],
                            check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    write(out / "native-report.json", report)
    # The existing two-layer frozen head; fail rather than silently alter architecture.
    if len(spec["layers"]) != 2 or len(spec["layers"][0]["bias"]) != 64:
        raise ValueError("parity runner requires the initial 64-unit head")
    x = np.stack([features(encodings[m["text"]], encodings[q["text"]])
                  for q in data["queries"] for m in data["memories"]])
    model = network(x.shape[1], 0)
    with torch.no_grad():
        for layer, values in zip([model[0], model[2]], spec["layers"]):
            layer.weight.copy_(torch.tensor(values["weight"])); layer.bias.copy_(torch.tensor(values["bias"]))
    p = probabilities(model, x)
    parity = float(np.max(np.abs(p - np.asarray(report["probabilities"]))))
    if not math.isfinite(parity) or parity > 0.0003:
        raise ValueError("native probability mismatch")
    summary = dict(schema="cogg:source-replay-result/v1", freeze_sha256=sha(out / "freeze.json"),
                   model_sha256=sha(out / "model.json"), parity_max_abs=parity,
                   encoder_time=encoding_time, query_encoding_ms=dict(median=float(np.median(timings)),
                   p95=float(np.percentile(timings, 95))),
                   strategies=[{k: v for k, v in r.items() if k != "rows"} for r in report["strategies"]],
                   store_rebuild_verify=report["store_rebuild_verify"], scope=protocol["scope"])
    methods = {m["method"]: m for m in summary["strategies"]}
    n = methods["neural"]; baselines = [methods[k] for k in ("balanced", "cosine")]
    summary["transfer_gate_passed"] = (
        n["source_hits"] > max(b["source_hits"] for b in baselines)
        and n["counterevidence_or_condition_hits"] >= max(b["counterevidence_or_condition_hits"] for b in baselines)
        and n["false_no_match_accepts"] <= min(b["false_no_match_accepts"] for b in baselines)
        and n["forbidden_selected"] == 0)
    write(out / "result.json", summary)
    print(json.dumps(summary))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("phase", choices=["freeze", "run"])
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--repo", type=Path, required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--model-cache", type=Path, required=True)
    p.add_argument("--corpus", type=Path)
    p.add_argument("--model", type=Path)
    p.add_argument("--cosine-threshold", type=float, default=0.55)
    args = p.parse_args()
    if args.phase == "freeze":
        if not args.corpus or not args.model or not math.isfinite(args.cosine_threshold):
            p.error("freeze needs corpus, model, and finite cosine threshold")
        freeze(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
