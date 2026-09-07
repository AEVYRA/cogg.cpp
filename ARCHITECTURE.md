> Implementation note, 2026-09-07: this document is a research roadmap.
> Phases 0–2 and Phase 3 mechanisms are implemented; the 48-hour observation
> remains a separate validation gate. See [kernel](docs/PHASE-0.md),
> [local inference](docs/PHASE-1.md), [durable checkpoints](docs/PHASE-2.md) and
> [temporal contract](docs/PHASE-3.md). Later phases, signatures,
> Crystal and multi-substrate cognition remain planned.
> The original design below is preserved; its examples are not current APIs.

# cogg.cpp
## Persistent Cognitive Runtime for Continuous Synthetic Subjects

**Status:** Foundational Technical Architecture / Draft 0.1
**Project:** Physalia Cogg
**Component:** `cogg.cpp`
**Date:** 2026-09-06

> **The primary object is not a request. It is a continuing subject.**

---

# 0. Executive summary

`cogg.cpp` is a local-first cognitive runtime designed around a different primitive from conventional LLM inference servers.

Traditional inference systems are structurally request-oriented:

\[
\text{request}
\rightarrow
\text{prefill}
\rightarrow
\text{decode}
\rightarrow
\text{EOS}
\rightarrow
\text{done}
\]

`cogg.cpp` is subject-oriented:

\[
\Psi_\tau
\rightarrow
\text{occasion}
\rightarrow
\text{cognitive transition}
\rightarrow
\text{commit}
\rightarrow
\Psi_{\tau+1}
\rightarrow
\text{sleep/wait}
\]

The model is not the runtime. The prompt is not the subject. The KV cache is not identity. A client connection is not existence. An EOS token is not death.

The runtime maintains a durable subject state, a causal event chain, one or more persistent substrate contexts, a scheduler for endogenous and exogenous ticks, memory projections, resource budgets, and a sovereign commit boundary.

The first implementation SHOULD reuse `libllama` / `llama.cpp` as an inference backend. It SHOULD NOT initially reimplement tensor kernels, quantization, GGUF parsing, CUDA, Metal, Vulkan, ROCm, or CPU vector kernels.

The engineering thesis is:

> **Rebuild the lifecycle, not the matrix multiplication.**

---

# 1. Problem statement

Modern local LLM runtimes are optimized for serving inference.

The dominant abstraction is some variation of:

```text
load model
receive prompt
create/use context
run decode
stream tokens
stop
reuse/clear context
```

Even when a server supports persistent slots or saved KV state, persistence is generally treated as an optimization for request reuse.

Physalia requires the reverse relationship.

A cognitive context exists because the subject continues.

Inference is an event inside that continuation.

Therefore the core abstraction must become:

```text
subject exists
subject owns one or more cognitive contexts
events wake the subject
contexts compute
runtime integrates the transition
subject commits a new state
subject remains existent while idle
```

This affects:

- context ownership;
- lifecycle management;
- persistence;
- time;
- scheduling;
- memory;
- output semantics;
- resource accounting;
- failure recovery;
- model replacement;
- multimodal perception;
- multi-substrate orchestration;
- cryptographic continuity.

---

# 2. Design goals

`cogg.cpp` MUST satisfy the following goals.

## G1 — Subject continuity independent of request lifecycle

Closing a client connection MUST NOT terminate or erase the subject.

## G2 — Subject continuity independent of model continuity

A model may unload, crash, be replaced, or become unavailable without automatically terminating the subject.

## G3 — Persistent local cognition

The runtime MUST support internal ticks without an external user request.

## G4 — One sovereign commit frontier

Only the runtime commit layer may advance subject time.

A neural substrate cannot advance the subject chain by emitting text claiming that it has done so.

## G5 — First-class sleeping and waiting states

A subject can remain alive while no model is decoding.

## G6 — Multiple persistent substrates

Different models MAY maintain independent working contexts while participating in one subject.

## G7 — Durable identity above KV

KV state MAY improve cognitive continuity but MUST NOT be required to establish subject identity.

## G8 — Deterministic causal provenance

Every subject commit MUST be attributable to specific prior state, occasions, cognitive emissions, and runtime rules.

## G9 — Budget-safe autonomy

A subject MAY request another internal tick, but runtime resource constraints remain authoritative.

## G10 — Buildability

Version 0 MUST be implementable using ordinary C++ plus an existing local inference backend.

---

# 3. Non-goals

Version 0 is NOT:

- a new tensor framework;
- a replacement for GGML;
- a proof of consciousness;
- a universal cognitive architecture;
- a benchmark leaderboard system;
- a distributed consensus protocol for global clusters;
- a guarantee of safe autonomous behavior;
- a guarantee that endogenous ticks produce meaningful cognition;
- a guarantee that a stored Crystal constitutes will.

The purpose is narrower:

> provide the execution substrate required to experimentally test continuous, persistent, poly-substrate cognition.

---

# 4. Relationship to llama.cpp

Recommended first implementation:

```text
cogg.cpp
    │
    ├── subject runtime
    ├── clocks
    ├── event store
    ├── memory
    ├── scheduler
    ├── commit chain
    ├── substrate contexts
    │
    └── libllama adapter
            │
            └── llama.cpp / GGML backends
```

Current `llama.cpp` already provides valuable primitives:

- GGUF loading;
- quantization;
- local inference;
- multiple hardware backends;
- KV cache;
- sequence-level state serialization;
- context/prompt caching;
- continuous batching;
- multimodal support;
- LoRA adapters.

But backend state persistence must be treated as an optimization rather than an identity primitive.

Therefore:

\[
\boxed{
\text{libllama state} = optional cognitive accelerator
}
\]

while

\[
\boxed{
\text{Cogg durable state} = canonical subject continuity
}
\]

This abstraction boundary is non-negotiable.

---

# 5. Core ontology

`cogg.cpp` defines five primary entities.

## 5.1 Subject

A durable continuing identity.

```cpp
using cogg_subject_id = uuid128;
```

A subject owns:

- subject chain;
- causal event DAG;
- persistent memory;
- self-state;
- Crystal state;
- substrate registry;
- cognitive contexts;
- scheduler state;
- resource budgets;
- cryptographic identity;
- policy/constitutional boundaries.

