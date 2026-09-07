#include "cogg/llama_backend.hpp"
#include <filesystem>
#include <iostream>

using namespace cogg;
namespace {
void check(bool ok, const char* message) { if (!ok) throw Error(message); }
template<class F> void rejects(F f, const std::string& expected) {
    try { f(); } catch (const std::exception& e) {
        check(std::string(e.what()).find(expected) != std::string::npos, e.what()); return;
    }
    throw Error("operation unexpectedly succeeded");
}
void options() {
    LlamaOptions o;
    o.context_tokens = 0;
    rejects([&] { LlamaBackend b(o); }, "invalid inference limits");
    o = {}; o.max_output_tokens = o.context_tokens;
    rejects([&] { LlamaBackend b(o); }, "invalid inference limits");
    o = {}; o.threads = 0;
    rejects([&] { LlamaBackend b(o); }, "invalid inference limits");
    o = {}; o.model_path = "/nonexistent/cogg-test-model.gguf";
    rejects([&] { LlamaBackend b(o); }, "cannot open GGUF");
}
void inference(const std::string& path) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("cogg-llama-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove_all(p); } } cleanup{dir};
    const auto db = (dir / "subject.db").string();
    LlamaOptions o; o.model_path = path; o.timeout_ms = 60000;
    bool cancelled = false;
    o.cancelled = [&] { return cancelled; };
    LlamaBackend b(o);
    check(b.name().size() <= 200, "backend identity cannot be admitted");
    Store store(db); store.create("s", {1, 100, 3600000}, 1000);
    auto a = store.admit("s", b.name(), 1000);
    check(a.has_value(), "missing initial occasion");
    std::cout << "checking cold/hot generation" << std::endl;
    auto proposal = b.propose(a->present);
    const auto prompt_tokens = b.stats().prompt_tokens;
    check(b.stats().generated_tokens > 0 && b.stats().reused_tokens == 0, "no cold inference");
    auto hot = b.propose(a->present);
    check(b.stats().reused_tokens > 0, "hot context not reused");
    check(b.stats().reused_tokens == b.stats().prompt_tokens - 1, "identical prompt prefix mismatch");
    std::cout << "cold proposal=" << proposal_json(proposal).dump() << '\n'
              << "hot proposal=" << proposal_json(hot).dump() << std::endl;
    // An uncommitted result, followed by cache eviction, must not become memory.
    check(store.snapshot("s").tick == 0, "backend mutated durable time");
    b.release_context();
    auto cold = b.propose(a->present);
    check(b.stats().reused_tokens == 0, "context release did not force reconstruction");
    check(b.stats().prompt_tokens == prompt_tokens, "reconstruction changed canonical prompt size");
    std::cout << "reconstructed proposal=" << proposal_json(cold).dump() << std::endl;
    // Host-authored fixture seeds an explicit obligation for a scheduled model wake.
    Proposal seed; seed.memory = {{"task", "On waking, say hello briefly, then wait."}};
    seed.wake_after_ms = 1;
    auto s = store.commit(a->id, seed, 1000);
    std::cout << "checking scheduled wake" << std::endl;
    check(s.tick == 1 && s.wake_at == 1001, "seed did not schedule wake");
    auto wake = store.admit("s", b.name(), 1001);
    check(wake && wake->present.occasion.kind == "scheduled", "scheduled occasion lost");
    auto waking = b.propose(wake->present);
    check(b.stats().reused_tokens > 0, "changed canonical present lost whole hot prefix");
    const auto woke = store.commit(wake->id, waking, 1001); store.verify("s");
    check(store.record(woke.head).at("proposal") == proposal_json(waking), "committed output mismatch");
    // Recovery through a new Store and a new backend, not just cache reset.
    {
        std::cout << "checking fresh backend recovery" << std::endl;
        Store reopened(db); reopened.verify("s");
        const auto before = reopened.snapshot("s");
        reopened.submit("s", "after-restart", {{"text", "Say hello briefly."}}, 100000);
        LlamaBackend replacement(o);
        Runtime runtime(reopened, replacement);
        auto resumed = runtime.step("s", 100000);
        check(resumed && resumed->tick == before.tick + 1, "restart did not continue subject");
        check(replacement.stats().reused_tokens == 0, "new backend inherited another cache");
        reopened.verify("s");
    }
    auto alien = a->present; alien.state.subject = "another";
    std::cout << "checking subject isolation and failure limits" << std::endl;
    b.propose(alien);
    check(b.stats().reused_tokens == 0, "foreign subject reused context");
    auto oversized = a->present; oversized.state.memory["big"] = std::string(40000, 'x');
    rejects([&] { b.propose(oversized); }, "exceeds context budget");
    cancelled = true;
    store.submit("s", "cancel-test", {{"text", "hello"}}, 200000);
    auto before = store.snapshot("s");
    Runtime runtime(store, b);
    rejects([&] { runtime.step("s", 200000); }, "cancelled");
    check(store.snapshot("s").head == before.head, "cancelled inference committed");
    auto after = store.admit("s", "retry", 200001);
    check(after.has_value(), "failed attempt consumed occasion");
    store.fail(after->id, "test done"); store.verify("s");
    cancelled = false;
    { // The observation protocol forbids speech without converting it into null.
        auto internal_options = o; internal_options.internal_only = true;
        LlamaBackend internal(internal_options);
        check(internal.name() != b.name(), "internal-only policy missing from identity");
        auto internal_present = a->present;
        internal_present.state.memory = {{"task", "Reflect briefly, then wait. Save one short note."}};
        const auto choice = internal.propose(internal_present);
        check(choice.kind != "speech" && choice.text.empty(), "internal-only grammar emitted speech");
    }
    o.max_output_tokens = 1;
    LlamaBackend tiny(o);
    rejects([&] { tiny.propose(a->present); }, "output token limit");
    o.max_output_tokens = 512; o.timeout_ms = 1;
    LlamaBackend deadline(o);
    rejects([&] { deadline.propose(a->present); }, "deadline exceeded");
    std::cout << "PASS real inference: hot/cold, eviction, scheduled wake, restart, isolation, limits, cancellation\n";
}
}
int main(int argc, char** argv) {
    try { options(); if (argc == 2) inference(argv[1]); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
