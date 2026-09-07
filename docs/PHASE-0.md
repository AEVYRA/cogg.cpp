# Phase 0 contract

Direction update, 2026-09-07: standalone runtime expansion is paused in favor of
reusing an existing foundation for Physalia Gyre. This implementation remains an
experiment; its successful tests do not establish a need for a new platform.

This document describes executable behavior in 0.1.0. `ARCHITECTURE.md` describes
the larger research program. The public C++ API is experimental.

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

The durable lifecycle describes the next waiting mode. Live execution activity
is represented by reservations, not by pretending `deliberating` survives a dead
process. CLI polling uses a short sleep and does not write on every idle poll.

## Recovery and evidence

`Runtime` verifies a subject before its first admission. The verifier checks
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

The in-process backend interface is a programming boundary, not an OS sandbox
against malicious native code. There are no model calls, token/VRAM/energy
quotas, inference timeouts, KV checkpoints, subject signatures or key rotation
yet. The tested replacement is a backend identity change above stored state;
it does not establish behavioral continuity between real models.

Memory is bounded JSON key assignment: 64 writes/proposal, 64 KiB/value, 1 MiB
total subject projection. Each commit stores a full projection, so disk usage
grows; this release has no archival/compaction policy or ingress disk quota.
Null JSON is a stored value, not deletion. Belief/relation/episodic semantics,
arbitrary DAG merges and multi-organ integration remain later phases. The
current causal structure links occasions to their observed subject head and
commits to the accepted occasion and reserved attempt.

## Validation

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
quality benchmark. CI repeats the Linux checks; CI itself has not run remotely.
