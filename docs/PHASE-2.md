# Phase 2: durable context checkpoints

Version 0.3.0 adds optional KV checkpoints to the local libllama backend.
The durable subject ledger remains authoritative. A checkpoint accelerates
reconstruction and cannot advance or roll back the subject's committed state.

## Use

Build with `-DCOGG_LLAMA=ON` as in [Phase 1](PHASE-1.md), then:

```sh
mkdir -m 700 checkpoints
./build-llama/cogg-cli init subject.db explorer 1000 20 3600000
./build-llama/cogg-cli run-model subject.db explorer model.gguf \
  --checkpoint-dir checkpoints --once
./build-llama/cogg-cli send subject.db explorer hello-1 "Hello."
./build-llama/cogg-cli run-model subject.db explorer model.gguf \
  --checkpoint-dir checkpoints --once
```

The directory must already exist. Omitting `--checkpoint-dir` preserves the
Phase 1 behavior without disk KV. `--checkpoint-mib N` sets a 1..4096 MiB blob
limit (default 512 MiB); the metadata limit is independently fixed at 1 MiB.
Library callers use `LlamaOptions::checkpoint_directory/max_checkpoint_bytes`.
`--release-context` now lets the next occasion restore from disk while keeping
weights loaded. Restarting the process loads weights and can restore the same KV.

Each accepted CLI result includes `checkpoint.read` (`disabled`, `resident`,
`missing`, `restored`, or `rejected`), `checkpoint.write` (`disabled`,
`not_attempted`, `saved`, or `failed`), a rejection detail, and
`maintenance_error`. A successful subject commit remains a successful result
even if its checkpoint could not be written. Inspect this separate error before
assuming future wakes will benefit from persisted KV.

## Commit first, cache second

1. Runtime admits an occasion and reserves the attempt under the existing budget.
2. The model generates a proposal outside the SQLite write transaction.
3. Store atomically commits the proposal, memory, head/tick and next wake.
4. Only then does Runtime call `Backend::committed(previous_present, successor)`.
5. The llama backend serializes its evaluated sequence and publishes one file
   anchored to that successor's subject, head and tick.

Failed inference and superseded proposals never invoke the commit callback.
Callback errors populate `Runtime::maintenance_error()` and cannot mark the
already committed attempt failed, consume its input again, or hide its successor.
Direct callers of Store may explicitly notify the backend after a valid commit;
Store alone has no backend reference and therefore does not save a checkpoint.

There is deliberately no cross-file transaction joining SQLite and KV. An
interruption after SQL COMMIT but before cache publication leaves an old or
missing cache. The next invocation validates against the current head and
reconstructs cold when necessary. A commit at tick N+1 can outlive a cache at N.
This is a normal recovery path, not a partially committed subject.

## Envelope and compatibility

One `<SHA256(subject-name)>.coggkv` file per subject contains:

| Field | Purpose |
|---|---|
| `COGGKV` magic + format byte | Identify the outer encoding |
| Two little-endian uint64 lengths | Bound metadata/blob reads before allocation |
| SHA-256 of metadata and of blob | Detect incomplete or corrupted bytes |
| `cogg-checkpoint/v1` metadata | Subject, accepted head/tick, source head and evaluated tokens |
| Compatibility descriptor | Model/config identity, adapter build hash, pinned llama revision, system features, pointer width, byte order, effective context size |
| Opaque sequence blob | `llama_state_seq_get_data`, sequence 0, normal host-memory format |

The metadata checksum covers the token vector as well as its head and backend
binding. The backend identity includes the GGUF SHA-256 and Phase 1 inference
configuration. The build hash includes adapter source, compiler and configured
build flags/mode; native feature information comes from the linked backend.
Exact descriptor equality is required. Even a harmless change to timeout,
compiler, adapter source or build mode can intentionally force a cold start.
The caller-supplied offline llama source override must still match the pinned
revision, as required by Phase 1.

