# Infrastructure preparation for Phase 5

The existing runtime remains v0.5.0 with an in-process CPU libllama adapter.
This page and `tools/infrastructure_smoke.py` prepare external executors; they
are not an implementation of Phase 5 registry/routing or distributed commits.
They do not read or mutate a subject database. The pinned Phase 3 observation
retains its original binary, model, database, protocol and deadline.

## Executor configuration

Copy [the example](../configs/infrastructure.example.json) to a private location,
such as `~/.config/cogg/infrastructure.json`, and set actual model names/endpoints.
No endpoint is discovered or contacted until it is explicitly selected:

```sh
python3 tools/infrastructure_smoke.py \
  --config ~/.config/cogg/infrastructure.json --only local-gpu
python3 tools/infrastructure_smoke.py \
  --config ~/.config/cogg/infrastructure.json --only deepseek kimi \
  --output /tmp/cogg-api-readiness.json
```

API calls consume account quota. Each selected executor receives one small
synthetic prompt and a bounded output request, with no automatic retries. The
reported timings are individual smoke observations, not performance benchmarks.
The shared fixture checks an exact JSON answer; it does not test memory quality,
subject continuity, long context, concurrent scheduling or model equivalence.

Credentials are read from the named process environment variable. A private
executor entry can optionally add `credential_file` pointing to an existing
simple dotenv file. Only the named key is read; shell code and variable expansion
are never executed. Keep that file outside the repository. Credentials require
HTTPS; redirects are rejected; prompts contain only the synthetic fixture.
Reports omit endpoint addresses, credentials, full response bodies and reasoning.
Providers' usage counters and reported model names are retained, with final
credential redaction. They are provider claims, not weight fingerprints.

## Local GPU

The preparation host reports GTX 1050 Ti, 4096 MiB VRAM, driver 550.163.01 and
Ollama 0.30.11. Existing embedding service residency is preserved. A separate
Qwen2.5 3B Q4_K_M model is prepared for a 4096-token smoke context, rather than
using the existing 7.2 GB model as a full-GPU assumption. The downloaded tag is
not immutable: record its resolved digest and actual `size_vram` from the smoke.
The script keeps its test model resident for two minutes for inspection; it does
not change the global Ollama service or other models' keep-alive settings.

Hardware support is documented by [Ollama](https://docs.ollama.com/gpu), and the
[model catalog](https://ollama.com/library/qwen2.5/tags) lists the quantizations.
VRAM and successful inference must be measured locally; file size alone does not
establish a fit. This GPU executor currently uses Ollama HTTP. It does not change
the native cogg libllama build into a CUDA-enabled backend.

Measured local result on 2026-09-08: Qwen2.5 3B Q4_K_M returned the exact fixture
JSON with a 4096-token configured context. Ollama reported all 2,155,169,709 model
residency bytes in VRAM, and `nvidia-smi` reported 2393 MiB total with the existing
embedding model still resident. The 21-token generation took 0.775 seconds
(27.11 tokens/s); the complete cold request took 6.267 seconds. This short result
is not a sustained-throughput or long-context benchmark. Resolved Ollama digest:
`357c53fb659c5076de1d65ccb0b397446227b71a42be9d1603d46168015c9e4b`.

```sh
ollama pull qwen2.5:3b-instruct-q4_K_M
```

An additional import of the existing pinned 0.5B GGUF exercised GPU inference
with all 479,954,205 residency bytes on GPU. It failed the exact JSON fixture.
That failure is retained in the private report; GPU execution and model answer
correctness are separate observations. Its two-minute residency expired normally.

## Mac executor

The deployment Mac has not been connected yet: its known Tailscale entry is
offline and its historical LAN address timed out on SSH/Ollama. An up-to-date
reachable SSH destination is still required; no remote changes were made.

Use an already available model on the Mac when possible. Inspect Apple chip,
free memory, disk and loaded models before downloading another model. Start with
a bounded context; 16 GB unified memory also serves the OS and other applications.
Ollama's local service can be reached through an SSH tunnel:

```sh
ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
  -L 127.0.0.1:11435:127.0.0.1:11434 USER@MAC
```

The example Mac endpoint uses that loopback tunnel. The private host/user and
actual model belong in deployment configuration, not this public repository.
A working tunnel/inference probe is infrastructure evidence; cogg's routing,
late-response and replacement semantics remain Phase 5 work.

## Remote API verification

On 2026-09-08 the authorized accounts' model-list endpoints returned DeepSeek V4
Flash/Pro and Kimi K3 plus K2.7 aliases. Direct synthetic inference succeeded:

| Executor | Model returned | Prompt/completion tokens | Elapsed |
|---|---|---|---|
| DeepSeek | deepseek-v4-flash | 46 / 12 | 1.120 s |
| Kimi Code API | k3 | 127 / 107, including 79 reasoning tokens | 5.221 s |

These are two specific authorized account probes. Kimi's Code API and Moonshot's
platform API are distinct configurations; do not assume their keys interchange.
The probe uses its own User-Agent and no CLI/persona session. DeepSeek thinking
is explicitly disabled for this small readiness query. Kimi K3 uses low reasoning
effort and retains its reasoning-token accounting, without logging reasoning text.
See [DeepSeek thinking controls](https://api-docs.deepseek.com/guides/thinking_mode/)
and [Kimi provider/model configuration](https://www.kimi.com/code/docs/en/kimi-code-cli/configuration/config-files.html).
