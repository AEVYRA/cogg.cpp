#pragma once

#include "cogg/kernel.hpp"
#include "cogg/checkpoint.hpp"

namespace cogg {
struct LlamaOptions {
    std::string model_path;
    std::uint32_t context_tokens = 4096;
    std::uint32_t max_output_tokens = 512;
    std::uint32_t batch_tokens = 128;
    int threads = 2;
    bool internal_only = false; // no speech transitions
    bool allow_abstention = false; // opt in to the Outcome grammar after model calibration
    millis timeout_ms = 30000;
    // Optional built-in llama chat template name; otherwise use model metadata.
    std::string chat_template;
    std::function<bool()> cancelled;
    // Empty disables disk checkpoints. Directory must already exist.
    std::string checkpoint_directory;
    std::uint64_t max_checkpoint_bytes = 536870912;
    std::function<void(CheckpointPoint)> checkpoint_hook;
};
struct InferenceStats {
    std::size_t prompt_tokens = 0;
    std::size_t reused_tokens = 0;
    std::size_t generated_tokens = 0;
    std::string checkpoint_read = "disabled";
    std::string checkpoint_write = "disabled";
    std::string checkpoint_detail;
};

// Single-thread-owned CPU backend. The model stays loaded across occasions.
// KV is a disposable acceleration of the canonical Present, never subject state.
class LlamaBackend final : public Backend {
public:
    explicit LlamaBackend(LlamaOptions options);
    ~LlamaBackend() override;
    LlamaBackend(const LlamaBackend&) = delete;
    LlamaBackend& operator=(const LlamaBackend&) = delete;
    std::string name() const override;
    Proposal propose(const Present&) override;
    Outcome respond_attempt(const Attempt&, millis timeout, const std::function<bool()>& cancelled) override;
    std::optional<MemoryPolicy> memory_policy() const override;
    bool context_fits(const Present&) const override;
    void committed(const Present&, const Snapshot&) override;
    void release_context();
    InferenceStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cogg
