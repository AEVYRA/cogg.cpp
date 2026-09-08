# Phase 4 — bounded working context and durable memory deposits

Version 0.5.0. Research and alternatives were reviewed before implementation;
see [research decision](MEMORY-RESEARCH-2026-09-08.md). No external memory service,
Akari installation, embedding model or second LLM is required. SQLite must include
FTS5 (`unicode61` and BM25). Existing Debian/Ubuntu build dependencies provide it.

## Three separate objects

1. `Snapshot` remains the complete committed legacy key/value projection.
2. Typed deposits and their provenance live durably in the subject commit chain.
3. `Present::working_memory` and `Present::memory_view` hold the selected context
   for a particular attempt. An omitted deposit has not been erased.

KV is a fourth, disposable implementation detail. It is not the source of any
of the three objects. cogg supplies working-context management and a local
persistent bridge; it is not a literal model of human short-term memory. Akari
also has an active State and long-lived knowledge, so the systems do not split
neatly into short-term versus long-term memory. Future Physalia can use the same
source-addressed API without making its entire memory architecture part of cogg.

## Typed writes and atomicity

The legacy `Proposal::memory` assignments still work and keep their old 1 MiB
state limit. For accumulating durable content, use `Proposal::notes`:

```cpp
cogg::Proposal proposal;
proposal.kind = "reflection";
proposal.notes.push_back({"workshop-code", "The workshop access code is cedar-582.", "fact"});
proposal.notes.push_back({"return-telescope", "Return the telescope to Mira.", "task", "open"});
// Pass this proposal through the normal admitted Store::commit / Runtime path.
```

A note contains `key`, `text`, `type`, `status`, `sources`, `covers`. Types are
`note`, `fact`, `episode`, `task`, `summary`; statuses are `active`, `open`,
`closed`, `retracted`. Only a task can be open. Types record the writer's
interpretation, not externally verified truth, importance or confidence.

Limits per transition: eight typed notes; 128-byte key, 8192-byte text, 32 unique
source IDs and cover IDs per note. Unknown fields and duplicate keys are rejected.
A model cannot supply the new note ID, tick, commit address or index mutations.

A deposit ID hashes the parent commit, note index and canonical note JSON. Its
materialized record also carries the accepting commit, subject and tick. A write
to the same key supersedes its previous version and preserves that version in
history. `previous` identifies this explicit version relation. Closing/retracting
an open task requires a task write; ordinary notes cannot silently overwrite it.
Different keys are distinct claims, even if their texts disagree. There is no
automatic semantic contradiction detector.

Typed deposits, source edges, search projection, legacy memory, head, occasion
consumption and wake plan are committed in the **same SQLite transaction**. A
failed or competing proposal cannot publish a separate memory history. Sources
must name earlier deposits of the same subject; arbitrary external identifiers
are not implicitly trusted imports.

## Compaction with recoverable detail

Write an active summary with explicit sources, and put the IDs it condenses in
`covers` (a subset of `sources`). Covered originals remain in the commit history
and lexical index; the recent-context lane normally uses their summary instead.
A precise query can still select a covered original, and `memory_record` can
retrieve it by address without relying on matching words.

Compaction must reduce the total source text bytes. It cannot cover an open task,
a superseded/retracted source, or a source being overwritten in the same commit.
This version supports **one-level summaries of original deposits**: recursive
summary sources are rejected. Source version replacement makes an old summary
stale, excluding it from default current recall. Its historical record survives.

These checks prove provenance and version consistency, **not semantic faithfulness**
of the summary. The model/backend proposes summary text, and the normal commit
boundary accepts it. There is no hidden background LLM, automatic certification,
recursive summary drift, or deletion of raw history. This is context compaction,
not SQLite file shrinking or archival retention enforcement.

## Retrieval and finite context

`Store::recall(subject, MemoryPolicy)` offers three policies. The default
`balanced` policy preserves current open tasks, then interleaves BM25 and recent
lanes. `lexical` and `recent` are explicit comparison baselines and do not provide
the open-task guarantee. A recent lane ordinarily excludes covered originals;
lexical lookup retains access to them. Default recall excludes superseded,
retracted and stale-summary entries. Historical recall labels each entry with
`current` and `summary_stale` at the selected head.

SQLite FTS5 supplies Unicode lexical matching. Input is split into at most 32
quoted terms joined by OR; it never becomes raw SQL or unescaped FTS syntax.
This tokenizer is not a semantic synonym model. Candidate limits bound result
materialization, not all work performed by SQLite's text index. The index uses
shared corpus term statistics within a DB; subject filters prevent returning
another subject's deposits.

Defaults: 8192 serialized context bytes, 16 items, 64 candidates per search lane.
Legacy working keys and typed records compete for the same selected-context
budget after open tasks. Unselected large records are skipped without destructive
truncation. A too-large fixed context or mandatory open-task set produces an
explicit `memory pressure` error. It does not pretend all obligations fitted.
As of v0.8.1, exceeding the open-task item count consistently raises
`ContextOverflow`, like byte/token overflow. An explicitly configured route can
then try an executor with sufficient capacity without charging the skipped attempt.

The libllama adapter opts into this projection. During admission its pure
`context_fits` callback tokenizes the **complete formatted prompt** and reserves
output tokens. Bytes are not treated as tokens. This callback does no generation;
inference still happens outside the SQLite transaction. Complete source snapshots
remain available to trusted embedding hosts, but the shipped adapter feeds only
the selected memory to the model. Custom backends must opt in and implement their
own exact `context_fits` to gain a token-bound guarantee; core byte bounds alone
cannot promise a model-specific token bound.