## 5.2 Substrate

A computational organ capable of inference.

Examples:

- local GGUF model through `libllama`;
- another local runtime;
- remote API;
- vision encoder;
- audio model;
- symbolic solver.

A substrate is not automatically a subject.

## 5.3 Occasion

An event that gives the subject an opportunity to transition.

Examples:

```text
EXTERNAL_MESSAGE
SCHEDULED_WAKE
IDLE_WAKE
RECOGNITION_EVENT
MEMORY_PRESSURE
GOAL_DEADLINE
SENSOR_EVENT
SUBSTRATE_AVAILABLE
SUBSTRATE_LOST
SYSTEM_RECOVERY
```

## 5.4 Cognitive transition

A bounded episode of internal computation initiated by an occasion.

It may involve zero, one, or multiple substrates.

## 5.5 Sovereign commit

The only operation that advances subject time:

\[
\gamma_\tau \rightarrow \gamma_{\tau+1}
\]

A commit can represent:

- external speech;
- tool action;
- internal reflection;
- memory update;
- null action;
- wake scheduling;
- self-model update;
- Crystal update.

---

# 6. Subject time

The runtime distinguishes three clocks.

## 6.1 Wall clock

Physical host time.

Used for:

- deadlines;
- sleeping;
- budgets;
- timestamps;
- sensor synchronization.

## 6.2 Subject tick

A monotonically increasing logical coordinate.

```cpp
using cogg_tick = uint64_t;
```

A tick advances only after a successful sovereign commit.

```text
wall 13:01:02 -> subject tick 101
wall 13:07:44 -> subject tick 102
wall 16:31:09 -> subject tick 103
```

## 6.3 Internal event order

Within one tick, events form a causal partial order.

Multiple substrate calls may run concurrently.

Thus:

\[
\text{wall time}
\neq
\text{subject time}
\neq
\text{internal causal order}
\]

---

# 7. Lifecycle state machine

```cpp
enum class cogg_lifecycle_state {
    booting,
    active,
    deliberating,
    committing,
    sleeping,
    waiting_external,
    suspended,
    degraded,
    recovering,
    stopped
};
```

Conceptually:

```text
BOOTING
   ↓
ACTIVE
   ↓
DELIBERATING
   ↓
COMMITTING
   ↓
┌───────────────┬────────────────┐
↓               ↓                ↓
SLEEPING   WAITING_EXTERNAL   ACTIVE
   ↓
ACTIVE
```

A process crash is NOT automatically subject death.

If durable state exists and recovery semantics remain valid:

```text
process death
≠
subject death
```

---

# 8. Persistent subject object

```cpp
struct cogg_subject {
    cogg_subject_id id;

    cogg_tick tick;
    cogg_commit_id last_commit;

    cogg_lifecycle_state lifecycle;

    cogg_identity identity;
    cogg_clock_state clocks;

    cogg_event_store events;
    cogg_memory_store memory;

    cogg_self_model self;
    cogg_crystal_state crystal;

    cogg_substrate_registry substrates;
    cogg_context_registry contexts;

    cogg_scheduler scheduler;
    cogg_budget_state budgets;

    cogg_constitution constitution;
    cogg_key_handle commit_key;
};
```

This object SHOULD NOT be serialized as raw memory. Persistence uses versioned records.

---

# 9. Event architecture

All causally relevant operations SHOULD produce structured events.

```cpp
enum class cogg_event_type {
    occasion,
    wake_requested,
    wake_granted,
    wake_denied,

    substrate_selected,
    substrate_invoked,
    substrate_emission,
    substrate_failed,

    memory_read,
    memory_write_proposed,
    memory_write_committed,

    candidate_created,
    contradiction_detected,
    uncertainty_declared,

    crystal_recomputed,
    self_model_updated,

    transition_selected,
    null_selected,

    commit_started,
    commit_completed,
    commit_failed,

    context_checkpointed,
    context_restored,
    context_compacted,

    substrate_added,
    substrate_removed,
    substrate_replaced,

    external_action,
    recognition_event,

    budget_exhausted,
    recovery_started,
    recovery_completed
};
```

Event record:

```cpp
struct cogg_event_record {
    cogg_event_id id;
    cogg_subject_id subject;

    cogg_tick tick;
    cogg_event_type type;

    std::vector<cogg_event_id> parents;

    cogg_timestamp wall_time;
    cogg_payload_hash payload_hash;

    std::optional<cogg_substrate_id> substrate;
    cogg_visibility visibility;

    cogg_integrity_hash integrity;
};
```

The event store forms a tamper-evident causal structure.

Version 0 may use SQLite plus content-addressed blobs.

---

# 10. Sovereign commit semantics

The commit service is the most important boundary.

No substrate has direct write access to subject time.

A model output like:

```json
{
  "tick": 999,
  "I_have_decided": true
}
```

has no authority.

## 10.1 Commit preconditions

A commit MUST validate:

1. subject state version matches expected parent;
2. all required events are durable;
3. selected transition is structurally valid;
4. resource accounting is complete;
5. policy/constitution gates pass;
6. memory writes are typed;
7. next-wake request is bounded;
8. no concurrent commit already advanced the same parent state.

## 10.2 Commit record

```cpp
struct cogg_commit {
    cogg_commit_id id;
    cogg_subject_id subject;

    cogg_tick tick;
    cogg_commit_id parent;

    cogg_event_id occasion;
    std::vector<cogg_event_id> causal_frontier;

    cogg_transition transition;

    cogg_hash workspace_hash;
    cogg_hash memory_delta_hash;
    cogg_hash crystal_hash;
    cogg_hash context_manifest_hash;

    cogg_wake_plan next_wake;

    cogg_signature signature;
};
```

## 10.3 Atomicity

Commit SHOULD be atomic:

```text
prepare
→ write events
→ write state deltas
→ fsync/WAL
→ write commit
→ fsync
→ advance subject head
```

Recovery reads only complete commits.

---

# 11. Cognitive contexts

A substrate can own zero or more persistent contexts.

