# Phase 5: heterogeneous executors

Contract: one Store owns the subject's history, clock, admission budget and commit.
A host-owned registry selects an ordered route of independent backend sessions.
Each (subject, executor) gets its own adapter instance. Eviction discards a cache,
not history. A replacement reconstructs its Present from the committed chain and
Phase 4 retrieval. KV is never transferred between incompatible models.

Routing is sequential, bounded and explicit. Remote execution requires an opt-in.
Capability requirements are checked before inference. Failed attempts consume the
normal admission budget; fallback never bypasses the minimum interval. A caller
may need to resume later when admission says wait. A conflict ends the step rather
than retrying on a new head. Overall deadlines and cancellation prevent late
responses from committing even if a custom backend cannot interrupt computation.

Schema 4 binds a normalized emission to its admitted attempt, parent, subject,
executor fingerprint and session. The host wraps the model's untrusted Proposal;
this is provenance within cogg, not a provider signature or proof of model weights.
Legacy schema 1–3 records retain their original hashes. Older binaries reject a
schema 4 database. Back up a database before upgrading; use a separate database
for experiments pinned to an older release.

Optional libcurl adapters support Ollama chat and Chat Completions (DeepSeek/Kimi).
Every request supplies the selected context explicitly. HTTP happens outside all
SQLite write transactions. TLS verification stays enabled, redirects are rejected,
credentials require HTTPS, response size and duration are bounded. Credentials
are read by name from the environment or a selected dotenv entry, never executed.
The registry contains credential references, never key values in provenance.

HTTP adapters advertise no exact tokenizer or transferable KV. Their prompt-byte
limit is a conservative operational bound, not a token guarantee. Provider usage
and model names are reported claims. Provider-side truncation/cache behavior is
outside cogg's proof boundary. Protected memory that cannot fit causes admission
to fail, rather than a false claim of retention.

Acceptance: independent sessions, capability and remote gates, fallback accounting,
malformed/partial/late responses, conflicting writers, replay and legacy migration;
then a shared history across local Qwen, DeepSeek and Kimi with explicit opt-in.
Phase 3's pinned 48-hour experiment remains untouched. Mac is postponed.
Deliberation, voting and resolution of model disagreement belong to Phase 6.

## Build and run

```sh
# Install libcurl 7.85+ development headers; offline HTTP tests also need Python 3.
cmake -S . -B build -DCOGG_HTTP=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
cp configs/phase5.example.json ~/.config/cogg/phase5.json
# Set credential environment variables or private credential_file references.
build/cogg-cli init /tmp/routed.db subject 1 20 3600000
build/cogg-cli run-route /tmp/routed.db subject ~/.config/cogg/phase5.json \
  --route local-gpu,deepseek,kimi --allow-remote
build/cogg-cli send /tmp/routed.db subject question-1 'What unfinished work is in memory?'
build/cogg-cli run-route /tmp/routed.db subject ~/.config/cogg/phase5.json \
  --route kimi --allow-remote
build/cogg-cli verify /tmp/routed.db subject
```

`run-route` executes **one routed step**, not an unattended service. Routes contain
1–8 unique registered IDs. It prints status, attempt trace and the committed
emission. Exit 0 means committed or waiting; 3 means exhausted/conflict/timeout;
130 means cancelled; 1 is a configuration or kernel error. Check the status even
when the process succeeds. Unknown IDs are rejected before inference. Only IDs in
the supplied route can run. A skipped capability/disabled/unavailable/context
candidate consumes no inference quota. An admitted failure always does.

If fallback reaches a rate/budget boundary, status is `waiting` with the current
schedule. There is no hidden retry loop or persistent route cursor: the next
invocation is a new ordered route. The host may resume with the remaining IDs at
the eligible time. The example's 1 ms admission floor is for short probes, not a
recommendation to loosen an existing subject's runtime limits.

Remote means a non-loopback transport or an explicit `"remote": true` declaration.
Only numeric loopback addresses count as local. Mark loopback SSH tunnels to a
remote machine with `remote: true`; cogg cannot infer their physical destination.
Credentials always require HTTPS, including on loopback. HTTP clients reject URL
userinfo/query/fragment, redirects, tool-call responses, length-limited outputs,
malformed JSON and provider errors. Ollama receives a JSON schema for the
proposal shape; cross-field constraints, source validity and size limits are
still validated by cogg. HTTP cancellation is cooperative; a provider
may finish or bill a request after the client disconnects. No provider-side
idempotency guarantee is assumed.

