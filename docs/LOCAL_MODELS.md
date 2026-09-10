# Local model inference

The optional native libllama adapter runs GGUF models on the CPU. It was introduced
in v0.2.0; current builds also support [durable KV checkpoints](CHECKPOINTS.md)
and [bounded memory retrieval](MEMORY.md).

## Build and run

The default build still has no inference dependency. Enable the adapter with:

```sh
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCOGG_LLAMA=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build-llama -j2
ctest --test-dir build-llama --output-on-failure
./build-llama/cogg-cli init subject.db explorer 1000 20 3600000
./build-llama/cogg-cli send subject.db explorer hello-1 "Hello. Remember my name is Alex."
./build-llama/cogg-cli run-model subject.db explorer /path/to/model.gguf --steps 2
./build-llama/cogg-cli inspect subject.db explorer
```

CMake fetches llama.cpp revision `0cae43063cf15170e91a2ff4d034da0ecef4a1b2`,
verifies the archive SHA-256 and builds its library. Offline builds may use
`-DFETCHCONTENT_SOURCE_DIR_LLAMA=/path/to/extracted/pinned/source`; the caller
must verify this override matches the pin. No model downloads happen at runtime.
This adapter uses CPU inference; GPU support and GPU admission remain future work.
The default native CPU build targets the build host. For a portable binary on an
older x86 host, disable unsupported ggml features explicitly; `GGML_NATIVE=OFF`
alone still enables AVX2/FMA/BMI2 in this pinned dependency. The tested i5-3570
build used `-DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=OFF -DGGML_FMA=OFF
-DGGML_F16C=ON -DGGML_BMI2=OFF -DGGML_OPENMP=OFF`.

`--steps N` counts new successful commits and waits for admissible occasions.
`--once` makes one admission attempt and exits, including when nothing is due.
No event or a denied budget never causes an invented model call or subject tick.
`--ctx`, `--tokens`, `--threads`, and `--timeout-ms` configure finite execution
limits. Defaults: 4096 context tokens, 512 output tokens, 2 CPU threads, 30 seconds.
`--template NAME` explicitly chooses a llama built-in template when metadata is
missing or unsupported. Unsupported templates fail instead of silently changing
the conversation format. `--release-context` evicts KV after each successful step;
the loaded weights remain resident. SIGINT/SIGTERM cancels generation cooperatively.

CLI output describes committed state and its accepted proposal, including speech.
The record is also accessible with `inspect`. This is not a streaming conversation UI or an
external delivery protocol. Retain the same database to continue after restart.

## Ownership and reconstruction

`LlamaBackend` owns one model and one context, reused across occasions. It is
single-thread-owned, like a Store connection. A prompt is built from one canonical
`Present`: subject/head/tick, committed memory, lifecycle/wake deadline, occasion
and the unsettled-attempt indicator. It does not read files, run tools, or own Store.
This is a current memory projection, not automatic retrieval of the full ledger.

The adapter retains decoded tokens and KV. Before every proposal it compares the
new canonical prompt token by token, removes the divergent suffix, and re-evaluates
the remainder. Even an identical prompt re-evaluates its last token to restore
logits. Changing subjects clears the context. Models that cannot remove a partial
sequence fall back to clearing all KV. Every failed generation evicts the context.
Thus speculative output can never enter the next prompt merely because it was
generated. Only exact matching canonical tokens may be reused.

After eviction or process restart the prompt is rebuilt from verified durable
state. KV is an acceleration cache, not the authority for memory or identity.
With `--checkpoint-dir`, compatible KV can be restored from disk; otherwise
restart reconstructs from committed state. Hot reuse does not establish a hidden stream of
thought beyond canonical state. In the local Qwen fixture, hot and cold greedy
outputs differed despite identical input tokens. Prefix reuse changes evaluation
batching; exact generated-text equality is not a contract. Both paths must still
propose valid transitions from the same committed state.

## Bounded generation and failure

The sampler applies a fixed transition grammar followed by greedy selection.
It accepts one complete JSON proposal, with up to 64 legacy memory assignments
and eight typed notes. Shared bounds live in `include/cogg/output_limits.hpp`.
The normal kernel parser then checks semantics, including duplicate keys, field
authority, memory sizes and wake range. Syntax-constrained generation alone does
not establish the truth or usefulness of a proposal.

The complete canonical prompt plus reserved output must fit the configured
context. Memory is never silently truncated to fit. Oversized input, invalid
output, cancellation, timeout or token exhaustion fails the attempt and leaves
the occasion and subject head unconsumed. Existing durable admission limits count
the failed attempt. The CLI reports the failure and exits; it does not hot-loop
unbounded retries. A caller can retry later under those same persisted limits.

Timeout uses a steady clock, checked between operations and by llama's CPU abort
callback during decoding. It is cooperative, not a hard real-time process limit.
Model hashing/loading are startup work outside this generation deadline. Context
and token limits do not bound all process RAM, model size or disk usage. Native
backends remain trusted code, not a security sandbox.

Admission records identify both the GGUF content SHA-256 and a configuration hash
covering adapter/revision, context/output/batch limits, threads, timeout, template
and grammar/greedy version. Keep the GGUF immutable while loaded. The callback's
external cancellation state is intentionally not a reproducible model parameter.

## Validation

The optional real-model test is enabled with `-DCOGG_TEST_MODEL=/path/model.gguf`.
It covers cold generation, hot reuse, explicit eviction, a changed present,
scheduled waking from a host-seeded obligation, reopening the durable Store with
a fresh backend, subject isolation, oversized prompts, output exhaustion and
cancelled inference without committing or consuming input. It does not assert
that arbitrary small models understand obligations or choose useful wake times.

The existing kernel and process-SIGKILL tests remain part of CTest. The test model
is a fixture for execution mechanics, not a recommended cognitive substrate.
No inference quality, subjective continuity or Physalia integration claim follows
from these checks. The original larger architecture remains the roadmap.

### Local evidence, 2026-09-07

On Linux/i5-3570, GCC 12.2, the Release CTest run passed all four suites in
67.42 seconds (kernel, adapter options, real inference, process crash).
The final CTest run with AddressSanitizer and UBSan passed all four suites in
82.82 seconds, including deadline exhaustion. cogg, its adapter and tests were
instrumented; the pinned third-party libllama objects remained Release builds.
The separately run `tests/model_crash_test.py` killed an admitted model process
at tick 0, reopened and verified the same database, committed tick 1, then
processed another input in a third process at tick 2. The abandoned reservation
remained visible and only one occasion consumption occurred on recovery.

A CLI smoke run asked the model for a 1000 ms wake. It committed memory and the
wake request at tick 2; a new process then handled the due occasion and committed
tick 3, retaining the earlier memory. These are scheduling/recovery observations,
not a claim about the quality of that model's repetitive answers.

Fixture: [Qwen2.5-0.5B-Instruct Q4_K_M](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/blob/9217f5d/qwen2.5-0.5b-instruct-q4_k_m.gguf),
SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`.
An earlier SmolLM2-135M Q8 fixture recursively copied payload JSON and exhausted
512 output tokens. That was a rejected attempt, not a successful transition.
The backend does not promise that every GGUF model can follow this contract.
