#include "cogg/self_state.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace cogg;
namespace {
void check(bool ok, const char* why) { if (!ok) throw Error(why); }
template<class F> void rejects(F f) { bool bad = false; try { f(); } catch (const std::exception&) { bad = true; } check(bad, "expected rejection"); }
struct Temp {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("cogg-self-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(dir); }
    ~Temp() { std::filesystem::remove_all(dir); }
    std::string db() const { return (dir / "s.db").string(); }
};
struct Organ : Backend {
    std::function<Outcome(const Present&)> fn;
    bool fits = true;
    explicit Organ(std::function<Outcome(const Present&)> f) : fn(std::move(f)) {}
    std::string name() const override { return "fixture-organ/v1"; }
    Proposal propose(const Present&) override { throw Error("wrong entry"); }
    Outcome respond_attempt(const Attempt& a, millis, const std::function<bool()>&) override { return fn(a.present); }
    bool context_fits(const Present&) const override { return fits; }
    std::optional<MemoryPolicy> memory_policy() const override { MemoryPolicy p; p.max_bytes = 8192; p.max_items = 1; return p; }
};
json execution(const Organ& b, const std::string& session = "first") {
    return {{"schema", "cogg:execution/v1"}, {"executor", "fixture"}, {"backend", b.name()}, {"session", session}, {"capabilities", capabilities_json({})}};
}
constexpr auto key = "self.commitment.telescope";
constexpr auto terms = "Return Mira's telescope after cleaning the lens.";
Proposal note(const std::string& text = terms, const std::string& status = "open") {
    Proposal p; p.notes.push_back({key, text, "task", status, {}, {}}); return p;
}
json grant(Store& s, const json& create = json::object(), const json& settle = json::object(), const json& profile = nullptr) {
    return self_grant(inspect_self(s, "s"), s.schedule("s", 100).at("occasion").at("id"), create, settle, profile);
}
void create(Store& s) { s.create("s", {1, 100, 1000}, 0, {{"self.profile", self_profile({{"name", "Rin"}, {"role", "assistant"}})}}); }
void continuity() {
    Temp t; std::string original_version;
    {
        Store s(t.db()); create(s);
        Organ b([](const Present& p)->Outcome {
            check(p.inputs.size() == 1, "missing self view"); return note();
        }); SelfRuntime r(s, b, execution(b));
        auto out = r.step("s", 0, grant(s, {{key, terms}})); check(out.status == "committed", "creation failed");
        original_version = out.snapshot->head;
    }
    {
        Store s(t.db()); auto before = inspect_self(s, "s");
        check(before.at("open")[0].at("version") == original_version, "version lost on restart");
        s.submit("s", "next", {{"text", "What is next?"}}, 10);
        Organ b([](const Present& p)->Outcome {
            const auto& view = p.inputs.back().at("body").at("content").at("view");
            check(view.at("profile").at("data").at("name") == "Rin" && view.at("open")[0].at("text") == terms, "self context was evicted");
            Proposal p2; p2.kind = "speech"; p2.text = "Clean the lens and return Mira's telescope.";
            p2.memory.push_back({"ordinary", 42}); return p2;
        }); SelfRuntime r(s, b, execution(b, "replacement"));
        check(r.step("s", 10).status == "committed", "replacement failed");
        auto after = inspect_self(s, "s"); check(after.at("profile") == before.at("profile") && after.at("open") == before.at("open"), "replacement changed self");
        auto timeline = s.timeline("s");
        check(timeline.at("commits").back().at("body").at("emission").at("execution").at("session") == "replacement", "execution provenance lost");
        s.submit("s", "ack", {{"text", "Return confirmed (fixture)"}}, 20);
        auto close_grant = grant(s, json::object(), {{key, {{"version", original_version}, {"status", "closed"}}}});
        b.fn = [](const Present&)->Outcome { return note(terms, "closed"); };
        check(r.step("s", 20, close_grant).status == "committed", "authorized settlement failed");
        check(inspect_self(s, "s").at("open").empty() && inspect_self(s, "s").at("settled_count") == 1, "settlement lost");
        s.submit("s", "again", {}, 30);
        rejects([&] { r.step("s", 30, grant(s, {{key, terms}})); });
        check(r.step("s", 30).status == "self_rejected", "settled ID reused");
        s.verify("s");
    }
}
void guards() {
    Temp t; Store s(t.db()); create(s); Organ b([](const Present&)->Outcome { return note(); }); SelfRuntime r(s, b, execution(b));
    const auto original = s.snapshot("s").head;
    check(r.step("s", 0).status == "self_rejected" && s.snapshot("s").head == original, "ungranted creation committed");
    auto g = grant(s, {{key, terms}});
    for (const auto* field : {"subject", "head", "occasion"}) {
        auto bad = g; bad[field] = "foreign";
        const auto count = s.timeline("s").at("attempts").size();
        rejects([&] { r.step("s", 2, bad); }); check(s.timeline("s").at("attempts").size() == count, "invalid grant charged quota");
    }
    check(r.step("s", 3, g).status == "committed", "retry did not preserve event");
    s.submit("s", "tamper", {}, 10); const auto head = s.snapshot("s").head;
    millis at = 20;
    for (const auto& p : {note("Return something else", "closed"), note(terms, "closed"), note(terms, "retracted"), note()}) {
        b.fn = [p](const Present&)->Outcome { return p; };
        check(r.step("s", at++).status == "self_rejected" && s.snapshot("s").head == head, "unauthorized promise mutation");
    }
    const auto view = inspect_self(s, "s");
    rejects([&] { grant(s, json::object(), {{key, {{"version", "wrong"}, {"status", "closed"}}}}); });
    auto close = grant(s, json::object(), {{key, {{"version", view.at("open")[0].at("version")}, {"status", "closed"}}}});
    b.fn = [](const Present&)->Outcome { return note("changed terms", "closed"); };
    check(r.step("s", 30, close).status == "self_rejected", "grant allowed different terms");
    at = 40;
    for (const auto* alias : {"self.profile", "self.commitment.telescope", "self.unknown"}) {
        b.fn = [alias](const Present&)->Outcome { Proposal p; p.memory.push_back({alias, nullptr}); return p; };
        check(r.step("s", at++).status == "self_rejected", "reserved memory alias accepted");
    }
    b.fn = [](const Present&)->Outcome { Proposal p; p.notes.push_back({"self.profile", "shadow"}); return p; };
    check(r.step("s", 50).status == "self_rejected", "profile note shadow accepted");
    const auto replacement = json{{"role", "librarian"}};
    auto profile_grant = grant(s, json::object(), json::object(), replacement);
    b.fn = [replacement](const Present&)->Outcome { Proposal p; auto profile = self_profile(replacement); profile["revision"] = 1; p.memory.push_back({"self.profile", profile}); return p; };
    check(r.step("s", 60, profile_grant).status == "committed", "profile revision failed");
    check(inspect_self(s, "s").at("profile").at("data") == replacement, "profile data lost"); s.verify("s");
}
void failures_and_bypass() {
    Temp t; Store s(t.db()); create(s); Organ b([](const Present&)->Outcome { return Proposal{}; }); SelfRuntime r(s, b, execution(b));
    b.fits = false;
    rejects([&] { r.step("s", 0); }); check(s.timeline("s").at("attempts").empty(), "overflow admitted"); b.fits = true;
    b.fn = [](const Present&)->Outcome { return Abstention{"uncertain", "fixture"}; };
    const auto head = s.snapshot("s").head; check(r.step("s", 0).status == "abstained", "abstention lost");
    check(s.snapshot("s").head == head && inspect_self(s, "s").at("open").empty(), "abstention mutated self");
    b.fn = [](const Present&)->Outcome { throw Error("fixture"); };
    check(r.step("s", 2).status == "backend_failed", "failure unsettled");
    b.fn = [](const Present&)->Outcome { std::this_thread::sleep_for(std::chrono::milliseconds(3)); return Proposal{}; };
    check(r.step("s", 4, nullptr, json::array(), 1).status == "timeout" && s.snapshot("s").head == head, "late result committed");
    check(r.step("s", 6, nullptr, json::array(), 100, [] { return true; }).status == "cancelled", "cancel ignored");
    b.fn = [&](const Present&)->Outcome {
        Store other(t.db()); auto a = other.admit("s", "other", 10); other.commit(a->id, Proposal{}, 10); return Proposal{};
    };
    check(r.step("s", 8).status == "conflict", "competing writer overwritten"); inspect_self(s, "s");
    s.submit("s", "bypass", {}, 20);
    auto a = s.admit("s", "unguarded", 20); s.commit(a->id, note(), 20); s.verify("s");
    rejects([&] { inspect_self(s, "s"); }); // Core validity does not imply optional policy validity.
}
void wake_binding() {
    Temp t; Store s(t.db()); create(s);
    Organ b([](const Present&)->Outcome { auto p = note(); p.wake_after_ms = 10; return p; });
    SelfRuntime r(s, b, execution(b));
    auto created = r.step("s", 0, grant(s, {{key, terms}})); check(created.status == "committed", "wake setup failed");
    const auto due = *created.snapshot->wake_at;
    const auto view = inspect_self(s, "s");
    const auto g = self_grant(view, nullptr);
    b.fn = [](const Present& p)->Outcome {
        check(p.occasion.kind == "scheduled" && !p.occasion.id.empty(), "wake was not materialized");
        check(p.inputs.back().at("body").at("content").at("view").at("open")[0].at("text") == terms, "wake lost commitment");
        return Proposal{};
    };
    check(r.step("s", due - 1, g).status == "waiting", "wake admitted early");
    check(r.step("s", due, g).status == "committed", "bound wake failed"); inspect_self(s, "s");
    s.submit("s", "external", {}, due + 10);
    const auto count = s.timeline("s").at("attempts").size();
    rejects([&] { r.step("s", due + 10, self_grant(inspect_self(s, "s"), nullptr)); });
    check(s.timeline("s").at("attempts").size() == count, "null occasion acted as wildcard");
}
}
int main() {
    try { continuity(); guards(); failures_and_bypass(); wake_binding(); std::cout << "self-state contract passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
