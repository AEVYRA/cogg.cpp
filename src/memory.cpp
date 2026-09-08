#include "memory_internal.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <openssl/evp.h>
#include <set>
#include <sqlite3.h>

namespace cogg {
namespace {
void need(bool ok, const std::string &why) {
    if (!ok)
        throw Error(why);
}
std::string digest(const json &j) {
    auto s = j.dump();
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    need(EVP_Digest(s.data(), s.size(), out, &n, EVP_sha256(), nullptr) == 1, "memory hash failed");
    std::string r;
    for (unsigned int i = 0; i < n; ++i) {
        r += "0123456789abcdef"[out[i] >> 4];
        r += "0123456789abcdef"[out[i] & 15];
    }
    return r;
}
void text_ok(const std::string &s, std::size_t limit) {
    need(!s.empty() && s.size() <= limit && s.find('\0') == std::string::npos, "invalid memory text/key");
}
struct Q {
    sqlite3 *db;
    sqlite3_stmt *p = nullptr;
    Q(sqlite3 *d, const std::string &s) : db(d) {
        need(sqlite3_prepare_v2(d, s.c_str(), -1, &p, nullptr) == SQLITE_OK, sqlite3_errmsg(d));
    }
    ~Q() { sqlite3_finalize(p); }
    Q &bind(int i, const std::string &s) {
        need(sqlite3_bind_text(p, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT) == SQLITE_OK,
             "memory bind failed");
        return *this;
    }
    Q &bind(int i, std::int64_t n) {
        need(sqlite3_bind_int64(p, i, n) == SQLITE_OK, "memory bind failed");
        return *this;
    }
    bool row() {
        auto r = sqlite3_step(p);
        need(r == SQLITE_ROW || r == SQLITE_DONE, sqlite3_errmsg(db));
        return r == SQLITE_ROW;
    }
    std::string str(int i) {
        auto *s = sqlite3_column_text(p, i);
        return s ? reinterpret_cast<const char *>(s) : "";
    }
    std::int64_t num(int i) { return sqlite3_column_int64(p, i); }
};
void exec(sqlite3 *db, const char *s) {
    char *e = nullptr;
    auto r = sqlite3_exec(db, s, nullptr, nullptr, &e);
    std::string message = e ? e : "memory SQL failed";
    sqlite3_free(e);
    need(r == SQLITE_OK, message);
}
json get(sqlite3 *db, const std::string &subject, const std::string &id) {
    Q q(db, "SELECT body FROM memory_entries WHERE subject=? AND id=?");
    q.bind(1, subject).bind(2, id);
    need(q.row(), "unknown or foreign memory source");
    return json::parse(q.str(0));
}
void policy_ok(const MemoryPolicy &p) {
    need(p.max_bytes >= 256 && p.max_bytes <= 1048576 && p.max_items > 0 && p.max_items <= 256 &&
             p.candidate_limit >= p.max_items && p.candidate_limit <= 512 && p.query.size() <= 65536,
         "invalid memory policy");
    need(p.strategy == "balanced" || p.strategy == "lexical" || p.strategy == "recent",
         "unknown memory strategy");
}
// Quote each token: user input never becomes FTS boolean syntax or SQL.
std::string terms(const std::string &s) {
    std::vector<std::string> words;
    std::string word;
    auto flush = [&] {
        if (!word.empty() && words.size() < 32)
            words.push_back(word);
        word.clear();
    };
    for (unsigned char c : s) {
        if (std::isalnum(c) || c >= 128 || c == '_') {
            if (word.size() < 128)
                word += static_cast<char>(c);
        } else
            flush();
    }
    flush();
    std::string out;
    for (const auto &w : words) {
        if (!out.empty())
            out += " OR ";
        out += '"' + w + '"';
    }
    return out;
}
// A summary whose source was replaced is not a current representation.
const std::string current_filter =
    " e.current=1 AND json_extract(e.body,'$.note.status')!='retracted' AND "
    "(json_extract(e.body,'$.note.type')!='summary' OR NOT EXISTS (SELECT 1 FROM memory_links l JOIN "
    "memory_entries s ON s.id=l.source WHERE l.note=e.id AND s.current=0)) ";
const std::string uncovered_filter =
    " NOT EXISTS (SELECT 1 FROM memory_links l JOIN memory_entries c ON c.id=l.note WHERE l.source=e.id AND "
    "l.covers=1 AND c.current=1 AND json_extract(c.body,'$.note.status')!='retracted' AND NOT EXISTS (SELECT "
    "1 FROM memory_links x JOIN memory_entries z ON z.id=x.source WHERE x.note=c.id AND z.current=0)) ";
json wire_at(sqlite3 *db, const std::string &subject, const json &b, std::int64_t at) {
    auto current = [&](const json &source) {
        Q q(db,
            "SELECT id FROM memory_entries WHERE subject=? AND key=? AND tick<=? ORDER BY tick DESC LIMIT 1");
        q.bind(1, subject).bind(2, source.at("note").at("key").get<std::string>()).bind(3, at);
        return q.row() && q.str(0) == source.at("id").get<std::string>();
    };
    bool stale = false;
    if (b.at("note").at("type") == "summary")
        for (const auto &id : b.at("note").at("sources"))
            if (!current(get(db, subject, id.get<std::string>())))
                stale = true;

    auto j = b.at("note");
    j["id"] = b.at("id");
    j["commit"] = b.at("commit");
    j["tick"] = b.at("tick");
    j["current"] = current(b);
    j["summary_stale"] = stale;
    return j;
}
} // namespace
json memory_note_json(const MemoryNote &n) {
    return {{"key", n.key},       {"text", n.text},       {"type", n.type},
            {"status", n.status}, {"sources", n.sources}, {"covers", n.covers}};
}
MemoryNote parse_memory_note(const json &j) {
    need(j.is_object(), "memory note must be an object");
    const std::set<std::string> fields = {"key", "text", "type", "status", "sources", "covers"};
    for (const auto &[k, v] : j.items()) {
        (void)v;
        need(fields.count(k) != 0, "unknown memory note field");
    }
    MemoryNote n;
    n.key = j.at("key").get<std::string>();
    n.text = j.at("text").get<std::string>();
    n.type = j.value("type", std::string("note"));
    n.status = j.value("status", std::string("active"));
    if (j.contains("sources"))
        n.sources = j.at("sources").get<std::vector<std::string>>();
    if (j.contains("covers"))
        n.covers = j.at("covers").get<std::vector<std::string>>();
    validate_memory_notes({n});
    return n;
}
void validate_memory_notes(const std::vector<MemoryNote> &notes) {
    need(notes.size() <= 8, "too many typed memory writes");
    std::set<std::string> keys;
    for (const auto &n : notes) {
        text_ok(n.key, 128);
        text_ok(n.text, 8192);
        need(keys.insert(n.key).second, "duplicate typed memory key");
        need(n.type == "note" || n.type == "fact" || n.type == "episode" || n.type == "task" ||
                 n.type == "summary",
             "invalid memory type");
        need(n.status == "active" || n.status == "open" || n.status == "closed" || n.status == "retracted",
             "invalid memory status");
        need(n.status != "open" || n.type == "task", "only tasks can be open");
        need(n.sources.size() <= 32 && n.covers.size() <= 32, "too many memory sources");
        std::set<std::string> src, cover;
        for (const auto &x : n.sources) {
            need(x.size() == 64 && x.find_first_not_of("0123456789abcdef") == std::string::npos,
                 "invalid source id");
            need(src.insert(x).second, "duplicate source");
        }
        for (const auto &x : n.covers)
            need(src.count(x) && cover.insert(x).second, "covers must be distinct explicit sources");
        need(n.covers.empty() || (n.type == "summary" && n.status == "active"),
             "only an active summary can compact");
        need(n.type != "summary" || !n.sources.empty(), "summary requires evidence sources");
    }
}
namespace detail {
void memory_schema(sqlite3 *db) {
    exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS memory_entries(id TEXT PRIMARY KEY,subject TEXT NOT NULL REFERENCES subjects(id),key TEXT NOT NULL,tick INTEGER NOT NULL,commit_id TEXT NOT NULL,body TEXT NOT NULL,current INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS memory_current ON memory_entries(subject,current,tick);
CREATE INDEX IF NOT EXISTS memory_keys ON memory_entries(subject,key,tick);
CREATE TABLE IF NOT EXISTS memory_links(subject TEXT NOT NULL,note TEXT NOT NULL,source TEXT NOT NULL,covers INTEGER NOT NULL,PRIMARY KEY(note,source));
CREATE INDEX IF NOT EXISTS memory_sources ON memory_links(source);
CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(id UNINDEXED,subject UNINDEXED,key,text,tokenize='unicode61');
)SQL");
}
void memory_apply(sqlite3 *db, const std::string &subject, const std::string &parent, std::int64_t tick,
                  const std::string &commit, const std::vector<MemoryNote> &notes) {
    validate_memory_notes(notes);
    std::set<std::string> writing;
    for (const auto &n : notes)
        writing.insert(n.key);
    std::size_t index = 0;
    for (const auto &n : notes) {
        std::size_t source_bytes = 0;
        for (const auto &id : n.sources) {
            auto b = get(db, subject, id);
            need(b.at("tick").get<std::int64_t>() < tick, "same-transition memory source");
            if (n.type == "summary") {
                Q current(db, "SELECT current FROM memory_entries WHERE id=?");
                current.bind(1, id); current.row();
                need(current.num(0) == 1 && b.at("note").at("status") != "retracted",
                     "summary requires current source versions");
                need(b.at("note").at("type") != "summary", "recursive summaries are not supported");
                need(!writing.count(b.at("note").at("key").get<std::string>()),
                     "cannot overwrite a summary source in the same transition");
            }
            if (std::find(n.covers.begin(), n.covers.end(), id) != n.covers.end()) {
                const auto &old = b.at("note");
                Q q(db, "SELECT current FROM memory_entries WHERE id=?");
                q.bind(1, id);
                q.row();
                need(q.num(0) == 1 && old.at("status") != "retracted",
                     "cannot compact superseded/retracted source");
                need(old.at("type") != "summary", "recursive summary compaction is not supported");
                need(old.at("status") != "open", "cannot compact an open task");
                need(!writing.count(old.at("key").get<std::string>()),
                     "cannot overwrite a compacted source in the same transition");
                source_bytes += old.at("text").get<std::string>().size();
            }
        }
        if (!n.covers.empty())
            need(n.text.size() < source_bytes, "compaction must reduce source text bytes");
        std::string previous;
        Q old(db, "SELECT id,body FROM memory_entries WHERE subject=? AND key=? AND current=1");
        old.bind(1, subject).bind(2, n.key);
        if (old.row()) {
            previous = old.str(0);
            auto was = json::parse(old.str(1)).at("note");
            if (was.at("status") == "open")
                need(n.type == "task" &&
                         (n.status == "open" || n.status == "closed" || n.status == "retracted"),
                     "open task needs explicit resolution");
        }
        auto note = memory_note_json(n);
        auto id = digest({{"parent", parent}, {"index", index++}, {"note", note}});
        auto body = json{{"id", id},         {"subject", subject},   {"tick", tick},
                         {"commit", commit}, {"previous", previous}, {"note", note}};
        Q update(db, "UPDATE memory_entries SET current=0 WHERE subject=? AND key=?");
        update.bind(1, subject).bind(2, n.key).row();
        Q insert(db, "INSERT INTO memory_entries VALUES(?,?,?,?,?,?,1)");
        insert.bind(1, id)
            .bind(2, subject)
            .bind(3, n.key)
            .bind(4, tick)
            .bind(5, commit)
            .bind(6, body.dump())
            .row();
        Q f(db, "INSERT INTO memory_fts(id,subject,key,text) VALUES(?,?,?,?)");
        f.bind(1, id).bind(2, subject).bind(3, n.key).bind(4, n.text).row();
        for (const auto &src : n.sources) {
            Q l(db, "INSERT INTO memory_links VALUES(?,?,?,?)");
            l.bind(1, subject)
                .bind(2, id)
                .bind(3, src)
                .bind(4, std::find(n.covers.begin(), n.covers.end(), src) != n.covers.end() ? 1 : 0)
                .row();
        }
    }
}
void memory_rebuild(sqlite3 *db, const std::string &subject) {
    Q links(db, "DELETE FROM memory_links WHERE subject=?");
    links.bind(1, subject).row();
    Q entries(db, "DELETE FROM memory_entries WHERE subject=?");
    entries.bind(1, subject).row();
    Q fts(db, "DELETE FROM memory_fts WHERE subject=?");
    fts.bind(1, subject).row();
    Q q(db, "SELECT id,body FROM commits WHERE subject=? ORDER BY tick");
    q.bind(1, subject);
    while (q.row()) {
        auto b = json::parse(q.str(1));
        if (b.at("tick") == 0)
            continue;
        auto p = parse_proposal(b.at("proposal"));
        memory_apply(db, subject, b.at("parent"), b.at("tick"), q.str(0), p.notes);
    }
}
void memory_verify(sqlite3 *db, const std::string &subject, bool compare) {
    // Replay only deposits into a disposable DB. Canonical commits were verified
    // by Store first; index loss never authorizes modifying their bytes.
    sqlite3 *raw = nullptr;
    need(sqlite3_open(":memory:", &raw) == SQLITE_OK, "memory replay allocation failed");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> replay(raw, sqlite3_close);
    exec(raw, "CREATE TABLE subjects(id TEXT PRIMARY KEY);");
    memory_schema(raw);
    Q q(db, "SELECT id,body FROM commits WHERE subject=? ORDER BY tick");
    q.bind(1, subject);
    while (q.row()) {
        auto b = json::parse(q.str(1));
        if (b.at("tick") == 0)
            continue;
        auto p = parse_proposal(b.at("proposal"));
        memory_apply(raw, subject, b.at("parent"), b.at("tick"), q.str(0), p.notes);
    }
    if (compare) {
        const std::vector<std::string> queries = {
            "SELECT id,subject,key,tick,commit_id,body,current FROM memory_entries WHERE subject=? ORDER BY "
            "id",
            "SELECT note,source,covers FROM memory_links WHERE subject=? ORDER BY note,source",
            "SELECT id,key,text,subject FROM memory_fts WHERE subject=? ORDER BY id"};
        for (const auto &sql : queries) {
            Q a(db, sql), b(raw, sql);
            a.bind(1, subject);
            b.bind(1, subject);
            while (true) {
                bool x = a.row(), y = b.row();
                need(x == y, "memory index differs from history; rebuild required");
                if (!x)
                    break;
                for (int i = 0; i < sqlite3_column_count(a.p); ++i)
                    need(a.str(i) == b.str(i), "memory index differs from history; rebuild required");
            }
        }
    }
    Q attempts(db, "SELECT body FROM attempts WHERE subject=? ORDER BY seq");
    attempts.bind(1, subject);
    while (attempts.row()) {
        auto a = json::parse(attempts.str(0));
        if (!a.contains("context"))
            continue;
        const auto &c = a.at("context");
        Q parent(db, "SELECT body FROM commits WHERE id=?");
        parent.bind(1, a.at("parent").get<std::string>());
        need(parent.row(), "missing context parent");
        auto state = json::parse(parent.str(0)).at("memory");
        need(c.at("memory").is_object(), "invalid context memory");
        for (const auto &[k, v] : c.at("memory").items())
            need(state.contains(k) && state.at(k) == v, "context legacy source mismatch");
        const auto &view = c.at("view");
        need(view.at("head") == a.at("parent") && view.at("schema") == "cogg:memory-view/v1",
             "context head mismatch");
        std::set<std::string> ids;
        for (const auto &entry : view.at("items")) {
            auto id = entry.at("id").get<std::string>();
            need(ids.insert(id).second, "duplicate context source");
            auto b = get(raw, subject, id);
            need(b.at("tick").get<std::int64_t>() <= a.at("tick").get<std::int64_t>() &&
                     wire_at(raw, subject, b, a.at("tick").get<std::int64_t>()) == entry,
                 "context memory source mismatch");
        }
        const auto bytes = json({{"memory", c.at("memory")}, {"view", view}}).dump().size();
        need(bytes <= view.at("max_bytes").get<std::size_t>(), "context byte budget mismatch");
        need(view.at("items").size() + c.at("memory").size() <= view.at("max_items").get<std::size_t>(),
             "context item budget mismatch");
    }
}
json memory_candidates(sqlite3 *db, const std::string &subject, const MemoryPolicy &p) {
    policy_ok(p);
    json mandatory = json::array(), lex = json::array(), recent = json::array();
    Q head(db, "SELECT tick FROM subjects WHERE id=?");
    head.bind(1, subject);
    need(head.row(), "subject missing");
    const auto at = head.num(0);
    auto collect = [&](Q &q, json &out) {
        while (q.row())
            out.push_back(wire_at(db, subject, json::parse(q.str(0)), at));
    };
    const auto limit = static_cast<std::int64_t>(p.candidate_limit);
    if (p.strategy == "balanced") {
        Q q(db, "SELECT e.body FROM memory_entries e WHERE e.subject=? AND " + current_filter +
                    " AND json_extract(e.body,'$.note.type')='task' AND "
                    "json_extract(e.body,'$.note.status')='open' ORDER BY e.tick,e.id LIMIT ?");
        q.bind(1, subject).bind(2, static_cast<std::int64_t>(p.max_items + 1));
        collect(q, mandatory);
        need(mandatory.size() <= p.max_items, "memory pressure: too many open tasks");
    }
    auto query = terms(p.query);
    if (!query.empty() && p.strategy != "recent") {
        Q q(db, "SELECT e.body FROM memory_fts JOIN memory_entries e ON e.id=memory_fts.id WHERE memory_fts "
                "MATCH ? AND e.subject=? AND " +
                    (p.history ? "1" : current_filter) +
                    " ORDER BY bm25(memory_fts),e.tick DESC,e.id LIMIT ?");
        q.bind(1, query).bind(2, subject).bind(3, limit);
        collect(q, lex);
    }
    if (p.strategy != "lexical") {
        Q q(db, "SELECT e.body FROM memory_entries e WHERE e.subject=? AND " +
                    (p.history ? "1" : current_filter + " AND " + uncovered_filter) +
                    " ORDER BY e.tick DESC,e.id LIMIT ?");
        q.bind(1, subject).bind(2, limit);
        collect(q, recent);
    }
    json all = json::array();
    std::set<std::string> seen;
    auto add = [&](json x, bool required, const char *lane) {
        auto id = x.at("id").get<std::string>();
        if (seen.insert(id).second)
            all.push_back({{"entry", x}, {"required", required}, {"lane", lane}});
    };
    for (auto &x : mandatory)
        add(x, true, "open_task");
    for (std::size_t i = 0; i < std::max(lex.size(), recent.size()); ++i) {
        if (i < lex.size())
            add(lex[i], false, "lexical");
        if (i < recent.size())
            add(recent[i], false, "recent");
    }
    Q count(db, "SELECT count(*) FROM memory_entries WHERE subject=?");
    count.bind(1, subject);
    count.row();
    return {{"entries", all}, {"total_deposits", count.num(0)}, {"candidate_limit", p.candidate_limit}};
}
void project_memory(Present &p, const json &candidates, const MemoryPolicy &policy,
                    const std::function<bool(const Present &)> &fits) {
    policy_ok(policy);
    const auto legacy = p.state.memory;
    p.working_memory = json::object();
    p.memory_view = {{"schema", "cogg:memory-view/v1"},
                     {"head", p.state.head},
                     {"strategy", policy.strategy},
                     {"query", policy.query},
                     {"max_bytes", policy.max_bytes},
                     {"max_items", policy.max_items},
                     {"candidate_limit", policy.candidate_limit},
                     {"history", policy.history},
                     {"total_deposits", candidates.at("total_deposits")},
                     {"legacy_keys", legacy.size()},
                     {"candidates", candidates.at("entries").size()},
                     {"items", json::array()},
                     {"omitted", true}};
    auto valid = [&] {
        return p.working_memory.size() + p.memory_view.at("items").size() <= policy.max_items &&
               json({{"memory", p.working_memory}, {"view", p.memory_view}}).dump().size() <=
                   policy.max_bytes &&
               (!fits || fits(p));
    };
    need(valid(), "memory pressure: fixed context exceeds budget");
    for (const auto &x : candidates.at("entries"))
        if (x.at("required").get<bool>()) {
            p.memory_view["items"].push_back(x.at("entry"));
            need(valid(), "memory pressure: open tasks exceed context budget");
        }
    // Legacy working state remains addressable as individual assignments. Sorting
    // matching keys/values first is transparent, not semantic inference.
    std::vector<std::string> keys;
    for (const auto &[k, v] : legacy.items()) {
        (void)v;
        keys.push_back(k);
    }
    auto score = [&](const std::string &k) {
        return !policy.query.empty() && (policy.query.find(k) != std::string::npos ||
                                         legacy.at(k).dump().find(policy.query) != std::string::npos);
    };
    std::stable_sort(keys.begin(), keys.end(),
                     [&](const auto &a, const auto &b) { return score(a) > score(b); });
    std::size_t li = 0;
    const auto &entries = candidates.at("entries");
    for (std::size_t i = 0; i < std::max(keys.size(), entries.size()); ++i) {
        if (i < entries.size() && !entries[i].at("required").get<bool>()) {
            p.memory_view["items"].push_back(entries[i].at("entry"));
            if (!valid())
                p.memory_view["items"].erase(p.memory_view["items"].end() - 1);
        }
        if (li < keys.size()) {
            const auto &k = keys[li++];
            p.working_memory[k] = legacy.at(k);
            if (!valid())
                p.working_memory.erase(k);
        }
    }
    // Keep this conservative: candidate caps, filtering and byte/token capacity
    // mean the view is never a completeness certificate for the entire history.
}
} // namespace detail
} // namespace cogg
