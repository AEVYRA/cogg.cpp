#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace cogg {
using json = nlohmann::json;
using millis = std::int64_t;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct Conflict : Error { using Error::Error; };
struct Limits {
    millis min_wake_ms = 30000;
    std::int64_t max_attempts = 20;
    millis period_ms = 3600000;
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
};
struct MemoryWrite { std::string key; json value; };
struct Proposal {
    std::string kind = "null";
    std::string text;
    std::vector<MemoryWrite> memory;
    std::optional<millis> wake_after_ms;
};
// Untrusted JSON has no tick, head, signature, or storage mutation fields.
Proposal parse_proposal(const json& value);
json proposal_json(const Proposal& value);

struct Attempt {
    std::string id;
    Present present;
};
enum class CommitPoint { before_sql_commit, after_sql_commit };

// Trusted host API. One Store connection per thread; independent connections
// cooperate through SQLite transactions. Model adapters receive only Present.
class Store {
public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void create(const std::string& subject, Limits limits, millis now);
    Snapshot snapshot(const std::string& subject);
    std::string submit(const std::string& subject, const std::string& key,
                       const json& payload, millis now);
    std::optional<Attempt> admit(const std::string& subject,
                                const std::string& backend, millis now);
    Snapshot commit(const std::string& attempt, const Proposal& proposal, millis now,
                    const std::function<void(CommitPoint)>& fault_hook = {});
    void fail(const std::string& attempt, const std::string& reason);
    json timeline(const std::string& subject);
    json record(const std::string& id);
    void verify(const std::string& subject);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual Proposal propose(const Present&) = 0;
};
class Runtime {
public:
    Runtime(Store& store, Backend& backend) : store_(store), backend_(backend) {}
    std::optional<Snapshot> step(const std::string& subject, millis now);
private:
    Store& store_;
    Backend& backend_;
    std::set<std::string> verified_;
};
millis wall_now();
} // namespace cogg