`max_prompt_bytes` bounds the complete JSON message array; `max_output_tokens`
bounds requested output, including reasoning where the provider counts it.
Ollama additionally receives `context_tokens`, `threads` and a two-minute
`keep_alive`. The example retains headroom for its 4096-token context. Byte bounds
are not tokenizer proofs; no remote adapter satisfies `--require-exact-tokens` or
`--require-checkpoint`. `--require-cancellation` is supported by HTTP.

## Embedding and session ownership

`Registry::add(id, capabilities, factory)` accepts any `Backend`, including the
existing `LlamaBackend` and application adapters. Factories run outside SQLite
transactions. Configuration and registry are trusted host input. Models only
return proposals; they cannot register executors, change routes or grant remote
access. A registry caches at most 32 sessions by default (configurable 1–1024).
Each session belongs to one `(subject, executor)`. Explicit `evict` releases an
idle session; the next use reconstructs from the Store. A single Registry/Store
is single-thread owned; use separate coordinators/connections for concurrency.

A native-backend factory should namespace checkpoint directories by executor;
subject names are already separated by the checkpoint layer. Native checkpoints
continue to validate exact backend/build compatibility. Native adapter factories
can be registered through C++; the JSON CLI currently constructs HTTP adapters.
Factories may share immutable weights, but must return independently owned
contexts. The base backend's routed hook calls `propose`; it need not implement
cooperative cancellation, so only declare capabilities actually provided by the
adapter. The router always checks deadline/cancellation after inference before
attempting commit. A slow factory or noncooperative backend may return late; it
cannot be forcefully stopped by this synchronous API. Hard process resource
isolation remains a deployment responsibility.

Every schema 4 admission has an execution field (null for the original Runtime).
Routed admissions require a matching emission at commit; mismatched attempt,
parent, occasion, subject, executor/session or proposal is rejected. `verify`
rechecks this binding in addition to chain hashes, temporal accounting and memory
replay. HTTP backend fingerprints include adapter source and configuration,
excluding credential references. They identify the adapter configuration, not
remote weights. HTTP session IDs are host adapter lifetimes, not provider KV IDs.
Raw responses, reasoning and error bodies are not persisted. Provider metadata
retains only bounded reported model names and nonnegative token counters.

## Opt-in live probe

```sh
python3 tools/phase5_smoke.py --cli build/cogg-cli \
  --config ~/.config/cogg/phase5.json --directory /tmp/cogg-phase5-live
```

The directory must not exist. The script creates a separate synthetic DB, calls
Qwen → DeepSeek → Kimi → Qwen in fresh CLI processes, verifies each commit and
checks the final exact memory plus the requested answers. It uses the three
explicit executor IDs from the example. There are no live provider calls in CI.
Do not point experiments pinned to v0.4/v0.5 at the new binary: opening their DB
would upgrade its schema. Their pinned binaries and databases remain separate.

Protocol references: [Ollama chat](https://docs.ollama.com/api/chat),
[structured outputs](https://docs.ollama.com/capabilities/structured-outputs),
[DeepSeek Chat Completions](https://api-docs.deepseek.com/api/create-chat-completion/),
[libcurl timeout](https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html) and
[cancellation callback](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html).

## Observed validation, 2026-09-08

The following historical smoke summary retains successes and earlier failures;
see [validation scope](VALIDATION.md) for current checks. With the common prompt and Ollama output
schema, Qwen3B → DeepSeek V4 Flash → Kimi K3 → Qwen3B completed four verified
transitions in separate CLI processes. DeepSeek read the original fixture value;
Kimi changed it; a fresh Qwen session read the updated value. The final memory
matched exactly, including the unchanged unfinished task. An additional synthetic
HTTP 503 → real DeepSeek route committed tick 5 with the same parent and occasion
across the failed and successful attempts. Both attempts remained accounted for.

Earlier JSON-mode runs failed: Qwen returned reflection instead of the requested
speech and copied input structure into invalid proposals. A too-constrained
Ollama schema also produced a provider grammar error. These are retained as
failures, not counted as successful continuations. The provider schema now
constrains shape; the original kernel validators still enforce all limits and
semantic/source constraints. A single successful fixture is not a reliability
estimate or evidence of human-like memory.

The full Release/libllama suite passed 11/11 (614.73 s). Debug core/HTTP and
ASan/UBSan passed 7/7 each. Final routing/HTTP changes were rebuilt and checked
separately under sanitizers and Release; native inference/checkpoint sources were
unchanged during those refinements. Offline faults cover process kill, restart,
cancellation, oversized/partial responses, simultaneous writers, capability and
remote gates, and a real Phase 4 database fixture. A core-only build performed a
transition and verification without linking libcurl.
