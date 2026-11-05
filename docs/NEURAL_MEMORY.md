# Optional neural evidence selection

`COGG_NEURAL_MEMORY=ON` builds `cogg-neural-memory`, a CPU C++ scorer for
independently encoded memory/query pairs. The default library still requires no
model or Python runtime. The host supplies token encodings and owns the derived
cache/index. There is no hidden model call from the kernel.

The first model estimates whether a source is useful evidence for this query.
Supporting and opposing evidence, including qualifications, can all be useful.
It does not decide whether an action is permitted or certify a statement.
The probability is model-specific and needs calibration on the intended workload.

## Using a scorer

```cpp
#include <cogg/neural_memory.hpp>
cogg::NeuralMemoryScorer scorer(model_json);
const cogg::MemoryEncoding query = host_encode(query_text);
cogg::MemoryPolicy policy;
policy.query = query_text;
policy.strategy = "semantic_only";
policy.semantic_scorer = [&](const nlohmann::json &entry, const std::string &) {
    return scorer.score(host_cached_encoding(entry.at("text")), query);
};
policy.semantic_candidates = [&](const std::string &subject,
                                  const std::string &, std::size_t limit) {
    return host_index.search(subject, query, limit); // Ordered deposit IDs.
};
auto view = store.recall(subject_id, policy);
```

The `host_*` functions illustrate application-owned interfaces, not cogg API.
Compute/cache encodings before calling Store. Callbacks run synchronously under
the Store lock and must not reenter Store or perform unbounded remote work.
For batched ranking, score candidates inside the provider and omit the scorer
callback; its order is preserved. With a scorer, positive finite scores are
sorted descending; zero/negative scores omit candidates.

`semantic_candidates(subject, query, limit)` may consult an index spanning the
whole subject history. It returns at most `limit` IDs. Store resolves every ID
against that subject, rejects unknown/foreign IDs and oversized responses,
deduplicates, and excludes superseded, retracted and stale-summary records for
current recall. Historical recall is explicit and labels source status.
Covered originals can be selected by address. A stale index may lose recall,
but cannot revive a superseded record. Synchronization remains the host's job.

`semantic_only` uses this lane plus protected open tasks. Empty semantic output
does not add lexical, recent or procedural fallback. Existing strategies remain
available. Without an index provider, `semantic_scorer` retains its bounded
recent scan of `4 * candidate_limit` eligible records. All selections still
pass context limits and normal admission/source verification. Required tasks
may cause explicit memory pressure even when the scorer returns nothing.

## Numeric model and encoding contract

Model schema `cogg:neural-memory/v1` contains an exact `encoder` identifier,
`dimension`, feature version `token-alignment/v1`, a dev-selected `threshold`,
and two or three dense `layers` (`weight` rows and `bias`). Hidden layers use
`tanh`; the final two logits yield a softmax applicability probability. The
module rejects nonfinite/unbounded values, incompatible dimensions/encoder IDs,
empty or overlength token matrices and malformed layer shapes. Maximum token
count is 128, embedding width 1024 and hidden width 256.

Features contain normalized memory/query means, difference/product/dot,
unaligned token residuals, left token context differences at soft aligned
positions, and eight alignment statistics. Width is `7 * dimension + 9`.
This retains local evidence beyond mean pooling; correct language interpretation
is not guaranteed. Arithmetic is specified by
`tools/neural_memory_train.py::features` and checked against C++ on exported runs.

The host must verify artifact hashes before loading and bind the cache to text,
encoder revision, tokenization and feature version. An ID string alone does not
prove supplied vectors came from that encoder. Replacing the encoder invalidates
the derived cache; original cogg deposits remain unchanged. Source IDs/text are
captured in normal admission receipts. Model/index version provenance is
additionally recorded by the experiment/embedding host.

## Reproducible controlled study

Training uses a frozen multilingual MiniLM checkpoint already cached locally:

```sh
cmake -S . -B build -DCOGG_NEURAL_MEMORY=ON -DCOGG_SELF=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
python tools/neural_memory_train.py prepare --output experiments/memory-run
OPENBLAS_NUM_THREADS=1 python tools/neural_memory_train.py fit \
  --output experiments/memory-run --model-cache /path/to/huggingface-cache
OPENBLAS_NUM_THREADS=1 python tools/neural_memory_train.py exam \
  --output experiments/memory-run --model-cache /path/to/huggingface-cache \
  --binary "$PWD/build/cogg-neural-memory-eval"
```

