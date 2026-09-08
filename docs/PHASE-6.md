# Phase 6 — Composition boundary (v0.7.0)

cogg owns a subject's durable state and the authority to accept one transition.
A model is an executor. An OS process is a deployment choice. A single backend
is sufficient; neither a committee nor a long-term memory service is required.
Separate model workers can serve one subject, while separate subject databases
represent separate histories. Restarting a worker does not create a new subject.

This phase deliberately narrows the original integration roadmap. The kernel
provides typed non-participation and provenance-bearing inputs. A host application
(such as a future Physalia controller) chooses participants, obtains observations,
compares claims, decides whether further work is useful, and proposes closure.
The existing optional sequential router remains useful for transport recovery.
Semantic voting, truth scoring, a disagreement engine and a distributed workflow
scheduler are **not implemented or required by cogg**.

## Result contract

`Backend::respond_attempt` returns `Outcome = variant<Proposal, Abstention>`.
Existing custom backends keep working through its default `propose_attempt`
implementation. HTTP and native llama.cpp adapters support both outcome forms.
The native Outcome grammar is explicitly enabled with `LlamaOptions.allow_abstention`
or `run-model --allow-abstention`; its default remains proposal-only. The 0.5B
fixture copied the abstention example even for questions answered by its memory.
That observed quality regression makes an automatic native protocol upgrade
inappropriate. The option is part of the backend/checkpoint fingerprint. The new
mode has its own real inference test; existing native recovery tests exercise the
default contract. This is an adapter choice, not a limitation of Store/Outcome.
Legacy `LlamaBackend::propose` retains its proposal-only grammar; direct HTTP
callers must use `respond_attempt` to receive abstentions without an exception.

A transition retains the Phase 0 proposal format. Non-participation is:

```json
{"kind":"abstain","reason":"missing_input","detail":"A camera observation is required"}
```

Reasons are `missing_input`, `unsupported`, `uncertain`, or `refused`. Detail is
at most 512 UTF-8 bytes. No memory, notes, speech, wake or other fields are allowed.
These are executor assertions, not independently established diagnoses.

| Outcome | Attempt | Subject tick / memory | Occasion | Default route |
|---|---|---|---|---|
| Valid proposal, including `null` | committed | one transition | consumed | returns committed |
| Typed abstention | abstained | unchanged | pending | returns abstained |
| Transport/invalid-output failure | failed | unchanged | pending | tries next explicit executor within limits |
| Late/cancelled result | failed | unchanged | pending | respects existing timeout/cancel contract |
| Competing commit won | superseded/conflict | winner remains | winner owns consumption | stops |

A null proposal is a subject decision and advances its logical time. An executor
abstention is not that decision. All admitted attempts retain their normal budget
cost. Abstention does not schedule a wake or grant another inference. Every typed
abstention stops the current route, irrespective of reason; there is no built-in
semantic fallback policy. The host receives the outcome and chooses its next act.
Applications requiring semantic approval before a transition should use
`Store::admit → Backend::respond_attempt → application validation → Store::commit`.
The convenience Runtime/Router commits syntactically valid proposals automatically;
it does not establish their truth or enforce a user request to preserve memory.
`Runtime::last_abstention()` distinguishes abstention from waiting for single-backend
callers; it is reset at the start of each step. Hosts must inspect it before retrying.

The HTTP adapter accepts the exact wire alias `kind=abstention`, normalizes it to
`abstain`, and records `provider.normalization=abstention_to_abstain`. All other
shape and field checks still apply; the core canonical form remains `abstain`.

Chat Completions `message.refusal` and `finish_reason=content_filter` become
`refused`; provider refusal text is not copied into the receipt. Invalid envelopes,
HTTP errors and tool calls retain their failure handling. Free-text refusal inside
an otherwise valid speech proposal is still speech: cogg does not classify natural
language or enforce a semantic content policy.

## Durable settlement

`Store::abstain(attempt, abstention, provider)` atomically inserts a content-addressed
`cogg:abstention/v1` receipt and marks the reserved attempt `abstained`. The receipt
binds subject, parent, occasion, attempt, admitted executor, outcome and bounded
provider telemetry. It is separate from the subject commit chain. The same
settlement is idempotent, including after a later subject commit; a different
settlement or a new result from a superseded attempt conflicts.