Stable memory payload precedes changing head/time metadata in the prompt to
preserve any useful common prefix. Changed selection may invalidate reuse;
there is no universal cache-hit or identical-generation guarantee.

## Durable context receipts and recovery

Every projected admission saves the chosen legacy values and typed entries,
query, policy, head, candidate and capacity information inside its hashed attempt.
The commit links that attempt. `verify` checks that the selected values came from
the referenced subject history and checks recorded byte/item limits. Token fit is
enforced by the identified adapter at admission; core replay does not load a
model or certify a third-party native backend's behavior. A receipt identifies
selected evidence, not a proof that the model used it correctly.

`memory_entries`, `memory_links` and `memory_fts` are derived projections. Verification
replays typed writes from canonical commits and compares the projection. If the
index is lost or inconsistent, `rebuild_memory` first verifies the canonical
chain, then reconstructs the derived rows without changing subject tick/head.
Malformed canonical history is an error, never repaired by trusting a cache.
Verification replays the complete history and costs O(history); this initial
implementation is not a constant-time verifier or an unbounded-disk runtime.

DB schema 3 reads legacy schema 1/2 records without changing their hashes. Old
binaries reject DB version 3. Opening a new reader upgrades the DB version; test
migration on a backup before a production binary rollback. The fixed Phase 3
48-hour experiment retains its pinned binary and original DB.

## Commands

```sh
build/cogg-cli recall subject.db explorer 'workshop access code'
build/cogg-cli recall subject.db explorer 'workshop' 8192 lexical 1
build/cogg-cli memory-record subject.db explorer NOTE_ID
build/cogg-cli rebuild-memory subject.db explorer
build/cogg-cli verify subject.db explorer
build/cogg-memory-benchmark
```

`run-model` automatically uses the bounded projection and supports typed notes in
its constrained proposal grammar. Like other proposals, a syntactically valid
note can still be rejected for nonexistent sources or an invalid compaction.

## Evidence and limitations

The small comparison in [memory-benchmark-20260908.json](memory-benchmark-20260908.json)
uses the same 2400-byte/four-item budget: recent-only 1/6 evidence hits, lexical
4/6, balanced 5/6. All fail the intentionally included synonym-only query. It is
a deterministic contract fixture, not a LongMemEval score, a model answer score,
or a head-to-head deployment test of Letta/Mastra/Hindsight. Timings include full
verification and were collected with other model tests running; they are not
latency claims. The quality hypothesis remains open for larger naturalistic benchmarks.

A subsequent [two-model read-quality experiment](MEMORY-QUALITY-2026-09-08.md)
compares grounded answers from balanced retrieval and the last four messages at
equal evidence caps. Both DeepSeek and Kimi answered 6/7 known-fact cases with
balanced retrieval versus 1/7 with the last messages; synonym-only retrieval still
failed. The small seeded fixture does not evaluate autonomous memory formation.

Tests cover old facts beyond the model window, protected open tasks, corrected
versions, source expansion after summary, Unicode, foreign source rejection,
recorded context, explicit capacity failure, missing-index reconstruction,
legacy v1/v2 continuation, concurrent writers, and SIGKILL around SQLite commit.
A real Qwen 0.5B fixture tests bounded generation, compaction, a new Store/backend
and missing KV. Separate subprocess probes cover actual process restart. It is a mechanics fixture, not a strong cognitive
substrate. In the full-suite bounded-history run the first 3421-token prompt
produced the correct `cedar-582` answer; after KV deletion/index rebuild the
3441-token prompt produced an echo of the question instead. The source survived
and fit the context, but correct answer use did not. The subprocess recovery
probe likewise sometimes echoed the input. This is an explicit actor-quality gap.

An earlier full run rejected duplicate legacy-memory keys generated by the small
model. The instruction now asks for unique keys and changes only. Host semantic
validation was not weakened, and no duplicate output is silently rewritten.
Constrained syntax cannot guarantee that arbitrary future proposals pass semantic
validation. Final validation (GCC 12.2 / Debian bookworm, CPU, pinned llama and Qwen fixture):

- Release: all **9/9** CTest suites pass (memory, kernel, temporal, options,
  memory-model, inference, llama-checkpoint, checkpoint, process-crash).
- Debug ASan/UBSan core: all **5/5** suites pass. This run does not instrument
  the llama adapter or third-party model library.
- Strengthened memory-model rerun: the original exact source is asserted in
  **both** hashed admission receipts, with prompt/output token bounds on both.
- Actual subprocess probe: SIGKILL at tick 0, recovery to tick 1, restored KV at
  tick 2, corrupt/deleted KV recovery, changed context compatibility at tick 5.
- Supervisor probe: restart preserves the observation deadline and introduces
  zero external occasions. It supplies no positive evidence of autonomous waking.
- Actual schema-v2 fixture continues without changing old record bytes; the
  pinned v0.4 binary explicitly rejects the upgraded v3 DB.
- The repaired tutorial compiled and executed to tick 2 before Phase 4 work.

Reproduce with the pinned model from `.github/workflows/llama.yml`:

```sh
cmake -S . -B build -DCOGG_LLAMA=ON -DCOGG_TEST_MODEL=/path/model.gguf
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 tests/model_crash_test.py build/cogg-cli /path/model.gguf
python3 tests/time_experiment_test.py build/cogg-cli /path/model.gguf
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCOGG_SANITIZE=ON
cmake --build build-asan -j2
ctest --test-dir build-asan --output-on-failure
```

Further work: semantic retrieval/reranking behind the same source and commit
contract; larger independent quality evaluation; recursive summaries with explicit
coverage/validity semantics; asynchronous extraction/consolidation; prospective
triggers and typed deadlines; scalable verification and archival storage policy.
None is silently supplied by an Akari dependency.
