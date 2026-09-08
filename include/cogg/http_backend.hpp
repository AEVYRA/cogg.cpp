#pragma once
#include "cogg/routing.hpp"
namespace cogg {
// Configuration contains credential references, never literal API keys.
// Construction validates local configuration only; no preflight HTTP requests.
class HttpBackend final : public Backend {
public:
    explicit HttpBackend(json configuration);
    ~HttpBackend() override;
    std::string name() const override;
    Proposal propose(const Present&) override;
    Proposal propose_attempt(const Attempt&, millis timeout, const std::function<bool()>& cancelled) override;
    Outcome respond_attempt(const Attempt&, millis timeout, const std::function<bool()>& cancelled) override;
    std::optional<MemoryPolicy> memory_policy() const override;
    bool context_fits(const Present&) const override;
    json telemetry() const override;
    Capabilities capabilities() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void register_http(Registry&, const json& configuration);
} // namespace cogg
