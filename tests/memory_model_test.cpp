#include "cogg/llama_backend.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace cogg;
void check(bool value, const char *why) {
    if (!value)
        throw Error(why);
}
int main(int argc, char **argv) {
    try {
        check(argc == 2, "model path required");
        auto dir = std::filesystem::temp_directory_path() / ("cogg-memory-model-" + std::to_string(getpid()));
        std::filesystem::create_directory(dir);
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() { std::filesystem::remove_all(p); }
        } cleanup{dir};
        std::filesystem::create_directory(dir / "kv");
        auto db = (dir / "s.db").string();
        Store s(db);
        const auto start = wall_now() - 10000;
        s.create("s", {1, 10000, 1000000}, start);
        millis now = start + 1;
        auto add = [&](std::vector<MemoryNote> notes) {
            s.submit("s", std::to_string(now), {{"text", "fixture"}}, now);
            auto a = s.admit("s", "seed", now);
            Proposal p;
            p.notes = std::move(notes);
            s.commit(a->id, p, now++);
        };
        add({{"workshop", "The workshop access code is cedar-582.", "fact"}});
        MemoryPolicy query;
        query.query = "workshop";
        auto source = s.recall("s", query).at("view").at("items").at(0).at("id").get<std::string>();
        add({{"overview", "Workshop ready.", "summary", "active", {source}, {source}}});
        for (int i = 0; i < 40; ++i)
            add({{"noise-" + std::to_string(i), "Routine unrelated observation. " + std::string(600, 'x'),
                  "episode"}});
        // The writer helper intentionally leaves one FIFO fixture event pending;
        // settle it before asking the actual model question.
        if (auto pending = s.admit("s", "drain", now))
            s.commit(pending->id, Proposal{}, now++);
        LlamaOptions options;
        options.model_path = argv[1];
        options.timeout_ms = 120000;
        options.max_output_tokens = 512;
        options.checkpoint_directory = (dir / "kv").string();
        auto question = [&](const char *key) {
            s.submit("s", key, {{"text", "What is the workshop access code? Reply briefly."}}, wall_now());
        };
        auto selected_source = [&](Store &store) {
            const auto history = store.timeline("s");
            const auto &items = history.at("attempts").back().at("body").at("context").at("view").at("items");
            bool found = false;
            for (const auto &item : items)
                if (item.at("id") == source && item.at("text") == "The workshop access code is cedar-582.")
                    found = true;
            check(found, "exact source missing from admitted model context");
        };
        question("first");
        std::int64_t tick = 0;
        {
            LlamaBackend backend(options);
            Runtime runtime(s, backend, wall_now);
            auto result = runtime.step("s", wall_now());
            check(result.has_value(), "first model step missing");
            tick = result->tick;
            selected_source(s);
            check(backend.stats().prompt_tokens + options.max_output_tokens <= options.context_tokens,
                  "model context overflow");
            std::cout << "first prompt_tokens=" << backend.stats().prompt_tokens
                      << " proposal=" << s.record(result->head).at("proposal").dump() << '\n';
            s.verify("s");
        }
        // Delete all disposable KV files, rebuild the derived search index, then
        // continue through a completely new backend/context and Store connection.
        for (const auto &e : std::filesystem::directory_iterator(dir / "kv"))
            std::filesystem::remove(e.path());
        s.rebuild_memory("s");
        check(s.memory_record("s", source).at("note").at("text") == "The workshop access code is cedar-582.",
              "compaction lost source");
        question("second");
        {
            Store reopened(db);
            LlamaBackend backend(options);
            Runtime runtime(reopened, backend, wall_now);
            auto result = runtime.step("s", wall_now());
            check(result && result->tick == tick + 1, "cold restart did not continue");
            check(backend.stats().checkpoint_read == "missing",
                  "KV deletion did not force cold reconstruction");
            selected_source(reopened);
            check(backend.stats().prompt_tokens + options.max_output_tokens <= options.context_tokens,
                  "recovery context overflow");
            reopened.verify("s");
            std::cout << "second prompt_tokens=" << backend.stats().prompt_tokens
                      << " proposal=" << reopened.record(result->head).at("proposal").dump() << '\n';
        }
        std::cout << "PASS real model bounded history, source compaction, index rebuild and KV loss\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
