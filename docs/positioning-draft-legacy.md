> Historical positioning draft by the founding session. Superseded as product claims
> by RESEARCH-2026-09-07.md and the working README. Preserved for provenance.
> Claims of being first, crash-proof, exact thought recovery, universal OOM prevention,
> and quantified superiority below have not been established.

# cogg.cpp — Marketing & Positioning Strategy

**Status:** Draft 0.2
**Target Audience:** Local AI enthusiasts, AI Agent developers, AI Alignment researchers.
**Core Mission:** Position `cogg.cpp` not just as another inference engine, but as the first "Operating System for Synthetic Subjects."

---

## 1. The Market Gap (Based on Current Web Trends)

After analyzing the current landscape of local LLMs and multi-agent systems, the pain points of the community are glaringly obvious:

1.  **The Persistence Problem:** Developers are currently duct-taping SQLite databases (like Mem0 or ContextStore) to stateless inference engines (Ollama, llama.cpp) via Python wrappers. When the Python script crashes, the agent loses its "train of thought" and has to do expensive re-prefilling of the context window.
2.  **The "Fake" Autonomy Problem:** To make an agent run in the background, developers are forced to use system-level `cron` jobs to trigger Python scripts (e.g., Hermes Agent, langchain-runner). The agent doesn't "live"; it's just a scheduled script hitting an API.
3.  **The Local Multi-Agent Nightmare:** Running multiple models on local hardware almost always leads to Out-Of-Memory (OOM) crashes or state corruption (agents overwriting shared files). Most settle for "Single-Engine Roleplay" (one model pretending to be multiple agents) because actual orchestration is too heavy.

## 2. The Positioning Statement (The Hook)

**"llama.cpp runs prompts. cogg.cpp runs subjects."**

Current local AI is like a goldfish: it wakes up, answers a prompt, and forgets everything. `cogg.cpp` is a local-first cognitive runtime that gives your models a memory, a heartbeat, and the right to disagree. It replaces the brittle `Cron + Python + Ollama` stack with a single, crash-proof C++ binary.

## 3. Core Value Propositions (The "Killer Features")

We need to sell the engineering elegance of `cogg.cpp` as solutions to the community's biggest headaches:

### A. Crash-Proof Causal Memory (The SQLite DAG)
*   **The Pitch:** Stop losing your agent's mind to a `SIGKILL`.
*   **The Feature:** `cogg.cpp` natively integrates a SQLite Write-Ahead Log (WAL) beneath the inference layer. Every cognitive transition is a cryptographic commit. If your machine reboots, `cogg.cpp` wakes up, reads its DAG, and resumes the exact thought process. No Python wrapper required.

### B. True Endogenous Time (Internal Clocks)
*   **The Pitch:** Your AI shouldn't need a human to hit "Enter" to think.
*   **The Feature:** `cogg.cpp` ditches external system crons. It has an internal scheduler. The subject can "sleep", manage background memory compaction, and wake itself up based on internal unresolved conflicts or deadlines. It introduces the concept of a "Subject Tick" independent of the "Wall Clock."

### C. Resource-Safe Multi-Substrate Orchestration
*   **The Pitch:** Run a team of experts on a single GPU without OOM crashes or file-locking nightmares.
*   **The Feature:** `cogg.cpp` manages model residency (hot/warm/cold). It natively integrates a "Disagreement Manifold." Instead of agents overwriting each other's text files, `cogg.cpp` orchestrates semantic conflicts transactionally at the C++ level.

## 4. Go-To-Market Strategy

### Phase 1: The "Hacker" Launch (Reddit & GitHub)
*   **Where:** `/r/LocalLLaMA`, HackerNews, GitHub trending.
*   **The Bait:** Release a 48-hour time-lapse log of a `cogg.cpp` agent running *without user input*. Show it waking up, re-evaluating its memory, finding a contradiction, resolving it, and going back to sleep.
*   **The Promise:** Emphasize the **Single Binary** architecture. "Just `make`, point it to your GGUFs, and watch it live."

### Phase 2: The Agent Developer Migration
*   **Where:** LangChain/AutoGPT/CrewAI community forums.
*   **The Bait:** Show how replacing their complex multi-container Docker setups with a single `cogg.cpp` binary eliminates 90% of their state-management bugs. Provide a simple REST API wrapper that mimics OpenAI but returns "Subject Commits" instead of just text strings.

## 5. Distribution Channels & Content Tactics (The "How")

Engineers are immune to standard marketing. We must sell through **demonstrable engineering superiority**.

### Key Platforms
*   **Hacker News (Y Combinator):** The holy grail. A well-timed `Show HN: cogg.cpp — A Local OS for Synthetic Subjects (crash-proof agents)` can bring thousands of stars in 24 hours.
*   **Reddit (`/r/LocalLLaMA`, `/r/Oobabooga`):** The core demographic of hardware enthusiasts pushing local models to their limits.
*   **X (Twitter) AI Engineering Sphere:** Target influencers who build agents and inference engines (e.g., swyx, Karpathy, llama.cpp contributors).
*   **Discord Communities:** EleutherAI, TheBloke, LocalLLaMA. Drop technical deep-dives into their architecture channels.

### Content Formats (Proof of Concept Demos)
1.  **The "Ghost in the Shell" Demo (Crash-Proofing):**
    *   *Concept:* A short GIF/Video showing `cogg.cpp` actively thinking. In a second terminal, execute `kill -9 <pid>`. The process dies. Restart the binary. It instantly prints *"Restoring from SQLite WAL... Resuming thought"* and finishes the exact task without losing context. This visually proves the architectural superiority over Ollama+Python.
2.  **The "48-Hour Solitude" Log (Endogenous Time):**
    *   *Concept:* A blog post titled *"I left my local Llama 3 running for a weekend with internal clocks. Here is what it thought about."* Publish the raw logs showing the system waking up autonomously, compressing its own memories, and resolving conflicts while the user was away.
3.  **The "Why LangChain Agents OOM" Article (Multi-Substrate):**
    *   *Concept:* A technical deep-dive for Hacker News explaining why current Python-based multi-agent orchestration is fundamentally flawed for local VRAM, and how `cogg.cpp`'s C++ kernel and hot/cold model residency solves it.

### Lowering the Barrier to Entry
To ensure conversion from "viewer" to "user":
*   **OpenAI API Drop-in:** Include a minimal HTTP server that mimics the OpenAI API schema, so users can plug `cogg.cpp` directly into their existing UIs (SillyTavern, Open WebUI).
*   **One-liner Docker Command:** Provide a robust `docker run` command that handles GGUF downloads and SQLite mounting automatically.

## 6. Potential Pitfalls to Avoid in Messaging
*   **Do not market it as AGI.** Keep the tone grounded in software engineering (DAGs, state machines, KV cache optimization).
*   **Do not scare away beginners.** While the Physalia architecture is deeply philosophical, the `cogg.cpp` README should read like a highly optimized developer tool. Hide the metaphysics behind flawless C++ performance.
