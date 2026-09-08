#pragma once
#include "cogg/kernel.hpp"
#include <map>

namespace cogg {
// Host declarations, not model-generated capabilities. A Registry is single-thread owned.
struct Capabilities {
    bool remote = false;
    bool exact_tokens = false;
    bool checkpoint = false;
    bool cancellation = false;
};
json capabilities_json(const Capabilities&);
std::string execution_hash(const json&);
void validate_execution(const json&);
void validate_emission(const json& emission, const std::string& attempt,
                       const json& admission, const json& proposal);
json make_emission(const Attempt&, const json& execution, const Proposal&, const json& provider = json::object());
enum class FailureKind { backend, transport, timeout, invalid_output, credentials };
struct BackendFailure : Error {
    FailureKind kind;
    explicit BackendFailure(const std::string& message, FailureKind failure = FailureKind::backend)
        : Error(message), kind(failure) {}
};
class Registry {
public:
    using Factory = std::function<std::unique_ptr<Backend>()>;
    explicit Registry(std::size_t max_sessions = 32);
    void add(std::string id, Capabilities capabilities, Factory factory);
    void enable(const std::string& id, bool enabled);
    void evict(const std::string& subject, const std::string& id);
    json describe() const;
private:
    friend class RoutedRuntime;
    struct Entry { Capabilities caps; Factory factory; bool enabled = true; };
    struct Session { std::unique_ptr<Backend> backend; json execution; };
    std::map<std::string, Entry> entries_;
    std::map<std::pair<std::string, std::string>, Session> sessions_;
    std::size_t max_sessions_;
    Session& session(const std::string& subject, const std::string& id);
};
struct Route {
    std::vector<std::string> executors;
    bool allow_remote = false;
    bool require_exact_tokens = false;
    bool require_checkpoint = false;
    bool require_cancellation = false;
    millis timeout_ms = 60000;
    millis attempt_timeout_ms = 30000;
    std::function<bool()> cancelled;
    json context = nullptr; // Explicitly bound {head, occasion, inputs}; no implicit retries on abstention.
};
struct RouteResult {
    std::optional<Snapshot> snapshot;
    std::string status;
    json attempts = json::array();
    std::string maintenance_error;
};
class RoutedRuntime {
public:
    RoutedRuntime(Store& store, Registry& registry, std::function<millis()> wall_clock = {});
    RouteResult step(const std::string& subject, const Route& route, millis now);
private:
    Store& store_;
    Registry& registry_;
    std::function<millis()> wall_clock_;
};
} // namespace cogg