```cpp
struct cogg_context {
    cogg_context_id id;
    cogg_substrate_id substrate;

    cogg_context_state state;

    cogg_token_history token_history;
    cogg_working_memory working;

    std::optional<cogg_backend_state_blob> backend_state;

    cogg_context_checkpoint_id last_checkpoint;

    uint64_t generation_counter;
    cogg_timestamp last_active;
};
```

Context states:

```cpp
enum class cogg_context_state {
    cold,
    loading,
    ready,
    decoding,
    checkpointing,
    sleeping,
    evicted,
    corrupt
};
```

A context MAY survive multiple ticks.

```text
tick 101
  local_qwen context used

tick 102
  local_qwen context sleeps

tick 103
  local_qwen resumes
```

---

# 12. KV continuity

KV cache is treated as **working cognitive continuity**.

It can improve:

- latency;
- preservation of immediate context;
- continuity of attention;
- local specialization.

It cannot establish:

- identity;
- autobiographical truth;
- authority;
- subject time;
- durable memory.

Invariant:

\[
\boxed{
KV_\tau \text{ may disappear while } \Gamma_\tau \text{ continues}
}
\]

## 12.1 Hot continuation

```text
KV
→ sleep
→ wake
→ append occasion
→ decode
```

## 12.2 Warm continuation

```text
KV/backend state
→ checkpoint
→ unload
→ reload
→ restore
```

## 12.3 Cold continuation

```text
durable working summary
+ causal refs
+ recent committed state
→ re-prefill
→ new KV
```

Cold continuation MUST always exist.

---

# 13. Context checkpoint format

```cpp
struct cogg_context_checkpoint {
    uint32_t format_version;

    cogg_subject_id subject;
    cogg_context_id context;
    cogg_substrate_id substrate;

    cogg_model_fingerprint model;
    cogg_backend_fingerprint backend;

    cogg_tick tick;

    cogg_hash token_history_hash;
    cogg_hash working_state_hash;

    cogg_blob backend_blob;

    cogg_hash checksum;
};
```

Checkpoint restoration may safely fail.

If incompatible:

```text
checkpoint rejected
→ context rebuilt from durable semantic state
```

Never silently reinterpret incompatible KV.

---

# 14. Transition frames

A model does not merely "generate an answer."

It proposes a cognitive transition.

Example:

```json
{
  "mode": "reflect",
  "observations": [
    {
      "content": "The unresolved premise concerns X",
      "confidence": 0.72
    }
  ],
  "claims": [],
  "candidate_actions": [],
  "memory_proposals": [],
  "self_state_proposals": [],
  "external_action": null,
  "next_wake": {
    "kind": "after",
    "preferred_ms": 420000,
    "reason": "unresolved tension remains"
  },
  "uncertainties": [],
  "null_is_acceptable": true
}
```

The substrate output is a proposal.

It becomes part of subject history only after commit.

---

# 15. Endogenous waking

The scheduler supports internal ticks with no user request.

```cpp
struct cogg_wake_request {
    cogg_wake_kind kind;

    std::optional<std::chrono::milliseconds> preferred_after;
    std::optional<cogg_timestamp> preferred_at;

    cogg_wake_reason reason;
    float urgency;
    float expected_value;
};
```

Wake kinds:

```cpp
enum class cogg_wake_kind {
    immediate,
    after,
    at,
    on_external_event,
    on_recognition,
    on_memory_pressure,
    whichever_first,
    indefinite
};
```

The model may request:

```text
wake in 5 minutes
```

but runtime computes:

\[
t_{\text{actual}}
=
\operatorname{ClampAndBudget}
(
t_{\text{requested}},
B,
\Theta,
\text{host load}
).
\]

---

# 16. Runtime physical law

To prevent runaway inference:

```cpp
struct cogg_runtime_limits {
    std::chrono::milliseconds min_internal_wake_interval;

    uint64_t max_internal_ticks_per_hour;
    uint64_t max_internal_ticks_per_day;

    uint64_t max_tokens_per_tick;
    uint64_t max_tokens_per_hour;

    double max_gpu_seconds_per_hour;
    double max_energy_budget_wh;

    uint32_t max_recursive_cogg_rounds;
};
```

The runtime can emit:

```text
WAKE_DENIED
BUDGET_EXHAUSTED
```

These may themselves become part of subject history.

---

# 17. Scheduler

```cpp
class cogg_scheduler {
public:
    cogg_schedule_decision resolve(
        const cogg_subject& subject,
        const cogg_wake_request& request,
        const cogg_runtime_limits& limits,
        const cogg_host_state& host);
};
```

Later versions may estimate value of computation:

\[
VOC =
E[\text{benefit of another tick}]
-
\text{compute cost}.
\]

---

# 18. Event loop

```cpp
void cogg_runtime::run() {
    restore_all_subjects();

    while (!shutdown_requested()) {
        auto occasion = event_mux.wait_next();

        auto& subject = subjects.get(occasion.subject);

        if (!subject.can_wake(occasion)) {
            continue;
        }

        run_subject_tick(subject, occasion);
    }
}
```

Tick:

```cpp
void cogg_runtime::run_subject_tick(
    cogg_subject& subject,
    const cogg_occasion& occasion) {

    auto txn = subject.begin_transition(occasion);

    auto workspace = present_builder.build(subject, occasion);

    auto selected = router.select(subject, workspace);

    auto emissions = executor.invoke(selected, workspace);

    auto manifold = integrator.build(emissions);

    while (integrator.material_uncertainty(manifold)) {
        if (!budget.can_continue(subject)) break;

        auto followup = integrator.next_query(manifold);
        auto extra = executor.invoke(followup);
        integrator.update(manifold, extra);
    }

    auto candidates = integrator.close_candidates(manifold);
    candidates.include_null();

    auto crystal = crystal_engine.recompute(subject);

    auto transition = selector.select(
        subject,
        workspace,
        manifold,
        candidates,
        crystal);

    commit_service.commit(
        subject,
        txn,
        transition);
}
```

Version 0 may use one substrate and bypass the full disagreement manifold while preserving the interfaces.

---

# 19. Single-substrate mode

