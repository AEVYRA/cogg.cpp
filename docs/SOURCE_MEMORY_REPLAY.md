# Source-grounded memory transfer replay

The initial neural head does **not** pass the first archival transfer gate.
Keep its synthetic result separate from effectiveness on natural source text.
This tool makes that distinction reproducible without changing live services.

`tools/source_memory_replay.py` freezes a supplied corpus, existing weights,
thresholds, replay binary and study code before examining scores. It never trains
or downloads an encoder. Python encoding uses the same pinned local checkpoint
as the controlled study; the actual Store replay and neural scoring run in C++.

## Corpus and provenance

Supply a `cogg:source-corpus/v1` JSON file with:

- `sources`: unique `id`, full 40-character Git `commit`, relative `path`, and
  SHA-256 of the complete blob. `git show COMMIT:PATH` reads the pinned version;
  the current working tree does not determine its contents.
- `memories`: unique `key`, `source` ID, `[start,end]` Unicode character `span`,
  exact `text`, and `role` (`evidence`, `counterevidence`, `condition`, `distractor`).
  Optional `logical_key` groups successive versions; `status` is `active` or
  `retracted`. Order declares the constructed replay, not historical chronology.
- `queries`: unique `id`, authored `text`, `expected` record keys, and optional
  `forbidden` record keys. Expected records must exist and be current/active.
  An empty expected set denotes no matching source within this supplied pool.

Source hashes prove correspondence to archived bytes, not truth, authority,
completeness or correctness of relevance labels. Audit those judgments separately.
Use explicit source spans of at most 128 encoder tokens, including special tokens.
Longer inputs fail; the tool never silently truncates an exception at the end.
The current harness is bounded to 200 source records and 256 questions. Private
text, encodings and weights need not be committed to the public code repository.

## Freeze, then run

Build with `COGG_NEURAL_MEMORY=ON`. With NumPy, PyTorch and Transformers installed:

```sh
OPENBLAS_NUM_THREADS=1 python tools/source_memory_replay.py freeze \
  --output experiments/source-run --repo /path/to/source-repository \
  --corpus /path/to/corpus.json --model /path/to/frozen.model.json \
  --model-cache /path/to/local-huggingface-cache \
  --binary build/cogg-neural-memory-eval --cosine-threshold 0.55
OPENBLAS_NUM_THREADS=1 python tools/source_memory_replay.py run \
  --output experiments/source-run --repo /path/to/source-repository \
  --model-cache /path/to/local-huggingface-cache \
  --binary build/cogg-neural-memory-eval
```

The threshold shown is the initial controlled study's fixed cosine threshold,
not a generally recommended value. Neural threshold is embedded in the frozen
model. `opened.json` reserves examination before scoring; an interrupted or failed
run is still an opened examination. A changed binary, source, model or protocol
requires an explicitly new run, never an undocumented retry masquerading as fresh.

The five preregistered methods are `balanced`, frozen-threshold cosine/neural,
and cosine/neural top-k diagnostics. Every method has four records and 3200 bytes
of context; semantic candidates are limited to 64. Versions stay in the host
index deliberately: Store must reject stale/retracted records independently of
the scorer. The report separates shortlist coverage, threshold coverage, final
source coverage, conditions/counterevidence, no-match output and forbidden records.
`irrelevant_selected` means outside the author's expected-label set; incomplete
labels can overstate irrelevance. Top-k modes cannot meaningfully abstain when
the corpus contains current records, and are not proposed deployment defaults.

The transfer gate requires neural source coverage above both fixed baselines,
at least their best condition/counterevidence coverage, no more no-match errors
than either baseline, and zero forbidden output. Passing this small gate would
still not authorize or establish live reliability. Native/Python probabilities
are compared on all source-query pairs. Encoding and Store retrieval timing are
reported separately; no unmeasured end-to-end percentile is inferred.

## First archival result, 2026-11-05

41 exact excerpts from six Akari pages, pinned before scoring; 35 authored
questions, 29 answerable and six no-match, with 33 expected source occurrences.
Two logical records were superseded. No synthetic noise was inserted. Weights
and both thresholds remained those from the initial controlled study.

| Method | Expected sources /33 | Conditions or counterevidence /30 | No-match false output /6 |
|---|---:|---:|---:|
| Balanced | 25 | 24 | 6 |
| Frozen cosine | 17 | 16 | 0 |
| Frozen neural | 11 | 11 | 0 |
| Cosine top-k, diagnostic | 26 | 24 | 6 |
| Neural top-k, diagnostic | 24 | 22 | 6 |

All 33 expected occurrences reached the semantic shortlist. The frozen neural
threshold rejected 22; removing it recovered some coverage but did not beat
cosine ranking. Lowering a threshold alone is therefore not an established fix.
No method returned a superseded record. A separate constant-positive-scorer
regression checks both supersession and retraction, independently of model scores.

Native/Python maximum probability difference was `1.24e-6` over 1,435 pairs.
Median retrieval was 27.8 ms balanced, 25.0 ms cosine and 131.4 ms neural;
query encoding separately took about 37.4 ms median. The small corpus differs
from the synthetic history, so these times are not a speedup over the first run.

The gate failed. This is an authored archival challenge, not independent labels
or production traffic. The next model needs natural-source training/development
data, a separately calibrated abstention decision and an unopened test split.
Preserve these failures as development evidence; this opened set can no longer
serve as an independent exam after using it to design a replacement.

<!-- <<aevyra:ty v1 wall=2026-11-05T13:29:40Z field=5218 file=1 art=cogg:docs/SOURCE_MEMORY_REPLAY.md hash=5143a89c19abfda7d53fdb35c1ba819da8d62d3ff299b5cef225d481225e9783 ref=5950f2cf01515ec1>> -->
