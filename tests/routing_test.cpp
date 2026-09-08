#include "cogg/routing.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace cogg;
namespace {
void check(bool ok, const char* message) { if (!ok) throw Error(message); }
template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected, "expected rejection"); }
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("cogg-route-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::filesystem::remove_all(path); }
    std::string db() const { return (path / "test.db").string(); }
};
struct TestBackend : Backend {
    std::function<Proposal(const Present&)> fn;
    int calls = 0;
    explicit TestBackend(std::function<Proposal(const Present&)> f = {}) : fn(std::move(f)) {}
    std::string name() const override { return "test/v1"; }
    Proposal propose(const Present& p) override {
        ++calls;
        if (fn) return fn(p);
        Proposal result; result.memory = {{"session_calls", calls}}; return result;
    }
};
void isolation_and_gates() {
    Temp t; Store store(t.db()); store.create("a", {1, 100, 1000}, 0); store.create("b", {1, 100, 1000}, 0);
    Registry registry(3); int created = 0;
    registry.add("local", {}, [&] { ++created; return std::make_unique<TestBackend>(); });
    registry.add("remote", {true}, [&] { ++created; return std::make_unique<TestBackend>(); });
    rejects([&] { registry.add("local", {}, [] { return std::make_unique<TestBackend>(); }); });
    RoutedRuntime runtime(store, registry); Route route; route.executors = {"remote", "local"};
    auto a = runtime.step("a", route, 1); check(a.snapshot && a.attempts.at(0).at("status") == "remote_denied", "remote gate failed");
    auto b = runtime.step("b", route, 1); check(created == 2 && b.snapshot->memory.at("session_calls") == 1, "subject sessions leaked");
    store.submit("a", "again", {}, 10); a = runtime.step("a", route, 10);
    check(a.snapshot->memory.at("session_calls") == 2, "session not reused");
    registry.evict("a", "local"); store.submit("a", "evicted", {}, 20);
    a = runtime.step("a", route, 20); check(a.snapshot->tick == 3 && a.snapshot->memory.at("session_calls") == 1, "eviction lost history");
    const auto before = store.timeline("a").at("attempts").size();
    store.submit("a", "gated", {}, 30); route.require_exact_tokens = true;
    auto gated = runtime.step("a", route, 30); check(gated.status == "exhausted" && store.timeline("a").at("attempts").size() == before, "capability gate spent attempts");
    route.require_exact_tokens = false; route.executors = {"missing"}; rejects([&] { runtime.step("a", route, 30); });
    store.verify("a"); store.verify("b");
}
void fallback_and_limits() {
    Temp t; Store store(t.db()); store.create("s", {1, 100, 1000}, 0, {{"promise", "keep"}});
    Registry registry;
    registry.add("broken", {}, [] { return std::make_unique<TestBackend>([](const Present&) -> Proposal {
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); throw Error("secret must not be logged"); }); });
    registry.add("good", {}, [] { return std::make_unique<TestBackend>(); });
    Route route; route.executors = {"broken", "good"}; RoutedRuntime runtime(store, registry);
    auto result = runtime.step("s", route, 1);
    check(result.snapshot && result.snapshot->tick == 1 && result.snapshot->memory.at("promise") == "keep", "fallback failed");
    auto trace = store.timeline("s"); check(trace.at("attempts").size() == 2 && trace.dump().find("secret") == std::string::npos, "failure accounting or redaction failed");
    check(trace.at("attempts").at(0).at("body").at("occasion") == trace.at("attempts").at(1).at("body").at("occasion"), "fallback changed occasion");
    store.verify("s");
    store.create("limited", {100, 1, 1000}, 0); result = runtime.step("limited", route, 1);
    check(result.status == "waiting" && store.timeline("limited").at("attempts").size() == 1, "fallback bypassed quota");
    store.verify("limited");
}
void binding_and_late_results() {
    Temp t; Store store(t.db()); store.create("s", {1, 100, 1000}, 0);
    json execution = {{"schema", "cogg:execution/v1"}, {"executor", "local"}, {"backend", "test/v1"}, {"session", "session"}, {"capabilities", capabilities_json({})}};
    auto a = *store.admit("s", "test/v1", 1, {}, {}, execution); Proposal p;
    auto emission = make_emission(a, execution, p);
    for (const auto* key : {"attempt", "parent", "subject", "occasion"}) {
        auto bad = emission; bad[key] = "foreign"; rejects([&] { store.commit(a.id, p, 2, {}, {}, bad); });
    }
    rejects([&] { store.commit(a.id, p, 2); });
    auto bad = emission; bad["proposal"]["kind"] = "speech"; rejects([&] { store.commit(a.id, p, 2, {}, {}, bad); });
    store.commit(a.id, p, 2, {}, {}, emission); rejects([&] { store.commit(a.id, p, 2, {}, {}, emission); }); store.verify("s");
    store.submit("s", "slow", {}, 10); Registry registry;
    registry.add("slow", {}, [] { return std::make_unique<TestBackend>([](const Present&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15)); return Proposal{}; }); });
    RoutedRuntime runtime(store, registry); Route route; route.executors = {"slow"}; route.timeout_ms = 5; route.attempt_timeout_ms = 5;
    auto result = runtime.step("s", route, 10); check(result.status == "timeout" && store.snapshot("s").tick == 1, "late backend committed");
    route.cancelled = [] { return true; }; result = runtime.step("s", route, 40); check(result.status == "cancelled", "cancellation ignored"); store.verify("s");
}
void context_and_factory_fallback() {
    Temp t; Store store(t.db()); store.create("s", {1, 100, 1000}, 0); Registry registry;
    struct TooSmall : TestBackend {
        std::optional<MemoryPolicy> memory_policy() const override { return MemoryPolicy{}; }
        bool context_fits(const Present&) const override { return false; }
    };
    registry.add("unavailable", {}, []() -> std::unique_ptr<Backend> { throw Error("model missing"); });
    registry.add("small", {}, [] { return std::make_unique<TooSmall>(); });
    registry.add("good", {}, [] { return std::make_unique<TestBackend>(); });
    RoutedRuntime runtime(store, registry); Route route; route.executors = {"unavailable", "small", "good"};
    auto result = runtime.step("s", route, 1);
    check(result.snapshot && result.attempts.size() == 3 && result.attempts[0]["status"] == "executor_unavailable" &&
        result.attempts[1]["status"] == "context_overflow" && store.timeline("s")["attempts"].size() == 1,
        "preflight fallback failed or spent inference quota"); store.verify("s");
}
void conflicting_writer() {
    Temp t; Store store(t.db()); store.create("s", {1, 100, 1000}, 0); Registry registry;
    registry.add("race", {}, [&] { return std::make_unique<TestBackend>([&](const Present&) {
        Store other(t.db()); auto a = other.admit("s", "competitor", 100); check(bool(a), "competitor not admitted");
        other.commit(a->id, Proposal{}, 101); return Proposal{}; }); });
    registry.add("unused", {}, [] { throw Error("fallback must not run after conflict"); return std::unique_ptr<Backend>{}; });
    RoutedRuntime runtime(store, registry); Route route; route.executors = {"race", "unused"};
    auto result = runtime.step("s", route, 1); check(result.status == "conflict" && store.snapshot("s").tick == 1, "competing head overwritten"); store.verify("s");
}
void open_task_capacity_fallback() {
    Temp t; Store store(t.db()); store.create("s", {1, 100, 1000}, 0);
    auto seed = *store.admit("s", "fixture", 0); Proposal tasks;
    tasks.notes = {{"first", "Keep first obligation.", "task", "open"}, {"second", "Keep second obligation.", "task", "open"}};
    store.commit(seed.id, tasks, 0); store.submit("s", "continue", {}, 10);
    struct Bounded : TestBackend {
        std::size_t items;
        int& invoked;
        Bounded(std::size_t limit, int& calls) : items(limit), invoked(calls) {}
        std::optional<MemoryPolicy> memory_policy() const override { MemoryPolicy p; p.max_items = items; return p; }
        Proposal propose(const Present& p) override {
            ++invoked;
            check(p.memory_view.at("items").size() == 2, "fallback dropped an obligation");
            return Proposal{};
        }
    };
    int small_calls = 0, large_calls = 0; Registry registry;
    registry.add("small", {}, [&] { return std::make_unique<Bounded>(1, small_calls); });
    registry.add("large", {}, [&] { return std::make_unique<Bounded>(4, large_calls); });
    RoutedRuntime runtime(store, registry); Route route; route.executors = {"small", "large"};
    const auto before = store.timeline("s").at("attempts").size();
    auto result = runtime.step("s", route, 10);
    check(result.status == "committed" && result.attempts[0]["status"] == "context_overflow", "task capacity prevented fallback");
    check(small_calls == 0 && large_calls == 1 && store.timeline("s").at("attempts").size() == before + 1, "capacity failure spent inference quota");
    store.verify("s");
}
}
int main() { try { open_task_capacity_fallback(); isolation_and_gates(); fallback_and_limits(); binding_and_late_results(); context_and_factory_fallback(); conflicting_writer(); std::cout << "routing invariants passed\n"; }
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