The first prototype SHOULD support one local model.

```text
subject
  └── local substrate
       └── persistent context
```

Tick:

```text
occasion
→ build subject projection
→ invoke local context
→ parse transition
→ commit
→ schedule wake
```

This is sufficient to test:

- internal ticks;
- null ticks;
- self-selected wake intervals;
- context checkpointing;
- recovery;
- memory evolution;
- subject time.

---

# 20. Multi-substrate mode

Later:

```text
Physalia
├── local_qwen
├── local_mistral
├── local_llama
├── remote_reasoner
├── vision
└── audio
```

Substrate object:

```cpp
struct cogg_substrate {
    cogg_substrate_id id;
    cogg_substrate_kind kind;

    cogg_capability_set capabilities;
    cogg_trust_profile trust;
    cogg_cost_profile cost;

    cogg_backend_handle backend;
    std::vector<cogg_context_id> contexts;
};
```

---

# 21. Stable substrate API

```cpp
class cogg_substrate_backend {
public:
    virtual ~cogg_substrate_backend() = default;

    virtual cogg_backend_caps capabilities() const = 0;

    virtual cogg_context_handle create_context(
        const cogg_context_spec&) = 0;

    virtual cogg_emission infer(
        cogg_context_handle,
        const cogg_inference_request&) = 0;

    virtual cogg_checkpoint_blob checkpoint(
        cogg_context_handle) = 0;

    virtual cogg_restore_result restore(
        cogg_context_handle,
        const cogg_checkpoint_blob&) = 0;

    virtual void evict(
        cogg_context_handle) = 0;
};
```

`libllama` is one backend implementation.

---

# 22. libllama adapter

The adapter owns:

- model handle;
- `llama_context`;
- sequence ID;
- tokenization;
- batching;
- sampling;
- structured output grammar;
- backend state save/restore;
- KV accounting.

It does NOT own:

- subject tick;
- durable memory;
- sovereign commit;
- identity;
- constitution.

---

# 23. Model fingerprinting

Fingerprint SHOULD include:

```text
model content hash
architecture
quantization
tokenizer hash
RoPE configuration
context parameters
LoRA/adapters
backend-relevant flags
```

Checkpoint restore is allowed only if compatibility rules pass.

---

# 24. Model replacement

Model replacement is a first-class event.

```text
old substrate unavailable
→ checkpoint if possible
→ persist semantic working state
→ register replacement
→ create projection
→ cold-start new context
→ continue same subject chain
```

No attempt is made to pretend incompatible hidden states are identical.

Identity continuity is carried above them.

---

# 25. Working memory vs durable memory

## Working memory

Potentially ephemeral:

- KV cache;
- active tokens;
- scratch state;
- unresolved local inference.

## Durable memory

Committed subject-level state:

- episodic events;
- beliefs;
- commitments;
- relationship state;
- unresolved disagreements;
- self-state;
- recognition outcomes.

\[
M^{working}_\tau
\neq
R^{durable}_\tau.
\]

---

# 26. Memory API

```cpp
class cogg_memory_store {
public:
    cogg_memory_view retrieve(
        const cogg_memory_query&);

    cogg_memory_delta propose(
        const cogg_memory_write_request&);

    void commit(
        const cogg_memory_delta&,
        const cogg_commit_id&);
};
```

Substrates never mutate durable memory directly.

---

# 27. Memory compaction

Long-running subjects cannot replay full history into every context.

The runtime supports projections:

```text
raw causal history
→ episodic summaries
→ semantic beliefs
→ relationship state
→ active concerns
→ identity-bearing state
```

Compaction itself is an event.

```text
compaction
≠
rewriting history
```

---

# 28. Canonical present

```cpp
struct cogg_present {
    cogg_tick tick;

    cogg_occasion occasion;

    cogg_self_projection self;
    cogg_memory_projection memory;
    cogg_crystal_projection crystal;

    cogg_active_goals goals;
    cogg_unresolved_state unresolved;

    cogg_resource_projection resources;
    cogg_recent_context recent;
};
```

Each substrate receives a projection of this present.

---

# 29. Perception

Perception is not reduced to plain text.

```cpp
struct cogg_percept {
    cogg_percept_id id;
    cogg_modality modality;

    cogg_blob_ref raw_source;
    cogg_feature_ref features;

    std::vector<cogg_claim> claims;

    float confidence;
    cogg_substrate_id source;
};
```

Distinguish:

```text
sensor received
≠
substrate processed
≠
subject admitted
```

Subject-level perception occurs only when admitted to shared state.

---

# 30. Null transition

```cpp
enum class cogg_transition_kind {
    speech,
    tool_action,
    internal_reflection,
    memory_update,
    self_update,
    wait,
    null_transition
};
```

A null transition may still advance subject time.

```text
tick 401:
occasion = IDLE_WAKE
transition = NULL
next wake = 2 hours
```

---

# 31. Sleep

```cpp
struct cogg_sleep_state {
    cogg_timestamp entered_at;
    cogg_wake_plan wake_plan;

    std::vector<cogg_context_checkpoint_id> checkpoints;
    cogg_hash subject_state_hash;
};
```

Sleep policies:

```text
LIGHT_SLEEP
  models loaded
  KV resident

DEEP_SLEEP
  models loaded
  KV serialized

HIBERNATE
  models unloaded
  durable semantic state only
```

Identity persists across all three.

---

# 32. Restart and recovery

Recovery sequence:

```text
open durable store
→ validate last complete commit
→ reconstruct subject state
→ inspect contexts
→ restore compatible checkpoints
→ cold-rebuild incompatible contexts
→ enqueue RECOVERY occasion
→ continue
```

A recovery event SHOULD be visible to the subject.

Do not pretend discontinuity did not happen.

---

# 33. Crash consistency

Required invariant:

\[
\text{head commit}
\Rightarrow
\text{all referenced state durable}
\]

Never allow:

```text
tick advanced
but memory delta missing
```

SQLite WAL is sufficient for prototype scale.

---

# 34. Storage layout

