#include "cogg/llama_backend.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace cogg;
namespace {
void check(bool ok, const char* msg) { if (!ok) throw Error(msg); }
std::vector<char> bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void run(const std::string& model) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("cogg-real-checkpoint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    struct Cleanup { std::filesystem::path dir; ~Cleanup() { std::filesystem::remove_all(dir); } } cleanup{dir};
    const auto db = (dir / "subject.db").string();
    std::string path;
    LlamaOptions options; options.model_path = model; options.checkpoint_directory = dir.string(); options.timeout_ms = 60000;
    bool cancelled = false; options.cancelled = [&] { return cancelled; };
    Store store(db); store.create("s", {1, 100, 3600000}, 0);
    millis now = 100000;
    std::string first_head;
    {
        LlamaBackend first(options); Runtime runtime(store, first);
        auto s = runtime.step("s", now);
        check(s && s->tick == 1 && first.stats().checkpoint_read == "missing" &&
              first.stats().checkpoint_write == "saved" && runtime.maintenance_error().empty(),
              "initial transition did not publish checkpoint");
        first_head = s->head;
        for (const auto& file : std::filesystem::directory_iterator(dir))
            if (file.path().extension() == ".coggkv") path = file.path().string();
        check(!path.empty() && path != checkpoint_path(dir.string(), "s"), "native cache still uses unscoped subject name");
        check(read_checkpoint(path, options.max_checkpoint_bytes)->metadata.at("head") == first_head,
              "checkpoint not bound to successful commit");
    }
    LlamaBackend backend(options); Runtime runtime(store, backend);
    auto step = [&](const std::string& expected) {
        now += 100000;
        store.submit("s", "event-" + std::to_string(now), {{"text", "Say hello briefly, then wait."}}, now);
        const auto before = store.snapshot("s");
        auto s = runtime.step("s", now);
        if (backend.stats().checkpoint_read != expected)
            throw Error("expected " + expected + ", got " + backend.stats().checkpoint_read + ": " + backend.stats().checkpoint_detail);
        check(s && s->tick == before.tick + 1 && runtime.maintenance_error().empty(), "recovery failed to commit");
        check(backend.stats().checkpoint_write == "saved", "new checkpoint not published");
        check(read_checkpoint(path, options.max_checkpoint_bytes)->metadata.at("head") == s->head, "new cache has stale head");
        store.verify("s");
        std::cout << "PASS checkpoint " << expected << " at tick " << s->tick << std::endl;
    };
    step("restored");
    check(backend.stats().reused_tokens > 0, "native checkpoint restored no reusable KV");
    backend.release_context();
    auto bad = bytes(path); bad.back() ^= 1;
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(bad.data(), static_cast<std::streamsize>(bad.size())); }
    step("rejected");
    check(backend.stats().reused_tokens == 0 && backend.stats().checkpoint_detail.find("checksum") != std::string::npos,
          "corrupt KV reached continuation");
    backend.release_context(); std::filesystem::remove(path);
    step("missing"); check(backend.stats().reused_tokens == 0, "deleted checkpoint was reused");
    // A complete but stale checkpoint must not overwrite newer semantic state.
    backend.release_context(); now += 100000;
    store.submit("s", "advance-without-cache", {{"text", "host"}}, now);
    auto a = store.admit("s", "host", now); check(a.has_value(), "host advance not admitted");
    Proposal p; p.memory = {{"host_fact", "survives stale cache"}};
    store.commit(a->id, p, now);
    step("rejected");
    check(backend.stats().checkpoint_detail.find("head mismatch") != std::string::npos &&
          store.snapshot("s").memory.at("host_fact") == "survives stale cache", "stale cache erased durable memory");
    // Recompute outer checksums to specifically exercise compatibility validation.
    backend.release_context();
    auto cp = *read_checkpoint(path, options.max_checkpoint_bytes);
    cp.metadata["compatibility"]["backend"] = "different model";
    write_checkpoint(path, cp, options.max_checkpoint_bytes);
    step("rejected");
    check(backend.stats().checkpoint_detail.find("compatibility") != std::string::npos, "foreign model accepted");
    backend.release_context();
    cp = *read_checkpoint(path, options.max_checkpoint_bytes);
    cp.metadata["tokens"][0] = -1;
    write_checkpoint(path, cp, options.max_checkpoint_bytes);
    step("rejected");
    check(backend.stats().checkpoint_detail.find("token id") != std::string::npos, "invalid tokens accepted");
    backend.release_context();
    cp = *read_checkpoint(path, options.max_checkpoint_bytes);
    cp.state.assign(64, 0); // Valid outer checksums, invalid native state magic.
    write_checkpoint(path, cp, options.max_checkpoint_bytes);
    step("rejected");
    check(backend.stats().checkpoint_detail.find("libllama rejected") != std::string::npos,
          "failed native restore not handled by cold reconstruction");
    // No new cache publication on failed or cancelled inference.
    const auto saved = bytes(path); const auto head = store.snapshot("s").head;
    cancelled = true; now += 100000;
    store.submit("s", "cancelled", {{"text", "hello"}}, now);
    bool threw = false;
    try { runtime.step("s", now); } catch (const Error&) { threw = true; }
    check(threw && bytes(path) == saved && store.snapshot("s").head == head,
          "cancelled inference changed subject/checkpoint");
    cancelled = false; options.max_checkpoint_bytes = 1;
    {
        LlamaBackend limited(options); Runtime failed_save(store, limited);
        auto s = failed_save.step("s", now + 100000);
        check(s && s->head != head && !failed_save.maintenance_error().empty() &&
              limited.stats().checkpoint_write == "failed" && bytes(path) == saved,
              "failed checkpoint hid/reverted commit or damaged prior file");
        store.verify("s");
    }
    std::cout << "PASS real KV roundtrip, corrupt/missing/stale/incompatible cache, native failure and post-commit limits\n";
}
}
int main(int argc, char** argv) {
    try { if (argc != 2) throw Error("expected GGUF fixture"); run(argv[1]); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
