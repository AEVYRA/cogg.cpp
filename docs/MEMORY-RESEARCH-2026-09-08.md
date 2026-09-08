# Phase 4 memory research and implementation decision

Research date: 2026-09-08. Scope: bounded working context over a durable local
subject, without requiring Akari, an external memory server, embeddings, a second
LLM, or a particular future Physalia ontology. This is a mechanism survey, not
an exhaustive enumeration of products or a claim to have benchmarked them all.

## What kind of memory?

Cowan distinguishes temporary storage from the attention/manipulation involved
in working memory; the boundaries themselves depend on the model of cognition.
A context manager is therefore a useful *engineering analogy* to working memory,
not an implementation of human short-term memory. [Cowan 2008](https://pubmed.ncbi.nlm.nih.gov/18394484/).

In cogg, tokens and KV are transient computational state; the canonical Present
is working context; committed deposits and their source history outlive context
and process. Phase 4 addresses the bridge between the last two. Akari 2.0 also
combines durable sources/compiled knowledge with a curated active State and
retrieval. The projects are not a clean short-term/long-term partition. Future
Physalia may connect its own longer-lived memory through these contracts; cogg
must work by itself. No Akari dependency is introduced.

## Search map: mechanisms, evidence, fit

| Family / primary source | Mechanism and useful part | Boundary for this runtime |
|---|---|---|
| [MemGPT](https://arxiv.org/abs/2310.08560), [Letta](https://docs.letta.com/v1-sdk/concepts/stateful-agents) | Model-directed virtual context across in-context and archival tiers; explicit memory operations | Reuse the separation and operation boundary. A small GGUF should not have to orchestrate all paging correctly before it can continue. |
| [Mem0](https://arxiv.org/abs/2504.19413) | Extract salient facts, decide updates, consolidate, retrieve; optional graph | Useful typed updates; LLM extraction and semantic match quality remain separate from storage truth. Paper benchmark claims are under its own protocol. |
| [A-Mem](https://arxiv.org/html/2502.12110v1) | Zettelkasten-inspired descriptions, links and evolving organization | Links should have explicit origin. Automatically inferred similarity is not a causal source link. |
| [Graphiti](https://github.com/getzep/graphiti) | Source episodes, temporal validity, hybrid vector/text/graph search | Preserve superseded versions and distinguish current from historical recall. Graph server and entity resolution need not be prerequisites for an embedded kernel. |
| [Hindsight](https://github.com/vectorize-io/hindsight) | Retain/recall/reflect; semantic, keyword, graph and temporal lanes; fusion/reranking | Multiple retrieval lanes and evidence-backed consolidation are valuable. Copying its complete service stack is unnecessary for the C++ boundary. |
| [Mastra observational memory](https://mastra.ai/research/observational-memory) | Observer compresses messages into dated observations; reflector condenses observations; stable prefixes improve cache reuse | Progressive summaries help working continuity, but may lose detail. Keep expandable original sources. Its reported LongMemEval scores use specified actor/observer models and cannot be transferred to Qwen 0.5B. |
| [LangMem](https://github.com/langchain-ai/langmem/blob/main/docs/docs/concepts/conceptual_guide.md) | Separate semantic/episodic/procedural memory and side-effect-free transformations | A proposal is distinct from a committed memory change. These types need not imply automatic truth or learning. |
| [RAPTOR](https://arxiv.org/abs/2401.18059) | Tree of summaries and retrieval at different levels of abstraction | Coarse context plus original leaves. Online mutation and provenance need their own protocol; a summary tree is not a free consistency solution. |
| [MemOS](https://github.com/MemTensor/MemOS), [MemMachine](https://github.com/MemMachine/MemMachine) | Broader lifecycle/memory services and agent integration | Surveyed as platforms via project descriptions; no implementation equivalence or comparative benchmark claimed. |
| [StreamingLLM](https://arxiv.org/abs/2309.17453), [H2O](https://arxiv.org/abs/2306.14048), [SnapKV](https://arxiv.org/abs/2404.14469) | Retain attention sinks, heavy hitters, or query-relevant KV positions | These optimize latent inference cache, not authenticated semantic deposits. Compatible future backend work, not a substitute for Phase 4. |
| [Titans](https://arxiv.org/abs/2501.00663) | Learned neural memory updated during test-time processing | Interesting architectural alternative, but requires a compatible trained model. Cannot add it to arbitrary GGUF by changing the host scheduler. |
| [SQLite FTS5](https://www.sqlite.org/fts5.html) | Embedded Unicode lexical indexing, BM25, rebuildable index | Reuse a mature implementation; exact lexical matching has clear semantic/paraphrase limits. |

The [forms/functions/dynamics survey](https://arxiv.org/html/2512.13564v1)
organizes token, latent and parametric memories and formation/evolution/retrieval.
This guards against treating KV compression and semantic consolidation as the
same operation. It also helps cover families beyond a short product list.

## Pinned code inspection

Downloaded selected source files, not whole deployments. No competitor service
was run. Paths are pinned so claims remain reviewable despite changing HEADs:

- [Mem0 memory/main.py, dae67f7](https://github.com/mem0ai/mem0/blob/dae67f74f5cc7bf138c7d7d6f9cec5ce4b4373b3/mem0/memory/main.py): separate add/search/update/delete paths. SHA-256 `5b1b75e2f00aca7bd368a6e9cd5905145d60fd05a0e36d6b1ef3e2f1b4f28ca1`.
- [Graphiti search_utils.py, 644e55e](https://github.com/getzep/graphiti/blob/644e55e20c4a2cabd6ce12d47ad1b5653c0b5aee/graphiti_core/search/search_utils.py): retrieval/fusion utilities. SHA-256 `b55b39f1ec547d40e3c88042830020d8bc2c664bb0601f6996ec856cdcd40808`.
- [Hindsight fusion.py, 9a84994](https://github.com/vectorize-io/hindsight/blob/9a84994ac6b009c6bf89e771ca1c0513562dc49a/hindsight-api-slim/hindsight_api/engine/search/fusion.py): RRF with per-arm ranks, and interleaving for consolidation. Its comments identify a concrete RRF failure: a top match in only one arm can be crowded out. This argues for testing protected lanes rather than assuming fusion is always better. SHA-256 `19ccdfd73e5c9da7c10561c92e4c86af2d6c7b6fed870f41a848fbce1d25dfa4`.
- [Mastra observational-memory.ts, 778ce29](https://github.com/mastra-ai/mastra/blob/778ce29c7b0ea4717cae7b38eacc8f70a8d670d8/packages/memory/src/processors/observational-memory/observational-memory.ts): observer/reflector thresholds, buffering, activation and token-budget preparation. Previously reflected lines can be replaced by a buffered reflection before token truncation. SHA-256 `56293a46a602e828b60b682536af2ca0681e9f843809e99acc76e4afff4297ce`.

## Decision before implementation

Implement a local composition of established mechanisms with explicit kernel
contracts: immutable typed deposits; current/historical versions; source-linked
summaries that never delete their leaves; indexed lexical recall plus recent
context and a protected lane for open tasks; exact adapter token-budget checks;
and a durable receipt of the context admitted for each attempt.

The important invariant is **selection is not erasure, summary is not evidence,
and a repeated claim is not an independent observation**. Runtime checks origin,
version and atomicity; it cannot prove that a model-written summary is semantically
faithful. A future embedding/reranker/Crystal layer can change ranking without
owning subject commits. No new model weights, remote service or auto-learning
permission is needed for the initial mechanism.

A compact summary should be useful for broad continuity while lexical lookup can
still return a precise covered original. Replacing a source must prevent its old
summary from masquerading as current. Open tasks survive unrelated queries; if
mandatory context cannot fit, expose memory pressure rather than silently dropping
it. A pinned model-specific tokenizer measures the complete prompt, including
metadata and reserved output, rather than equating bytes with tokens.

This is a candidate improvement for *this contract*, not a demonstrated universal
winner. Source preservation, version-aware recall and explicit selection receipts
are assessable engineering properties. Answer quality, learned salience and
semantic recall require model benchmarks. The system should expose where a lexical
retriever fails, not rename BM25 as a new intelligence method.

## Acceptance and comparison plan (declared before implementation)

Keep dataset and budgets equal across recency-only, lexical-only and combined
retrieval. Include old exact facts, irrelevant recent distractors, corrected facts,
open and closed tasks, facts hidden by summaries, Unicode, foreign subjects,
missing evidence and paraphrases without shared terms. Report evidence recall,
stale/foreign leakage, selected context size and latency separately. Include a
paraphrase counterexample; do not omit it because it weakens the result.

Mechanical acceptance: history longer than context, transactional summaries,
source lookup after compaction, recovery after process kill and loss of KV/index,
legacy DB continuation, concurrency, rejected/oversized writes, and a real GGUF
transition from the bounded view. Stronger quality acceptance later uses
[LongMemEval](https://arxiv.org/html/2410.10813v2): extraction, multi-session and
temporal reasoning, updates, abstention. [RULER](https://arxiv.org/abs/2404.06654)
and [Lost in the Middle](https://arxiv.org/abs/2307.03172) motivate evaluating useful
context, not simply allocating a larger window.

A small deterministic fixture is a regression/comparison instrument; it is not
LongMemEval, a benchmark of all listed products, or proof of human memory.
