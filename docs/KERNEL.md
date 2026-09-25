# Durable kernel

The trusted host owns storage and admission. A backend proposes a transition;
the kernel validates and commits it. The public C++ API remains experimental.
The contract originated in v0.1.0 and is extended by [Scheduling](SCHEDULING.md),
[Memory](MEMORY.md) and [Composition](COMPOSITION.md). See
[Local models](LOCAL_MODELS.md) for native inference and
[Executors](EXECUTORS.md) for HTTP routing.

## Ownership and transition

`Store` belongs to the trusted host. `Backend::propose` receives a value projection
of committed state and an occasion. A `Proposal` contains a transition kind,
optional speech, JSON memory assignments and an optional wake interval. It has
no tick/head/key/storage authority. `parse_proposal` rejects unknown fields,
unsupported actions, fractional/negative/excessive wake delays and duplicate keys.

A subject starts at tick 0 with a hashed genesis including immutable runtime
limits and a random nonce. Creation also queues a durable `created` occasion.
Messages are persisted before they become available for computation. Keys are
namespaced and idempotent per subject; conflicting key reuse fails.

`admit` opens a short SQLite `BEGIN IMMEDIATE` transaction, chooses a pending
occasion or materializes a due wake, checks the persisted budget and reserves an
attempt. Inference occurs outside the write transaction. Reservation alone never
advances subject time. Multiple host processes may compute from one head; their
reservations all count, and exactly one valid successor can commit.

`commit` validates the reservation and expected head, then writes these together:

1. Versioned, hashed successor record and its memory projection.
2. Subject head/tick and granted wake deadline.
3. Occasion consumption and attempt settlement.

SQLite WAL with `synchronous=FULL` supplies the durability boundary. There is no
separate file fsync protocol pretending to form an atomic cross-file commit.
Storage must honor SQLite's filesystem and sync assumptions; use local storage.

## Time and scheduling

- Subject tick advances once per completed commit, including null transitions.
- Wall milliseconds make deadlines portable across process restarts.
- Admission/commit accounting clamps backward wall movement to the last recorded
  accounting time. Clock rollback cannot refund attempts; a forward host-clock
  jump can expire a window. This is a local operational clock, not Tyveth or a
  proof of subjective time.
- The scheduler honors requested delay subject to the minimum interval. A null
  wake request enters `waiting_external`; a deadline enters `sleeping`.
- Per-subject limits count **all admitted attempts**, including failed,
  interrupted and superseded ones. Minimum spacing applies to external messages
  as well as internal wakes in this first version.
- The half-open rolling window is `(now-period, now]`. Its boundary and the
  independent spacing floor are tested. Quota denial leaves the occasion queued.
- A scheduled occasion belongs to the head that requested it. Obsolete queued
  timers are retained as evidence and cannot drive a newer head.

### Undeliverable occasions

The inbox is ordered: the oldest pending occasion is admitted first. A failed
attempt leaves its occasion pending, so an occasion that no executor can handle
used to be retried until the attempt quota was exhausted, and every later
message waited behind it.

`Store::fail(attempt, reason, scope, now)` settles a failure with a scope.
`transient` failures (transport, timeout, credentials, cancellation, conflicts
and memory pressure) never count against the occasion: an outage or a full
memory must not discard a healthy message. `occasion` failures (invalid output,
a generic backend failure, a proposal the kernel or a host policy rejects) are
recorded in `occasion_failures`. When an occasion reaches
`Limits::max_occasion_failures` (default 3 for new subjects; 0 disables), the
kernel commits a host-authored null transition in the same transaction:

- `proposal.kind` is `null` with no text, memory or notes; `emission` is null;
- a `disposition` record (`cogg:disposition/v1`) lists exactly the counted
  failed attempts and the limit. Failure reasons stay in the attempts table;
  provider messages never enter the hashed history;
- a pending future wake is preserved, except when the disposed occasion is
  itself the scheduled wake or a task-maintenance admission;
- the occasion is consumed, so the next message is admitted.

The disposition is part of the subject's history, not a silent drop, and
`request_trace` returns it for the original idempotency key. The verifier
checks every listed failure against its attempt and occasion. The provided
runtimes classify failures this way; `Runtime::last_disposition()`,
`RouteResult::status == "disposed"` and `SelfResult::status == "disposed"`
report a settlement. The two-argument `fail` remains a transient failure.
Subjects created before this field existed have no limit and keep their genesis
bytes. A history containing a routed disposition needs a verifier from this
revision or later.

The durable lifecycle describes the next waiting mode. Live execution activity
is represented by reservations, not by pretending `deliberating` survives a dead
process. CLI polling uses a short sleep and does not write on every idle poll.

## Recovery and evidence

`Runtime` and `RoutedRuntime` verify a subject before their first admission for
it; later steps rely on the head-checked commit path. Call `Store::verify` for a
full forensic check. The verifier checks
SQLite integrity/FKs, record hashes, chain parents/ticks, occasion/attempt refs,
and replayed memory and wake state against the persisted subject projection.
It detects inconsistent records; it does not authenticate a chain rewritten by
an actor with direct database access. Copying a database while it is live is
not a backup protocol; use SQLite backup or stop all writers before copying it.

An unsettled reservation remains visible after a crash. The next projection says
`prior_unsettled_attempt`; this may also reflect a concurrent worker and does not
assert that the earlier computation failed. Retried inference is at-least-once;
occasion consumption and successor commitment are atomic. This is not an
exactly-once external-tool guarantee. No external effects are implemented.

Hashes use SHA-256 over this schema's compact nlohmann JSON serialization,
including sorted object keys. This is a versioned internal encoding, not a claim
of RFC 8785 or cross-serializer canonicalization. Genesis nonces distinguish
independent creations using the same display identifier.

## Scope and limits

The in-process backend interface is a programming boundary, not an OS sandbox.
Core admission counts attempts and constrains waking. Native adapters add
cooperative inference deadlines and context/output limits; these do not establish
complete RAM, VRAM, energy or disk quotas. Subject signatures and key rotation
remain unimplemented. Hash-chain verification detects inconsistent records but
cannot authenticate a database fully rewritten by a privileged host.

Legacy key/value memory permits 64 writes/proposal, 64 KiB/value and a 1 MiB
subject projection. Null JSON is a stored value, not deletion. [Typed deposits](MEMORY.md)
add bounded working context and source-linked compaction without deleting the
original durable history. Full projections and history verification have growing
storage/replay costs; no history archival or ingress disk quota is supplied.
Arbitrary DAG merges and semantic claim integration are outside this kernel.

## Historical validation — v0.1.0

Linux, GCC 12.2, SQLite 3.40.1, OpenSSL 3.0.20, nlohmann/json 3.11.2:

- Twelve kernel scenarios, including 2,000 durable transitions.
- Two-connection simultaneous successor race: one winner.
- Failure accounting, restart, rollback clock and quota-window boundaries.
- Durable inbox, conflicting duplicate input, backend replacement and null ticks.
- Rejected proposals and transactional rollback leave no partial memory/head/wake.
- History/projection corruption and recovery verification before backend invocation.
- Two POSIX subprocess cases: SIGKILL immediately before and after SQL commit,
  followed by reopening, verifying and continuing the same subject.
- Both CTest suites passed in Debug and with AddressSanitizer + UBSan.

These are process-crash tests, not a hardware power-loss campaign or an inference
quality benchmark. Current remote CI and reproduction commands are linked from
[Validation](VALIDATION.md).
