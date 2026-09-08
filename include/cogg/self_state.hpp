#pragma once
#include "cogg/routing.hpp"

namespace cogg {
// Optional trusted-host policy over ordinary memory. No extra database or kernel fields.
// All writers of a guarded subject must use this policy; inspect_self detects bypasses.
json self_profile(const json& application_data);
json inspect_self(Store&, const std::string& subject);
// Exact, host-issued permissions. A grant is bound to one subject/head/occasion.
// create: {"self.commitment.ID":"exact terms"}
// settle: {"self.commitment.ID":{"version":"last commit", "status":"closed"|"retracted"}}
// profile: null or the exact replacement application data (revision is host-computed).
json self_grant(const json& view, const json& occasion,
                const json& create = json::object(), const json& settle = json::object(),
                const json& profile = nullptr);
struct SelfResult {
    std::string status;
    std::string attempt;
    json outcome = nullptr;
    std::optional<Snapshot> snapshot;
    std::string maintenance_error;
};
class SelfRuntime {
public:
    SelfRuntime(Store& store, Backend& backend, json execution)
        : store_(store), backend_(backend), execution_(std::move(execution)) {}
    // null grant is read-only self access; ordinary memory proposals are unaffected.
    // Supplied inputs precede the generated self packet and retain their provenance.
    SelfResult step(const std::string& subject, millis now, const json& grant = nullptr,
                    const json& inputs = json::array(), millis timeout_ms = 30000,
                    const std::function<bool()>& cancelled = {});
private:
    Store& store_;
    Backend& backend_;
    json execution_;
};
} // namespace cogg
