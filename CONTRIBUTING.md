# Contributing

Describe the concrete problem, a minimal reproduction and the expected behavior
in an issue or pull request. Include your revision, OS, compiler, SQLite version
and enabled CMake options. For model-dependent behavior, include the model
revision/quantization and adapter configuration with credentials removed.

## Local checks

Follow [Validation](docs/VALIDATION.md) to reproduce the CI build. Run the tests
affected by your change and the full relevant suite before submitting a runtime
change. Add a regression test when fixing a behavioral defect. Documentation-only
changes should preserve working links and executable examples.

Keep database compatibility, atomic commits, admission accounting and source
verification explicit. A passing small-model fixture is not a general claim about
model quality. Document failures and remaining limitations with measured results.

## What belongs in Git

Source, tests (including SQL migration fixtures), example configurations, build
files and maintained user/developer documentation belong in the repository.
Keep generated databases, model weights, checkpoints, logs, private deployment
configs and transcripts outside it. Use ignored `reports/`, `artifacts/` or
`experiments/` directories for local output. Dated internal reviews are not part
of the public documentation.

Before committing, review `git diff --cached` and `git status --short`. Ignore
rules do not remove files already tracked by Git or erase earlier history.
Keep example configurations free of credentials; use environment references
and private files as described in [Infrastructure](docs/INFRASTRUCTURE.md).

No live provider call is required by the default tests. Run optional API probes
only with an account and data you are authorized to use.
