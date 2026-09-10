# cogg.cpp

[![C++ kernel](https://github.com/AEVYRA/cogg.cpp/actions/workflows/build.yml/badge.svg)](https://github.com/AEVYRA/cogg.cpp/actions/workflows/build.yml)
[![Local model adapter](https://github.com/AEVYRA/cogg.cpp/actions/workflows/llama.yml/badge.svg)](https://github.com/AEVYRA/cogg.cpp/actions/workflows/llama.yml)

A C++20 runtime for AI agents with durable state, bounded scheduling and
recoverable model execution.

`cogg.cpp` stores an agent's state, memory, inbox and logical clock in SQLite.
A model proposes a transition; the kernel checks its limits and commits accepted
changes atomically to a SHA-256 history chain. After a process crash, the host
can reopen the database and continue from the last committed state. Interrupted
inference may run again and produce different text.

**Status: experimental, v0.10.0.** The project originated in the Physalia Gyre
research program and can be embedded independently. Logical clocks and stored
self-state are runtime mechanisms; they do not establish subjective experience.

## What it provides

- **Durable transitions:** atomic state and inbox updates, idempotent input,
  admission accounting and verifiable commit history.
- **Bounded waking:** model-requested wake times constrained by host intervals
  and attempt quotas.
- **Memory:** typed deposits, protected open tasks, version-aware lexical
  retrieval and source-linked compaction.
- **Executor choices:** a native CPU [llama.cpp](https://github.com/ggml-org/llama.cpp)
  adapter, optional Ollama/Chat Completions adapters, or a custom C++ backend.
- **Recovery:** compatible native KV checkpoints can be restored; missing or
  incompatible caches cause reconstruction from committed state. HTTP
  executors reconstruct context and do not transfer native KV.
- **Optional host modules:** guarded self-state, a separate terminal client
  and host, and image observation with retained memory.

## Build

Dependencies: C++20 compiler, CMake 3.20+, SQLite3 with FTS5, OpenSSL libcrypto
and nlohmann/json 3.10+. CI runs on Ubuntu 24.04.

```sh
sudo apt-get install g++ cmake ninja-build libsqlite3-dev libssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The default build needs no model service. Python 3 enables additional process
tests. [Dockerfile.build](Dockerfile.build) provides a Debian build environment.

| CMake option | Enables |
|---|---|
| `COGG_LLAMA=ON` | Pinned native libllama backend; downloads its source at configure time |
| `COGG_HTTP=ON` | Ollama and Chat Completions; requires libcurl 7.85+, Python 3 for tests |
| `COGG_SELF=ON` | Host-authorized self-state policy |
| `COGG_TUI=ON` | Linux host and terminal client; downloads pinned FTXUI |
| `COGG_PERCEPTION=ON` | POSIX image host module; requires HTTP |
| `COGG_SANITIZE=ON` | AddressSanitizer and UndefinedBehaviorSanitizer |

Options default to OFF. See [validation](docs/VALIDATION.md) for the full CI
configuration and real-model tests.

## Try the CLI

This demo uses a deterministic backend. Run it with a new database:

```sh
./build/cogg-cli init demo.db explorer 100 100 60000
./build/cogg-cli send demo.db explorer greeting-1 "Hello, world!"
./build/cogg-cli run demo.db explorer 5 100
./build/cogg-cli inspect demo.db explorer
./build/cogg-cli verify demo.db explorer
```

For actual inference, follow [Local models](docs/LOCAL_MODELS.md) or
[HTTP executors](docs/EXECUTORS.md). For an interactive session, see
[Terminal interface](docs/TUI.md).

## Embed and explore

The [embedding tutorial](docs/GETTING_STARTED.md) introduces `cogg::Store`,
`cogg::Backend` and `cogg::Runtime`. Public headers live in
[`include/cogg`](include/cogg). The [documentation index](docs/README.md) covers
each implemented module; [ARCHITECTURE.md](ARCHITECTURE.md) preserves the broader
research roadmap and includes proposals beyond the current implementation.

Current limits include CPU-only native inference, lexical retrieval misses,
model-dependent answer quality, and incomplete long-duration scheduling
validation. Audio, subject signatures and broader cognitive integration remain
research work. See [validation and limitations](docs/VALIDATION.md).

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for checks and issue reports.
MIT; see [LICENSE](LICENSE). Dependencies retain their own licenses.
