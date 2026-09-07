# cogg.cpp

A local C++ runtime for persistent subject state, committed transitions, and
bounded autonomous waking. Part of the Physalia Gyre research program.

**Status: experimental Phase 0, version 0.1.0.** The executable uses a deterministic
demo backend. Local model inference is not implemented.

**Direction revised after Sasha's review, 2026-09-07:** further standalone kernel
development is paused. The priority is to build Physalia Gyre on an existing
runtime and implement only its missing domain mechanisms. Phase 0 is retained
as an experiment and source of invariant tests, not a decision to maintain a
second general-purpose agent runtime. See the [reuse decision](docs/RESEARCH-2026-09-07.md#reuse-decision-after-sashas-review).

## What runs today

- A durable subject with a monotonic logical tick and SHA-256 commit chain.
- An idempotent inbox that survives process exit.
- Atomic commits of memory, input consumption, subject head and next wake.
- A persisted admission budget, including failed and interrupted attempts.
- A backend proposal boundary: only the runtime advances the subject tick.
- Restart verification and replay of committed memory and wake state.
- A CLI loop which can continue without user messages while a wake is scheduled.

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

- [Current implementation contract](docs/PHASE-0.md)
- [Prior art and language decision, 2026-09-07](docs/RESEARCH-2026-09-07.md)
- [Original long-term architecture](ARCHITECTURE.md)

AIOS, Agent-libOS, OpenFang, Letta, durable execution systems and inference-cache
projects already address substantial parts of this problem. cogg.cpp does not
claim to be the first agent OS. Its working focus is an embeddable transition
kernel with an explicit, testable relationship between persistent state, model
contexts and the authority to commit a successor.

Next: evaluate a minimal Physalia integration on Agent-libOS's host/module and
transaction boundaries, with existing inference infrastructure. A dedicated
libllama integration in cogg.cpp is deferred until a concrete unmet requirement
justifies it. KV checkpoint envelopes, cryptographic subject
signatures, tool-effect settlement, compaction and multi-substrate integration
remain future work. The original roadmap includes experiments needed to evaluate
whether these mechanisms improve cognitive continuity.

## License

MIT; see [LICENSE](LICENSE). Dependencies retain their own licenses.
