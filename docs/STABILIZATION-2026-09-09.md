# Small stabilization before perception (v0.9.1)

Native KV files now use the subject's genesis origin and the executor/build
identity in their namespace. Independent subject lineages or adapters sharing
a directory no longer overwrite one another's cache. Renaming/moving a database
preserves its lineage. Exact copies of one lineage with the same adapter can
still share a cache; use distinct directories for intentionally forked copies.
Head/tick/native-compatibility checks remain mandatory before restoration.

Old unscoped files remain untouched and are not imported automatically. The
first new run reconstructs context from committed state. There is no database
migration or change to the commit chain, quotas, routing, or self-state policy.
The two-argument `checkpoint_path` helper remains for callers that already give
each custom cache a private directory; the native backend uses the scoped form.

`output_limits.hpp` supplies core and adapter cardinalities: 64 working-memory
assignments, eight typed notes, 32 sources/covers. Native grammar and Ollama's
JSON schema now derive their array bounds from those constants; parser limits
remain authoritative, including UTF-8 byte limits which JSON Schema character
lengths cannot express. Native decoding retains its whitespace/order conventions.
Backend fingerprints include the limits header. No `n-1` template truncation,
extra fail transaction, or speculative wake cleanup was introduced.

Validation in an isolated Debian build:

- 11 offline/core/HTTP/self/native-options tests passed (24.36 s).
- Real pinned Qwen GGUF inference and checkpoint recovery both passed
  (160.50 / 268.29 s under concurrent build load).
- Checkpoint coverage includes missing, damaged, stale and incompatible files,
  native restore rejection, cancelled inference and failed post-commit writes.
- Namespace tests retain different lineage/adapter files in one directory;
  HTTP tests inspect the actual emitted schema bounds.

The ongoing Phase 3 observation retains its original binary, DB and cache bundle.
