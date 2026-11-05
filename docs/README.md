# Documentation

Start with the [build and CLI quick start](../README.md), then use the
[embedding tutorial](GETTING_STARTED.md) to connect a custom backend.

| Guide | Scope |
|---|---|
| [Kernel](KERNEL.md) | Transactions, admission, clocks and recovery |
| [Local models](LOCAL_MODELS.md) | Native CPU libllama inference |
| [Checkpoints](CHECKPOINTS.md) | Compatible KV restoration and reconstruction |
| [Scheduling](SCHEDULING.md) | Model-requested waking and observation protocol |
| [Memory](MEMORY.md) | Typed deposits, retrieval and compaction |
| [Neural memory](NEURAL_MEMORY.md) | Optional native applicability scorer and bounded full-history candidate provider |
| [Source memory replay](SOURCE_MEMORY_REPLAY.md) | Pinned source excerpts, version checks and the failed first archival transfer gate |
| [Executors](EXECUTORS.md) | HTTP adapters, sessions and bounded routing |
| [Composition](COMPOSITION.md) | Abstention and host-provided evidence |
| [Self-state](SELF_STATE.md) | Optional guarded profile and commitments |
| [Terminal interface](TUI.md) | Separate host and terminal client |
| [Image perception](PERCEPTION.md) | Image observation, answer and recall |
| [Infrastructure](INFRASTRUCTURE.md) | Opt-in endpoint readiness probes |
| [Validation](VALIDATION.md) | Reproduction commands and known limitations |
| [Memory design](MEMORY_DESIGN.md) | Design rationale and related research |
| [Memory evaluation](MEMORY_EVALUATION.md) | Small seeded comparison and its limits |

The guides retain historical phase numbers where useful for schema and design
provenance. Historical test counts describe those revisions; current automated
checks are defined in [CI](../.github/workflows).

[ARCHITECTURE.md](../ARCHITECTURE.md) is the original research roadmap, including
unimplemented proposals. Public C++ headers in [`include/cogg`](../include/cogg)
and the implementation guides describe the current API.

See [Contributing](../CONTRIBUTING.md) for changes and repository hygiene.

- [Request recovery and memory budgets](EMBEDDING_RECOVERY.md): bounded reads, read-only stores and task maintenance.

<!-- <<aevyra:ty v1 wall=2026-11-05T13:29:40Z field=5219 file=1 art=cogg:docs/README.md hash=9128d2e5627bc0fb16db38237629e70a9a5da37c2656af514caf9d5c67ac3815 ref=042fdba030fc865a>> -->
