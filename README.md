# cogg.cpp

A lightweight, durable C++ runtime for autonomous AI agents with persistent state, deterministic event loops, and bounded autonomous waking.

`cogg.cpp` provides a low-level engine where an LLM's subjective time and memory are cryptographically coupled to a host SQLite database. It is designed as a foundational transition kernel that gives local models a continuous, crash-resilient existence tied to real host resources. This implements logical commit coordinates, not evidence of a human-like subjective experience.

This project originated as the runtime component for the **Physalia Gyre** research program, but it is built as an open, embeddable foundation for any developer creating durable AI systems.

If the host process is interrupted mid-generation, `cogg.cpp` can recover the last committed state, replay pending inbox events, restore a compatible KV cache or reconstruct the context, and allow the model to try again, leaving a verifiable cryptographic trail.

## Core Concepts

* **Durable Subject:** Every agent (subject) has a monotonic logical clock and a SHA-256 commit chain. Memory, state, and inbox consumption are saved in atomic SQLite transactions.
* **Autonomous Scheduling (Endogenous Time):** The model can request its next wake-up time. The kernel enforces admission budgets, minimum/maximum sleep intervals, and quotas, decoupling raw CPU time from the agent's subjective continuation.
* **Crash-Resilient Inference:** Powered by an embedded `llama.cpp` backend. KV caches are persisted to disk and tied to specific commit hashes, enabling seamless warm/cold process restoration after a crash or eviction.
* **Bounded Working Memory:** Typed deposits remain in durable history while the model receives a selected context. Open tasks, source-linked summaries and version-aware retrieval are checked by the kernel.
* **Explicit Non-participation:** An executor can abstain without changing subject state. The host can supply source-linked evidence for another attempt at the same request.
* **Optional Self-State:** A host policy preserves an application-defined profile and explicit commitments across model replacement, checking authorization before they change.
* **Idempotent Inbox:** External events and messages queue safely and survive process exits.

## Relationship with llama.cpp

`cogg.cpp` uses the excellent [llama.cpp](https://github.com/ggerganov/llama.cpp) (`libllama`) for all tensor operations and local GGUF model inference. 

What `cogg.cpp` adds architecturally on top of this inference engine is the **durable transition kernel**:
* **State & Time:** While `llama.cpp` computes the next tokens, `cogg.cpp` manages the agent's lifecycle. It decides *when* the model is allowed to run, tracking its subjective time, and enforcing autonomous scheduling and quotas.
* **Persistence:** `cogg.cpp` reserves attempts and commits accepted results in SQLite transactions; inference runs outside the write lock. It saves the model's memory, inbox events, and wake decisions to a cryptographically hashed commit chain.
* **KV Cache Lifecycle:** `cogg.cpp` manages the persistence of `llama.cpp`'s KV cache to disk, tying specific cache blobs to exact commit hashes. A restarted process restores a compatible cache and reuses the matching canonical prefix. Missing, stale or incompatible caches trigger reconstruction from committed state. Interrupted generation may be repeated and may produce different text; checkpoint publication is separate from the SQLite commit.

## Status

**Current Version: 0.9.0 (Experimental host / TUI; Phase 7 kernel)**

The optional [terminal interface](docs/TUI.md) adds an independent `cogg-host` and
`cogg-tui`: durable conversation, state, timeline, memory, commit inspection and
explicit admission controls. Closing the terminal leaves the host running.
Build with `-DCOGG_TUI=ON`; HTTP/Ollama and self-state remain separate options.

The [0.8.1 repairs](docs/REPAIRS-2026-09-08.md) preserve routing after open-task capacity failures and report self-runtime transport, timeout and cancellation outcomes accurately.

- [x] **Phase 0:** Durable kernel contract
- [x] **Phase 1:** Local model build and inference (libllama)
- [x] **Phase 2:** Durable checkpoint usage (KV cache)
- [x] **Phase 3 mechanisms:** Explicit commit time and bounded model-requested scheduling
- [ ] **Phase 3 validation:** Real 48-hour observation and state-dependent waking assessment; see [protocol](docs/PHASE-3.md)
- [x] **Phase 4 mechanisms:** Typed deposits, bounded working context, retrieval and source-preserving compaction; [contract and evidence](docs/PHASE-4.md)
- [x] **Phase 5:** Heterogeneous executors, isolated sessions, bounded routing and admission-bound emissions
- [x] **Phase 6 mechanisms:** Typed abstention and bounded provenance inputs; [composition boundary](docs/PHASE-6.md)
- [ ] **Phase 6 model quality:** Reliable evidence-aware participation across models; see [validation](docs/phase6-validation-20260908.json)
- [x] **Phase 7 mechanisms:** Optional durable self-state and guarded commitments; [contract and model-switch example](docs/PHASE-7.md)
- [ ] **Full Crystal integration:** Semantic self-model, drives and cognitive evaluation remain research work
- [ ] **Phase 8:** Multimodal perception

Subject signatures remain a planned cross-cutting capability. The detailed research roadmap is in [ARCHITECTURE.md](ARCHITECTURE.md#60-development-phases).

For local GPU and API routing, see [Phase 5](docs/PHASE-5.md) and [the executor configuration](configs/phase5.example.json). Build with `-DCOGG_HTTP=ON` to enable Ollama/Chat Completions. [Infrastructure probes](docs/INFRASTRUCTURE.md) remain separate transport checks. Mac deployment is postponed.

## Build Instructions

**Dependencies:** C++20 compiler, CMake 3.20+, SQLite3 with FTS5, OpenSSL (`libcrypto`), and `nlohmann/json` 3.10+.
The core runtime requires neither Python nor model services. Optional HTTP adapters require libcurl 7.85+ (`libcurl4-openssl-dev`); their offline tests use Python 3.

**Debian/Ubuntu:**
```sh
sudo apt-get install g++ cmake ninja-build libsqlite3-dev libssl-dev nlohmann-json3-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```
*Use `-DCOGG_SANITIZE=ON` for ASan and UBSan. A Dockerfile (`Dockerfile.build`) is also provided.*

## Quick Start: The CLI Demo

You can run the deterministic CLI demo using the compiled binary. The parameters define the agent's time limits (e.g., min wake 100ms, max 100 attempts per 60s).

```sh
# 1. Initialize a new agent database 
./build/cogg-cli init demo.db explorer 100 100 60000

# 2. Send an external event to the agent's inbox
./build/cogg-cli send demo.db explorer greeting-1 "Hello, world!"

# 3. Run the agent loop for 5 commits (or until budget exhausted)
./build/cogg-cli run demo.db explorer 5 100

# 4. Inspect the resulting cryptographic commit chain
./build/cogg-cli inspect demo.db explorer
./build/cogg-cli verify demo.db explorer
```

## Architecture & API

The project is designed to be embeddable. The core C++ API (see `include/cogg/kernel.hpp`) separates storage, inference and coordination:
* `cogg::Store`: Manages SQLite transactions, admission quotas, and cryptographic chain verification.
* `cogg::Backend`: The abstract interface for model inference, with native libllama and optional HTTP implementations.
* `cogg::Runtime`: Steps the subject forward with one backend.
* `cogg::Registry` and `cogg::RoutedRuntime`: Own executor sessions and apply explicit routes while retaining one subject history.
* Optional `cogg::SelfRuntime` (`-DCOGG_SELF=ON`): Applies host-issued self-state permissions before committing a model proposal; uses the existing database and memory.

For deeper technical dives and research notes, see the documentation in the `docs/` folder.

## License

MIT License. See [LICENSE](LICENSE) for details. Dependencies retain their own licenses.
