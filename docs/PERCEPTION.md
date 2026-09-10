# Image perception — v0.10.0

This implements the first Phase 8 path: explicit local image → vision observation
→ actor speech and memory in one accepted transition → recall after restart.
The optional host module composes existing Phase 6 inputs and Phase 4 deposits.
It introduces no database migration or new kernel transition type.

## Use

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCOGG_HTTP=ON -DCOGG_SELF=ON -DCOGG_TUI=ON -DCOGG_PERCEPTION=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/cogg-host --db /path/to/subject.db --subject explorer \
  --socket /private/directory/host.sock --config /path/to/executors.json \
  --executor deepseek --vision-executor kimi --allow-remote
./build/cogg-tui --socket /private/directory/host.sock
```

Use an existing schema-v5 subject or initialize a new one with `cogg-cli init`
as in [TUI](TUI.md). Enter `/resume`, then `/image /absolute/path/cat photo.jpg`.
The entire remainder after `/image ` is the path; do not add shell quotes.
After the answer, ask a normal text question about the image. The host always
starts paused after restart; use `/resume` again. An already open old client
needs restarting to acquire the new command.

Executors use the [Phase 5 config](../configs/phase5.example.json). The vision
executor must support images: Chat Completions receives a data URL; Ollama
receives base64 in its image array. Both actor and vision require explicit remote
permission when remote. Credentials remain in the host. The successful live
check used the configured Kimi and DeepSeek providers; Ollama transport is not
claimed as live-validated by that check.

## Contract and recovery

- Import accepts nonempty regular PNG/JPEG files up to 5 MiB and rejects symlinks.
  Format sniffing checks magic bytes, not full image decoding; malformed images
  may still be refused by the provider. No URLs, camera, audio or auto-discovery.
- Bytes are fsync'd under `<db>.media/<sha256>.image`, in a private directory,
  before queuing a descriptor with SHA-256, MIME and size. The caller's original
  path is unnecessary after import. Back up the media directory with the DB.
- A successful vision call produces a bounded description (4096 UTF-8 bytes)
  and a `cogg:vision/v1` input. Its fsync'd receipt is bound to the current subject
  head, exact external occasion, image descriptor and vision adapter identity.
  The actor receives the description and provenance, never base64 pixels.
- The receipt survives a crash before actor commit. Retry at the same binding
  reuses it; a crash before receipt publication may repeat the provider call.
  This is durable local reuse, not provider-side exactly-once billing.
- The actor must produce speech plus the exact requested episode note, or an
  explicit abstention. Silent or missing-note proposals fail acceptance and
  leave the image request pending. Image notes retain the image hash and label
  the description as an observation; this proves retention, not visual truth.
  The ordinary atomic commit preserves the admitted observation, speech, memory
  and inbox consumption. Existing read-only self-state policy still applies.
- Vision failure/abstention pauses the host before actor admission. Actor
  failure/abstention follows the existing failure ledger and pauses retries.
  `/resume` explicitly retries. `/pause` does not cancel an in-flight vision
  request; it may save a receipt, but actor admission waits for resume. An
  already admitted actor may still commit. SIGTERM requests cancellation.
- Subsequent text questions retrieve ordinary deposits. They do not reopen the
  image or invoke vision. The regression test deletes retained pixels before
  recall to exercise this separation.

The host operation is deliberately separate from a subject attempt: each vision
call has a 60-second timeout and a bounded input/output, but does not consume the
kernel's actor-attempt quota. There is no aggregate vision-spend quota or media
garbage collector yet. Explicit imports/retries control use; successful receipts
are retained. Only trusted same-user operators may access the host. This is one
vision organ followed by one actor, not an autonomous multi-organ scheduler.

## Validation

The process suite exercises real HTTP fixtures, host subprocesses, IPC, SQLite,
file retention and crash/retry behavior: successful recall, absent memory note,
silent actor, crash during actor inference, damaged pixels, vision abstention
and read-only watch rejection. CI enables the module for Debug and ASan/UBSan.

The live smoke tool is `tools/perception_smoke.py`; it creates a fresh subject,
uses one supplied image, restarts the host and asks a follow-up without pixels.
It requires explicit provider permission and never installs a service or retries
failed inference automatically. The result is summarized below; see
[validation scope](VALIDATION.md) for broader limits.

The original smoke found a real usability defect: vision and retention worked,
but the first actor transition was silent. Its initially optimistic harness
verdict was superseded by a failed acceptance assessment. The corrected host,
prompt and harness require a visible initial answer; the new run passed. This
single scene is evidence for the integrated path, not general visual accuracy
or a guarantee of reliable memory retrieval on every future question.

Audio, video, native vision embeddings, automatic organ selection, semantic
retrieval improvements and broader visual calibration remain open. The scheduling observation protocol is an independent validation task.