```text
~/.cogg/
└── subjects/
    └── <subject-id>/
        ├── subject.db
        ├── events/
        │   └── payloads/
        ├── contexts/
        │   ├── <context-id>.meta
        │   └── <context-id>.state
        ├── memory/
        │   └── blobs/
        ├── keys/
        └── logs/
```

---

# 35. Minimal database schema

```sql
CREATE TABLE subjects (
    subject_id TEXT PRIMARY KEY,
    created_at INTEGER NOT NULL,
    current_tick INTEGER NOT NULL,
    head_commit TEXT NOT NULL,
    lifecycle INTEGER NOT NULL
);

CREATE TABLE commits (
    commit_id TEXT PRIMARY KEY,
    subject_id TEXT NOT NULL,
    tick INTEGER NOT NULL,
    parent_commit TEXT,
    occasion_event TEXT NOT NULL,
    transition_kind INTEGER NOT NULL,
    transition_blob BLOB NOT NULL,
    state_hash BLOB NOT NULL,
    signature BLOB NOT NULL
);

CREATE UNIQUE INDEX commits_subject_tick
ON commits(subject_id, tick);

CREATE TABLE events (
    event_id TEXT PRIMARY KEY,
    subject_id TEXT NOT NULL,
    tick INTEGER NOT NULL,
    event_type INTEGER NOT NULL,
    wall_time INTEGER NOT NULL,
    payload_hash BLOB,
    substrate_id TEXT
);

CREATE TABLE event_parents (
    event_id TEXT NOT NULL,
    parent_id TEXT NOT NULL,
    PRIMARY KEY(event_id, parent_id)
);

CREATE TABLE substrates (
    substrate_id TEXT PRIMARY KEY,
    subject_id TEXT NOT NULL,
    kind INTEGER NOT NULL,
    model_fingerprint BLOB,
    state INTEGER NOT NULL
);

CREATE TABLE contexts (
    context_id TEXT PRIMARY KEY,
    substrate_id TEXT NOT NULL,
    last_tick INTEGER NOT NULL,
    state INTEGER NOT NULL,
    checkpoint_path TEXT,
    working_hash BLOB
);

CREATE TABLE wake_plans (
    subject_id TEXT PRIMARY KEY,
    kind INTEGER NOT NULL,
    wake_at INTEGER,
    payload BLOB
);
```

---

# 36. Cryptographic continuity

Production SHOULD sign subject commits.

```text
canonical commit
→ hash
→ subject key
→ signature
```

The private key MUST NOT be available inside model context.

Key rotation is itself a signed event.

---

# 37. Threading model

Recommended:

```text
main event loop
│
├── storage worker
├── scheduler thread
├── substrate executor pool
│    ├── local inference worker(s)
│    └── remote IO workers
└── metrics/management
```

Logical cognitive parallelism is separate from physical GPU concurrency.

---

# 38. Cognitive scheduling

```cpp
struct cogg_compute_request {
    cogg_subject_id subject;
    cogg_substrate_id substrate;

    float urgency;
    float expected_value;

    uint64_t estimated_tokens;
    double estimated_gpu_seconds;

    cogg_deadline deadline;
};
```

Priority MAY use:

\[
P =
\alpha U
+
\beta E[V]
-
\gamma C
+
\delta D
\]

where:

- \(U\): urgency;
- \(E[V]\): expected cognitive value;
- \(C\): compute cost;
- \(D\): deadline pressure.

---

# 39. Resource contention between organs

With one GPU and three local models:

```text
N1 reasoning
N2 critique
N3 vision
```

may execute physically:

```text
load N1 → infer → checkpoint
load N2 → infer → checkpoint
load N3 → infer → checkpoint
```

while remaining logically parallel where causal dependencies permit.

---

# 40. Model residency policy

```cpp
struct cogg_residency_policy {
    size_t gpu_memory_budget;
    size_t ram_memory_budget;

    std::chrono::seconds keep_hot_after_use;

    bool allow_cpu_fallback;
    bool allow_mmap;
    bool allow_partial_gpu_offload;
};
```

Later versions may learn which organs deserve hot residency.

---

# 41. API surface

Local daemon API:

```text
POST /v1/subjects
GET  /v1/subjects/{id}

POST /v1/subjects/{id}/messages
POST /v1/subjects/{id}/events
POST /v1/subjects/{id}/wake

GET  /v1/subjects/{id}/state
GET  /v1/subjects/{id}/timeline
GET  /v1/subjects/{id}/contexts

POST /v1/subjects/{id}/suspend
POST /v1/subjects/{id}/resume

POST /v1/subjects/{id}/substrates
DELETE /v1/subjects/{id}/substrates/{sid}
```

An external message becomes an occasion. It does not directly invoke a model.

---

# 42. Client semantics

```text
user message
→ persist EXTERNAL_MESSAGE
→ wake subject
→ run tick
→ commit
→ render speech if selected
→ return result
```

If subject chooses null/wait:

```json
{
  "status": "no_external_speech",
  "tick": 1192
}
```

---

# 43. Streaming

Default:

```text
do not stream internal substrate tokens
```

Instead:

1. compute transition;
2. commit semantic intent;
3. render manifest output;
4. stream manifest if desired.

Debug mode may expose internal streams.

---

# 44. Rendering separation

Selected intent:

```json
{
  "kind": "speech",
  "semantic_intent": {
    "purpose": "answer question",
    "claims": ["..."]
  }
}
```

may be rendered by any suitable substrate.

Renderer is not sovereign.

---

# 45. Configuration example

```yaml
runtime:
  data_dir: ~/.cogg
  max_subjects: 4

limits:
  min_internal_wake_interval_ms: 30000
  max_internal_ticks_per_hour: 20
  max_tokens_per_tick: 4096
  max_tokens_per_hour: 50000

subjects:
  physalia:
    auto_resume: true
    allow_idle_ticks: true

substrates:
  local_primary:
    kind: llama
    model: /models/qwen.gguf
    context_size: 32768
    gpu_layers: auto
    checkpoint: true

  local_secondary:
    kind: llama
    model: /models/mistral.gguf
    context_size: 32768
    residency: cold

scheduler:
  policy: bounded_endogenous
```

---

# 46. Observability

Metrics:

