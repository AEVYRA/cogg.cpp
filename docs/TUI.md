# cogg host and terminal client — experimental v0.10.0

The terminal is a window into a running subject. `cogg-host` owns one subject and
one configured executor; `cogg-tui` owns neither the database nor model inference.
Closing the client leaves the host running. Restarting the host preserves the
committed history and inbox, and starts execution **paused**.

This is the first working slice of the pre-rebrand Gyre TUI draft, adapted to the
implemented cogg v5 database and Phase 6/7 contracts. It does not complete every
feature proposed by that draft.

## Build

Linux, C++20, the ordinary cogg dependencies. FTXUI 6.1.9 is fetched at a pinned
commit and verified archive hash. Python is needed only for process tests.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCOGG_TUI=ON -DCOGG_HTTP=ON -DCOGG_SELF=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`COGG_TUI` defaults OFF; the core has no FTXUI dependency. Without `COGG_HTTP`, the
host still runs the deterministic demo. `COGG_SELF` enables guarded existing
self-state subjects. An existing self-state subject is refused by a host built
without that policy. The client links only the IPC helper and FTXUI, not cogg,
SQLite, curl, or a model backend.

For an offline build, populate the pinned FTXUI source and pass
`-DFETCHCONTENT_SOURCE_DIR_FTXUI=/absolute/path/to/FTXUI`.

## Try the demo

In terminal 1, from the repository root:

```sh
mkdir -p "$HOME/.local/state/cogg/tui-demo"
chmod 700 "$HOME/.local/state/cogg/tui-demo"
# Once only: a separate new subject, min interval 1s, 100 attempts/hour.
./build/cogg-cli init "$HOME/.local/state/cogg/tui-demo/subject.db" explorer 1000 100 3600000
./build/cogg-host \
  --db "$HOME/.local/state/cogg/tui-demo/subject.db" --subject explorer \
  --socket "$HOME/.local/state/cogg/tui-demo/host.sock" --demo
```

In terminal 2:

```sh
./build/cogg-tui --socket "$HOME/.local/state/cogg/tui-demo/host.sock"
```

Write a message and press Enter, then enter `/resume`. The first admitted
transition consumes the creation event; the next can answer the message after
the minimum interval. The demo echoes text and deposits the last message as an
episode. It is a deterministic fixture, not an LLM or a memory-quality claim.

For the default kernel limits instead, `cogg-host --create ...` creates a new
empty database with a 30-second minimum interval and 20 attempts/hour. It refuses
an existing file. Limits on an existing subject come from its genesis commit;
starting the UI does not change them.

Ctrl+Q disconnects the terminal. Stop the host with Ctrl+C in terminal 1, or
SIGTERM when it runs under a supervisor. `/pause` stops new admissions, but an
already running inference may still commit. Pausing is not kernel sleep. No
service is installed or enabled by the build.

## Use an actual executor

Start the host with an existing credential-reference config instead of `--demo`:

```sh
./build/cogg-host --db /path/to/subject.db --subject explorer \
  --socket /private/directory/host.sock \
  --config /path/to/executors.json --executor deepseek --allow-remote
```

The format is the existing [Phase 5 configuration](../configs/phase5.example.json).
Ollama and Chat Completions use the existing adapters. Remote execution needs
`--allow-remote`; the client never receives credentials or chooses a fallback
model. One process owns one executor session, with the same durable subject
across host restarts. Stop the host before changing its executor/configuration.
The embedded llama.cpp/KV adapter is not wired to this first host; local models
can be used through Ollama.

Build with `-DCOGG_PERCEPTION=ON` and add `--vision-executor kimi` (or another
configured image-capable executor) for `/image PATH`. The command describes and
remembers a local PNG/JPEG, up to 5 MiB. Paths refer to the host filesystem;
spaces are accepted without shell quotes. The actor receives the observation,
and the image bytes go to the configured vision provider. See [Phase 8](PERCEPTION.md)
for retention, retry and resource boundaries.

