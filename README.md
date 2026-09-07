# cogg.cpp

A local C++ runtime for persistent subject state, committed transitions, and
bounded autonomous waking. Part of the Physalia Gyre research program.

**Status: experimental Phase 3, version 0.4.0; 48-hour observation pending.** An optional pinned libllama backend
runs local GGUF models through the durable transition loop, with compatible
disk KV checkpoints and cold reconstruction. The default build retains the
deterministic demo without an inference dependency.

cogg.cpp is developed as an open foundation for other builders and as a runtime
component for Physalia Gyre. It reuses established inference and storage libraries
while owning the relationship between subject state, computation and continuation.

## What runs today

- A durable subject with a monotonic logical tick and SHA-256 commit chain.
- An idempotent inbox that survives process exit.
- Atomic commits of memory, input consumption, subject head and next wake.
- A persisted admission budget, including failed and interrupted attempts.
- A backend proposal boundary: only the runtime advances the subject tick.
- Restart verification and replay of committed memory and wake state.
- A CLI loop which can continue without user messages while a wake is scheduled.
- Local grammar-constrained generation with token/context limits and CPU cancellation.
- A resident model and reusable KV prefix, rebuilt from committed memory after eviction or restart.
- Optional atomic disk checkpoints, tied to the accepted commit and exact backend configuration.
- Explicit subject-clock windows and replayable requested/granted wake decisions.
- A bounded observation harness separating model timing choices from runtime limits.

The persistence boundary is a completed SQLite transaction. Uncommitted inference
can be retried after a crash and can produce a different proposal. Hashes detect
inconsistent records; this prototype does not sign subject commits or authenticate
history against a database owner capable of rewriting the entire chain.

## Build

Dependencies: C++20 compiler, CMake 3.20+, SQLite3, OpenSSL libcrypto and
nlohmann/json 3.10+. The runtime needs no Python interpreter or model service.

Debian/Ubuntu development dependencies:

```sh
sudo apt-get install g++ cmake ninja-build libsqlite3-dev libssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

An optional build container is supplied in `Dockerfile.build`; Docker is not a
runtime dependency. This revision is tested on Linux/GCC 12. Other platforms are
not yet validated. Use `-DCOGG_SANITIZE=ON` for ASan and UBSan.

## Try the runtime

Use a new database path. The low limits below are for a short deterministic demo:

```sh
# Minimum admission spacing 100 ms, at most 100 attempts per 60 seconds.
./build/cogg-cli init demo.db explorer 100 100 60000
./build/cogg-cli send demo.db explorer greeting-1 "Hello"
# Perform five new commits, requesting state-dependent demo wake intervals.
./build/cogg-cli run demo.db explorer 5 100
./build/cogg-cli inspect demo.db explorer
./build/cogg-cli verify demo.db explorer
# Same subject, more commits after opening a new process.
./build/cogg-cli run demo.db explorer 3 100
```

`run ... 0 100` runs until SIGINT/SIGTERM. `idle-base-ms=0` requests no further
autonomous wake. `steps` counts successful new commits, so a finite run waits if
there is no occasion or its admission budget is exhausted. `send` works from a
second process and reusing the same key/content does not duplicate the occasion.

The demo increments committed memory and varies its requested delay from that
counter. This demonstrates the scheduling mechanism, not useful autonomous
reasoning or subjective time. Speech is recorded in the commit proposal and is
visible through `inspect`; no external tools execute.

## Design and research

- [Temporal contract and 48-hour observation protocol](docs/PHASE-3.md)
- [Durable checkpoint usage and Phase 2 contract](docs/PHASE-2.md)
- [Local model build, usage and Phase 1 contract](docs/PHASE-1.md)
- [Durable kernel contract](docs/PHASE-0.md)
- [Prior art and language decision, 2026-09-07](docs/RESEARCH-2026-09-07.md)
- [Original long-term architecture](ARCHITECTURE.md)

AIOS, Agent-libOS, OpenFang, Letta, durable execution systems and inference-cache
projects already address substantial parts of this problem. cogg.cpp does not
claim to be the first agent OS. Its working focus is an embeddable transition
kernel with an explicit, testable relationship between persistent state, model
contexts and the authority to commit a successor.

The runtime uses libllama for inference and preserves the component's ownership
of durable transitions. Alternative platforms remain research references.
Cryptographic subject
signatures, tool-effect settlement, compaction and multi-substrate integration
remain future work. The original roadmap includes experiments needed to evaluate
whether these mechanisms improve cognitive continuity.

## License

MIT; see [LICENSE](LICENSE). Dependencies retain their own licenses.
