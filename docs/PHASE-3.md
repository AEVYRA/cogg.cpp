# Phase 3 — endogenous time

Version 0.4.0 implements explicit temporal provenance, replayable wake decisions,
physical eligibility and an observation harness. The 48-hour real-model experiment
is a separate validation gate; passing scheduler tests does not complete that gate.

## Clock contract

The reference process is one subject's verified commit parent chain. Its identity
is `cogg:commit:<genesis hash>`, its grain epoch is
`cogg:committed-transition/v1`, and its unit is one committed transition. Genesis
is coordinate zero; every accepted transition, including null, advances one unit.
Failed/reserved/superseded attempts, inbox delivery, polling, sleep and checkpoint
maintenance do not belong to this reference chain. Commit schema 2 preserves the
existing membership rule, so schema migration does not create a new grain epoch.

`Store::duration(subject, from, to)` verifies history and measures `[from,to)`:
self-duration is zero, adjacent commits are one unit apart, and same-chain
intervals are additive. Reversed anchors, attempt/event IDs and foreign subjects
are rejected. The returned coverage is for the local verified chain only. This
is not a general cross-clock atlas, cognitive effort measure or consciousness test.

```sh
build/cogg-cli inspect subject.db explorer
build/cogg-cli duration subject.db explorer GENESIS_HASH HEAD_HASH
build/cogg-cli schedule subject.db explorer
```

## Wake request, grant and execution

`wake_after_ms=null` chooses waiting for an external occasion. Zero requests an
immediate wake. A non-null request is resolved at commit:

1. Clamp its delay between the subject's minimum and maximum requested interval.
2. Move the granted deadline later if existing admissions exhaust the quota.
3. Atomically commit the original proposal, `wake_plan`, memory and new head.

The immutable plan records the request, requested deadline, granted delay/deadline,
policy version, reasons and the admission-budget snapshot used to decide it.
Reasons include `minimum_interval`, `maximum_interval`, `attempt_budget` and
`waiting_external`. The maximum bounds a *requested* delay; budget constraints
can still move actual eligibility beyond that maximum. A later competing attempt
can also postpone execution. A granted wake is an earliest deadline, not a
reservation of CPU or a guarantee that a process will be running.

The conservative policy applies minimum spacing and quota to **all admissions**,
including external occasions, failures, interrupted work and competing proposals.
No restart, failed proposal or quota-denied poll refunds a spent admission.
The rolling budget counts admissions in `(accounting_now-period, accounting_now]`;
an admission at the lower endpoint has expired. The earliest permitted retry is
visible through `schedule`, along with the denial reason. Polling itself is read-only.

Live scheduling keeps raw observed wall time separate from a persistent accounting
high-water mark. A backwards wall adjustment does not refund quota or make a
scheduled wake physically due early. Commit accounting also includes steady-clock
inference elapsed time, so a rollback during inference cannot erase that duration.
The CLI samples wall time again after inference. Embedded hosts can pass a wall
sampler to `Runtime`; without one, `step(subject, now)` extrapolates its supplied
anchor using steady elapsed time. This is reported host time, not an external
certified clock. Forward clock jumps can expire wall-based quotas sooner in real
elapsed time; hard monotonic budgets across machine restarts are not implemented.

External input can preempt sleep when admission limits permit it. A new commit
supersedes the previous wake. No second occasion or tick is created for the old
wake. Choosing null cancels autonomous waking until an external occasion arrives.

## Model context and storage compatibility

`Present::temporal` contains the chain descriptor and coordinate, observed wall
and accounting values, admission decision/budget, prior wake plan and actual
scheduled-wake lateness. These facts are also saved in the attempt record. They
let a backend condition its next choice on temporal state without acquiring
permission to edit clocks or policy. The normal proposal schema stays unchanged.

Database version 2 accepts and upgrades version 1 stores without rewriting their
existing hashed records. Legacy commits replay with their original wake policy;
new commits use schema 2. Older cogg binaries reject the newer database version.
The new reader verifies wake decisions against the recorded admission watermark,
checks new admission budgets and validates the accounting projection. Checkpoint
compatibility changes with this adapter/build; an old KV cache safely falls back
to canonical reconstruction.

Creation accepts an optional host-authored seed memory object. It is hashed into
genesis and remains visibly distinct from later model-authored writes:

```sh
build/cogg-cli init subject.db explorer 60000 12 3600000 900000 seed-memory.json
```

The fourth limit is the maximum requested wake delay; it defaults to one year.
Existing three-limit calls remain valid.