The serialized sequence represents evaluated tokens of the accepted computation;
the final sampled closing token may not have been evaluated. It is not a
serialized live sampler or an exact mid-generation continuation. At the next
occasion the canonical prompt is rebuilt from the verified subject. Only its
exact token prefix can be reused; divergent generated suffixes are removed.
No rejected thought becomes memory through the cache. No logits/sampler state
are required because the final retained prompt token is evaluated again.

The restore path checks envelope integrity, exact subject/head/tick, compatibility,
token count and vocabulary range before native deserialization. It also checks
the restored sequence position against the saved token count. A native failure
destroys the context before cold reconstruction, because failed deserialization
need not leave native memory unchanged. Allocation or deadline failure can still
fail the inference normally under its admission budget.

## Publication and crash boundaries

The POSIX writer creates a unique mode-0600 temporary file in the same directory,
writes header plus blob, fsyncs the file, renames it over the previous checkpoint,
then fsyncs the directory. Readers use one open descriptor, reject non-regular
files and symlinks, and verify exact length and both checksums. They see one
complete version while other processes replace the published path.

Process death before rename leaves the previous complete file; death after
rename exposes the new complete file. A power failure before directory fsync
can still lose the rename. Either outcome is tolerated by cold reconstruction
from SQLite. Filesystem/SQLite sync guarantees and trustworthy local storage are
assumptions, not a hardware power-loss proof.

Concurrent writers use different temporary names. A delayed older writer may
replace a newer cache; head validation will reject that stale version. This may
waste computation but cannot roll back the ledger. Abandoned `.tmp.*` files from
SIGKILL are ignored. They are not automatically deleted because another writer
may still own one; remove leftovers only when writers are stopped.

## Limits

- Trusted local cache only. Checksums detect corruption, not an owner who can
  rewrite both payload and checksums. Native state files are not a safe interchange
  format for arbitrary third-party input. Subject records remain hashed, not signed.
- CPU/POSIX implementation, with no new GPU, model-unloading, tool-effect or
  multi-substrate guarantees. Unsupported platforms report checkpoint failure
  separately; the durable kernel can continue without KV.
- Snapshot limits bound the blob and metadata, not total model/context RAM,
  aggregate disk use across subjects or crash-leftover temporary files.
- Saving/loading is synchronous and can delay the caller. No inference occurs
  during save. Cache maintenance does not create a subject tick; its elapsed time
  may mean an already committed wake is due when saving finishes.
- Durable KV does not promise identical hot/warm/cold generated text or useful
  cognition. Behavioral continuity remains a separate research question.

## Validation

`checkpoint` CTest exercises checksums, truncation, oversized and trailing data,
atomic replacement, cleanup after a handled publication error, concurrent
readers/writers, and four real SIGKILL boundaries. Those crash cases run a
deterministic backend through Runtime so they check that committed memory and
wake survive interruption of its checkpoint callback.

`llama-checkpoint` uses the real pinned Qwen GGUF to test native serialization
and fresh-backend restore, missing/corrupt/stale/incompatible files, invalid
token metadata, rejected native state, cancelled inference without publication,
and a cache-size failure after a successful subject commit. The original
inference and kernel suites remain enabled.

`python3 tests/model_crash_test.py build/cogg-cli model.gguf` additionally kills
an admitted real model process at tick 0, recovers tick 1, restores its checkpoint
in a new process at tick 2, corrupts it and reconstructs at tick 3, then deletes
it and reconstructs at tick 4. Every stage verifies the same durable chain.
Changing the context limit in a further process rejects the incompatible cache
and commits tick 5 with a newly compatible checkpoint.

Local validation on 2026-09-07: all six suites passed in Release (176.97 s)
and with ASan/UBSan (189.13 s); the three standalone core suites passed without
libllama (13.28 s). Sanitizers instrumented cogg, its adapter and tests; the
external pinned libllama remained a Release build. The separate real-model
process test also passed through tick 5. These checks used the pinned Qwen
fixture and the older-i5 CPU flags documented in Phase 1.

Phase 3 temporal mechanisms and the observation protocol are now described in [PHASE-3.md](PHASE-3.md). Durable context is now
an execution mechanism; the scheduling, temporal provenance and cognitive
semantics still need the separate architectural work described in the roadmap.
