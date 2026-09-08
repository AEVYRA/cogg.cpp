# Phase 7 — optional durable self-state

Implemented in v0.8.0 with `-DCOGG_SELF=ON`. The boundary is a small host policy over
existing cogg memory: an application-defined self-description and explicitly
accepted commitments survive process exits and executor replacement. The kernel
does not require a persona, a long-term knowledge system, multiple models, or this
library. Full Aevyra Crystal integration remains future work.

## Contract

`self.profile` is an ordinary working-memory key, initialized at subject genesis:

```json
{"schema":"cogg:self/v1","revision":0,"data":{"name":"Rin","role":"assistant","limitations":["No physical access"]}}
```

`data` is application-defined JSON. The library does not interpret a name, decide
whether a limitation is true, or turn stored descriptions into measured executor
capabilities. The current executor still has its separate Phase 5 descriptor.

Commitments are ordinary typed memory deposits, with keys `self.commitment.ID`,
type `task`, and initial status `open`. `ID` uses ASCII letters, digits, `_` or `-`.
Each commitment's text is immutable. A host grant permits its creation with exact
terms, or an `open → closed/retracted` transition with those same terms. A settled
ID cannot be reused. Amending terms requires a new ID and explicit disposition of
the original commitment; it cannot quietly change an existing promise.

`SelfRuntime` implements:

1. Verify core history and replay the self policy; reject previous unguarded self
   changes. Build the current profile and all open commitments with their last
   mutation commit IDs.
2. Validate host grants against subject, head, occasion and commitment version.
   A null grant supplies no self mutation permissions. Grants are permissions,
   not commands: a model may abstain or make an ordinary proposal instead.
3. Freeze the view, contract and grant as a Phase 6 input at admission. This fixed
   input survives normal retrieval selection. If it cannot fit, admission fails
   before charging an attempt; cogg never silently drops an open self commitment.
4. Run one executor outside SQLite's write transaction. Reject unauthorized self
   mutations before commit. A rejection spends the admitted attempt, leaves the
   head and request unchanged, and records `self_rejected`; it does not repair the
   proposal or automatically try another model.
5. Commit valid proposals with execution/emission provenance through the existing
   atomic path. Abstention preserves the previous self state and pending occasion.
   Cancellation, late results and competing commits cannot publish a new self state.

Profile replacement requires the exact host-granted `data` and `revision + 1`.
All `self.*` keys are reserved in both memory and notes, preventing cross-store
aliases. Commitment sources/covers must be empty: their creation and settlement
provenance is already in the admission and commit chain. Ordinary memory and note
operations retain their existing kernel rules.

## Host authorization and trust

Public C++ API: [`self_state.hpp`](../include/cogg/self_state.hpp).

`self_grant(view, occasion, create, settle, profile)` builds an explicit permission
document. `create` maps full keys to exact text; `settle` maps them to
`{"version":"last mutation commit ID","status":"closed"}` (or `retracted`).
`profile` is null or the exact desired replacement data. The host must authorize
these choices itself. It must not mechanically promote a model's proposed grant
or a quoted instruction in a prompt into host permission.

For a due wake that has not yet been materialized, `occasion:null` binds to the
one expected wake at the specified head. This is an additive Phase 6 context
validation change: null is accepted only when admission itself finds that due
unmaterialized wake. It never matches an external or already materialized event.
The actual created event ID and the pre-admission schedule are both recorded.
If another attempt materializes the wake first, rebuild the grant/context using
the now-concrete ID. Stale contexts fail without a new attempt charge.

The optional guard is a **trusted-host convention**. All writers of a guarded
subject must use it. A direct `Store::commit` can bypass the guard; `inspect_self`
then rejects that history. Hashes establish integrity and recorded lineage, not
cryptographic authorization by a person. A host with database access can fabricate
grants. Signing, permission delegation and external action confirmation are not
implemented here.

Speech is not the commitment ledger. A model can still say "I finished" without
closing a task, or give an incorrect answer. A host-authorized `closed` record also
does not prove a physical action occurred. Applications must establish such facts
through their own observation/action protocols.

