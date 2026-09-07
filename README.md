# cogg.cpp

A local C++ runtime for persistent subject state, committed transitions, and
bounded autonomous waking. Part of the Physalia Gyre research program.

**Status: experimental Phase 1, version 0.2.0.** An optional pinned libllama backend
runs local GGUF models through the durable transition loop. The default build
retains the deterministic demo without an inference dependency.

**Scope clarification, 2026-09-07:** cogg.cpp remains a proposed open foundation
for other builders as well as a possible runtime for Physalia Gyre. Sofia's
previous announcement that standalone development should be abandoned in favor
of another runtime was an overcorrection, not Sasha's decision. Existing solutions
inform the architecture; they do not settle whether to build, extend or reuse a
platform. Phase 0 remains implemented and available.

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

- [Local model build, usage and Phase 1 contract](docs/PHASE-1.md)
- [Durable kernel contract](docs/PHASE-0.md)
- [Prior art and language decision, 2026-09-07](docs/RESEARCH-2026-09-07.md)
- [Original long-term architecture](ARCHITECTURE.md)

AIOS, Agent-libOS, OpenFang, Letta, durable execution systems and inference-cache
projects already address substantial parts of this problem. cogg.cpp does not
claim to be the first agent OS. Its working focus is an embeddable transition
kernel with an explicit, testable relationship between persistent state, model
contexts and the authority to commit a successor.

After reviewing the whole Physalia architecture, Sasha authorized continued
cogg.cpp development. This increment uses libllama for inference and preserves
the component's ownership of durable transitions. Alternative platforms remain
research references. KV checkpoint envelopes, cryptographic subject
signatures, tool-effect settlement, compaction and multi-substrate integration
remain future work. The original roadmap includes experiments needed to evaluate
whether these mechanisms improve cognitive continuity.

## License

MIT; see [LICENSE](LICENSE). Dependencies retain their own licenses.
