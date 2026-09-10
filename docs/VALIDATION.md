# Validation and known limitations

The two [GitHub workflows](../.github/workflows) are the source of truth for
current automated checks. Badges in the README link to their run history.

## Offline kernel and host tests

On Ubuntu 24.04, install the base dependencies from the README plus
`libcurl4-openssl-dev` and `python3`, then run:

```sh
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCOGG_HTTP=ON -DCOGG_SELF=ON -DCOGG_TUI=ON -DCOGG_PERCEPTION=ON
cmake --build build-ci -j2
ctest --test-dir build-ci --output-on-failure
```

Repeat in a separate build directory with `-DCOGG_SANITIZE=ON` for ASan/UBSan.
Tests use temporary databases, subprocesses and local HTTP fixtures. They do not
require provider credentials. TUI configuration downloads pinned FTXUI sources.

Coverage includes concurrent connections, genuine FTS corruption, history
verification, migration fixtures, crashes around commits, routing, abstention,
self-state permissions, terminal IPC and image observation/recovery.

Ubuntu 24.04's GCC 13/json 3.11.3 requires explicit JSON extraction at several
string comparisons. SQLite 3.45.1 can also report a false FTS integrity failure
after another connection changes the index. The verifier refreshes FTS through
an empty-phrase read within its verification snapshot before running
`PRAGMA integrity_check`; checks remain enabled and corrupted indexes still fail.

## Native model tests

Use the exact GGUF URL and SHA-256 in [llama.yml](../.github/workflows/llama.yml).
The model is a downloaded test fixture, not a repository asset:

```sh
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCOGG_LLAMA=ON -DBUILD_SHARED_LIBS=OFF -DCOGG_TEST_MODEL=/path/to/model.gguf
cmake --build build-llama -j2
ctest --test-dir build-llama --output-on-failure
python3 tests/model_crash_test.py build-llama/cogg-cli /path/to/model.gguf
python3 tests/time_experiment_test.py build-llama/cogg-cli /path/to/model.gguf
```

These tests exercise inference, retained source context, cache reuse and fallback,
and actual process recovery. They do not establish cognitive quality or
bit-identical output after interrupted generation. Native inference currently
uses CPU execution; GPU execution is available through the Ollama HTTP adapter.

## Research observations

Per-run JSON reports and internal operational notes are kept outside the public
source tree. The tools and SQL fixtures needed to repeat experiments remain
versioned. Historical counts in module guides refer to the revisions named there.

- Memory retrieval is lexical: synonym-only queries can miss stored facts.
  The [seeded memory evaluation](MEMORY_EVALUATION.md) found 6/7 grounded answers
  with selected evidence versus 1/7 with a last-message window for two models.
  It is a small authored fixture, not a general benchmark or an evaluation of
  autonomous memory formation.
- Small native models can echo questions or propose invalid transitions even
  when the correct source fits in context. Kernel validation does not repair
  model semantics or guarantee useful answers.
- Executor switching and image recall have bounded smoke evidence; broader
  reliability and evidence-aware participation remain open. The initial image
  smoke produced a silent actor response; the corrected path requires a visible
  answer. See [Perception](PERCEPTION.md).
- The real 48-hour, state-dependent waking validation remains open. The
  [scheduling supervisor](SCHEDULING.md) tests recovery mechanics independently.
- Audio, subject signatures and broader cognitive integration are not supplied
  by the current implementation. The native code runs in the host process;
  admission limits are not an OS sandbox or a complete RAM/disk resource limit.