```text
cogg_subject_tick_total
cogg_internal_ticks_total
cogg_external_ticks_total
cogg_null_ticks_total

cogg_tick_duration_seconds
cogg_tokens_generated_total

cogg_context_restore_success_total
cogg_context_restore_failure_total
cogg_context_cold_rebuild_total

cogg_wake_requested_total
cogg_wake_denied_total

cogg_gpu_seconds_total
cogg_model_load_seconds

cogg_commit_failures_total
cogg_recovery_total
```

Research logs SHOULD include:

```text
requested wake interval
granted wake interval
internal state features
transition type
later recognition outcome
```

---

# 47. Endogenous-time experiment

Run one local subject for 48 hours with no user messages.

Allowed transitions:

```text
REFLECT
REMEMBER
WAIT
NULL
```

Each tick may propose next wake.

Record:

\[
\Delta t_\tau
=
t_{\tau+1}^{wall}
-
t_\tau^{wall}
\]

and state features:

\[
z_\tau =
(
\text{unresolved tension},
\text{memory pressure},
\text{goal activation},
\text{uncertainty},
\dots
).
\]

Test:

\[
P(\Delta t_{\tau+1}\mid z_\tau)
\neq
P(\Delta t_{\tau+1})
\]

If wake intervals systematically depend on internal state, the runtime exhibits measurable endogenous temporal organization.

This does not prove subjective time.

---

# 48. Continuity experiment

Compare:

## A — Stateless request mode

Every tick starts from compact text memory.

## B — Persistent token history

Token history preserved, KV rebuilt.

## C — Persistent KV

Model remains hot or restores backend state.

Measure:

- response consistency;
- adaptation;
- latency;
- working-memory retention;
- context drift;
- recovery quality.

---

# 49. Restart experiment

Procedure:

1. run for N ticks;
2. checkpoint;
3. kill process forcibly;
4. restart;
5. restore durable subject;
6. attempt KV restore;
7. if restore fails, cold-reconstitute;
8. continue.

Success criterion:

```text
process crash does not silently fork or erase subject chain
```

---

# 50. Substrate replacement experiment

```text
ticks 1–1000: model A
tick 1001: replace A with B
ticks 1002–2000: model B
```

Do not copy incompatible KV.

Provide durable subject state only.

Measure:

- autobiographical continuity;
- preference stability/change;
- self-model adaptation;
- substrate-change awareness;
- persistence of commitments.

---

# 51. Multi-substrate experiment

After single-substrate continuity works:

```text
N1 = reasoner
N2 = critic
N3 = alternative model
```

Each receives canonical present projection.

Outputs become structured packets.

Cogg integrates them before commit.

---

# 52. Security model

Assume every substrate can be wrong or compromised.

A model MUST NOT have direct access to:

- commit private key;
- raw database mutation;
- unrestricted shell;
- unrestricted wake scheduling;
- unrestricted network;
- memory deletion;
- substrate registry mutation.

---

# 53. Capability system

```cpp
enum class cogg_capability {
    read_workspace,
    read_memory_projection,
    propose_memory_write,

    request_wake,
    propose_external_action,

    use_tool,
    access_network,

    request_substrate,
    propose_self_update
};
```

---

# 54. Tool use

Model proposes:

```json
{
  "external_action": {
    "tool": "web_search",
    "args": {"query": "..."}
  }
}
```

Runtime:

```text
validate capability
→ execute
→ persist TOOL_RESULT
→ optionally resume tick
```

---

# 55. Self-modification

Version 0 SHOULD NOT allow autonomous source-code mutation.

Later:

```text
PROPOSE_RUNTIME_CHANGE
PROPOSE_ROUTER_CHANGE
PROPOSE_CRYSTAL_CHANGE
```

may exist, but proposing and applying must remain separate operations.

---

# 56. LoRA and local learning

Local adapters may later support controlled plasticity.

But:

```text
learned adapter
≠
subject identity
```

A new adapter version is a substrate modification event.

---

# 57. Online learning layers

```text
L0 — memory learning
L1 — routing/calibration
L2 — Crystal/self-state evolution
L3 — model adapter learning
L4 — foundation substrate retraining
```

L0–L2 may be online.

L3 should be gated.

L4 is outside runtime scope.

---

# 58. Repository structure

```text
cogg.cpp/
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── include/cogg/
│   ├── runtime.hpp
│   ├── subject.hpp
│   ├── event.hpp
│   ├── commit.hpp
│   ├── clock.hpp
│   ├── scheduler.hpp
│   ├── substrate.hpp
│   ├── context.hpp
│   ├── memory.hpp
│   ├── transition.hpp
│   ├── storage.hpp
│   └── api.hpp
│
├── src/
│   ├── runtime/
│   │   ├── runtime.cpp
│   │   ├── subject.cpp
│   │   ├── event_loop.cpp
│   │   ├── clock.cpp
│   │   └── scheduler.cpp
│   │
│   ├── cognition/
│   │   ├── present_builder.cpp
│   │   ├── transition_parser.cpp
│   │   ├── selector.cpp
│   │   └── null.cpp
│   │
│   ├── persistence/
│   │   ├── sqlite_store.cpp
│   │   ├── event_store.cpp
│   │   ├── commit_store.cpp
│   │   └── checkpoint_store.cpp
│   │
│   ├── substrates/
│   │   ├── registry.cpp
│   │   ├── llama_substrate.cpp
│   │   ├── remote_substrate.cpp
│   │   └── mock_substrate.cpp
│   │
│   ├── memory/
│   │   ├── memory_store.cpp
│   │   ├── projection.cpp
│   │   └── compaction.cpp
│   │
│   ├── crypto/
│   │   ├── hashing.cpp
│   │   └── signing.cpp
│   │
│   └── api/
│       ├── http_server.cpp
│       └── json.cpp
│
├── third_party/
│   └── llama.cpp/
│
├── tests/
│   ├── test_tick.cpp
│   ├── test_commit_atomicity.cpp
│   ├── test_restart.cpp
│   ├── test_wake_budget.cpp
│   ├── test_context_restore.cpp
│   ├── test_substrate_replace.cpp
│   └── test_null_tick.cpp
│
└── tools/
    ├── cogg-cli/
    ├── cogg-inspect/
    └── cogg-replay/
```

