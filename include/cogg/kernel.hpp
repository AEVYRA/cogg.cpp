#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <variant>
#include <nlohmann/json.hpp>
#include "cogg/memory.hpp"

namespace cogg {
using json = nlohmann::json;
using millis = std::int64_t;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct ContextOverflow : Error { using Error::Error; };
struct Conflict : Error { using Error::Error; };
struct Limits {
    millis min_wake_ms = 30000;
    std::int64_t max_attempts = 20;
    millis period_ms = 3600000;
    millis max_wake_ms = 365LL * 24 * 60 * 60 * 1000;
};
struct Snapshot {
    std::string subject;
    std::int64_t tick = 0;
    std::string head;
    json memory = json::object();
    std::optional<millis> wake_at;
    std::string lifecycle;
};
struct Occasion {
    std::string id;
    std::string kind;
    json payload;
};
struct Present {
    Snapshot state;
    Occasion occasion;
    // An earlier reservation has no settlement. This can mean a crash OR a
    // concurrent worker; it is not evidence that the earlier inference failed.
    bool prior_unsettled_attempt = false;
    // Runtime-issued clock, wake provenance and admission accounting.
    json temporal = json::object();
    json working_memory = nullptr; // Selected legacy keys; Snapshot remains complete.
    json memory_view = nullptr; // Frozen retrieval receipt and source-addressed deposits.
    json inputs = json::array(); // Host-supplied evidence, frozen at admission; not memory writes.
};
struct MemoryWrite { std::string key; json value; };
struct Proposal {
    std::string kind = "null";
    std::string text;
    std::vector<MemoryWrite> memory;
    std::optional<millis> wake_after_ms;
    std::vector<MemoryNote> notes;
};
// Untrusted JSON has no tick, head, signature, or storage mutation fields.
Proposal parse_proposal(const json& value);
json proposal_json(const Proposal& value);

// Non-participation is neither a failed transport nor a subject-level null transition.
struct Abstention {
    std::string reason; // missing_input, unsupported, uncertain, refused
    std::string detail; // Bounded diagnostic, never a user-facing answer or instruction.
};
using Outcome = std::variant<Proposal, Abstention>;
Outcome parse_outcome(const json&);
json outcome_json(const Outcome&);
// Inputs are content-addressed host assertions, not certified observations.
json make_input(const std::string& kind, const std::string& producer, const json& content,
                const std::vector<std::string>& sources = {});
void validate_inputs(const json&);
void validate_admission_context(const json&); // null or {head, occasion, inputs}

struct Attempt {
    std::string id;
    Present present;
};
enum class CommitPoint { before_sql_commit, after_sql_commit };

// Trusted host API. One Store connection per thread; independent connections
// cooperate through SQLite transactions. Model adapters receive an Attempt/Present, never a Store.
class Store {
public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void create(const std::string& subject, Limits limits, millis now,
                const json& initial_memory = json::object());
    Snapshot snapshot(const std::string& subject);
    std::string submit(const std::string& subject, const std::string& key,
                       const json& payload, millis now);
    std::optional<Attempt> admit(const std::string& subject,
                                const std::string& backend, millis now,
                                const std::optional<MemoryPolicy>& memory_policy = std::nullopt,
                                const std::function<bool(const Present&)>& fits = {},
                                const json& execution = nullptr, const json& context = nullptr);
    Snapshot commit(const std::string& attempt, const Proposal& proposal, millis now,
                    const std::function<void(CommitPoint)>& fault_hook = {},
                    std::optional<millis> inference_elapsed_ms = std::nullopt,
                    const json& emission = nullptr);
    // Atomic, idempotent settlement; preserves head, memory, wake and pending occasion.
    std::string abstain(const std::string& attempt, const Abstention&, const json& provider = json::object());
    void fail(const std::string& attempt, const std::string& reason);
    // Read-only eligibility; polling never creates an occasion or a subject tick.
    json schedule(const std::string& subject, millis now);
    json clock(const std::string& subject);
    // Verified same-chain duration [from,to), in committed-transition units.
    json duration(const std::string& subject, const std::string& from, const std::string& to);
    json timeline(const std::string& subject);
    json record(const std::string& id);
    void verify(const std::string& subject);
    json recall(const std::string& subject, MemoryPolicy policy = {});
    void rebuild_memory(const std::string& subject);
    json memory_record(const std::string& subject, const std::string& id);
private:
    void verify_impl(const std::string& subject, bool compare_memory_index);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual Proposal propose(const Present&) = 0;
    // Cooperative interruption is optional; RoutedRuntime also rejects late results.
    virtual Proposal propose_attempt(const Attempt& a, millis, const std::function<bool()>&) { return propose(a.present); }
    virtual Outcome respond_attempt(const Attempt& a, millis timeout, const std::function<bool()>& cancel) {
        return propose_attempt(a, timeout, cancel);
    }
    virtual json telemetry() const { return json::object(); }
    virtual std::optional<MemoryPolicy> memory_policy() const { return std::nullopt; }
    // Trusted pure host callback: complete prompt plus reserved output must fit.
    virtual bool context_fits(const Present&) const { return true; }
    // Optional cache/maintenance work after a successful durable commit.
    // Exceptions cannot undo the commit and are reported by Runtime separately.
    virtual void committed(const Present&, const Snapshot&) {}
};
class Runtime {
public:
    // Pass a wall sampler in a live host to observe clock adjustments during inference.
    // Without it, step(now) extrapolates its supplied timestamp using steady elapsed time.
    Runtime(Store& store, Backend& backend, std::function<millis()> wall_clock = {})
        : store_(store), backend_(backend), wall_clock_(std::move(wall_clock)) {}
    std::optional<Snapshot> step(const std::string& subject, millis now, const json& context = nullptr);
    const std::optional<Abstention>& last_abstention() const { return abstention_; }
    const std::string& maintenance_error() const { return maintenance_error_; }
private:
    Store& store_;
    Backend& backend_;
    std::function<millis()> wall_clock_;
    std::set<std::string> verified_;
    std::string maintenance_error_;
    std::optional<Abstention> abstention_;
};
millis wall_now();
} // namespace cogg