## Bounds and compatibility

- No additional tables or migration: database version remains **5**. Existing
  subject/history storage and Phase 4 task deposits are authoritative.
- Profile data: object, at most 4096 serialized bytes. Commitment terms: at most
  2048 bytes; at most 16 open commitments. Active self view: at most 12288 bytes.
- At most 8 create/settle permissions per grant; grant at most 16384 bytes. Existing
  proposal limits, 16 inputs/32768 bytes, and executor context limits still apply.
  These are independent caps, not a promise all maxima fit together.
- Replay retains settled IDs to prevent reuse, but prompts contain only the active
  view plus a settled count. Full history remains accessible through core APIs.
- Inspection currently reads/verifies full history: cost grows with history and
  attempts. This is a small correctness-first adapter, not a large-history cache.
  Concurrent head changes during inspection cause a conflict for the host to retry.
- Self profile initialization is supported at genesis only. Existing plain subjects
  remain plain unless an explicit future migration policy is implemented.
- Timeouts depend on a cooperative backend to return promptly; late returns are
  rejected even when a backend ignores interruption. No background worker is added.

## Run the example

```sh
cmake -S . -B build -DCOGG_SELF=ON -DCOGG_HTTP=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure

# Explicitly invokes three model calls: first → second → first.
# CONFIG uses the executor configuration from Phase 5, with credential references.
python3 tools/self_demo.py build CONFIG_JSON NEW_ARTIFACT_DIRECTORY deepseek kimi
```

The script initializes Rin, has the first model accept a synthetic telescope
commitment, exits, and starts the second model against the same database. That
model must recall the name, object and prerequisite while leaving the original
commitment intact. A third process receives a synthetic host acknowledgement and
an exact settlement grant. It closes the original commitment. Each step retains
its admission, model receipt and distinct executor session. There is no telescope
interaction, camera, or real-world completion in this fixture.

The smaller executable also supports manual operation:

```text
cogg-self-example init DB SUBJECT PROFILE_DATA_JSON
cogg-self-example view DB SUBJECT
cogg-cli send DB SUBJECT IDEMPOTENCY_KEY TEXT
cogg-self-example grant DB SUBJECT GRANT_REQUEST_JSON
cogg-self-example run DB SUBJECT EXECUTOR_CONFIG EXECUTOR GRANT_JSON_OR_DASH [TIMEOUT_MS]
```

The grant request contains only optional `create`, `settle`, `profile` fields.
Save the emitted grant JSON before passing it to `run`; `-` means no self rights.
Choosing an executor in this example explicitly authorizes its configured transport.
Exit codes: 0 committed/waiting, 4 abstained, 3 unsuccessful attempt, 2 host error.
Since v0.8.1, typed backend failures retain `transport_failed`, `timeout`,
`invalid_output`, `credentials_unavailable` or `backend_failed` in the result and
attempt ledger. Cancellation and elapsed deadlines take precedence even if the
backend throws. Provider exception messages are never copied into this ledger.
`outcome` in an unsuccessful result is the rejected candidate, never an accepted
subject response. Use `status` and the verified `view` when presenting results.

## Evidence and remaining research

See [the validation record](phase7-validation-20260908.json). Offline tests cover
unauthorized creation/settlement/profile changes, immutable terms, stale grants,
reserved aliases, session/process replacement, killed inference, context pressure,
abstention, late returns, concurrent commits, due-wake binding, and bypass detection.
The live two-model example is a bounded smoke test, not a comparative benchmark.

This phase supplies durable declared self-state, not a theory of identity, drives,
subjective experience, or a complete cognitive cycle. It does not claim superiority
over memory systems. Phase 3's pinned 48-hour observation, Phase 4 retrieval quality,
and Phase 6 evidence-aware participation remain separate open gates. Physalia can
later connect the same commitments to broader memory and action without making
those systems mandatory dependencies of cogg.