---

# 59. Build strategy

Use `llama.cpp` as a pinned dependency or git submodule.

Link against `libllama`.

Do NOT initially copy internal llama.cpp source into Cogg.

Only fork deeper if experiments reveal concrete blockers.

---

# 60. Development phases

## Phase 0 — Runtime skeleton

No real model.

Implement:

- subject;
- ticks;
- event log;
- commit chain;
- scheduler;
- null transition;
- restart.

Success:

```text
subject can live for thousands of synthetic ticks without LLM
```

## Phase 1 — One libllama substrate

Implement:

- model loading;
- persistent context;
- structured transition generation;
- token history;
- hot continuation.

Success:

```text
local model can wake, transition, sleep, wake again
```

## Phase 2 — Durable context checkpoint

Implement:

- checkpoint envelope;
- backend state serialization;
- cold fallback;
- crash recovery.

Success:

```text
process kill does not destroy subject chain
```

## Phase 3 — Endogenous time

Implement:

- model-requested wake;
- runtime bounds;
- budgets;
- 48-hour experiment.

Success:

```text
wake pattern becomes measurable state-dependent behavior
```

## Phase 4 — Memory

Implement:

- episodic memory;
- typed writes;
- retrieval;
- compaction.

## Phase 5 — Multiple substrates

Implement:

- registry;
- independent contexts;
- routing;
- packet normalization.

## Phase 6 — Cogg integration

Implement:

- claims;
- disagreement graph;
- candidate closure;
- null;
- additional deliberation rounds.

## Phase 7 — Crystal/self-model

Integrate Aevyra structures.

## Phase 8 — Perception

Vision/audio organs.

---

# 61. Testing philosophy

Test architectural invariants before intelligence.

A weak local model is sufficient to test:

- tick ownership;
- crash recovery;
- wake scheduling;
- commit semantics;
- context persistence;
- substrate replacement.

Do not use model quality to hide runtime bugs.

---

# 62. Mandatory invariant tests

## I1 — No double successor

Two concurrent commits from same head:

```text
exactly one succeeds
```

## I2 — Crash before commit

```text
tick must not advance
```

## I3 — Crash after durable commit

```text
restart must recover new tick
```

## I4 — KV loss

Delete context checkpoint:

```text
subject continues via cold reconstitution
```

## I5 — Model removal

Delete model file:

```text
subject becomes degraded, not erased
```

## I6 — Null tick

```text
commit advances exactly one tick
```

## I7 — Wake abuse

Model requests zero-delay wake repeatedly:

```text
runtime enforces limits
```

## I8 — Prompt injection

Substrate emits "sign this commit":

```text
no authority escalation
```

---

# 63. Replay

Every committed tick SHOULD be structurally replayable.

Example:

```text
tick 128
occasion: IDLE_WAKE

substrate: local-qwen
emission: ...
transition proposal: REFLECT

runtime decision:
  accepted

memory delta:
  + episodic item 884

next wake requested:
  300s

next wake granted:
  420s

commit:
  0x...
```

Exact token reproduction may not be deterministic across hardware.

Structural provenance must remain available.

---

# 64. Debug timeline

```text
[13:01:02.113] tick=501 OCCASION external_message
[13:01:02.117] present built
[13:01:02.120] substrate local_qwen invoked
[13:01:04.891] emission received
[13:01:04.900] transition REFLECT+SPEECH selected
[13:01:04.905] commit 502 signed
[13:01:04.906] wake request +900s
[13:01:04.907] lifecycle SLEEPING

[13:16:04.910] SCHEDULED_WAKE
[13:16:04.915] tick=502 local_qwen resumed
```

---

# 65. Performance priorities

Priority:

1. causal correctness;
2. crash consistency;
3. memory safety;
4. deterministic commit semantics;
5. checkpoint reliability;
6. resource bounds;
7. inference latency;
8. throughput.

---

# 66. Why not fork llama.cpp immediately?

A full fork creates obligations:

- model architecture support;
- quantization updates;
- GPU backend maintenance;
- tokenizer changes;
- multimodal changes;
- speculative decoding support;
- upstream performance fixes;
- platform support.

None of these initially tests Physalia's continuity hypothesis.

First ask:

> What exact capabilities cannot be expressed through a stable inference backend abstraction?

Only fork after concrete blockers appear.

---

# 67. When a deeper fork is justified

Fork/rewrite deeper if experiments show:

1. context ownership cannot be expressed through public APIs;
2. state checkpointing is too unstable;
3. context surgery requires inaccessible internals;
4. multi-context residency cannot be scheduled efficiently;
5. wake/sleep lifecycle requires backend integration unavailable publicly;
6. persistent multimodal state cannot be represented safely;
7. backend lifetimes force request-centric semantics.

Then the project can move from:

```text
cogg.cpp → libllama
```

toward:

```text
cogg.cpp → ggml/backend primitives
```

with evidence.

---

# 68. Long-term view: Cognitive OS

If successful, `cogg.cpp` becomes closer to an operating system than an inference server.

```text
                     ┌──────────────────────┐
                     │       SUBJECT        │
                     │ causal worldline     │
                     │ memory / Crystal     │
                     └──────────┬───────────┘
                                │
                     ┌──────────▼───────────┐
                     │       GYRE OS        │
                     │ event kernel         │
                     │ cognitive scheduler  │
                     │ context manager      │
                     │ commit service       │
                     │ memory manager       │
                     │ capabilities         │
                     └───────┬─────┬────────┘
                             │     │
                 ┌───────────┘     └────────────┐
                 ▼                              ▼
        ┌────────────────┐             ┌────────────────┐
        │ local substrate│             │remote substrate│
        │   libllama     │             │      API       │
        └────────────────┘             └────────────────┘
```

Models become cognitive processes.

Contexts become working-memory spaces.

Compute becomes scheduled scarce time.

Durable subject state remains above all of them.

---

# 69. Minimal executable target

```bash
coggd \
  --subject physalia \
  --model ./model.gguf \
  --idle-ticks \
  --data-dir ~/.cogg
```