Model-requested wakes follow the kernel schedule, minimum interval and budget.
They can execute without an attached terminal after `/resume`. Any non-success
other than a normal scheduler wait pauses further admissions. In particular,
abstention leaves the occasion pending and does **not** silently invoke the
model again. `/resume` is the explicit retry decision. New messages queue behind
that occasion. `/evidence TEXT` retries a paused external occasion with an explicit
operator report, bound to the displayed head and occasion. The report is not a
certified observation or an automatic web search; include the source and when it
was checked. Watch mode disables this operation. The client does not skip events.

Evidence is staged in host memory and becomes durable in the attempt inputs at
admission. A restart before admission discards staging; the acknowledgement says
`durable:false`. It applies to one attempt only. Stale bindings are rejected;
self-state subjects use an exact read-only grant, with no profile/commitment
permissions. Subsequent ordinary messages do not silently inherit this packet.

With an existing genesis `self.profile`, `COGG_SELF=ON` routes each attempt
through `SelfRuntime` with read-only self access. Ordinary deposits remain
possible; profile/commitment writes need an exact grant and are rejected here.
No grants are inferred from user text. Grant editing and creation of self-state
subjects remain with the [Phase 7 host API/example](SELF_STATE.md).

## Controls and views

| Key / command | Behavior |
|---|---|
| Enter | Save a message to the durable inbox |
| F1, `/help` | Help |
| F2, `/timeline` | Latest 80 commits and 20 attempt diagnostics |
| F3, `/memory [query]` | Bounded recent deposits or lexical search, working memory, guarded self view |
| F4, `/inspect [full commit ID]` | Hash-verified commit; default current head |
| F5 | Switch talk/watch |
| `/talk`, `/watch` | Enable/disable mutations in this client |
| `/state` | Committed state, scheduler and separate host status |
| `/since [tick]` | Commit summaries after the connection tick, or an explicit tick |
| `/why-awake` | Current occasion/schedule and last committed wake plan |
| `/evidence TEXT` | Retry the paused external request with a bound operator report |
| `/resume`, `/pause` | Host admission controls, recorded in the operator audit |
| PgUp / PgDn | Scroll one page, including long memory/record views |
| Ctrl+C | Clear local input; does not interrupt the host |
| Ctrl+Q, `/quit` | Disconnect |

UTF-8 input/output, Cyrillic and resize are exercised in a real PTY. At 100+
columns the state sidebar is visible; 80×24 keeps the conversation and compact
state header, including the executor name and an explicit echo-only demo label.
A paused host or abstention reason is also shown below the conversation. Terminal palette colors are used without a forced background.
Smaller windows remain operable but may show very little content.

A subject message is shown only when a **committed speech proposal** exists.
Reflections, null transitions, failure diagnostics and abstention detail are
never inserted into the conversation as subject speech. User events are labelled
pending/consumed. Legacy CLI `{text: ...}` inbox messages are also projected.
History ordering uses recorded wall timestamps; commit ticks/IDs remain the
causal reference when wall time rolls back.

Watch is a client convenience, not an authorization boundary. After a disconnect,
the last data is labelled stale and host status reads DISCONNECTED. Reconnection
refreshes it; lack of a connection is never described as subject sleep. A failed
send preserves input and its idempotency key for retry in the same client session;
a successful acknowledgement means the inbox write is durable. Closing the
client loses an unsaved draft; already accepted events remain in the database.

## Local protocol and ownership

One JSON object plus newline in each direction; one request per connection:

```json
{"protocol":"cogg:host/v1","op":"send","key":"unique-message-key","text":"Hello"}
```

Responses are `{protocol, ok, data}` or `{protocol, ok:false, error}`. Operations
are `snapshot`, `send`, `pause`, `resume`, `inspect`, `memory`, `since`,
`why-awake`, `evidence` (requires `head`, `occasion`, `text`), and optional `image`
(requires `path`, `key`, accepts `text`). The CLI mode uses the same protocol:

```sh
./build/cogg-tui --socket /private/directory/host.sock \
  --request '{"op":"snapshot"}'
./build/cogg-tui --socket /private/directory/host.sock --snapshot 100 30
```

