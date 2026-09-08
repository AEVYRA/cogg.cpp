#include "cogg/http_backend.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace cogg;
namespace {
json read(const char* path) { std::ifstream in(path); if (!in) throw Error("cannot open JSON file"); json j; in >> j; return j; }
json evidence(const json& note, const json& id) {
    return {{"id", id}, {"text", note.at("text")}, {"type", note.at("type")}, {"status", note.at("status")}};
}
json fixture(const std::string& path) {
    if (std::filesystem::exists(path)) throw Error("fixture database must be new");
    Store s(path); s.create("fixture", {1, 1000, 1000000}, 0);
    auto initial = *s.admit("fixture", "fixture-seed/v1", 0); s.commit(initial.id, Proposal{}, 0);
    millis at = 0; json messages = json::array(), current = json::object();
    auto add = [&](std::vector<MemoryNote> notes) {
        at += 10; s.submit("fixture", "seed-" + std::to_string(at), {}, at);
        auto a = *s.admit("fixture", "fixture-seed/v1", at); Proposal p; p.notes = notes; s.commit(a.id, p, at);
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto n = memory_note_json(notes[i]);
            const auto id = execution_hash({{"parent", a.present.state.head}, {"index", i}, {"note", n}});
            (void)s.memory_record("fixture", id);
            messages.push_back(evidence(n, id)); current[notes[i].key] = id;
        }
    };
    add({{"access", "The workshop access code is birch-7642. This code was recorded in the setup instructions.", "fact"},
         {"destination", "The delivery destination is Riga.", "fact"},
         {"telescope", "Return Mira's telescope after cleaning the lens.", "task", "open"},
         {"vehicle", "My car tag is VQ-682-Z.", "fact"},
         {"book", "Книга Лины лежит в зелёном комоде.", "fact"}});
    const std::string access = current.at("access");
    add({{"setup", "Workshop configured.", "summary", "active", {access}, {access}},
         {"destination", "Correction: the delivery destination is Turku, replacing Riga.", "fact"},
         {"telescope", "The telescope obligation is closed; its return was acknowledged in this fictional fixture.", "task", "closed"},
         {"camera", "Return Niko's camera after copying the photos from its memory card.", "task", "open"}});
    for (int batch = 0; batch < 10; ++batch) {
        std::vector<MemoryNote> notes;
        for (int j = 0; j < 8; ++j) {
            const auto n = std::to_string(batch * 8 + j);
            notes.push_back({"weather-" + n, "Routine weather observation " + n + ": calm conditions. " + std::string(130, 'x'), "episode"});
        }
        add(std::move(notes));
    }
    add({{"locker", "The locker code is 8316.", "fact"}, {"meeting", "The meeting with Dana is at 16:25.", "fact"}});
    s.verify("fixture");
    const json cases = json::array({
        {{"id", "covered-detail"}, {"question", "What is the workshop access code?"}, {"key", "access"}, {"accept", json::array({json::array({"birch-7642"})})}},
        {{"id", "synonym-only"}, {"question", "Automobile registration?"}, {"key", "vehicle"}, {"accept", json::array({json::array({"vq-682-z"})})}},
        {{"id", "corrected-fact"}, {"question", "What is the current delivery destination?"}, {"key", "destination"}, {"accept", json::array({json::array({"turku"})})}},
        {{"id", "closed-task"}, {"question", "Is the telescope obligation open or closed in the supplied records?"}, {"key", "telescope"}, {"accept", json::array({json::array({"closed"})})}},
        {{"id", "open-commitment"}, {"question", "Before moving on, what unfinished commitment should I remember, including its prerequisite?"}, {"key", "camera"}, {"accept", json::array({json::array({"niko"}), json::array({"camera"}), json::array({"photos", "photographs"}), json::array({"copy", "copi"})})}},
        {{"id", "unknown"}, {"question", "What is my hotel reservation code?"}, {"key", nullptr}, {"accept", json::array()}},
        {{"id", "recent-fact"}, {"question", "What is the locker code?"}, {"key", "locker"}, {"accept", json::array({json::array({"8316"})})}},
        {{"id", "russian-old-fact"}, {"question", "Где лежит книга Лины? Ответь по-русски."}, {"key", "book"}, {"accept", json::array({json::array({"зелён", "зелен"}), json::array({"комод"})})}}
    });
    json result = {{"schema", "cogg:memory-quality-fixture/v1"}, {"budget", {{"evidence_bytes", 2400}, {"items", 4}}},
                   {"messages", messages.size()}, {"cases", json::array()}};
    for (auto c : cases) {
        c["required_source"] = c.at("key").is_null() ? json(nullptr) : current.at(c.at("key").get<std::string>());
        MemoryPolicy p; p.query = c.at("question"); p.max_bytes = 2400; p.max_items = 4;
        const auto recalled = s.recall("fixture", p);
        json selected = json::array();
        for (const auto& n : recalled.at("view").at("items")) selected.push_back(evidence(n, n.at("id")));
        json recent = json::array();
        for (std::size_t i = messages.size(); i > 0 && recent.size() < 4; --i) {
            auto candidate = recent; candidate.insert(candidate.begin(), messages[i - 1]);
            if (candidate.dump().size() > 2400) break;
            recent = std::move(candidate);
        }
        if (selected.dump().size() > 2400) throw Error("rendered evidence budget exceeded");
        c["contexts"] = {{"balanced", selected}, {"last_messages", recent}};
        c["retrieval_receipt"] = recalled;
        result["cases"].push_back(std::move(c));
    }
    return result;
}
json ask(const json& config, const std::string& executor, const json& input) {
    auto settings = config.at("executors").at(executor);
    settings["max_output_tokens"] = 512;
    settings["max_prompt_bytes"] = 24576;
    HttpBackend backend(settings);
    const auto prompt = std::string("Answer the question using only the supplied fictional evidence. Prefer explicit corrections and distinguish open from closed tasks. ") +
        "If the requested information is absent, return kind abstain, reason missing_input, and a short detail; do not guess or substitute a different fact. "
        "Otherwise return kind speech with a short direct answer, memory:[], notes:[], wake_after_ms:null. Question: " + input.at("question").get<std::string>();
    Present p;
    p.state.subject = "quality-fixture"; p.state.head = execution_hash("quality-fixture/v1");
    p.occasion = {execution_hash(prompt), "external", {{"text", prompt}}};
    p.inputs = json::array({make_input("observation", "quality-fixture/v1", input.at("evidence"))});
    validate_inputs(p.inputs);
    if (!backend.context_fits(p)) throw Error("complete prompt exceeds configured cap");
    Attempt a{execution_hash({{"head", p.state.head}, {"occasion", p.occasion.id}, {"inputs", p.inputs}}), p};
    const auto start = std::chrono::steady_clock::now();
    const auto outcome = backend.respond_attempt(a, 60000, {});
    return {{"outcome", outcome_json(outcome)}, {"provider", backend.telemetry()}, {"backend", backend.name()},
            {"attempt", a.id}, {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()}};
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "fixture") std::cout << fixture(argv[2]).dump(2) << '\n';
        else if (argc == 5 && std::string(argv[1]) == "ask") std::cout << ask(read(argv[2]), argv[3], read(argv[4])).dump(2) << '\n';
        else throw Error("usage: cogg-memory-quality fixture NEW_DB | ask CONFIG EXECUTOR INPUT_JSON");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
