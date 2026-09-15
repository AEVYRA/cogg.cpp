# Recovering requests and managing memory pressure

This guide uses a support assistant as an example. A host delivers a question,
the model answers, and the process may stop before the host receives the answer.
The host must discover what was committed before deciding whether to retry.

## Read the result of one request

```cpp
cogg::Store store("assistant.db", cogg::OpenMode::read_only);
store.verify("assistant"); // Full integrity check at the recovery boundary.
auto receipt = store.request_trace("assistant", "customer-request-42");
if (!receipt["commits"].empty()) {
    // Inspect proposal.kind: a consuming reflection need not be an answer.
    auto committed = receipt["commits"][0]["body"];
}
auto charged = receipt["attempt_count"].get<std::int64_t>();
```

`request_trace` takes the same **external key** passed to `submit`. It returns
one SQLite read snapshot: head/tick, occasion, consuming commit, recent attempts,
abstentions and schedule. `attempt_count` is exact; the attempt list defaults to
64 entries and is capped at 256. `truncated` makes omitted attempts explicit.
Each reservation counts, including failed or interrupted work. Polling while
admission is unavailable does not count as a reservation. Remote providers may
still bill interrupted requests; the ledger does not promise exactly-once billing.

These are indexed read APIs, not implicit full verification. Verify at recovery
or ownership boundaries; keep one authorized writer or serialize writers. A
resulting head is a snapshot anchor, not a promise that no other writer can append.

`commits_page(subject, after_tick, limit, expected_head)` returns bounded history
pages. Start with `after_tick=-1` to include genesis, then use `next_after_tick`.
Pass the first page's `head` to subsequent pages; a changed head raises `Conflict`.
The host can restart pagination explicitly. Full `timeline` export and forensic
`verify` remain available.

`OpenMode::read_only` requires an existing schema-5 database. It performs no
schema migration, creates no missing database and rejects writes. SQLite may
need its normal WAL/shared-memory sidecars; this is not a promise that opening a
live WAL file leaves every filesystem metadata field untouched.

## Make the query explicit

A custom backend should set `MemoryPolicy.query` from its application's actual
question. For example, a host payload might call that field `question` or
`message`; the generic kernel does not know every application's envelope.
An empty query derives from `occasion.payload.text` for compatibility.
The selected query, head and source IDs are retained in the admission receipt.

## Keep new tasks within the memory budget

With the balanced policy, open tasks are protected from ordinary pruning. New
admissions record `cogg:memory-capacity/v1`. Their commits are checked against
the admitted item/byte budget **after** applying the proposed notes, inside the
same transaction. If mandatory tasks no longer fit, `ContextOverflow` rolls
back the commit and all its memory writes. The reservation still exists and the
host must settle it as a failed attempt, as the provided runtimes do.

This checks the memory projection budget, not the size of every future model's
complete prompt. Model/context changes can still require explicit maintenance.
Direct trusted-host admissions without a memory policy remain available and do
not acquire an implicit model budget. Existing history retains its original
contract; old commits are not rewritten or retroactively rejected for capacity.

## Recover an already overloaded history

1. Use `open_tasks(subject, after_key, limit)` to inspect a bounded page of open
   tasks. It returns the current head and exact deposit IDs. Check the head when
   combining pages; concurrent changes require a fresh view.
2. Explicitly choose 1–8 IDs, set `strategy="task_maintenance"`, and pass those
   IDs in `MemoryPolicy.maintenance_ids` to a suitable executor.
3. Supply the actual reason/evidence for settling each task. Maintenance may
   close or retract only the named tasks with unchanged terms; it cannot add new
   tasks, write legacy state, schedule a wake or silently settle unseen tasks.
4. Resume balanced retrieval after sufficient pressure has been relieved.

The receipt says that the task view is partial. Other tasks remain open in the
database. A maintenance executor must still fit the selected task text and its
fixed prompt; a smaller batch or a larger executor may be necessary. No automatic
policy declares commitments fulfilled merely to free capacity. Ordinary notes
and a speech response are allowed during maintenance; guarded self commitments
still require their separate host grants.

## Prompt payloads are rendered once

HTTP and native adapters keep the full request at `occasion.payload`. In the
prompt-only temporal projection, the duplicated admission payload becomes
`payload_ref: "/occasion/payload"`. The stored admission receipt and `Present`
remain complete. This prevents a large source excerpt from consuming the prompt
budget twice. It is an explicit reference, not truncation of the source.

## Cost and validation

Verification loads admission timestamps once, then checks saved budget prefixes
using binary searches. This removes repeated SQL scans of all prior admissions
while preserving quota watermarks and clock-rollback semantics. Full verification
still replays history and checks SQLite integrity and memory provenance; it is
not constant-time. No persistent trusted cache is introduced.

`cogg-embedding-tests` exercises capacity rollback, item/byte pressure recovery,
request reconciliation, pagination and read-only access. Existing temporal tests
cover quota windows and clock rollback. For a repeatable size experiment:

```sh
./build/cogg-history-benchmark /tmp/new-history-benchmark
```

The tool creates fresh databases at 100, 1,000 and 10,000 transitions with 2 KiB
request payloads and a short note per transition. It reports full verification,
request lookup, timeline export, construction time and database bytes. These are
single-run measurements, not application latency percentiles or model-quality
results.