## 48-hour observation protocol

Requires Python 3 on POSIX only for the harness; the C++ runtime does not require it.
Use the pinned Qwen fixture and build settings from [Phase 1](PHASE-1.md).

```sh
python3 tools/time_experiment.py run build/cogg-cli model.gguf experiments/time-48h
python3 tools/time_experiment.py report build/cogg-cli experiments/time-48h
```

The harness creates one subject with a fixed, explicitly host-authored seed:
compare ways of remembering an unfinished question and assess four numeric state
features. It sends **no external messages after genesis** and does not fabricate
wakes if the model chooses to wait. `--internal-only` constrains generation to
reflection/null plus memory writes. The model chooses whether and when to return;
no script maps a state feature to a requested interval.

Default runtime bounds are minimum 60 seconds, maximum requested interval 15
minutes, and 12 attempts per rolling hour; each inference is limited to 512
output tokens, 4096 context tokens and 120 seconds. The Python supervisor adds
a process timeout and ends the observation at the persisted deadline. Requests
for indefinite waiting remain permitted. Bounds may produce a constant granted
pattern even when requested intervals differ.

The immutable manifest pins the binary, model, runner, settings, seed and wall
observation window. Resuming the same directory does not reset its deadline or
recreate its subject. A changed configuration is rejected. One writer holds a
file lock. The supervisor reopens the model process at each admitted opportunity;
compatible KV is restored from disk. This experiment therefore measures the
warm/cold process path, not a continuously resident-model optimization.

For a run that must survive SSH exit, launch the command through a service manager
with persistent data/model/binary paths. A transient user service with lingering
can survive SSH exit; reboot persistence requires an installed/enabled service.
Do not put the only copy of an ongoing experiment under `/tmp`.

Artifacts:

- `manifest.json`, `seed.json`: fixed protocol and provenance.
- `subject.db`, `kv/`: authoritative subject chain and disposable KV.
- `status.json`: latest eligibility/status and original end deadline.
- `runs.jsonl`: invocations, failures, output/token/cache statistics, starts/stops
  and five-minute heartbeats; process crashes can leave a missing final log entry.
- `transitions.csv`: committed state features before/after each choice, original
  and granted intervals, next scheduled admission interval and lateness.
- `report.json`: verified-chain counts and descriptive Spearman correlations.

The next scheduled interval uses observed wall times; the separate accounting
interval preserves the rollback-resistant projection. Negative wall intervals
are retained in CSV and excluded from correlations. At least 20 usable pairs
and variation in both variables are required to produce a coefficient. Features
are model self-reports; correlations are descriptive, with no significance or
causal claim. Autocorrelation, host timing, policy clamps and model quality remain
confounders. Requested and granted intervals must be assessed separately.
A constant feature, constant interval or decision to stop waking is a valid
negative/insufficient-data result, not a reason to inject new work.

`observation_window_elapsed` means the persisted wall deadline was reached. It
does not certify 48 hours of uninterrupted observation: inspect heartbeats,
restarts, failures and wall adjustments. Final scientific acceptance requires the
actual 48-hour artifact review; a virtual-clock run is only a scheduler test.

## Validation boundaries

The temporal suite checks quota/minimum/maximum grants, replay after restart,
read-only denial polling, wall rollback with an already queued wake, elapsed
inference under rollback, external preemption, indefinite waiting, verified
same-chain windows, overflow rejection, a Phase 2-generated legacy fixture and
48 simulated hours with deterministic state-dependent requests.

Existing kernel, process-crash, checkpoint and real-model suites remain enabled.
The real-model suite also checks the internal-only grammar and backend identity.
`tests/time_experiment_test.py` runs a short actual model observation, interrupts
and resumes its supervisor, checks that the deadline and subject survive, and
rejects changed protocol settings. This short smoke is not the 48-hour experiment.

GPU/energy quotas, rolling token quotas, cross-clock comparison, externally typed
deadline occasions, subjective-time claims and learned scheduling remain outside
this implementation. Phase 4 is memory retrieval and compaction; its existence
must not turn the pending observation gate into a claimed successful experiment.

Local validation on 2026-09-07: seven Release suites passed (358.79 s), four
standalone core suites passed (33.39 s), and all seven ASan/UBSan suites passed.
Sanitizers instrumented cogg, the adapter and tests; external pinned libllama
remained Release. The separate native-host real-model crash test passed through
tick 5. The 126-second supervisor smoke passed stop/resume with a fixed deadline
and no external occasions. These are mechanism checks, not 48-hour results.
