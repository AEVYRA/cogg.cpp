#include "cogg/kernel.hpp"
#include <sqlite3.h>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace cogg;
namespace {
void check(bool ok, const char* msg) { if (!ok) throw Error(msg); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "operation unexpectedly accepted");
}
struct Temp {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("cogg-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(dir); }
    ~Temp() { std::filesystem::remove_all(dir); }
    std::string path() const { return (dir / "subject.db").string(); }
};
Attempt admitted(Store& s, millis now, const std::string& subject = "s") {
    auto a = s.admit(subject, "test/v1", now); check(a.has_value(), "expected admission"); return *a;
}
void mutate(const std::string& path, const char* query) {
    sqlite3* db = nullptr;
    check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "test DB open failed");
    const int result = sqlite3_exec(db, query, nullptr, nullptr, nullptr);
    sqlite3_close(db); check(result == SQLITE_OK, "test DB mutation failed");
}
void restart_and_atomic_state() {
    Temp t; std::string first_head;
    {
        Store s(t.path()); s.create("s", {10, 10, 1000}, 1000);
        auto a = admitted(s, 1000);
        Proposal p; p.memory = {{"promise", "keep this"}}; p.wake_after_ms = 0;
        auto out = s.commit(a.id, p, 1000);
        check(out.tick == 1 && out.wake_at == 1010, "wake clamp or tick failed");
        first_head = out.head;
        rejects([&] { s.commit(a.id, p, 1000); });
        s.verify("s");
    }
    {
        Store s(t.path()); s.verify("s");
        auto snap = s.snapshot("s");
        check(snap.head == first_head && snap.memory.at("promise") == "keep this", "restart lost state");
        check(!s.admit("s", "test", 1009), "early wake");
        auto a = admitted(s, 1010);
        check(a.present.occasion.kind == "scheduled", "wake did not survive restart");
        auto out = s.commit(a.id, Proposal{}, 1010);
        check(out.tick == 2 && !out.wake_at, "null tick failed to stop waking");
        check(!s.admit("s", "test", 3000), "unrequested wake");
        s.verify("s");
    }
}
void queue_idempotency_and_replacement() {
    Temp t; Store s(t.path()); s.create("s", {1, 100, 1000}, 0);
    s.commit(admitted(s, 0).id, Proposal{}, 0);
    auto id = s.submit("s", "request-1", {{"text", "hello"}}, 1);
    check(id == s.submit("s", "request-1", {{"text", "hello"}}, 2), "duplicate input");
    rejects([&] { s.submit("s", "request-1", {{"text", "different"}}, 2); });
    {
        Store resumed(t.path());
        auto a = resumed.admit("s", "replacement-model/v2", 2);
        check(a && a->present.state.tick == 1 && a->present.occasion.id == id, "replacement lost chain");
        Proposal p; p.kind = "speech"; p.text = "hello";
        resumed.commit(a->id, p, 2); resumed.verify("s");
    }
    check(!s.admit("s", "test", 20), "consumed event replayed");
}
void attempts_survive_failure_restart_and_clock_rollback() {
    Temp t;
    {
        Store s(t.path()); s.create("s", {10, 2, 100}, 1000);
        auto a = admitted(s, 1000); s.fail(a.id, "backend failed");
        check(!s.admit("s", "test", 1009), "failure bypassed rate floor");
        admitted(s, 1010); // unresolved inference, as after a process crash
    }
    {
        Store s(t.path());
        check(!s.admit("s", "test", 999), "clock rollback refunded quota");
        check(!s.admit("s", "test", 1099), "restart refunded quota");
        auto a = admitted(s, 1100);
        check(a.present.prior_unsettled_attempt, "uncertain predecessor hidden");
        check(s.snapshot("s").tick == 0, "admission advanced subject tick");
        s.commit(a.id, Proposal{}, 1100); s.verify("s");
    }
}
void floor_longer_than_budget_window() {
    Temp t; Store s(t.path()); s.create("s", {100, 2, 10}, 0);
    auto a = admitted(s, 0); s.fail(a.id, "failed");
    check(!s.admit("s", "test", 11), "expired budget window bypassed minimum interval");
    check(s.admit("s", "test", 100).has_value(), "minimum interval never expires");
}
void concurrent_successor() {
    Temp t; Store s(t.path()); s.create("s", {1, 10, 1000}, 0);
    const auto a = admitted(s, 0), b = admitted(s, 1);
    Store left(t.path()), right(t.path());
    std::barrier gate(2); std::atomic<int> won{0}, lost{0};
    auto work = [&](Store& db, std::string id) {
        gate.arrive_and_wait();
        try { db.commit(id, Proposal{}, 2); ++won; }
        catch (const Conflict&) { ++lost; }
    };
    std::thread t1(work, std::ref(left), a.id), t2(work, std::ref(right), b.id);
    t1.join(); t2.join();
    check(won == 1 && lost == 1 && s.snapshot("s").tick == 1, "double successor");
    s.verify("s");
}
void untrusted_transition_and_atomic_rejection() {
    rejects([] { parse_proposal({{"kind", "null"}, {"tick", 9000}}); });
    rejects([] { parse_proposal({{"kind", "tool"}, {"text", "run shell"}}); });
    rejects([] { parse_proposal({{"kind", "null"}, {"wake_after_ms", -1}}); });
    rejects([] { parse_proposal({{"kind", "null"}, {"wake_after_ms", 1.5}}); });
    rejects([] { parse_proposal({{"kind", "null"}, {"wake_after_ms", UINT64_MAX}}); });
    Temp t; Store s(t.path()); s.create("s", {1, 10, 1000}, 0);
    const auto a = admitted(s, 0);
    Proposal bad; bad.memory = {{"same", 1}, {"same", 2}};
    rejects([&] { s.commit(a.id, bad, 0); });
    check(s.snapshot("s").tick == 0 && s.snapshot("s").memory.empty(), "invalid commit leaked state");
    s.commit(a.id, Proposal{}, 0); s.verify("s");
}
void rollback_before_sql_commit() {
    Temp t; Store s(t.path()); s.create("s", {1, 10, 1000}, 0);
    auto a = admitted(s, 0); Proposal p; p.memory = {{"x", 10}}; p.wake_after_ms = 25;
    rejects([&] { s.commit(a.id, p, 0, [](CommitPoint stage) {
        if (stage == CommitPoint::before_sql_commit) throw Error("injected rollback");
    }); });
    check(s.snapshot("s").tick == 0 && s.snapshot("s").memory.empty() && !s.snapshot("s").wake_at,
          "partial commit leaked");
    s.commit(a.id, p, 0); s.verify("s");
}
void corrupt_projection_fails_verification() {
    Temp t; Store s(t.path()); s.create("s", {1, 10, 1000}, 0);
    s.commit(admitted(s, 0).id, Proposal{}, 0);
    mutate(t.path(), "UPDATE subjects SET memory='{\"invented\":true}'");
    rejects([&] { s.verify("s"); });
}
void corrupt_history_fails_verification() {
    Temp t; Store s(t.path()); s.create("s", {1, 10, 1000}, 0);
    s.commit(admitted(s, 0).id, Proposal{}, 0);
    mutate(t.path(), "UPDATE commits SET body=json_set(body,'$.committed_at',5) WHERE tick=1");
    rejects([&] { s.verify("s"); });
}
void subject_isolation() {
    Temp t; Store s(t.path()); s.create("s", {1, 1, 1000}, 0); s.create("other", {1, 1, 1000}, 0);
    Proposal p; p.memory = {{"private", "s"}};
    s.commit(admitted(s, 0).id, p, 0);
    auto a = admitted(s, 0, "other");
    check(a.present.state.memory.empty(), "foreign memory leaked");
    s.commit(a.id, Proposal{}, 0); s.verify("s"); s.verify("other");
}
class BrokenBackend : public Backend {
public:
    int calls = 0;
    std::string name() const override { return "broken"; }
    Proposal propose(const Present&) override { ++calls; throw Error("inference failed"); }
};
void runtime_failure_and_corruption_gate() {
    Temp t; Store s(t.path()); s.create("s", {1, 1, 10000}, 0);
    BrokenBackend backend; Runtime runtime(s, backend);
    rejects([&] { runtime.step("s", 0); });
    check(!runtime.step("s", 1) && backend.calls == 1, "failed backend bypassed budget");
    check(s.snapshot("s").tick == 0, "failed inference became a null commit");
    mutate(t.path(), "UPDATE subjects SET memory='{\"forged\":true}'");
    Runtime resumed(s, backend);
    rejects([&] { resumed.step("s", 10001); });
    check(backend.calls == 1, "corrupt recovery invoked backend");
}
void thousands_of_ticks() {
    Temp t; Store s(t.path()); s.create("s", {1, 5000, 10000}, 0);
    Proposal p; p.wake_after_ms = 0;
    for (int i = 0; i < 2000; ++i) {
        p.memory = {{"counter", i + 1}};
        s.commit(admitted(s, i).id, p, i);
    }
    s.verify("s");
    check(s.snapshot("s").tick == 2000 && s.snapshot("s").memory.at("counter") == 2000,
          "long chain failed");
}
void maintenance_after_commit() {
    Temp t; Store s(t.path()); s.create("s", {1, 100, 1000}, 0);
    struct CacheFailure : Backend {
        Store& store; int callbacks = 0;
        explicit CacheFailure(Store& s) : store(s) {}
        std::string name() const override { return "cache-failure"; }
        Proposal propose(const Present&) override { Proposal p; p.memory = {{"durable", true}}; return p; }
        void committed(const Present& before, const Snapshot& after) override {
            ++callbacks;
            check(after.tick == before.state.tick + 1 && store.snapshot("s").head == after.head,
                  "maintenance ran before durable commit");
            throw Error("cache disk full");
        }
    } backend(s);
    Runtime runtime(s, backend);
    auto result = runtime.step("s", 0);
    check(result && result->tick == 1 && result->memory.at("durable") == true, "cache failure hid durable commit");
    check(runtime.maintenance_error() == "cache disk full", "maintenance failure not observable");
    check(s.timeline("s").at("attempts").at(0).at("status") == "committed", "cache error marked inference failed");
    check(!runtime.step("s", 1) && backend.callbacks == 1 && runtime.maintenance_error().empty(), "cache failure retried subject act");
    s.verify("s");
    s.submit("s", "next", {{"text", "next"}}, 2);
    struct Superseded : Backend {
        Store& store; int callbacks = 0;
        explicit Superseded(Store& s) : store(s) {}
        std::string name() const override { return "loser"; }
        Proposal propose(const Present&) override {
            auto winning = store.admit("s", "winner", 3);
            check(winning.has_value(), "missing competing admission");
            store.commit(winning->id, Proposal{}, 3); return {};
        }
        void committed(const Present&, const Snapshot&) override { ++callbacks; }
    } loser(s);
    Runtime race(s, loser);
    check(!race.step("s", 2) && loser.callbacks == 0, "superseded proposal published cache");
    s.verify("s");
}
}
int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"restart/atomic memory/wake/null", restart_and_atomic_state},
        {"durable inbox/idempotency/backend replacement", queue_idempotency_and_replacement},
        {"failed/unsettled attempt budgets/restart/clock rollback", attempts_survive_failure_restart_and_clock_rollback},
        {"minimum interval beyond quota window", floor_longer_than_budget_window},
        {"two concurrent writers, one successor", concurrent_successor},
        {"untrusted proposal rejection", untrusted_transition_and_atomic_rejection},
        {"transaction rollback", rollback_before_sql_commit},
        {"corrupt projection", corrupt_projection_fails_verification},
        {"corrupt history", corrupt_history_fails_verification},
        {"subject isolation", subject_isolation},
        {"runtime failure budget and recovery verification", runtime_failure_and_corruption_gate},
        {"post-commit maintenance failure and stale proposal", maintenance_after_commit},
        {"2000 persistent ticks", thousands_of_ticks}
    };
    try {
        for (const auto& [name, run] : cases) { run(); std::cout << "PASS " << name << '\n'; }
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
