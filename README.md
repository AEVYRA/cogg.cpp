# cogg.cpp

A lightweight, durable C++ runtime for autonomous AI agents with persistent state, deterministic event loops, and bounded autonomous waking.

`cogg.cpp` provides a low-level engine where an LLM's subjective time and memory are cryptographically coupled to a host SQLite database. It is designed as a foundational transition kernel that gives local models a continuous, crash-resilient existence tied to real host resources.

This project originated as the runtime component for the **Physalia Gyre** research program, but it is built as an open, embeddable foundation for any developer creating durable AI systems.

If the host process is interrupted mid-generation, `cogg.cpp` can resume the exact state, replay the inbox, load the KV cache from disk, and allow the model to safely try again, leaving a verifiable cryptographic trail.

## Core Concepts

* **Durable Subject:** Every agent (subject) has a monotonic logical clock and a SHA-256 commit chain. Memory, state, and inbox consumption are saved in atomic SQLite transactions.
* **Autonomous Scheduling (Endogenous Time):** The model can request its next wake-up time. The kernel enforces admission budgets, minimum/maximum sleep intervals, and quotas, decoupling raw CPU time from the agent's subjective continuation.
* **Crash-Resilient Inference:** Powered by an embedded `llama.cpp` backend. KV caches are persisted to disk and tied to specific commit hashes, enabling seamless warm/cold process restoration after a crash or eviction.
* **Idempotent Inbox:** External events and messages queue safely and survive process exits.

## Relationship with llama.cpp

`cogg.cpp` uses the excellent [llama.cpp](https://github.com/ggerganov/llama.cpp) (`libllama`) for all tensor operations and local GGUF model inference. 

What `cogg.cpp` adds architecturally on top of this inference engine is the **durable transition kernel**:
* **State & Time:** While `llama.cpp` computes the next tokens, `cogg.cpp` manages the agent's lifecycle. It decides *when* the model is allowed to run, tracking its subjective time, and enforcing autonomous scheduling and quotas.
* **Persistence:** `cogg.cpp` wraps the inference in SQLite transactions. It saves the model's memory, inbox events, and wake decisions to a cryptographically hashed commit chain.
* **KV Cache Lifecycle:** `cogg.cpp` manages the persistence of `llama.cpp`'s KV cache to disk, tying specific cache blobs to exact commit hashes. This allows the host process to be completely killed and later perfectly restored to the exact context state without recalculating prompts.

## Status

**Current Version: 0.4.0 (Experimental Phase 3)**

- [x] **Phase 0:** Durable kernel contract
- [x] **Phase 1:** Local model build and inference (libllama)
- [x] **Phase 2:** Durable checkpoint usage (KV cache)
- [x] **Phase 3:** Endogenous time and autonomous scheduling
- [ ] **Phase 4:** Memory retrieval and compaction
- [ ] **Phase 5:** Cryptographic subject signatures

## Build Instructions

**Dependencies:** C++20 compiler, CMake 3.20+, SQLite3, OpenSSL (`libcrypto`), and `nlohmann/json` 3.10+.  
*Note: The runtime does not require a Python interpreter or any external model services.*

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

The project is designed to be embeddable. The core C++ API (see `include/cogg/kernel.hpp`) is separated into three main abstractions:
* `cogg::Store`: Manages SQLite transactions, admission quotas, and cryptographic chain verification.
* `cogg::Backend`: The abstract interface for model inference (currently implemented by `llama_backend.cpp`).
* `cogg::Runtime`: The orchestrator that steps the subject forward, handling the clock and backend interaction.

For deeper technical dives and research notes, see the documentation in the `docs/` folder.

## License

MIT License. See [LICENSE](LICENSE) for details. Dependencies retain their own licenses.
