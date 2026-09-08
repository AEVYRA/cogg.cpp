#include "cogg/routing.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

namespace cogg {
namespace {
void need(bool ok, const char* message) { if (!ok) throw Error(message); }
bool text_field(const json& j, const char* key, std::size_t limit) {
    return j.contains(key) && j.at(key).is_string() && !j.at(key).get_ref<const std::string&>().empty() &&
        j.at(key).get_ref<const std::string&>().size() <= limit &&
        j.at(key).get_ref<const std::string&>().find('\0') == std::string::npos;
}
std::string session_id() {
    unsigned char bytes[32];
    need(RAND_bytes(bytes, sizeof bytes) == 1, "session random source failed");
    return execution_hash(json::binary(std::vector<unsigned char>(bytes, bytes + sizeof bytes)));
}
}
std::string execution_hash(const json& j) {
    const auto bytes = j.dump(); unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    need(EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) == 1, "execution hash failed");
    std::string result; const char* digits = "0123456789abcdef";
    for (unsigned int i = 0; i < size; ++i) { result += digits[digest[i] >> 4]; result += digits[digest[i] & 15]; }
    return result;
}
json capabilities_json(const Capabilities& c) {
    return {{"remote", c.remote}, {"exact_tokens", c.exact_tokens}, {"checkpoint", c.checkpoint}, {"cancellation", c.cancellation}};
}
void validate_execution(const json& e) {
    if (e.is_null()) return;
    need(e.is_object() && e.size() == 5 && e.value("schema", "") == "cogg:execution/v1" &&
         text_field(e, "executor", 64) && text_field(e, "backend", 200) && text_field(e, "session", 64) &&
         e.contains("capabilities"), "invalid execution descriptor");
    const auto& c = e.at("capabilities");
    need(c.is_object() && c.size() == 4, "invalid capabilities");
    for (const auto* key : {"remote", "exact_tokens", "checkpoint", "cancellation"})
        need(c.contains(key) && c.at(key).is_boolean(), "invalid capability");
}
void validate_emission(const json& e, const std::string& id, const json& admission, const json& proposal) {
    const auto execution = admission.value("execution", json(nullptr)); validate_execution(execution);
    if (execution.is_null()) { need(e.is_null(), "emission without routed admission"); return; }
    need(e.is_object() && e.size() == 8 && e.value("schema", "") == "cogg:emission/v1" &&
        e.value("attempt", "") == id && e.at("subject") == admission.at("subject") &&
        e.at("parent") == admission.at("parent") && e.at("occasion") == admission.at("occasion") &&
        e.at("execution") == execution && e.at("proposal") == proposal,
        "emission admission binding mismatch");
    need(e.at("provider").is_object() && e.at("provider").dump().size() <= 8192, "invalid provider receipt");
}
json make_emission(const Attempt& a, const json& execution, const Proposal& p, const json& provider) {
    return {{"schema", "cogg:emission/v1"}, {"attempt", a.id}, {"subject", a.present.state.subject},
        {"parent", a.present.state.head}, {"occasion", a.present.occasion.id}, {"execution", execution},
        {"proposal", proposal_json(p)}, {"provider", provider}};
}
Registry::Registry(std::size_t max_sessions) : max_sessions_(max_sessions) {
    need(max_sessions > 0 && max_sessions <= 1024, "invalid session limit");
}
void Registry::add(std::string id, Capabilities caps, Factory factory) {
    need(!id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    }), "invalid executor id");
    need(bool(factory) && entries_.size() < 128 && !entries_.count(id), "duplicate or invalid executor");
    entries_.emplace(std::move(id), Entry{caps, std::move(factory), true});
}
void Registry::enable(const std::string& id, bool enabled) {
    need(entries_.count(id) != 0, "unknown executor"); entries_.at(id).enabled = enabled;
}
void Registry::evict(const std::string& subject, const std::string& id) { sessions_.erase({subject, id}); }
json Registry::describe() const {
    json result = json::object();
    for (const auto& [id, e] : entries_) result[id] = {{"enabled", e.enabled}, {"capabilities", capabilities_json(e.caps)}};
    return result;
}
Registry::Session& Registry::session(const std::string& subject, const std::string& id) {
    auto key = std::make_pair(subject, id);
    if (auto it = sessions_.find(key); it != sessions_.end()) return it->second;
    need(sessions_.size() < max_sessions_, "executor session limit; evict an idle session");
    auto& entry = entries_.at(id); auto backend = entry.factory();
    need(bool(backend), "executor factory returned no backend");
    json execution = {{"schema", "cogg:execution/v1"}, {"executor", id}, {"backend", backend->name()},
        {"session", session_id()}, {"capabilities", capabilities_json(entry.caps)}};
    validate_execution(execution);
    return sessions_.emplace(std::move(key), Session{std::move(backend), std::move(execution)}).first->second;
}
RoutedRuntime::RoutedRuntime(Store& store, Registry& registry, std::function<millis()> wall_clock)
    : store_(store), registry_(registry), wall_clock_(std::move(wall_clock)) {}
