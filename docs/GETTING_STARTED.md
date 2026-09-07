# Getting Started with cogg.cpp

This guide explains how to embed the `cogg.cpp` transition kernel into your own C++ application. 

While `cogg.cpp` provides a built-in `cogg-cli` and a `llama.cpp` backend out of the box, its true power lies in its embeddable API. You can use it to give any AI model (local or API-based) a persistent, crash-resilient lifecycle and its own subjective sense of time.

## The Core Architecture

To run a durable agent, you need three components:
1. **`cogg::Store`**: The SQLite database that safely handles atomic commits, admission quotas, and the agent's inbox.
2. **`cogg::Backend`**: The interface that wraps your model logic. It receives the current state (`Present`) and returns a decision (`Proposal`).
3. **`cogg::Runtime`**: The orchestrator that steps time forward, checking time limits and coordinating the Store and Backend.

---

## 1. Implementing a Custom Backend

To plug your model (or custom logic) into the kernel, inherit from `cogg::Backend`. The kernel will call `propose()` whenever the agent is allowed to wake up.

```cpp
#include <cogg/kernel.hpp>
#include <iostream>

class MyAgentBackend : public cogg::Backend {
public:
    std::string name() const override { return "my_custom_backend"; }

    cogg::Proposal propose(const cogg::Present& present) override {
        std::cout << "Agent woke up! Tick: " << present.state.tick << "\n";
        
        // 1. Read the state (memory, inbox event, etc.)
        if (present.occasion.kind == "user_message") {
            std::cout << "Received: " << present.occasion.payload["text"] << "\n";
        }

        // 2. Formulate a proposal (what happens next)
        cogg::Proposal prop;
        prop.kind = "thought";
        prop.text = "I am processing the input.";
        
        // 3. Write to memory (atomic key-value update)
        prop.memory.push_back({"last_thought", "I am processing the input."});
        
        // 4. Request the next autonomous wake-up (Endogenous Time)
        // e.g., Request to wake up in exactly 5 seconds (5000 ms)
        prop.wake_after_ms = 5000; 

        return prop;
    }
};
```

## 2. Setting up the Store and Runtime

Now, let's wire it up in `main()`.

```cpp
int main() {
    // 1. Initialize the SQLite Store
    cogg::Store store("my_agent.db");

    // 2. Define the agent's time and admission limits
    cogg::Limits limits;
    limits.min_wake_ms = 1000;     // Cannot wake up faster than 1s
    limits.max_attempts = 10;      // Max 10 wakes per period
    limits.period_ms = 60000;      // The period is 60s (10 wakes / minute)

    // 3. Create the agent (subject)
    store.create("agent_01", limits, cogg::wall_now());

    // 4. Send an external event to the inbox
    store.submit(
        "agent_01",               // Subject ID
        "msg_123",                // Unique Idempotency Key
        R"({"text": "Hello!"})"_json, 
        cogg::wall_now()
    );

    // 5. Initialize Backend and Runtime
    MyAgentBackend backend;
    cogg::Runtime runtime(store, backend, cogg::wall_now);

    // 6. Step the agent forward
    // The Runtime checks quotas, reads the inbox, calls backend.propose(), 
    // and commits the result atomically to SQLite.
    auto snapshot = runtime.step("agent_01", cogg::wall_now());

    if (snapshot) {
        std::cout << "Success! New Tick: " << snapshot->tick << "\n";
        std::cout << "Scheduled Wake: " << snapshot->wake_at.value_or(0) << "\n";
    }

    return 0;
}
```

## What Just Happened?

When `runtime.step()` is called:
1. **Time Check:** The `Store` checked if `agent_01` was eligible to wake up according to its `Limits`.
2. **Execution:** The `Runtime` passed the `Present` state (containing the `Hello!` event) to your `MyAgentBackend`.
3. **Persistence:** The `Proposal` (including the 5-second sleep request and the memory write) was cryptographically hashed and saved to SQLite in a single transaction.
    
Because `cogg.cpp` is fail-closed, if someone pulled the server's power cord during `propose()`, the event remains safe in the inbox. When the server reboots, calling `runtime.step()` will seamlessly retry the operation.
