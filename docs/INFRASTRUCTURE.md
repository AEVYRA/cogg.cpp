# Executor readiness probes

`tools/infrastructure_smoke.py` checks configured Ollama or Chat Completions
endpoints independently of the runtime. It does not read or mutate a subject
database. For durable routing, see [Executors](EXECUTORS.md).

## Configure and run

Copy [the example](../configs/infrastructure.example.json) to a private location
and replace the model names and endpoints with ones available to you:

```sh
mkdir -p ~/.config/cogg
cp configs/infrastructure.example.json ~/.config/cogg/infrastructure.json
python3 tools/infrastructure_smoke.py \
  --config ~/.config/cogg/infrastructure.json --only local-gpu
python3 tools/infrastructure_smoke.py \
  --config ~/.config/cogg/infrastructure.json --only deepseek kimi \
  --output /tmp/cogg-api-readiness.json
```

Only selected executors are contacted. API probes consume account quota. Each
receives one synthetic prompt and a bounded output request, without automatic
retries. Provider access and model availability depend on your account.

Credentials come from the configured environment variable, or an optional
`credential_file` containing a simple dotenv assignment. Only the named key is
read; shell code and variable expansion are never executed. Keep credentials
and deployment configurations outside the repository. Authenticated requests
require HTTPS; redirects are rejected. Reports omit endpoint addresses,
credentials, complete response bodies and reasoning text.

## Local or remote Ollama

Use an installed model and measure actual memory residency on your hardware.
A model file's size alone does not establish a GPU fit. The script keeps the
selected model resident for two minutes; it does not change global service
settings. Ollama HTTP execution does not enable CUDA in the native CPU adapter.

An optional remote Ollama endpoint can be reached through an SSH tunnel:

```sh
ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
  -L 127.0.0.1:11435:127.0.0.1:11434 USER@HOST
```

Use `http://127.0.0.1:11435` in your private configuration. Hostnames, usernames
and actual model choices are deployment settings.

## Interpretation

A successful probe establishes transport and a small structured-answer fixture.
It does not establish memory quality, long-context reliability, executor
equivalence or sustained performance. Usage counters and model names are
provider reports, not independent weight fingerprints. Store generated reports
outside the source tree, for example under `reports/` (ignored by Git).
