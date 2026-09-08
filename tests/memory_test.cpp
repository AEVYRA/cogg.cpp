#include "cogg/kernel.hpp"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace cogg;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw Error(why);
}
template <class F> void rejects(F f, const std::string &why) {
    try {
        f();
    } catch (const std::exception &e) {
        check(std::string(e.what()).find(why) != std::string::npos, e.what());
        return;
    }
    throw Error("expected rejection: " + why);
}
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("cogg-memory-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::filesystem::remove_all(path); }
    std::string db() { return (path / "s.db").string(); }
};
void sql(const std::string &path, const char *text) {
    sqlite3 *d = nullptr;
    sqlite3_open(path.c_str(), &d);
    auto code = sqlite3_exec(d, text, nullptr, nullptr, nullptr);
    std::string err = sqlite3_errmsg(d);
    sqlite3_close(d);
    check(code == SQLITE_OK, err.c_str());
}
Snapshot write(Store &s, std::vector<MemoryNote> notes, millis now) {
    s.submit("s", std::to_string(now), {{"text", "fixture"}}, now);
    auto a = s.admit("s", "fixture", now);
    check(a.has_value(), "no admission");
    Proposal p;
    p.notes = std::move(notes);
    return s.commit(a->id, p, now);
}
json recall(Store &s, const std::string &query, bool history = false) {
    MemoryPolicy p;
    p.query = query;
    p.history = history;
    p.max_bytes = 32000;
    p.max_items = 64;
    return s.recall("s", p);
}
json entry(const json &result, const std::string &key) {
    for (const auto &e : result.at("view").at("items"))
        if (e.at("key") == key)
            return e;
    throw Error("missing key: " + key);
}
bool has(const json &result, const std::string &key) {
    for (const auto &e : result.at("view").at("items"))
        if (e.at("key") == key)
            return true;
    return false;
}
void lifecycle() {
    Temp tmp;
    Store s(tmp.db());
    s.create("s", {1, 10000, 1000000}, 0);
    write(s,
          {{"door", "The workshop access code is cedar-582. It was written down at setup.", "fact"},
           {"promise", "Return the borrowed telescope to Mira.", "task", "open"}},
          1);
    auto initial = recall(s, "workshop");
    auto code = entry(initial, "door").at("id").get<std::string>();
    auto promise = entry(initial, "promise").at("id").get<std::string>();
    for (int i = 0; i < 80; ++i)
        write(s,
              {{"noise-" + std::to_string(i),
                std::string(400, 'x') + " unrelated routine weather observation", "episode"}},
              10 + i);
    MemoryPolicy small;
    small.query = "workshop access code";
    small.max_bytes = 2400;
    small.max_items = 4;
    auto view = s.recall("s", small);
    check(view.dump().size() <= 2400, "byte bound");
    check(has(view, "door") && has(view, "promise"), "old fact or open task crowded out");
    MemoryNote summary{"overview", "Workshop configured.", "summary", "active", {code}, {code}};
    write(s, {summary}, 100);
    auto summary_id = entry(recall(s, "configured"), "overview").at("id").get<std::string>();
    rejects([&] { write(s, {{"recursive", "Summary of summary", "summary", "active", {summary_id}, {}}}, 101); },
            "recursive summaries");
    check(has(recall(s, "cedar-582"), "door"), "compaction destroyed exact source");
    auto before = s.snapshot("s");
    auto original = s.record(before.head);
    s.rebuild_memory("s");
    check(s.snapshot("s").head == before.head && s.record(before.head) == original,
          "index rebuild changed subject");
    MemoryNote bad{"bad", "done", "summary", "active", {promise}, {promise}};
    rejects([&] { write(s, {bad}, 102); }, "open task");
    check(s.snapshot("s").head == before.head, "invalid compaction leaked commit");
    write(s, {{"door", "The workshop access code is now maple-913.", "fact"}}, 103);
    auto latest = recall(s, "workshop");
    check(entry(latest, "door").at("text").get<std::string>().find("maple") != std::string::npos,
          "stale fact selected");
    check(!has(latest, "overview"), "stale summary selected");
    auto past = recall(s, "cedar", true);
    bool labelled = false;
    for (const auto &e : past.at("view").at("items"))
        if (e.at("id") == code)
            labelled = !e.at("current").get<bool>();
    check(labelled, "historical value not labelled");
    write(s, {{"promise", "Telescope returned to Mira.", "task", "closed"}}, 104);
    s.verify("s");
    before = s.snapshot("s");
    sql(tmp.db(), "DELETE FROM memory_entries; DELETE FROM memory_fts;");
    rejects([&] { s.verify("s"); }, "memory index");
    s.rebuild_memory("s");
    check(s.snapshot("s").head == before.head, "lost-index recovery changed head");
    check(has(recall(s, "maple-913"), "door"), "index recovery lost deposit");
    Store reopened(tmp.db());
    reopened.verify("s");
    check(has(recall(reopened, "maple"), "door"), "restart retrieval failed");
    std::cout << "PASS long history, bounded recall, tasks, summary recovery, versions, index rebuild\n";
}
void boundaries() {
    Temp tmp;
    Store s(tmp.db());
    s.create("s", {1, 1000, 1000000}, 0);
    s.create("other", {1, 1000, 1000000}, 0);
    auto a = s.admit("other", "fixture", 1);
    Proposal p;
    p.notes = {{"private", "foreign-secret", "fact"}};
    s.commit(a->id, p, 1);
    auto foreign = s.recall("other").at("view").at("items").at(0).at("id").get<std::string>();
    rejects([&] { write(s, {{"forged", "do not import", "note", "active", {foreign}, {}}}, 1); }, "foreign");
    check(!has(recall(s, "foreign-secret"), "private"), "subject leak");
    rejects(
        [&] {
            parse_proposal(
                {{"kind", "null"}, {"notes", json::array({{{"key", "x"}, {"text", "y"}, {"tick", 100}}})}});
        },
        "unknown memory note field");
    write(s, {{"задача", "Вернуть книгу Софии.", "task", "open"}}, 2);
    check(has(recall(s, "книгу"), "задача"), "Unicode recall failed");
    MemoryPolicy policy;
    policy.max_bytes = 1024;
    const auto before = s.snapshot("s");
    rejects(
        [&] {
            s.admit("s", "bounded", 3, policy,
                    [](const Present &v) { return v.memory_view.at("items").empty(); });
        },
        "open tasks");
    check(s.snapshot("s").head == before.head, "context overflow changed head");
    auto first = s.admit("s", "bounded", 4, MemoryPolicy{});
    check(first && has(json{{"view", first->present.memory_view}}, "задача"), "missing admitted receipt");
    s.commit(first->id, Proposal{}, 4);
    s.verify("s");
    // Two independent writers cannot publish two deposit sets for one head.
    s.submit("s", "race", {{"text", "go"}}, 5);
    Store other(tmp.db());
    auto x = s.admit("s", "one", 5);
    auto y = other.admit("s", "two", 6);
    check(x && y, "missing concurrent admissions");
    Proposal left;
    left.notes = {{"winner", "left"}};
    Proposal right;
    right.notes = {{"winner", "right"}};
    s.commit(x->id, left, 6);
    rejects([&] { other.commit(y->id, right, 7); }, "settled");
    s.verify("s");
    check(entry(recall(s, "winner"), "winner").at("text") == "left", "stale writer leaked memory");
    std::cout << "PASS isolation, Unicode, pressure, receipt, parser, concurrent writes\n";
}
void legacy() {
    Temp tmp;
    std::ifstream file(COGG_LEGACY_V2_FIXTURE); std::stringstream contents; contents << file.rdbuf();
    check(!contents.str().empty(), "legacy v2 fixture missing"); sql(tmp.db(), contents.str().c_str());
    Store s(tmp.db()); s.verify("s"); auto before=s.snapshot("s"); auto old=s.record(before.head);
    check(before.tick==2 && before.memory.at("counter")==2, "legacy fixture state mismatch");
    write(s, {{"continued", "Typed memory after legacy v2.", "fact"}}, wall_now()+1000);
    check(s.record(before.head)==old && s.snapshot("s").tick==3, "legacy history was rewritten");
    s.verify("s"); check(has(recall(s,"legacy"),"continued"),"legacy continuation lost typed memory");
    MemoryPolicy missing; missing.query="zxqnonexistent"; missing.strategy="lexical";
    auto absent=s.recall("s",missing); check(absent.at("view").at("items").empty(),"no-match query fabricated memory");
    std::cout << "PASS v2 continuation with unchanged old records and honest no-match recall\n";
}
void crash() {
    for (auto point : {CommitPoint::before_sql_commit, CommitPoint::after_sql_commit}) {
        Temp tmp;
        {
            Store s(tmp.db());
            s.create("s", {1, 100, 100000}, 0);
        }
        auto pid = fork();
        check(pid >= 0, "fork failed");
        if (pid == 0) {
            Store s(tmp.db());
            auto a = s.admit("s", "crash", 1);
            Proposal p;
            p.notes = {{"survives", "Atomic memory deposit.", "fact"}};
            s.commit(a->id, p, 1, [&](CommitPoint pnt) {
                if (pnt == point)
                    kill(getpid(), SIGKILL);
            });
            _exit(3);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "missing SIGKILL");
        Store s(tmp.db());
        s.verify("s");
        auto exists = has(recall(s, "Atomic"), "survives");
        check(exists == (point == CommitPoint::after_sql_commit), "deposit crossed transaction boundary");
    }
    std::cout << "PASS SIGKILL before/after atomic typed-memory commit\n";
}
} // namespace
int main() {
    try {
        legacy();
        lifecycle();
        boundaries();
        crash();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
