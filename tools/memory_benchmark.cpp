#include "cogg/kernel.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace cogg;
int main() {
    try {
        const auto dir =
            std::filesystem::temp_directory_path() / ("cogg-memory-bench-" + std::to_string(getpid()));
        std::filesystem::create_directory(dir);
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() { std::filesystem::remove_all(p); }
        } cleanup{dir};
        Store s((dir / "bench.db").string());
        s.create("s", {1, 10000, 1000000}, 0);
        millis now = 1;
        auto add = [&](std::vector<MemoryNote> notes) {
            s.submit("s", std::to_string(now), {{"text", "fixture"}}, now);
            auto a = s.admit("s", "benchmark", now);
            Proposal p;
            p.notes = std::move(notes);
            s.commit(a->id, p, now++);
        };
        add({{"workshop", "The workshop access code is cedar-582.", "fact"},
             {"delivery", "Delivery destination is Oslo.", "fact"},
             {"promise", "Return the telescope to Mira.", "task", "open"},
             {"transport", "My car tag is QJ-417.", "fact"},
             {"книга", "Книга Софии лежит в синем шкафу.", "fact"}});
        auto lookup = s.recall("s");
        std::string source;
        for (const auto &n : lookup.at("view").at("items"))
            if (n.at("key") == "workshop")
                source = n.at("id");
        add({{"overview", "Workshop configured.", "summary", "active", {source}, {source}},
             {"delivery", "Delivery destination changed to Bergen.", "fact"}});
        for (int i = 0; i < 120; ++i)
            add({{"weather-" + std::to_string(i),
                  "Routine weather observation number " + std::to_string(i) + ": " + std::string(320, 'x'),
                  "episode"}});
        struct Case {
            const char *name;
            const char *query;
            const char *key;
            const char *text;
        };
        const std::vector<Case> cases = {
            {"covered exact detail", "workshop access code", "workshop", "cedar-582"},
            {"corrected fact", "delivery destination", "delivery", "Bergen"},
            {"prospective unrelated occasion", "weather today", "promise", "telescope"},
            {"Unicode old fact", "книга Софии", "книга", "синем"},
            {"paraphrase counterexample", "automobile registration", "transport", "QJ-417"},
            {"recent information", "weather observation 119", "weather-119", "119"}};
        json result = {
            {"schema", "cogg:memory-comparison/v1"},
            {"fixture", "hand-authored-contract-cases/v1"},
            {"budgets", {{"max_bytes", 2400}, {"max_items", 4}, {"candidate_limit", 64}}},
            {"strategies", json::array()},
            {"scope", "Evidence selection only; no answer-generating model or competitor deployment. Cases "
                      "are deterministic mechanism probes, not an external quality benchmark."}};
        for (const auto &strategy : {"recent", "lexical", "balanced"}) {
            json rows = json::array();
            int hits = 0;
            double ms = 0;
            for (const auto &c : cases) {
                MemoryPolicy p;
                p.strategy = strategy;
                p.query = c.query;
                p.max_bytes = 2400;
                p.max_items = 4;
                auto start = std::chrono::steady_clock::now();
                auto view = s.recall("s", p);
                auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                        .count();
                bool hit = false, stale = false;
                for (const auto &e : view.at("view").at("items")) {
                    if (e.at("key") == c.key &&
                        e.at("text").get<std::string>().find(c.text) != std::string::npos)
                        hit = true;
                    if (!e.at("current").get<bool>() || e.at("summary_stale").get<bool>())
                        stale = true;
                }
                hits += hit;
                ms += elapsed;
                rows.push_back({{"case", c.name},
                                {"hit", hit},
                                {"stale", stale},
                                {"bytes", view.dump().size()},
                                {"latency_ms", elapsed}});
            }
            result["strategies"].push_back({{"strategy", strategy},
                                            {"evidence_hits", hits},
                                            {"cases", cases.size()},
                                            {"mean_latency_ms", ms / static_cast<double>(cases.size())},
                                            {"results", rows}});
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