`inspect` exposes `abstentions`. `verify` checks hashes, bindings, settlement
consistency, foreign keys, provenance and ordinary budget/commit invariants.
This is host-recorded integrity, not a provider signature, independent evidence of
truth, or protection against an administrator replacing the whole database.

Database version **5** adds the receipt table and freezes `inputs` in new
admissions. Record schemas 1–4 remain readable and their bytes are preserved.
Opening a legacy database migrates it; older cogg binaries reject version 5.
**Do not open the pinned Phase 3 experiment database with this binary.**

## Supplying evidence without consuming the request

`Store::admit`, `Runtime::step`, and `Route::context` accept either null or:

```json
{"head":"CURRENT_COMMIT_HASH","occasion":"PENDING_OCCASION_HASH","inputs":[]}
```

Both anchors are checked in the admission transaction. An old context cannot
silently move to the next subject head or a different FIFO event. On conflict no
attempt is charged and no inference runs. Inputs are frozen before prompt fitting;
fixed evidence is never silently truncated to fit a model. The host can explicitly
choose a larger context/executor. Each packet is `{id, body}`; `id` is SHA-256 of
`body.dump()` using cogg's canonical nlohmann JSON serialization:

```json
{
  "schema":"cogg:input/v1",
  "kind":"observation",
  "producer":"synthetic-fixture:camera/frame-1",
  "content":{"description":"The cat is lying on a cushion."},
  "sources":[]
}
```

Use `make_input` in C++ to construct packets. An `observation` has no packet sources.
An `inference` has one or more unique sources that must already appear in this same
packet list. IDs, topology and duplicates are validated. Limits: 16 packets,
32 KiB total, producer up to 200 bytes. Cycles, forward/absent references, duplicate
sources and hash mismatches fail before inference. A packet is a host assertion:
cogg does not fetch a URI, open a camera, authenticate its producer or inspect media.
Labels alone cannot prevent a host or model from making a false observation claim.

Two interpretations referencing the same observation retain the same source ID.
The kernel does not count them as votes or estimate statistical independence. The
controller must preserve source links when it builds later inputs. This small
provenance graph is not the original semantic disagreement graph.

Inputs live in the admission history and enter the current prompt. They do not
silently become working-memory keys or retrievable deposits. A later request needs
an explicit memory write or a new host-supplied context. `MemoryNote.sources` still
refers to existing subject deposits, not these input packet IDs.

## Cat example and process boundary

1. A text executor receives the request, returns `missing_input`; the request stays pending.
2. The host obtains an observation from a real sensor/vision worker. That worker may
   run in another process or service; it need not be another cogg subject.
3. The host builds an input packet and binds it to the original head and occasion.
4. A fresh cogg process invokes the same or another executor with that context.
5. One proposal commits. A concurrently returned result from the old head cannot commit.

There is no implicit camera connection or automatic second step here. These are
explicit host operations, exercised by the offline test with a labelled fixture.
One process/one model is a complete deployment. Enforcing that equation in the
kernel would couple identity and recovery to a model's lifetime without adding a
useful state invariant. Phase 5's optional registry does not change this boundary.

## CLI and validation

`run-route ... --context FILE` supplies a bound context. `run-model ... --once
--context FILE` does the same for native GGUF inference. Both commands return
**exit 4** for abstention; existing success/waiting, failure and cancellation codes
remain unchanged. `run-model` exits on abstention instead of polling it indefinitely.

Offline `outcomes` and `http-faults` tests cover the four reasons, no implicit
fallback, null distinction, mixed mutation rejection, persistence, idempotence,
quota, stale contexts/results, input tampering, incomplete provenance, model
capacity, separate CLI processes and legacy v3/v4 fixtures. Existing crash,
checkpoint, memory, time, routing and native-model suites remain regression gates.

Opt-in real model probe (two paid/local inference calls per executor):

```sh
python3 tools/phase6_smoke.py --cli build/cogg-cli \
  --config /path/to/private-executors.json --output /new/artifact-directory \
  --executors local-gpu deepseek kimi
```

This uses a **synthetic observation**, not a real camera or vision model. The test
checks missing-input abstention, then a supported attributed answer after a fresh
process supplies evidence for the same request. Passing checks the tested scenario,
not general epistemic calibration. Results and limitations are recorded in
[the validation report](phase6-validation-20260908.json).