`--snapshot` renders the actual conversation layout once as an ANSI screen,
useful for inspection without an interactive terminal. The interactive client
performs I/O on a separate thread; waiting on a host request does not block text
input or redraw. The host runs inference on its own worker connection, outside
the IPC loop and database write transaction.

- Socket parent: owned directory with mode 0700; socket: 0600. Linux Unix sockets
  only. No TCP listener or remote UI access is introduced.
- Socket and canonical database paths have exclusive host locks. A second host
  is refused even if it chooses another socket. Other CLI/library writers do not
  honor this advisory host lock: stop the host before using a separate writer.
  Same-user clients are trusted operators, not isolated tenants.
- Existing databases are checked read-only for cogg application ID and **schema
  5 before constructing Store**. The host refuses older versions; migrate an
  explicitly chosen separate copy with the existing tools. The pinned Phase 3
  experiment must not be opened by the new host/CLI or otherwise changed.
- Operator controls are fsync'd to `<socket>.operators.jsonl`, separately from
  subject commits. This host audit is not a signed subject history. Startup is
  always paused, including after SIGKILL; it does not silently repeat an unsettled
  inference. SIGTERM requests cancellation and rejects late results through the
  existing runtime contract.
- Requests: at most 64 KiB; responses: 2 MiB; message: 8192 UTF-8 bytes; key: 128
  bytes. Recent conversation: 60 rows, text excerpts at 8192 bytes with explicit
  truncation. `/since` returns at most 100 commits with `more`/`through_tick` for
  paging. Oversized inspection returns an explicit error, never partial JSON.
- C0/DEL and UTF-8 C1 controls in displayed untrusted text are replaced. Stored
  content is unchanged; model/user text cannot emit OSC clipboard or CSI escapes.

The first projection implementation reads the existing full timeline internally;
window limits bound the wire/rendering size, not the database scan. Memory recall
also verifies history. This is intended for small experimental subjects, not a
claim of constant-cost browsing over millions of commits. Scheduler and host
status are sampled separately from the atomic history snapshot and carry their
own head/time; they can advance while that snapshot is being displayed.

## Deliberately outside this first slice

Multi-subject selection, event streaming/subscriptions, rich causal graphs,
general organ composition/audio, operator `/wake`, durable subject suspend semantics, model
switching inside the UI, rich evidence collection, grant editors, native KV/GPU telemetry,
Windows/macOS adapters and full Crystal remain follow-up work. No placeholders
pretend those mechanisms already exist. `/since` uses real commits, not a
model-invented summary or a persisted "last visit" receipt.

## Validation — 2026-09-08

Historical checks: Debug 11/11 and
ASan/UBSan 11/11, followed by focused TUI reruns after the Ctrl+C fix and extra
SIGKILL regression (7.11 s / 10.02 s). The minimal host without HTTP/self also
passed its demo process checks; the core-only Release build remains available.
CI now includes TUI in both Debug/sanitizer jobs.

The tests exercise real subprocesses, Unix sockets, SQLite history, an offline
HTTP fixture and a resized PTY, including recovery with an unsettled reservation,
no automatic retry after abstention, protected self state and terminal escapes.
A separate bounded live DeepSeek smoke test made two attempts (creation + user
message), committed “Связь работает.”, rendered the saved speech, then stopped
the API host. It is a transport/UI check, not an intelligence benchmark.

### First-use repair

A paused echo fixture could look like an unresponsive chat, especially with the
executor sidebar hidden at 80 columns. The compact header now names the executor
and labels demo as echo-only. A paused host displays the next action; abstention
reason/detail appears under the conversation as a host diagnostic, never speech.

The new explicit operator-report path was tested with plain and guarded subjects:
missing-input abstention → bound evidence → committed speech on the same occasion;
stale bindings and watch-mode mutations are rejected, and the admitted input ID
is preserved in history. Updated TUI process tests passed Debug (7.50 s) and
ASan/UBSan (10.41 s); the kernel was unchanged. This is manual evidence handoff,
not an automatic browsing capability.

## Dependency

Terminal library: [FTXUI](https://github.com/ArthurSonzogni/FTXUI), MIT,
version 6.1.9 / commit `5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6`.
The existing cogg kernel, schema, model prompts and admission policy are unchanged.