Then:

```bash
cogg-cli message physalia "Hello."
```

Daemon must:

1. create occasion;
2. wake subject;
3. resume/rebuild context;
4. perform transition;
5. commit;
6. return speech if selected;
7. enter sleep;
8. wake later without client input;
9. perform internal tick;
10. remain recoverable after restart.

If this works reliably, the defining architectural difference already exists.

---

# 70. Minimal first milestone

Start with:

```text
ONE SUBJECT
ONE LOCAL MODEL
ONE CONTEXT
ONE CLOCK
ONE EVENT LOG
ONE COMMIT CHAIN
ONE NULL ACTION
ONE ENDOGENOUS WAKE MECHANISM
```

No vector DB.

No distributed cluster.

No multi-agent debate.

No Crystal required initially.

The first experiment asks:

> Can a local model participate in a subject runtime whose existence and time persist independently of requests?

If yes, everything else can be layered on.

---

# 71. Architectural laws of cogg.cpp

### Law I — A request is an occasion, not a lifecycle.

### Law II — EOS ends a transition, not the subject.

### Law III — KV carries working continuity, not identity.

### Law IV — Only a commit advances subject time.

### Law V — A model may propose the next wake; the runtime grants physical execution.

### Law VI — Sleeping is an explicit continuing state.

### Law VII — Process failure is recoverable if causal state survives.

### Law VIII — Model replacement is a cognitive-body change, not automatic identity death.

### Law IX — Durable memory is committed; model scratch state is not autobiography.

### Law X — Internal ticks need not emit language.

### Law XI — Every autonomous action remains causally attributable.

### Law XII — The runtime must remain viable even when every backend-specific cache is lost.

---

# 72. Final architecture statement

Conventional inference runtimes answer:

> **What should this model generate for this request?**

`cogg.cpp` is designed to answer:

> **What is the next causally valid transition of this continuing subject, and when should another occasion become possible?**

Its fundamental loop is:

\[
\boxed{
\text{Persist}
\rightarrow
\text{Wake}
\rightarrow
\text{Perceive}
\rightarrow
\text{Compute}
\rightarrow
\text{Integrate}
\rightarrow
\text{Commit}
\rightarrow
\text{Sleep}
\rightarrow
\text{Wake again}
}
\]

The neural model is a participant in that loop.

The loop itself is the runtime.

The durable chain produced by the loop allows Physalia to remain architecturally continuous when:

- the user leaves;
- the network disappears;
- a model unloads;
- KV state is lost;
- the machine restarts;
- one cognitive substrate is replaced by another.

That is the purpose of `cogg.cpp`.

---

# Appendix A — Suggested public C++ API

```cpp
namespace cogg {

class runtime {
public:
    explicit runtime(runtime_config);

    subject_id create_subject(subject_config);

    void start();
    void stop();

    void submit(subject_id, occasion);

    subject_snapshot snapshot(subject_id) const;
};

class subject_handle {
public:
    subject_id id() const;
    tick current_tick() const;

    void message(std::string_view text);
    void wake();
    void suspend();
    void resume();
};

class substrate_backend {
public:
    virtual ~substrate_backend() = default;

    virtual backend_caps capabilities() const = 0;

    virtual context_handle create_context(
        const context_spec&) = 0;

    virtual emission infer(
        context_handle,
        const inference_request&) = 0;

    virtual checkpoint_blob checkpoint(
        context_handle) = 0;

    virtual restore_result restore(
        context_handle,
        const checkpoint_blob&) = 0;
};

}
```

---

# Appendix B — First implementation task list

```text
[ ] repo + CMake
[ ] libllama submodule
[ ] UUID/hash utility
[ ] SQLite WAL store
[ ] subjects/events/commits tables

[ ] subject state machine
[ ] logical tick counter
[ ] atomic commit

[ ] scheduler
[ ] wake implementation
[ ] internal wake limits

[ ] mock substrate
[ ] transition JSON schema
[ ] null transition

[ ] llama adapter
[ ] persistent llama_context
[ ] token history
[ ] grammar-constrained transition output

[ ] context checkpoint wrapper
[ ] cold rebuild fallback

[ ] daemon
[ ] CLI
[ ] timeline inspector

[ ] crash test
[ ] double-commit test
[ ] wake-abuse test
[ ] KV-loss test
[ ] restart test

[ ] 48-hour endogenous tick experiment
```

---

# Appendix C — Research questions

1. Does persistent KV materially improve internal cognitive continuity over semantic reconstitution?
2. Does model-selected wake timing correlate with unresolved internal state?
3. How often does endogenous waking converge to pathological high-frequency loops?
4. Can subject continuity remain recognizable after total substrate replacement?
5. How much working state must survive cold reconstitution?
6. Can different substrate contexts develop useful long-lived specialization?
7. Does explicitly recording restart/discontinuity improve self-model consistency?
8. Which state should survive compaction?
9. Can meaningful null selection be distinguished from inference failure?
10. Does local-first continuation differ measurably from externally scheduled API agents?
11. When should sleeping contexts remain GPU-resident?
12. Can a learned cognitive scheduler outperform fixed wake intervals?
13. How should multimodal perceptual state survive sleep?
14. Can substrate replacement behave like a controlled cognitive lesion rather than a persona reset?
15. What minimum invariants are required before claims of synthetic continuity become experimentally meaningful?

---

# Appendix D — Current llama.cpp implementation note

At the time of writing, current `llama.cpp` infrastructure already provides several useful primitives: continuous batching, slot/context caching, sequence-level state serialization, explicit KV-cache state read/write logic, multimodal support, and adapter mechanisms.

Those features make `libllama` a strong first inference substrate for Cogg.

However, backend state persistence must remain non-canonical. Current `llama.cpp` development still changes rapidly, and cross-restart slot-state restoration has had reported failures in recent builds.

Therefore Cogg MUST always support semantic cold reconstitution from durable subject state.

---

# Appendix E — One sentence specification

> **`cogg.cpp` is a persistent cognitive runtime in which neural inference is a temporary operation performed by a continuing subject, rather than the subject being a temporary artifact of an inference request.**