Training needs Python, NumPy, PyTorch and Transformers; C++ inference does not.
No weights or generated token matrices are shipped in Git. The script never
downloads a model. It refuses implicit truncation and reuse of an opened exam.

The authored bilingual curriculum separates topic families, locations and query
surfaces across train/dev/test. Prohibition and conditional sources are positive
evidence labels alongside permissions. Wrong location/action and missing-source
queries are negatives. It compares random initialization plus 300 AdamW steps,
a supervised ridge/PCA/Newton fit, and the fitted initialization plus 50 anchored
steps. Three stochastic seeds are optimization repeats; the deterministic fit
is reported once. Selection uses dev only, then inputs, models and training code
are hashed before opening test.

The native replay inserts test sources before 360 irrelevant deposits and
compares `balanced`, cosine and neural selection with equal context limits.
The latter two use a linear cosine index over the same history and at most 64
candidates; this is not an ANN implementation. It records source recall,
counterevidence/conditions, no-match errors, irrelevant output, numeric parity
and CPU latency. Tokenization/encoder timing is separate and must be added for
a new query; cached retrieval alone is not full latency.

The curriculum is controlled, synthetic and authored by the implementer. It
cannot establish general retrieval quality, action selection, or independent
validation. A deployed index must handle real updates/histories, and the model
must beat relevant baselines on that workload.

## Initial controlled result

Initial controlled run (2026-11-05): a `2697 -> 64 -> 2` head with **172,802**
parameters, over the frozen **117,653,760**-parameter multilingual encoder,
was selected by dev before test. The encoder is required for new text; the
small head is not a complete language model. Training used 2,816 pairs, dev
1,408; test contained 72 sources, 64 queries and 360 later distractor deposits.
All 144 expected source occurrences include permission, counterevidence or a
condition. Sixteen queries deliberately have no matching source.

| Method | Expected sources returned /144 | Counterevidence or conditions /96 | No-match queries with false output /16 | Irrelevant records returned |
|---|---:|---:|---:|---:|
| Existing balanced | 72 | 48 | 16 | 184 |
| Cosine + threshold | 69 | 22 | 12 | 82 |
| Neural + threshold | 122 | 77 | 1 | 4 |

The selected ridge/PCA/Newton fit had 92.29% test pair balanced accuracy;
random-initialized 300-step models had 86.23–87.58%, and 50-step anchored
variants 89.22–91.24%. Extra training did not improve this run. Head fitting
took 2.48 seconds versus 1.37–1.73 seconds for random training, excluding the
shared encoder work: no training-speed advantage is claimed.

Native/Python probabilities differed by at most `2.18e-6` across 4,608 test
pairs. The neural selector still missed 22 expected source occurrences,
including 19 counterevidence/condition occurrences. These misses matter.
On the final isolated CPU replay, median recall was about 287 ms for neural,
237 ms for balanced and 230 ms for cosine, including Store work but excluding query
encoding (separately about 47 ms median). These are local measurements, not
production latency guarantees. Model/thresholds were unchanged during replay.

## Research provenance and limits

Initialization and anchored training are informed by Emma's Noon WSP study
(`noon:8998e7f`, `research/emma-weight-space/wsp_emma.py`) and prior R7 alignment
work (`noon:26a26a2`). This implementation fits a new, smaller binary model on a
new cogg curriculum. Ridge and Newton fitting are learning on data; absence of
autograd does not mean absence of gradients or learning. The prior study's
iterations, timings and gauge-equivalent seeds are not independent evidence of
this module's quality. Sofia's WSP-02 (`noon:3d0c1f7`) additionally motivates
version-bound representations and source history separate from derived state.
cogg's exact transaction/version rules remain authoritative.

<!-- <<aevyra:ty v1 wall=2026-11-05T11:33:36Z field=5191 file=1 art=cogg:docs/NEURAL_MEMORY.md hash=cc36a8d409a04fe7d6a62f7463292bea473454461d582618d59c9a66989eb927 ref=f7553e796b8a8efb>> -->
