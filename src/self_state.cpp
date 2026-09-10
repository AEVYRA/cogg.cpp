#include "cogg/self_state.hpp"
#include <chrono>
#include <map>
#include <limits>

namespace cogg {
namespace {
constexpr auto producer = "cogg:self/v1";
constexpr auto prefix = "self.commitment.";
constexpr auto contract =
    "Self state is host-verified application data, independent of the current model. "
    "Read profile and open commitments below on every attempt. Grants are permissions, not commands. "
    "To accept a create grant, emit a notes entry with its exact key/text, type task, status open. "
    "Commitment keys belong ONLY in notes; NEVER copy them into memory. Use memory:[] unless updating other allowed keys. "
    "To settle, emit the same key and ORIGINAL text, type task, and the granted status. "
    "Never change terms, reuse settled IDs, or mutate self.* without the matching grant. "
    "Profile changes use memory key self.profile, schema cogg:self/v1, revision + 1, and exact granted data. "
    "Use empty sources/covers for commitment notes. Without grants, answer using kind speech and no self writes. "
    "A closed status records authorized settlement; it does not prove a physical action occurred.";
void need(bool ok, const char* why) { if (!ok) throw Error(why); }
void fields(const json& j, std::initializer_list<const char*> names) {
    need(j.is_object() && j.size() == names.size(), "invalid self object fields");
    for (auto name : names) need(j.contains(name), "missing self field");
}
bool reserved(const std::string& key) { return key.starts_with("self."); }
bool commitment(const std::string& key) {
    if (!key.starts_with(prefix) || key.size() <= std::char_traits<char>::length(prefix) || key.size() > 128) return false;
    for (auto ch : key.substr(std::char_traits<char>::length(prefix)))
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    return true;
}
void data_valid(const json& data) {
    need(data.is_object() && data.dump().size() <= 4096, "self profile data must be an object of at most 4096 bytes");
}
void profile_valid(const json& p) {
    fields(p, {"schema", "revision", "data"});
    need(p.at("schema") == producer && p.at("revision").is_number_integer(), "invalid self profile schema/revision");
    const auto& revision = p.at("revision");
    need(revision >= 0 && revision < std::numeric_limits<std::int64_t>::max(), "self profile revision out of range");
    data_valid(p.at("data"));
}
struct State {
    json profile;
    std::string profile_version;
    std::map<std::string, json> tasks; // Includes settled IDs for non-reuse, never copied wholesale into a prompt.
};
json view_of(const State& state, const std::string& subject, const std::string& head) {
    json open = json::array();
    for (const auto& [key, task] : state.tasks) if (task.at("status") == "open") {
        auto item = task; item["key"] = key; open.push_back(std::move(item));
    }
    need(open.size() <= 16, "too many open self commitments");
    json view = {{"schema", "cogg:self-view/v1"}, {"subject", subject}, {"head", head},
        {"profile", state.profile}, {"profile_version", state.profile_version}, {"open", open},
        {"settled_count", state.tasks.size() - open.size()}};
    need(view.dump().size() <= 12288, "self view exceeds 12288 bytes");
    return view;
}
void grant_valid(const json& g, const json& view, const json& occasion) {
    fields(g, {"schema", "subject", "head", "occasion", "create", "settle", "profile"});
    need(g.at("schema") == "cogg:self-grant/v1" && g.at("subject") == view.at("subject") &&
        g.at("head") == view.at("head") && g.at("occasion") == occasion, "stale or foreign self grant");
    // Reuse the kernel's digest/binding validation (null denotes the due wake at this head).
    validate_admission_context({{"head", g.at("head")}, {"occasion", occasion}, {"inputs", json::array()}});
    need(g.at("create").is_object() && g.at("settle").is_object() &&
        g.at("create").size() + g.at("settle").size() <= 8, "invalid self grant size");
    for (const auto& [key, text] : g.at("create").items()) {
        need(commitment(key) && text.is_string() && !text.get_ref<const std::string&>().empty() &&
            text.get_ref<const std::string&>().size() <= 2048 && text.get_ref<const std::string&>().find('\0') == std::string::npos &&
            !g.at("settle").contains(key), "invalid commitment create grant");
    }
    for (const auto& [key, settlement] : g.at("settle").items()) {
        need(commitment(key), "invalid commitment key"); fields(settlement, {"version", "status"});
        need(settlement.at("status") == "closed" || settlement.at("status") == "retracted", "invalid settlement status");
        bool found = false;
        for (const auto& task : view.at("open")) if (task.at("key") == key) {
            need(settlement.at("version") == task.at("version"), "stale commitment version"); found = true;
        }
        need(found, "settlement requires an open commitment");
    }
    if (!g.at("profile").is_null()) data_valid(g.at("profile"));
    need(g.dump().size() <= 16384, "self grant too large");
}
void grant_unused_ids(const State& state, const json& g) {
    for (const auto& [key, unused] : g.at("create").items())
        need(!state.tasks.contains(key), "commitment ID already used");
}
bool protected_write(const Proposal& p) {
    for (const auto& w : p.memory) if (reserved(w.key)) return true;
    for (const auto& n : p.notes) if (reserved(n.key)) return true;
    return false;
}
void apply(State& state, const Proposal& p, const json& grant, const std::string& version) {
    for (const auto& w : p.memory) if (reserved(w.key)) {
        need(w.key == "self.profile", "reserved self memory key");
        need(!grant.at("profile").is_null(), "profile change not granted");
        profile_valid(w.value);
        need(w.value.at("data") == grant.at("profile") &&
            w.value.at("revision") == state.profile.at("revision").get<std::int64_t>() + 1, "profile change differs from grant");
        state.profile = w.value; state.profile_version = version;
    }
    for (const auto& n : p.notes) if (reserved(n.key)) {
        need(commitment(n.key) && n.type == "task" && n.sources.empty() && n.covers.empty(), "invalid self commitment note");
        const auto old = state.tasks.find(n.key);
        if (old == state.tasks.end()) {
            need(n.status == "open" && grant.at("create").contains(n.key) && grant.at("create").at(n.key) == n.text,
                 "commitment creation not granted");
        } else {
            need(old->second.at("status") == "open" && old->second.at("text") == n.text, "commitment terms immutable or ID settled");
            need(grant.at("settle").contains(n.key) && grant.at("settle").at(n.key).at("status") == n.status &&
                grant.at("settle").at(n.key).at("version") == old->second.at("version"), "commitment settlement not granted");
        }
        state.tasks[n.key] = {{"text", n.text}, {"status", n.status}, {"version", version}};
    }
}
json content_of(const json& view, const json& grant) {
    return {{"schema", producer}, {"contract", contract}, {"view", view}, {"grant", grant}};
}
State replay(Store& store, const std::string& subject, json& view) {
    auto timeline = store.timeline(subject);
    store.verify(subject);
    if (store.snapshot(subject).head != timeline.at("head").get<std::string>()) throw Conflict("self history changed during verification");
    State state;
    std::map<std::string, json> attempts;
    for (const auto& a : timeline.at("attempts")) attempts.emplace(a.at("id"), a.at("body"));
    std::string head;
    for (const auto& c : timeline.at("commits")) {
        const auto& b = c.at("body"); const auto id = c.at("id").get<std::string>();
        if (b.at("tick") == 0) {
            for (const auto& [key, value] : b.at("memory").items())
                need(!reserved(key) || key == "self.profile", "reserved genesis self key");
            need(b.at("memory").contains("self.profile"), "self profile must be initialized at genesis");
            state.profile = b.at("memory").at("self.profile"); profile_valid(state.profile);
            need(state.profile.at("revision") == 0, "genesis self revision must be zero"); state.profile_version = id;
        } else {
            const auto p = parse_proposal(b.at("proposal"));
            const auto& a = attempts.at(b.at("attempt").get<std::string>());
            json packet = nullptr;
            for (const auto& input : a.value("inputs", json::array())) if (input.at("body").at("producer") == producer) {
                need(packet.is_null(), "duplicate self input");
                need(input.at("body").at("kind") == "observation", "invalid self input kind");
                packet = input.at("body").at("content");
            }
            if (!packet.is_null()) {
                auto before = view_of(state, subject, head);
                const auto& g = packet.at("grant");
                const auto& admitted_occasion = a.at("temporal").at("admission").at("occasion").at("id");
                grant_valid(g, before, admitted_occasion); grant_unused_ids(state, g);
                fields(packet, {"schema", "contract", "view", "grant"});
                // The schema identifies the enforced policy; prompt wording is retained
                // verbatim in history but may improve between compatible host builds.
                need(packet.at("schema") == producer && packet.at("view") == before &&
                    packet.at("contract").is_string() && packet.at("contract").get_ref<const std::string&>().size() <= 2048,
                    "self admission view/contract mismatch");
                apply(state, p, g, id);
            } else need(!protected_write(p), "unguarded self mutation in history");
        }
        head = id; view = view_of(state, subject, head);
    }
    return state;
}
} // namespace
json self_profile(const json& data) {
    data_valid(data); return {{"schema", producer}, {"revision", 0}, {"data", data}};
}
json inspect_self(Store& store, const std::string& subject) {
    json view; (void)replay(store, subject, view); return view;
}
json self_grant(const json& view, const json& occasion, const json& create, const json& settle, const json& profile) {
    json grant = {{"schema", "cogg:self-grant/v1"}, {"subject", view.at("subject")}, {"head", view.at("head")},
        {"occasion", occasion}, {"create", create}, {"settle", settle}, {"profile", profile}};
    grant_valid(grant, view, occasion); return grant;
}
SelfResult SelfRuntime::step(const std::string& subject, millis now, const json& grant,
                            const json& inputs, millis timeout, const std::function<bool()>& cancelled) {
    need(timeout > 0 && timeout <= 3600000, "invalid self attempt timeout");
    validate_execution(execution_);
    need(!execution_.is_null() && execution_.at("backend") == backend_.name(), "self execution backend mismatch");
    validate_inputs(inputs);
    for (const auto& input : inputs) need(input.at("body").at("producer") != producer, "self input is host-generated");
    SelfResult result;
    if (cancelled && cancelled()) { result.status = "cancelled"; return result; }
    json view; auto state = replay(store_, subject, view);
    const auto schedule = store_.schedule(subject, now);
    if (schedule.at("occasion").is_null()) { result.status = "waiting"; return result; }
    const auto occasion = schedule.at("occasion").at("id");
    auto g = grant.is_null() ? self_grant(view, occasion) : grant;
    grant_valid(g, view, occasion); grant_unused_ids(state, g);
    auto packets = inputs; packets.push_back(make_input("observation", producer, content_of(view, g)));
    json context = {{"head", view.at("head")}, {"occasion", occasion}, {"inputs", packets}};
    auto a = store_.admit(subject, backend_.name(), now, backend_.memory_policy(),
        [&](const Present& p) { return backend_.context_fits(p); }, execution_, context);
    if (!a) { result.status = "waiting"; return result; }
    result.attempt = a->id;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };
    auto fail = [&](const char* status) { store_.fail(a->id, status); result.status = status; };
    Outcome outcome;
    const char* failure = nullptr;
    try { outcome = backend_.respond_attempt(*a, timeout, cancelled); (void)outcome_json(outcome); }
    catch (const BackendFailure& error) {
        switch (error.kind) {
            case FailureKind::transport: failure = "transport_failed"; break;
            case FailureKind::timeout: failure = "timeout"; break;
            case FailureKind::invalid_output: failure = "invalid_output"; break;
            case FailureKind::credentials: failure = "credentials_unavailable"; break;
            default: failure = "backend_failed";
        }
    }
    catch (...) { failure = "backend_failed"; }
    // Cancellation/deadline remain authoritative even when the backend throws.
    // Never persist an untrusted provider exception message.
    if (cancelled && cancelled()) { fail("cancelled"); return result; }
    const auto duration = elapsed();
    if (duration >= timeout) { fail("timeout"); return result; }
    if (failure) { fail(failure); return result; }
    result.outcome = outcome_json(outcome);
    if (const auto* p = std::get_if<Proposal>(&outcome)) {
        try { apply(state, *p, g, std::string(64, '0')); (void)view_of(state, subject, std::string(64, '0')); }
        catch (...) { fail("self_rejected"); return result; }
    }
    try {
        if (const auto* abstention = std::get_if<Abstention>(&outcome)) {
            store_.abstain(a->id, *abstention, backend_.telemetry()); result.status = "abstained"; return result;
        }
        const auto& p = std::get<Proposal>(outcome);
        // Store clamps commit wall against admission and monotonic inference duration.
        result.snapshot = store_.commit(a->id, p, now, {}, duration, make_emission(*a, execution_, p, backend_.telemetry()));
    } catch (const Conflict&) { fail("conflict"); return result; }
    catch (...) { fail("commit_rejected"); throw; }
    result.status = "committed";
    try { backend_.committed(a->present, *result.snapshot); }
    catch (...) { result.maintenance_error = "backend maintenance failed"; }
    return result;
}
} // namespace cogg