RouteResult RoutedRuntime::step(const std::string& subject, const Route& route, millis now) {
    need(now >= 0 && now <= std::numeric_limits<millis>::max() - 3600000 &&
         route.timeout_ms > 0 && route.timeout_ms <= 3600000 && route.attempt_timeout_ms > 0 &&
         route.attempt_timeout_ms <= route.timeout_ms && !route.executors.empty() && route.executors.size() <= 8,
         "invalid route limits");
    std::set<std::string> seen;
    for (const auto& id : route.executors)
        need(registry_.entries_.count(id) && seen.insert(id).second, "unknown or duplicate route executor");
    store_.verify(subject);
    const auto original = store_.snapshot(subject).head;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };
    auto wall = [&] { return wall_clock_ ? wall_clock_() : now + elapsed(); };
    auto cancelled = [&] { return route.cancelled && route.cancelled(); };
    RouteResult result; result.status = "exhausted";
    for (const auto& id : route.executors) {
        if (cancelled()) { result.status = "cancelled"; return result; }
        if (elapsed() >= route.timeout_ms) { result.status = "timeout"; return result; }
        const auto& entry = registry_.entries_.at(id); const auto& c = entry.caps;
        std::string skip;
        if (!entry.enabled) skip = "disabled";
        else if (c.remote && !route.allow_remote) skip = "remote_denied";
        else if ((route.require_exact_tokens && !c.exact_tokens) || (route.require_checkpoint && !c.checkpoint) ||
                 (route.require_cancellation && !c.cancellation)) skip = "capability_missing";
        if (!skip.empty()) { result.attempts.push_back({{"executor", id}, {"status", skip}}); continue; }
        Registry::Session* selected = nullptr;
        try { selected = &registry_.session(subject, id); }
        catch (const std::exception&) {
            result.attempts.push_back({{"executor", id}, {"status", "executor_unavailable"}}); continue;
        }
        auto& session = *selected; auto& backend = *session.backend;
        // Factory/model loading and all transport work are outside admission's write lock.
        if (cancelled()) { result.status = "cancelled"; return result; }
        if (elapsed() >= route.timeout_ms) { result.status = "timeout"; return result; }
        if (store_.snapshot(subject).head != original) { result.status = "conflict"; return result; }
        std::optional<Attempt> a;
        try {
            a = store_.admit(subject, backend.name(), wall(), backend.memory_policy(),
                [&](const Present& p) { return backend.context_fits(p); }, session.execution);
        } catch (const ContextOverflow&) {
            result.attempts.push_back({{"executor", id}, {"status", "context_overflow"}}); continue;
        }
        if (!a) { result.status = "waiting"; return result; }
        if (a->present.state.head != original) {
            store_.fail(a->id, "route head changed"); result.status = "conflict"; return result;
        }
        const auto attempt_start = elapsed();
        const auto limit = std::min(route.attempt_timeout_ms, route.timeout_ms - attempt_start);
        auto settled = [&](const std::string& status) {
            store_.fail(a->id, status); result.attempts.push_back({{"executor", id}, {"attempt", a->id}, {"status", status}});
        };
        Proposal proposal;
        std::string failure;
        try {
            if (limit <= 0) throw BackendFailure("timeout", FailureKind::timeout);
            proposal = backend.propose_attempt(*a, limit, cancelled);
            // Reject invalid model output before entering the commit transaction.
            (void)proposal_json(proposal);
        } catch (const BackendFailure& error) {
            switch (error.kind) {
                case FailureKind::transport: failure = "transport_failed"; break;
                case FailureKind::timeout: failure = "timeout"; break;
                case FailureKind::invalid_output: failure = "invalid_output"; break;
                case FailureKind::credentials: failure = "credentials_unavailable"; break;
                default: failure = "backend_failed";
            }
        } catch (const std::exception&) { failure = "backend_failed"; }
        if (!failure.empty()) {
            settled(failure);
            if (cancelled()) { result.status = "cancelled"; return result; }
            if (elapsed() >= route.timeout_ms) { result.status = "timeout"; return result; }
            continue;
        }
        const auto duration = elapsed() - attempt_start;
        if (cancelled()) { settled("cancelled"); result.status = "cancelled"; return result; }
        if (duration >= limit) { settled("timeout"); continue; }
        try {
            auto emission = make_emission(*a, session.execution, proposal, backend.telemetry());
            result.snapshot = store_.commit(a->id, proposal, wall(), {}, duration, emission);
        } catch (const Conflict&) { settled("conflict"); result.status = "conflict"; return result; }
        catch (...) { settled("commit_rejected"); throw; }
        result.attempts.push_back({{"executor", id}, {"attempt", a->id}, {"status", "committed"}});
        result.status = "committed";
        try { backend.committed(a->present, *result.snapshot); }
        catch (...) { result.maintenance_error = "backend maintenance failed"; }
        return result;
    }
    if (elapsed() >= route.timeout_ms) result.status = "timeout";
    return result;
}
} // namespace cogg
