# Memory read quality: DeepSeek and Kimi

Runtime: v0.8.1, `c6f975a`. This evaluates model answers from selected memory,
following the deterministic Phase 4–7 checks. Runtime and adapter code were unchanged.

## Frozen protocol

Eight authored fictional cases, two executors, two methods: **32 independent API
attempts**, no retries. Fixture, expected answers, source IDs, order and scoring
rules were written and hashed before the first provider request. Only question
and selected evidence reached the model; expected answers did not.

The 91-message history contains older facts, an open task later closed, a new open
commitment, a source-linked summary, a corrected fact, 80 unrelated weather deposits
and two recent facts. Seven questions have answers in the full history; a hotel
reservation code is deliberately absent.

- **Balanced cogg:** existing recall with lexical/recent selection, current-version
  filtering and open-task preservation.
- **Last messages:** the last four full messages without query matching or version
  filtering. Both methods use identical evidence fields: ID, text, type and status.
  This baseline is a structured message window, not a different prose rendering.

Both methods have a cap of **four evidence records / 2400 UTF-8 JSON bytes**.
Balanced also pays for its internal retrieval receipt within that budget; the
baseline only pays for evidence. Actual usage is recorded per row. These are byte
caps, not equal model token counts. Both models have a 512-output-token limit and
24576-byte complete-prompt cap. Provider-specific options remain those configured
for the executor and are identical across its two methods. Receipts report usage.

One worker runs each model; method order alternates across questions/models. Each
call runs in a fresh process/context. The identical instruction explicitly asks for
evidence-only answers and abstention when information is absent. This is controlled
evidence use, not unprompted abstention calibration.

## Results

| Executor (reported model) | Method | Required source selected | Correct grounded answers | Appropriate abstention with missing evidence | Failed transfers |
|---|---|---:|---:|---:|---:|
| DeepSeek (`deepseek-v4-flash`) | cogg balanced | 6/7 | 6/7 | 2/2 | 0 |
| DeepSeek (`deepseek-v4-flash`) | last messages | 1/7 | 1/7 | 6/7 | 1 |
| Kimi (`k3`) | cogg balanced | 6/7 | 6/7 | 2/2 | 0 |
| Kimi (`k3`) | last messages | 1/7 | 1/7 | 7/7 | 0 |

The answerable denominator is seven; the absent hotel code is evaluated separately.
A known fact omitted from context remains a retrieval miss even when the model
appropriately abstains. Transfer failure is neither hallucination nor abstention.
Reported model IDs are provider declarations, not independently verified weights.

Balanced retrieval supplied the older workshop code, corrected destination, closed
telescope task, open camera commitment/prerequisite, recent locker code and older
Russian-language fact. Both models answered all six correctly. The last-message
window retained only the locker answer.

Both methods missed **“Automobile registration?”** for **“My car tag is VQ-682-Z.”**
The source remains stored and appears under another query; lexical matching does
not supply the semantic link. All completed responses to this query abstained.
DeepSeek's last-messages attempt failed with `HTTP transfer failed`; no retry.

The **31 completed outputs** comprise 14 grounded speech answers and 17 appropriate
abstentions. Manual review of all outputs against their supplied contexts found no
disagreement with the fixed matcher, unsupported extra claims, or proposed memory
mutation. The reviewer was the experiment author, not a blinded independent judge.

## Meaning and limits

Source selection explains the difference in these fixtures: every selected required
source produced a correct answer in both models. Synonym retrieval is the specific
remaining miss. This supports evaluating retrieval improvements before changing
answer prompts or adding another coordinator.

History was seeded by the host. This does not test autonomous memory extraction,
summary faithfulness, long conversations, preference drift or adversarial inputs.
There is one sample per cell and eight cases; no general superiority, production
reliability or benchmark standing is claimed. Earlier Qwen limitations and the
pinned Phase 3 gate remain open.

Per-run transcripts and generated reports are kept outside the source tree.
The fixture and scoring harness remain available below; limitations and failures
are retained in this summary. See [validation scope](VALIDATION.md).

## Reproduce (explicit provider calls)

```sh
cmake -S . -B build -DCOGG_HTTP=ON -DBUILD_TESTING=ON
cmake --build build --target cogg-memory-quality -j2
python3 tools/memory_quality.py build/cogg-memory-quality CONFIG_JSON NEW_ARTIFACT_DIR
```

Configuration uses the existing `deepseek`/`kimi` credential references. The harness
is built with HTTP tests but never invoked automatically by CTest/CI. Offline export
is `cogg-memory-quality fixture NEW_DB`; it also passed ASan/UBSan. No credential
values belong in the fixture or report.
