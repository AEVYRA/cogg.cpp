#include "cogg/kernel.hpp"
#include "cogg/output_limits.hpp"
#include "memory_internal.hpp"
#include "cogg/routing.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>

namespace cogg {
namespace {
constexpr std::size_t max_payload = 65536;
constexpr std::size_t max_state = 1048576;
constexpr millis max_delay = 365LL * 24 * 60 * 60 * 1000;

std::string hex(const unsigned char* p, std::size_t n) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[p[i] >> 4]; out += digits[p[i] & 15];
    }
    return out;
}
std::string hash(const json& value) {
    const auto bytes = value.dump();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1)
        throw Error("SHA-256 failed");
    return hex(digest, size);
}
std::string random_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1) throw Error("random source failed");
    return hex(bytes, sizeof bytes);
}
void require(bool ok, const std::string& msg) { if (!ok) throw Error(msg); }
void check_text(const std::string& s, std::size_t max, const char* field) {
    require(!s.empty() && s.size() <= max && s.find('\0') == std::string::npos,
            std::string("invalid ") + field);
}
void check_time(millis now) { require(now >= 0, "negative wall clock"); }
millis add(millis a, millis b) {
    require(b >= 0 && a <= std::numeric_limits<millis>::max() - b, "time overflow");
    return a + b;
}
json optional_time(std::optional<millis> t) { return t ? json(*t) : json(nullptr); }
std::optional<millis> read_time(const json& t) {
    return t.is_null() ? std::nullopt : std::optional<millis>(t.get<millis>());
}
json limits_json(Limits l) {
    require(l.min_wake_ms > 0 && l.min_wake_ms <= max_delay && l.max_attempts > 0 &&
            l.max_attempts <= 1000000 && l.period_ms > 0 && l.period_ms <= max_delay &&
            l.max_wake_ms >= l.min_wake_ms && l.max_wake_ms <= max_delay &&
            l.max_occasion_failures >= 0 && l.max_occasion_failures <= 64,
            "invalid runtime limits");
    json result = {{"min_wake_ms", l.min_wake_ms}, {"max_attempts", l.max_attempts},
                   {"period_ms", l.period_ms}, {"max_wake_ms", l.max_wake_ms}};
    // Omitted when disabled so a disabled subject keeps the historical genesis bytes.
    if (l.max_occasion_failures > 0) result["max_occasion_failures"] = l.max_occasion_failures;
    return result;
}
void validate(const Proposal& p) {
    validate_memory_notes(p.notes);
    require(p.kind == "null" || p.kind == "reflection" || p.kind == "speech",
            "unsupported transition kind");
    require(p.text.size() <= max_payload, "speech too large");
    require(p.kind == "speech" || p.text.empty(), "only speech may emit text");
    require(p.memory.size() <= output_limits::memory_writes, "too many memory writes");
    std::set<std::string> keys;
    for (const auto& w : p.memory) {
        check_text(w.key, 128, "memory key");
        require(keys.insert(w.key).second, "duplicate memory key");
        require(w.value.dump().size() <= max_payload, "memory value too large");
    }
    require(!p.wake_after_ms || (*p.wake_after_ms >= 0 && *p.wake_after_ms <= max_delay),
            "wake interval outside supported range");
}
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw Error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(s_); }
    Statement& bind(int i, const std::string& v) {
        if (sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw Error(sqlite3_errmsg(db_));
        return *this;
    }
    Statement& bind(int i, std::int64_t v) {
        if (sqlite3_bind_int64(s_, i, v) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
        return *this;
    }
    bool row() {
        const int rc = sqlite3_step(s_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw Error(sqlite3_errmsg(db_));
        return rc == SQLITE_ROW;
    }
    void done() { require(!row(), "unexpected SQL result"); }
    std::string text(int i) const {
        const auto* p = sqlite3_column_text(s_, i);
        return p ? std::string(reinterpret_cast<const char*>(p),
                               static_cast<std::size_t>(sqlite3_column_bytes(s_, i))) : "";
    }
    std::int64_t number(int i) const { return sqlite3_column_int64(s_, i); }
private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};
void sql(sqlite3* db, const char* query) {
    char* msg = nullptr;
    if (sqlite3_exec(db, query, nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string error = msg ? msg : "SQL failure";
        sqlite3_free(msg); throw Error(error);
    }
}
struct Transaction {
    sqlite3* db;
    bool active = true;
    explicit Transaction(sqlite3* d, bool write = true) : db(d) {
        sql(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction() { if (active) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db, "COMMIT"); active = false; }
};
} // namespace

json prompt_temporal(const Present& present) {
    auto temporal = present.temporal;
    if (temporal.contains("admission") && temporal.at("admission").contains("occasion")) {
        auto& occasion = temporal["admission"]["occasion"];
        if (occasion.is_object() && occasion.contains("payload") && occasion.at("payload") == present.occasion.payload) {
            occasion.erase("payload"); occasion["payload_ref"] = "/occasion/payload";
        }
    }
    return temporal;
}

json proposal_json(const Proposal& p) {
    validate(p);
    json writes = json::array();
    for (const auto& w : p.memory) writes.push_back({{"key", w.key}, {"value", w.value}});
    json result = {{"kind", p.kind}, {"text", p.text}, {"memory", writes},
            {"wake_after_ms", optional_time(p.wake_after_ms)}};
    if (!p.notes.empty()) { result["notes"] = json::array(); for (const auto& n : p.notes) result["notes"].push_back(memory_note_json(n)); }
    return result;
}
Proposal parse_proposal(const json& j) {
    require(j.is_object(), "transition must be an object");
    const std::set<std::string> fields = {"kind", "text", "memory", "wake_after_ms", "notes"};
    for (const auto& [key, value] : j.items()) {
        (void)value;
        require(fields.count(key) != 0, "unknown transition field: " + key);
    }
    Proposal p;
    p.kind = j.at("kind").get<std::string>();
    p.text = j.value("text", std::string{});
    if (j.contains("wake_after_ms") && !j.at("wake_after_ms").is_null()) {
        const auto& t = j.at("wake_after_ms");
        require(t.is_number_integer(), "wake interval must be an integer");
        if (t.is_number_unsigned())
            require(t.get<std::uint64_t>() <= static_cast<std::uint64_t>(max_delay), "wake overflow");
        p.wake_after_ms = t.get<millis>();
    }
    if (j.contains("memory")) {
        require(j.at("memory").is_array(), "memory must be an array");
        for (const auto& w : j.at("memory")) {
            require(w.is_object() && w.size() == 2 && w.contains("key") && w.contains("value"),
                    "invalid memory write");
            p.memory.push_back({w.at("key").get<std::string>(), w.at("value")});
        }
    }
    if (j.contains("notes")) {
        require(j.at("notes").is_array(), "notes must be an array");
        for (const auto& n : j.at("notes")) p.notes.push_back(parse_memory_note(n));
    }
    validate(p); return p;
}

namespace {
struct BudgetHistory {
    std::vector<std::int64_t> seq;
    std::vector<millis> at;
};
}
struct Store::Impl {
    sqlite3* db = nullptr;
    ~Impl() { if (db) sqlite3_close(db); }
    Snapshot snapshot(const std::string& subject) {
        Statement s(db, "SELECT tick,head,memory,wake FROM subjects WHERE id=?");
        s.bind(1, subject); require(s.row(), "subject not found");
        Snapshot result{subject, s.number(0), s.text(1), json::parse(s.text(2)),
                        read_time(json::parse(s.text(3))), ""};
        result.lifecycle = result.wake_at ? "sleeping" : "waiting_external";
        return result;
    }
    json config(const std::string& subject) {
        Statement q(db, "SELECT limits FROM subjects WHERE id=?");
        q.bind(1, subject); require(q.row(), "subject not found");
        return json::parse(q.text(0));
    }
    json clock(const std::string& subject) {
        Statement q(db, "SELECT id FROM commits WHERE subject=? AND tick=0");
        q.bind(1, subject); require(q.row(), "subject clock missing");
        return {{"clock_id", "cogg:commit:" + q.text(0)}, {"origin", q.text(0)},
                {"grain_epoch", "cogg:committed-transition/v1"}, {"unit", "committed_transition"},
                {"membership", "verified subject commit parent chain; genesis is coordinate zero"}};
    }
    // Every admission spends quota, regardless of its eventual settlement. The
    // watermark makes a committed wake decision replayable after later attempts.
    json budget(const std::string& subject, const json& limits, millis now,
                std::int64_t through = std::numeric_limits<std::int64_t>::max(),
                const BudgetHistory* history = nullptr) {
        const auto period = limits.at("period_ms").get<millis>();
        const auto lower = now >= period ? now - period : -1;
        const auto cap = limits.at("max_attempts").get<std::size_t>();
        std::optional<millis> last, quota_source;
        std::int64_t watermark = 0;
        std::size_t used = 0;
        if (history) {
            const auto end = static_cast<std::size_t>(std::upper_bound(history->seq.begin(), history->seq.end(), through) - history->seq.begin());
            const auto begin = static_cast<std::size_t>(std::upper_bound(history->at.begin(), history->at.begin() + static_cast<std::ptrdiff_t>(end), lower) - history->at.begin());
            used = end - begin;
            if (end) { last = history->at[end - 1]; watermark = history->seq[end - 1]; }
            if (used >= cap) quota_source = history->at[end - cap];
        } else {
            Statement tail(db, "SELECT admitted_at,seq FROM attempts WHERE subject=? AND seq<=? ORDER BY seq DESC LIMIT 1");
            tail.bind(1, subject).bind(2, through);
            if (tail.row()) { last = tail.number(0); watermark = tail.number(1); }
            Statement count(db, "SELECT count(*) FROM attempts WHERE subject=? AND admitted_at>? AND seq<=?");
            count.bind(1, subject).bind(2, lower).bind(3, through); count.row();
            used = static_cast<std::size_t>(count.number(0));
            if (used >= cap) {
                Statement kth(db, "SELECT admitted_at FROM attempts WHERE subject=? AND admitted_at>? AND seq<=? ORDER BY admitted_at DESC,seq DESC LIMIT 1 OFFSET ?");
                kth.bind(1, subject).bind(2, lower).bind(3, through).bind(4, static_cast<std::int64_t>(cap - 1));
                require(kth.row(), "budget source missing"); quota_source = kth.number(0);
            }
        }
        millis floor_at = now, quota_at = now;
        if (last) floor_at = std::max(now, add(*last, limits.at("min_wake_ms").get<millis>()));
        if (quota_source) quota_at = add(*quota_source, period);
        return {{"used", used}, {"limit", cap}, {"period_ms", period},
                {"through_seq", watermark}, {"floor_at", floor_at}, {"quota_at", quota_at},
                {"eligible_at", std::max(floor_at, quota_at)}};
    }
    json wake_plan(const std::string& subject, const json& limits, const Proposal& p,
                   millis now, std::int64_t through = std::numeric_limits<std::int64_t>::max(),
                   const BudgetHistory* history = nullptr) {
        const auto b = budget(subject, limits, now, through, history);
        json reasons = json::array();
        std::optional<millis> requested, granted;
        if (p.wake_after_ms) {
            requested = add(now, *p.wake_after_ms);
            auto delay = *p.wake_after_ms;
            const auto minimum = limits.at("min_wake_ms").get<millis>();
            const auto maximum = limits.value("max_wake_ms", max_delay);
            if (delay < minimum) { delay = minimum; reasons.push_back("minimum_interval"); }
            if (delay > maximum) { delay = maximum; reasons.push_back("maximum_interval"); }
            granted = add(now, delay);
            if (b.at("eligible_at").get<millis>() > *granted) {
                granted = b.at("eligible_at").get<millis>(); reasons.push_back("attempt_budget");
            }
        } else reasons.push_back("waiting_external");
        return {{"policy", "cogg:wake/v2"}, {"decided_at", now},
                {"requested_after_ms", optional_time(p.wake_after_ms)},
                {"requested_at", optional_time(requested)}, {"granted_at", optional_time(granted)},
                {"granted_after_ms", granted ? json(*granted - now) : json(nullptr)},
                {"reasons", reasons}, {"budget", b}};
    }
    json schedule(const std::string& subject, millis observed) {
        const auto s = snapshot(subject);
        Statement time(db, "SELECT accounting_wall FROM subjects WHERE id=?");
        time.bind(1, subject); time.row();
        const auto accounting = std::max(observed, time.number(0));
        const auto b = budget(subject, config(subject), accounting);
        json occasion = nullptr;
        // A queued wake is still a physical deadline after a wall-clock rollback.
        Statement q(db, "SELECT id,kind,body FROM occasions WHERE subject=? AND consumed IS NULL "
                        "AND (kind!='scheduled' OR (parent=? AND ? >= ?)) ORDER BY seq LIMIT 1");
        q.bind(1, subject).bind(2, s.head).bind(3, observed)
         .bind(4, s.wake_at.value_or(std::numeric_limits<millis>::max()));
        if (q.row()) {
            const auto e = event(q.text(0));
            occasion = {{"id", q.text(0)}, {"kind", q.text(1)}, {"payload", e.at("payload")}};
        } else if (s.wake_at && observed >= *s.wake_at) {
            occasion = {{"id", nullptr}, {"kind", "scheduled"}, {"payload", {{"due_at", *s.wake_at}}}};
        }
        std::string status;
        std::optional<millis> eligible;
        if (occasion.is_null()) {
            status = s.wake_at ? "sleeping" : "waiting_external";
            if (s.wake_at) eligible = std::max(*s.wake_at, b.at("eligible_at").get<millis>());
        } else {
            eligible = b.at("eligible_at").get<millis>();
            status = b.at("quota_at").get<millis>() > accounting ? "budget_exhausted" :
                     b.at("floor_at").get<millis>() > accounting ? "minimum_interval" : "ready";
        }
        return {{"status", status}, {"eligible_at", optional_time(eligible)}, {"occasion", occasion},
                {"wall_observed_at", observed}, {"accounting_at", accounting},
                {"wall_rollback_ms", accounting - observed}, {"budget", b},
                {"head", s.head}, {"tick", s.tick}, {"wake_at", optional_time(s.wake_at)}};
    }
    json event(const std::string& id) {
        Statement s(db, "SELECT body FROM occasions WHERE id=?");
        s.bind(1, id); require(s.row(), "occasion missing");
        auto body = json::parse(s.text(0)); require(hash(body) == id, "occasion hash mismatch");
        return body;
    }
    std::string enqueue(const Snapshot& s, const std::string& key,
                        const std::string& kind, const json& payload, millis now) {
        Statement old(db, "SELECT id,body FROM occasions WHERE subject=? AND key=?");
        old.bind(1, s.subject).bind(2, key);
        if (old.row()) {
            const auto b = json::parse(old.text(1));
            require(b.at("payload") == payload && b.at("kind") == kind,
                    "idempotency key reused with different content");
            return old.text(0);
        }
        json body = {{"schema", 1}, {"subject", s.subject}, {"key", key},
                     {"kind", kind}, {"payload", payload}, {"received_at", now},
                     {"parents", json::array({s.head})}};
        const auto id = hash(body);
        Statement q(db, "INSERT INTO occasions(id,subject,key,kind,parent,body) VALUES(?,?,?,?,?,?)");
        q.bind(1, id).bind(2, s.subject).bind(3, key).bind(4, kind).bind(5, s.head)
            .bind(6, body.dump()).done();
        return id;
    }
};

Store::Store(const std::string& path, OpenMode mode) : impl_(std::make_unique<Impl>()) {
    require(path != ":memory:" && !path.empty(), "a durable database path is required");
    if (sqlite3_open_v2(path.c_str(), &impl_->db, (mode == OpenMode::read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                        SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        throw Error(sqlite3_errmsg(impl_->db));
    auto* db = impl_->db;
    sqlite3_busy_timeout(db, 5000);
    // Avoid changing an unrelated application's database.
    {
        Statement q(db, "PRAGMA application_id"); q.row();
        require(q.number(0) == 0 || q.number(0) == 1129269063, "not a cogg database");
        if (q.number(0) == 0) {
            Statement tables(db, "SELECT count(*) FROM sqlite_master WHERE type='table'");
            tables.row(); require(tables.number(0) == 0, "nonempty unrecognized database");
        }
    }
    if (mode == OpenMode::read_only) {
        Statement q(db, "PRAGMA application_id"); q.row();
        require(q.number(0) == 1129269063, "not a cogg database");
        Statement v(db, "PRAGMA user_version"); v.row();
        require(v.number(0) == 5, "read-only open requires schema 5; migrate explicitly with a writable host");
        sql(db, "PRAGMA query_only=ON; PRAGMA foreign_keys=ON;");
        return;
    }
    sql(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    Transaction tx(db);
    {
        Statement q(db, "PRAGMA user_version"); q.row();
        require(q.number(0) == 0 || q.number(0) == 1 || q.number(0) == 2 || q.number(0) == 3 || q.number(0) == 4 || q.number(0) == 5, "unsupported database version");
    }
    sql(db, R"SQL(
CREATE TABLE IF NOT EXISTS subjects(
 id TEXT PRIMARY KEY, tick INTEGER NOT NULL, head TEXT NOT NULL,
 memory TEXT NOT NULL, wake TEXT NOT NULL, limits TEXT NOT NULL,
 accounting_wall INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS occasions(
 seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE,
 subject TEXT NOT NULL REFERENCES subjects(id), key TEXT NOT NULL,
 kind TEXT NOT NULL, parent TEXT NOT NULL, body TEXT NOT NULL,
 consumed TEXT, UNIQUE(subject,key));
CREATE TABLE IF NOT EXISTS attempts(
 seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE,
 subject TEXT NOT NULL REFERENCES subjects(id), parent TEXT NOT NULL,
 occasion TEXT NOT NULL REFERENCES occasions(id), admitted_at INTEGER NOT NULL,
 body TEXT NOT NULL, status TEXT NOT NULL, error TEXT NOT NULL DEFAULT '');
CREATE INDEX IF NOT EXISTS attempts_subject_seq ON attempts(subject,seq);
CREATE INDEX IF NOT EXISTS attempts_occasion ON attempts(subject,occasion,seq);
CREATE INDEX IF NOT EXISTS attempts_budget ON attempts(subject,admitted_at);
CREATE TABLE IF NOT EXISTS commits(
 id TEXT PRIMARY KEY, subject TEXT NOT NULL REFERENCES subjects(id),
 tick INTEGER NOT NULL, body TEXT NOT NULL, UNIQUE(subject,tick));
PRAGMA application_id=1129269063;
CREATE TABLE IF NOT EXISTS abstentions(
 id TEXT PRIMARY KEY, attempt TEXT NOT NULL UNIQUE REFERENCES attempts(id), body TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS occasion_failures(
 attempt TEXT PRIMARY KEY REFERENCES attempts(id),
 occasion TEXT NOT NULL REFERENCES occasions(id), reason TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS occasion_failures_occasion ON occasion_failures(occasion);
PRAGMA user_version=5;
)SQL");
    detail::memory_schema(db);
    tx.commit();
}
Store::~Store() = default;

void Store::create(const std::string& subject, Limits limits, millis now, const json& initial_memory) {
    require(initial_memory.is_object() && initial_memory.dump().size() <= max_state, "invalid initial memory");
    check_text(subject, 128, "subject"); check_time(now);
    auto l = limits_json(limits);
    Transaction tx(impl_->db);
    json genesis = {{"schema", 5}, {"subject", subject}, {"tick", 0}, {"parent", nullptr},
                    {"created_at", now}, {"nonce", random_id()}, {"limits", l},
                    {"memory", initial_memory}, {"wake_at", nullptr}};
    const auto id = hash(genesis);
    Statement s(impl_->db, "INSERT INTO subjects VALUES(?,0,?,?,'null',?,?)");
    s.bind(1, subject).bind(2, id).bind(3, initial_memory.dump()).bind(4, l.dump()).bind(5, now).done();
    Statement c(impl_->db, "INSERT INTO commits VALUES(?,?,0,?)");
    c.bind(1, id).bind(2, subject).bind(3, genesis.dump()).done();
    impl_->enqueue(impl_->snapshot(subject), "system:created", "created", json::object(), now);
    tx.commit();
}
Snapshot Store::snapshot(const std::string& subject) { return impl_->snapshot(subject); }
std::string Store::submit(const std::string& subject, const std::string& key,
                          const json& payload, millis now) {
    check_time(now); check_text(key, 200, "idempotency key");
    require(payload.dump().size() <= max_payload, "occasion payload too large");
    Transaction tx(impl_->db);
    auto id = impl_->enqueue(impl_->snapshot(subject), "user:" + key, "external", payload, now);
    tx.commit(); return id;
}
std::optional<Attempt> Store::admit(const std::string& subject, const std::string& backend, millis now,
                                    const std::optional<MemoryPolicy>& memory_policy,
                                    const std::function<bool(const Present&)>& fits, const json& execution, const json& context) {
    validate_admission_context(context);
    validate_execution(execution);
    require(execution.is_null() || execution.at("backend") == backend, "execution backend mismatch");
    check_time(now); check_text(backend, 200, "backend");
    auto* db = impl_->db;
    Transaction tx(db);
    auto s = impl_->snapshot(subject);
    const auto decision = impl_->schedule(subject, now);
    if (!context.is_null() && (context.at("head") != s.head ||
        decision.at("occasion").is_null() || context.at("occasion") != decision.at("occasion").at("id")))
        throw Conflict("stale admission context");
    if (decision.at("status") != "ready") { tx.commit(); return std::nullopt; }
    const auto observed = now;
    now = decision.at("accounting_at").get<millis>();
    auto selected = decision.at("occasion");
    if (selected.at("id").is_null())
        selected["id"] = impl_->enqueue(s, "wake:" + s.head, "scheduled", selected.at("payload"), observed);
    Occasion o{selected.at("id").get<std::string>(), selected.at("kind").get<std::string>(), selected.at("payload")};
    auto temporal = json{{"clock", impl_->clock(subject)}, {"coordinate", s.tick},
                         {"admission", decision}, {"previous_wake", nullptr}};
    if (s.tick > 0) {
        const auto previous = record(s.head);
        temporal["previous_wake"] = previous.contains("wake_plan") ? previous.at("wake_plan") :
            json{{"policy", "cogg:wake/v1"}, {"granted_at", previous.at("wake_at")}};
    }
    temporal["wake_lateness_ms"] = o.kind == "scheduled" ?
        json(observed - o.payload.at("due_at").get<millis>()) : json(nullptr);
    Statement prior(db, "SELECT count(*) FROM attempts WHERE subject=? AND status='reserved'");
    prior.bind(1, subject); prior.row();
    bool unsettled = prior.number(0) > 0;
    Statement account(db, "UPDATE subjects SET accounting_wall=? WHERE id=?");
    account.bind(1, now).bind(2, subject).done();
    json body = {{"schema", 5}, {"nonce", random_id()}, {"subject", subject},
                 {"parent", s.head}, {"tick", s.tick}, {"occasion", o.id},
                 {"backend", backend}, {"admitted_at", now}, {"prior_unsettled", unsettled},
                 {"wall_observed_at", observed}, {"temporal", temporal}, {"execution", execution}};
    Present present{s, o, unsettled, temporal};
    if (!context.is_null()) present.inputs = context.at("inputs");
    body["inputs"] = present.inputs;
    if (memory_policy) {
        auto policy = *memory_policy;
        if (policy.query.empty()) policy.query = o.payload.is_object() && o.payload.contains("text") && o.payload.at("text").is_string() ? o.payload.at("text").get<std::string>() : "";
        auto candidates = detail::memory_candidates(db, subject, policy);
        detail::project_memory(present, candidates, policy, fits);
        body["context"] = {{"memory", present.working_memory}, {"view", present.memory_view}};
        if (policy.strategy == "balanced") body["capacity_policy"] = {{"schema", "cogg:memory-capacity/v1"},
            {"max_items", policy.max_items}, {"max_bytes", policy.max_bytes}};
    }
    if (fits && !fits(present)) throw ContextOverflow("admitted context exceeds backend capacity");
    const auto id = hash(body);
    Statement insert(db, "INSERT INTO attempts(id,subject,parent,occasion,admitted_at,body,status) "
                         "VALUES(?,?,?,?,?,?,'reserved')");
    insert.bind(1, id).bind(2, subject).bind(3, s.head).bind(4, o.id).bind(5, now)
          .bind(6, body.dump()).done();
    tx.commit();
    return Attempt{id, present};
}

Snapshot Store::commit(const std::string& attempt, const Proposal& proposal, millis now,
                       const std::function<void(CommitPoint)>& hook, std::optional<millis> inference_elapsed_ms, const json& emission) {
    if (inference_elapsed_ms) check_time(*inference_elapsed_ms);
    check_time(now);
    Transaction tx(impl_->db);
    auto result = commit_locked(attempt, proposal, now, inference_elapsed_ms, emission, nullptr);
    if (hook) hook(CommitPoint::before_sql_commit);
    tx.commit();
    if (hook) hook(CommitPoint::after_sql_commit);
    return result;
}
// Caller owns the write transaction. A non-null disposition is the kernel's own
// settlement of an undeliverable occasion; it carries no model emission.
Snapshot Store::commit_locked(const std::string& attempt, const Proposal& proposal, millis now,
                              std::optional<millis> inference_elapsed_ms, const json& emission,
                              const json& disposition) {
    const auto p = proposal_json(proposal);
    auto* db = impl_->db;
    Statement aq(db, "SELECT body,status FROM attempts WHERE id=?");
    aq.bind(1, attempt); require(aq.row(), "attempt missing");
    if (aq.text(1) != "reserved") throw Conflict("attempt already settled");
    const auto a = json::parse(aq.text(0));
    require(hash(a) == attempt, "attempt hash mismatch");
    if (disposition.is_null()) validate_emission(emission, attempt, a, p);
    else require(emission.is_null(), "disposition cannot carry a model emission");
    const auto subject = a.at("subject").get<std::string>();
    auto s = impl_->snapshot(subject);
    if (s.head != a.at("parent").get<std::string>() || s.tick != a.at("tick").get<std::int64_t>()) {
        throw Conflict("stale subject head");
    }
    const auto oid = a.at("occasion").get<std::string>();
    const auto e = impl_->event(oid);
    require(e.at("subject") == subject, "foreign occasion");
    Statement oq(db, "SELECT consumed FROM occasions WHERE id=?");
    oq.bind(1, oid); oq.row();
    if (!oq.text(0).empty()) throw Conflict("occasion already consumed");
    require(s.tick < std::numeric_limits<std::int64_t>::max(), "tick exhausted");
    const auto tick = s.tick + 1;
    // Commit wall cannot move behind admission, including a recovered attempt.
    Statement wall(db, "SELECT accounting_wall FROM subjects WHERE id=?");
    wall.bind(1, subject); wall.row();
    const auto observed = now;
    now = std::max({now, a.at("admitted_at").get<millis>(), wall.number(0)});
    if (inference_elapsed_ms) now = std::max(now, add(a.at("admitted_at").get<millis>(), *inference_elapsed_ms));
    auto memory = s.memory;
    for (const auto& w : proposal.memory) memory[w.key] = w.value;
    require(memory.dump().size() <= max_state, "state exceeds phase-0 limit");
    auto limits = impl_->config(subject);
    const auto plan = impl_->wake_plan(subject, limits, proposal, now);
    const auto wake = read_time(plan.at("granted_at"));
    json body = {{"schema", 5}, {"subject", subject}, {"tick", tick}, {"parent", s.head},
                 {"occasion", oid}, {"attempt", attempt}, {"proposal", p},
                 {"memory", memory}, {"wake_at", optional_time(wake)}, {"committed_at", now},
                 {"wall_observed_at", observed}, {"wake_plan", plan},
                 {"inference_elapsed_ms", optional_time(inference_elapsed_ms)}, {"emission", emission}};
    if (!disposition.is_null()) body["disposition"] = disposition;
    auto id = hash(body);
    detail::memory_transition(proposal, a);
    detail::memory_apply(db, subject, s.head, tick, id, proposal.notes);
    Statement insert(db, "INSERT INTO commits VALUES(?,?,?,?)");
    insert.bind(1, id).bind(2, subject).bind(3, tick).bind(4, body.dump()).done();
    Statement head(db, "UPDATE subjects SET tick=?,head=?,memory=?,wake=?,accounting_wall=? WHERE id=? AND head=?");
    head.bind(1, tick).bind(2, id).bind(3, memory.dump()).bind(4, optional_time(wake).dump())
        .bind(5, now).bind(6, subject).bind(7, s.head).done();
    require(sqlite3_changes(db) == 1, "head update failed");
    detail::memory_capacity(db, Snapshot{subject,tick,id,memory,wake,""}, a);
    Statement consumed(db, "UPDATE occasions SET consumed=? WHERE id=? AND consumed IS NULL");
    consumed.bind(1, id).bind(2, oid).done();
    require(sqlite3_changes(db) == 1, "occasion update failed");
    Statement settled(db, "UPDATE attempts SET status='committed' WHERE id=?");
    settled.bind(1, attempt).done();
    Statement obsolete(db, "UPDATE attempts SET status='superseded' WHERE subject=? AND parent=? AND status='reserved'");
    obsolete.bind(1, subject).bind(2, s.head).done();
    return Snapshot{subject, tick, id, memory, wake, wake ? "sleeping" : "waiting_external"};
}
std::string Store::abstain(const std::string& attempt, const Abstention& outcome, const json& provider) {
    const auto value = outcome_json(outcome);
    require(provider.is_object() && provider.dump().size() <= 8192, "invalid provider receipt");
    auto* db = impl_->db; Transaction tx(db);
    Statement aq(db, "SELECT body,status FROM attempts WHERE id=?");
    aq.bind(1, attempt); require(aq.row(), "attempt missing");
    const auto a = json::parse(aq.text(0)); require(hash(a) == attempt, "attempt hash mismatch");
    json body = {{"schema", "cogg:abstention/v1"}, {"attempt", attempt}, {"subject", a.at("subject")},
        {"parent", a.at("parent")}, {"occasion", a.at("occasion")},
        {"execution", a.value("execution", json(nullptr))}, {"outcome", value}, {"provider", provider}};
    const auto id = hash(body);
    Statement old(db, "SELECT id FROM abstentions WHERE attempt=?"); old.bind(1, attempt);
    if (old.row()) {
        if (old.text(0) != id || aq.text(1) != "abstained") throw Conflict("different abstention already settled");
        tx.commit(); return id;
    }
    if (aq.text(1) != "reserved" || impl_->snapshot(a.at("subject").get<std::string>()).head != a.at("parent").get<std::string>())
        throw Conflict("attempt already settled or stale");
    Statement insert(db, "INSERT INTO abstentions VALUES(?,?,?)");
    insert.bind(1, id).bind(2, attempt).bind(3, body.dump()).done();
    Statement settle(db, "UPDATE attempts SET status='abstained' WHERE id=?"); settle.bind(1, attempt).done();
    tx.commit(); return id;
}
void Store::fail(const std::string& attempt, const std::string& reason) {
    Statement q(impl_->db, "UPDATE attempts SET status='failed',error=? WHERE id=? AND status='reserved'");
    q.bind(1, reason.substr(0, 1024)).bind(2, attempt).done();
}
std::optional<Snapshot> Store::fail(const std::string& attempt, const std::string& reason,
                                    FailureScope scope, millis now) {
    check_time(now);
    const auto error = reason.substr(0, 1024);
    auto* db = impl_->db;
    Transaction tx(db);
    Statement aq(db, "SELECT body,status FROM attempts WHERE id=?");
    aq.bind(1, attempt); require(aq.row(), "attempt missing");
    if (aq.text(1) != "reserved") { tx.commit(); return std::nullopt; } // Idempotent, like fail().
    const auto a = json::parse(aq.text(0));
    require(hash(a) == attempt, "attempt hash mismatch");
    auto mark_failed = [&] {
        Statement q(db, "UPDATE attempts SET status='failed',error=? WHERE id=? AND status='reserved'");
        q.bind(1, error).bind(2, attempt).done();
    };
    if (scope == FailureScope::transient) { mark_failed(); tx.commit(); return std::nullopt; }
    const auto subject = a.at("subject").get<std::string>();
    const auto oid = a.at("occasion").get<std::string>();
    const auto limits = impl_->config(subject);
    const auto cap = limits.value("max_occasion_failures", std::int64_t{0});
    if (cap == 0) { mark_failed(); tx.commit(); return std::nullopt; }
    Statement record_failure(db, "INSERT INTO occasion_failures VALUES(?,?,?)");
    record_failure.bind(1, attempt).bind(2, oid).bind(3, error).done();
    json failures = json::array();
    Statement f(db, "SELECT f.attempt FROM occasion_failures f JOIN attempts a ON a.id=f.attempt "
                    "WHERE f.occasion=? AND f.attempt!=? ORDER BY a.seq LIMIT ?");
    f.bind(1, oid).bind(2, attempt).bind(3, cap - 1);
    while (f.row()) failures.push_back(f.text(0));
    failures.push_back(attempt);
    const auto s = impl_->snapshot(subject);
    Statement consumed(db, "SELECT consumed FROM occasions WHERE id=?");
    consumed.bind(1, oid); consumed.row();
    const bool settle = cap > 0 && static_cast<std::int64_t>(failures.size()) >= cap &&
                        s.head == a.at("parent").get<std::string>() && consumed.text(0).empty();
    if (!settle) { mark_failed(); tx.commit(); return std::nullopt; }
    // The bounded query selects the earliest cap-1 failures plus this attempt.
    Proposal settlement; // kind null: no text, memory, notes or model emission.
    const auto e = impl_->event(oid);
    const bool maintenance = a.contains("context") &&
        a.at("context").at("view").value("strategy", "") == "task_maintenance";
    if (s.wake_at && e.at("kind") != "scheduled" && !maintenance) {
        // Commit clamps its wall against admission and accounting; keep the same instant.
        Statement wall(db, "SELECT accounting_wall FROM subjects WHERE id=?");
        wall.bind(1, subject); wall.row();
        const auto at = std::max({now, a.at("admitted_at").get<millis>(), wall.number(0)});
        settlement.wake_after_ms = std::max<millis>(0, *s.wake_at - at);
    }
    // Failure reasons stay in the mutable attempts table; an untrusted provider
    // message never enters the hashed, immutable history.
    const json disposition = {{"schema", "cogg:disposition/v1"}, {"reason", "occasion_failures"},
                              {"limit", cap}, {"failures", failures}};
    sql(db, "SAVEPOINT disposition");
    try {
        auto result = commit_locked(attempt, settlement, now, std::nullopt, nullptr, disposition);
        sql(db, "RELEASE disposition");
        tx.commit(); return result;
    } catch (const ContextOverflow&) {
        // Capacity pressure leaves the input pending. Storage/integrity errors
        // instead propagate and roll back the whole failure transaction.
        sql(db, "ROLLBACK TO disposition; RELEASE disposition");
        mark_failed(); tx.commit(); return std::nullopt;
    }
}
FailureScope failure_scope(FailureKind kind) {
    return kind == FailureKind::invalid_output ?
        FailureScope::occasion : FailureScope::transient;
}
json Store::schedule(const std::string& subject, millis now) {
    check_time(now);
    Transaction tx(impl_->db, false);
    auto result = impl_->schedule(subject, now);
    tx.commit(); return result;
}
json Store::clock(const std::string& subject) { return impl_->clock(subject); }
json Store::duration(const std::string& subject, const std::string& from, const std::string& to) {
    // Committed anchors are immutable through the host API. Concurrent appends
    // preserve a verified prefix; arbitrary out-of-band DB edits are unsupported.
    verify(subject);
    const auto a = record(from), b = record(to);
    require(a.at("subject") == subject && b.at("subject") == subject, "foreign clock anchor");
    const auto i = a.at("tick").get<std::int64_t>(), j = b.at("tick").get<std::int64_t>();
    require(j >= i, "reversed clock window");
    return {{"clock", clock(subject)}, {"from", from}, {"to", to},
            {"window", "[from,to)"}, {"duration", j - i}, {"coverage", "complete_verified_chain"}};
}
json Store::timeline(const std::string& subject) {
    auto* db = impl_->db;
    Transaction tx(db, false);
    const auto s = impl_->snapshot(subject);
    json result = {{"subject", subject}, {"tick", s.tick}, {"head", s.head},
                   {"memory", s.memory}, {"wake_at", optional_time(s.wake_at)},
                   {"lifecycle", s.lifecycle}, {"clock", impl_->clock(subject)}, {"commits", json::array()},
                   {"attempts", json::array()}, {"occasions", json::array()}};
    Statement c(db, "SELECT id,body FROM commits WHERE subject=? ORDER BY tick"); c.bind(1, subject);
    while (c.row()) result["commits"].push_back({{"id", c.text(0)}, {"body", json::parse(c.text(1))}});
    Statement a(db, "SELECT id,body,status,error FROM attempts WHERE subject=? ORDER BY seq"); a.bind(1, subject);
    while (a.row()) result["attempts"].push_back({{"id", a.text(0)}, {"body", json::parse(a.text(1))},
                                               {"status", a.text(2)}, {"error", a.text(3)}});
    result["abstentions"] = json::array();
    Statement outcomes(db, "SELECT o.id,o.body FROM abstentions o JOIN attempts a ON a.id=o.attempt WHERE a.subject=? ORDER BY a.seq");
    outcomes.bind(1, subject);
    while (outcomes.row()) result["abstentions"].push_back({{"id", outcomes.text(0)}, {"body", json::parse(outcomes.text(1))}});
    Statement e(db, "SELECT id,body,consumed FROM occasions WHERE subject=? ORDER BY seq"); e.bind(1, subject);
    while (e.row()) result["occasions"].push_back({{"id", e.text(0)}, {"body", json::parse(e.text(1))},
                                                {"consumed", e.text(2)}});
    tx.commit(); return result;
}
json Store::request_trace(const std::string& subject, const std::string& key, std::size_t limit) {
    require(limit > 0 && limit <= 256, "request trace limit must be 1..256");
    auto* db = impl_->db; Transaction tx(db, false);
    auto state = impl_->snapshot(subject);
    json out = {{"subject", subject}, {"head", state.head}, {"tick", state.tick},
        {"occasions", json::array()}, {"attempts", json::array()}, {"commits", json::array()},
        {"abstentions", json::array()}, {"attempt_count", 0}, {"truncated", false}};
    Statement o(db, "SELECT id,body,consumed FROM occasions WHERE subject=? AND key=?");
    o.bind(1, subject).bind(2, "user:" + key);
    if (o.row()) {
        auto oid = o.text(0);
        out["occasions"].push_back({{"id", oid}, {"body", json::parse(o.text(1))}, {"consumed", o.text(2)}});
        if (!o.text(2).empty()) out["commits"].push_back({{"id", o.text(2)}, {"body", record(o.text(2))}});
        Statement count(db, "SELECT count(*) FROM attempts WHERE subject=? AND occasion=?");
        count.bind(1, subject).bind(2, oid); count.row();
        out["attempt_count"] = count.number(0); out["truncated"] = count.number(0) > static_cast<std::int64_t>(limit);
        Statement a(db, "SELECT id,body,status,error FROM attempts WHERE subject=? AND occasion=? ORDER BY seq DESC LIMIT ?");
        a.bind(1, subject).bind(2, oid).bind(3, static_cast<std::int64_t>(limit));
        while (a.row()) {
            out["attempts"].push_back({{"id", a.text(0)}, {"body", json::parse(a.text(1))}, {"status", a.text(2)}, {"error", a.text(3)}});
            Statement b(db, "SELECT id,body FROM abstentions WHERE attempt=?"); b.bind(1, a.text(0));
            if (b.row()) out["abstentions"].push_back({{"id", b.text(0)}, {"body", json::parse(b.text(1))}});
        }
    }
    out["schedule"] = impl_->schedule(subject, wall_now());
    tx.commit(); return out;
}
json Store::open_tasks(const std::string& subject, const std::string& after, std::size_t limit) {
    require(limit > 0 && limit <= 256, "task page limit must be 1..256");
    auto* db = impl_->db; Transaction tx(db, false); const auto state = impl_->snapshot(subject);
    Statement q(db, "SELECT key,body FROM memory_entries WHERE subject=? AND current=1 AND key>? AND json_extract(body,'$.note.type')='task' AND json_extract(body,'$.note.status')='open' ORDER BY key LIMIT ?");
    q.bind(1,subject).bind(2,after).bind(3,static_cast<std::int64_t>(limit + 1));
    json items = json::array(); auto cursor = after; bool more = false;
    while (q.row()) {
        if (items.size() == limit) { more = true; break; }
        items.push_back(json::parse(q.text(1))); cursor = q.text(0);
    }
    tx.commit(); return {{"head",state.head},{"items",items},{"next_after_key",cursor},{"has_more",more}};
}
json Store::commits_page(const std::string& subject, std::int64_t after, std::size_t limit, const std::string& expected_head) {
    require(after >= -1 && limit > 0 && limit <= 256, "invalid history page bounds");
    auto* db = impl_->db; Transaction tx(db, false); auto state = impl_->snapshot(subject);
    if (!expected_head.empty() && state.head != expected_head) throw Conflict("history page head changed");
    json items = json::array(); auto cursor = after;
    Statement c(db, "SELECT id,body,tick FROM commits WHERE subject=? AND tick>? ORDER BY tick LIMIT ?");
    c.bind(1, subject).bind(2, after).bind(3, static_cast<std::int64_t>(limit));
    while (c.row()) { items.push_back({{"id", c.text(0)}, {"body", json::parse(c.text(1))}}); cursor = c.number(2); }
    tx.commit(); return {{"head", state.head}, {"items", items}, {"next_after_tick", cursor}, {"has_more", cursor < state.tick}};
}
void Store::verify(const std::string& subject) { verify_impl(subject, true); }
void Store::verify_impl(const std::string& subject, bool compare_memory_index) {
    auto* db = impl_->db;
    Transaction tx(db, false);
    // Enter this read snapshot through FTS5 before xIntegrity uses its cache.
    // SQLite 3.45.1 otherwise reports a malformed index after another connection
    // changes it. The empty phrase returns no hits and does not modify the DB.
    {
        Statement refresh(db, "SELECT rowid FROM memory_fts WHERE memory_fts MATCH '\"\"'");
        while (refresh.row()) {}
    }
    Statement integrity(db, "PRAGMA integrity_check");
    require(integrity.row(), "SQLite integrity_check returned no result");
    const auto integrity_result = integrity.text(0);
    require(integrity_result == "ok", "SQLite integrity failure: " + integrity_result);
    Statement fk(db, "PRAGMA foreign_key_check"); require(!fk.row(), "foreign key failure");
    auto s = impl_->snapshot(subject);
    // Loop invariants: one parse of limits and clock instead of one per record.
    const auto limits = impl_->config(subject);
    json memory = json::object();
    std::optional<millis> wake;
    std::string head;
    std::int64_t tick = 0;
    std::map<std::string, std::int64_t> chain; // verified commit id -> tick
    std::set<std::string> disposed; // attempts that settled their occasion as undeliverable
    bool failure_table = false;
    {
        Statement t(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='occasion_failures'");
        t.row(); failure_table = t.number(0) == 1; // Absent in older files opened read-only.
    }
    millis committed_wall = 0;
    BudgetHistory history;
    {
        Statement rows(db, "SELECT seq,admitted_at FROM attempts WHERE subject=? ORDER BY seq");
        rows.bind(1, subject);
        while (rows.row()) {
            require(rows.number(1) >= 0 && (history.at.empty() || rows.number(1) >= history.at.back()), "nonmonotonic admission clock");
            history.seq.push_back(rows.number(0)); history.at.push_back(rows.number(1));
        }
    }
    Statement q(db, "SELECT id,tick,body FROM commits WHERE subject=? ORDER BY tick"); q.bind(1, subject);
    while (q.row()) {
        auto b = json::parse(q.text(2));
        require(q.number(1) == tick && b.at("tick") == tick && b.at("subject") == subject &&
                (b.at("schema") == 1 || b.at("schema") == 2 || b.at("schema") == 3 || b.at("schema") == 4 || b.at("schema") == 5) && hash(b) == q.text(0), "commit integrity failure");
        if (tick == 0) {
            committed_wall = b.at("created_at").get<millis>();
            check_time(committed_wall);
            memory = b.at("memory");
            require(memory.is_object() && memory.dump().size() <= max_state, "invalid genesis memory");
            require(b.at("parent").is_null() && b.at("limits") == limits &&
                    b.at("memory") == memory && b.at("wake_at").is_null(), "genesis mismatch");
        } else {
            require(b.at("parent") == head, "broken commit parent");
            const auto oid = b.at("occasion").get<std::string>();
            auto e = impl_->event(oid);
            require(e.at("subject") == subject && e.at("parents").size() == 1 &&
                    chain.count(e.at("parents").at(0).get<std::string>()) == 1, "unknown event parent");
            Statement consumed(db, "SELECT consumed FROM occasions WHERE id=?"); consumed.bind(1, oid); consumed.row();
            require(consumed.text(0) == q.text(0), "occasion settlement mismatch");
            Statement a(db, "SELECT body,status FROM attempts WHERE id=?");
            const auto aid = b.at("attempt").get<std::string>(); a.bind(1, aid);
            require(a.row(), "missing committed attempt");
            const auto ab = json::parse(a.text(0));
            require(hash(ab) == aid && ab.at("subject") == subject && ab.at("parent") == head &&
                    ab.at("tick") == tick - 1 && ab.at("occasion") == oid && a.text(1) == "committed",
                    "attempt provenance mismatch");
            if (b.at("schema") >= 4) require(b.contains("emission"), "missing emission field");
            auto p = parse_proposal(b.at("proposal"));
            if (!b.contains("disposition")) {
                validate_emission(b.value("emission", json(nullptr)), aid, ab, b.at("proposal"));
            } else {
                const auto& d = b.at("disposition");
                const auto cap = limits.value("max_occasion_failures", std::int64_t{0});
                require(failure_table && cap > 0 && d.is_object() && d.size() == 4 &&
                        d.value("schema", "") == "cogg:disposition/v1" && d.value("reason", "") == "occasion_failures" &&
                        d.at("limit") == cap && d.at("failures").is_array() &&
                        static_cast<std::int64_t>(d.at("failures").size()) == cap &&
                        d.at("failures").back() == aid, "disposition integrity failure");
                require(b.at("emission").is_null() && p.kind == "null" && p.memory.empty() && p.notes.empty() &&
                        b.at("inference_elapsed_ms").is_null(), "disposition carries transition content");
                std::set<std::string> listed;
                for (const auto& id : d.at("failures")) {
                    const auto fid = id.get<std::string>();
                    require(listed.insert(fid).second, "duplicate disposition failure");
                    Statement f(db, "SELECT a.status,a.occasion,f.occasion FROM occasion_failures f "
                                    "JOIN attempts a ON a.id=f.attempt WHERE f.attempt=? AND a.subject=?");
                    f.bind(1, fid).bind(2, subject);
                    require(f.row() && f.text(1) == oid && f.text(2) == oid &&
                            f.text(0) == (fid == aid ? "committed" : "failed"), "disposition failure provenance mismatch");
                }
                disposed.insert(aid);
            }
            for (const auto& w : p.memory) memory[w.key] = w.value;
            require(b.at("memory") == memory, "memory replay mismatch");
            const auto at = b.at("committed_at").get<millis>();
            require(at >= ab.at("admitted_at").get<millis>() && at >= committed_wall, "commit precedes admission or predecessor");
            committed_wall = at;
            if (b.contains("inference_elapsed_ms") && !b.at("inference_elapsed_ms").is_null()) {
                const auto elapsed = b.at("inference_elapsed_ms").get<millis>();
                check_time(elapsed);
                require(at >= add(ab.at("admitted_at").get<millis>(), elapsed), "inference elapsed mismatch");
            }
            if (b.at("schema") == 1) {
                wake = p.wake_after_ms ? std::optional<millis>(add(at, std::max(*p.wake_after_ms,
                    limits.at("min_wake_ms").get<millis>()))) : std::nullopt;
            } else {
                const auto& saved = b.at("wake_plan");
                const auto replay = impl_->wake_plan(subject, limits, p, at,
                    saved.at("budget").at("through_seq").get<std::int64_t>(), &history);
                require(saved == replay, "wake decision replay mismatch");
                wake = read_time(replay.at("granted_at"));
            }
            require(b.at("wake_at") == optional_time(wake), "wake replay mismatch");
        }
        head = q.text(0); chain.emplace(head, tick); ++tick;
    }
    require(tick > 0 && s.tick == tick - 1 && s.head == head && s.memory == memory && s.wake_at == wake,
            "subject projection differs from committed history");
    const auto clock = impl_->clock(subject);
    Statement events(db, "SELECT id,body,key,kind,parent,consumed FROM occasions WHERE subject=?");
    events.bind(1, subject);
    while (events.row()) {
        auto b = json::parse(events.text(1));
        require(hash(b) == events.text(0) && b.at("subject") == subject && b.at("key") == events.text(2) &&
                b.at("kind") == events.text(3) && b.at("parents") == json::array({events.text(4)}) &&
                chain.count(events.text(4)) == 1, "occasion index mismatch");
        if (!events.text(5).empty()) require(chain.count(events.text(5)) == 1, "missing consuming commit");
    }
    Statement attempts(db, "SELECT id,body,parent,occasion,admitted_at,status,seq FROM attempts WHERE subject=? ORDER BY seq");
    attempts.bind(1, subject);
    millis previous = -1;
    while (attempts.row()) {
        auto b = json::parse(attempts.text(1));
        require(hash(b) == attempts.text(0) && b.at("subject") == subject &&
                b.at("parent") == attempts.text(2) && b.at("occasion") == attempts.text(3) &&
                b.at("admitted_at") == attempts.number(4) && chain.count(attempts.text(2)) == 1 &&
                attempts.number(4) >= previous, "admission integrity failure");
        require(b.at("schema").is_number_integer() && b.at("schema") >= 1 && b.at("schema") <= 5, "unsupported admission schema");
        validate_execution(b.value("execution", json(nullptr)));
        if (b.at("schema") >= 4) require(b.contains("execution"), "missing execution descriptor");
        if (b.contains("execution") && !b.at("execution").is_null())
            require(b.at("execution").at("backend") == b.at("backend"), "admission backend mismatch");
        if (b.at("schema") >= 5) validate_inputs(b.at("inputs"));
        const auto event = impl_->event(attempts.text(3));
        require(event.at("subject") == subject, "foreign admitted occasion");
        if (b.at("schema").get<int>() >= 2) {
            const auto at = attempts.number(4);
            const auto expected = impl_->budget(subject, limits, at, attempts.number(6) - 1, &history);
            const auto& temporal = b.at("temporal");
            require(expected == temporal.at("admission").at("budget") && expected.at("eligible_at").get<millis>() <= at,
                    "admission budget replay mismatch");
            require(temporal.at("clock") == clock && temporal.at("coordinate") == b.at("tick"),
                    "admission clock mismatch");
            // The parent is in the verified chain above; its tick needs no re-read and re-hash.
            require(chain.at(attempts.text(2)) == b.at("tick"), "admission coordinate mismatch");
            if (event.at("kind") == "scheduled")
                require(b.at("wall_observed_at").get<millis>() >= event.at("payload").at("due_at").get<millis>(),
                        "scheduled wake admitted before physical deadline");
        }
        previous = attempts.number(4);
        const auto status = attempts.text(5);
        require(status == "reserved" || status == "committed" || status == "failed" || status == "superseded" || status == "abstained",
                "unknown attempt status");
        Statement outcome(db, "SELECT id,body FROM abstentions WHERE attempt=?"); outcome.bind(1, attempts.text(0));
        const bool exists = outcome.row();
        require(exists == (status == "abstained"), "abstention settlement mismatch");
        if (exists) {
            const auto o = json::parse(outcome.text(1));
            require(o.is_object() && o.size() == 8 && hash(o) == outcome.text(0) &&
                o.at("schema") == "cogg:abstention/v1" && o.at("attempt") == attempts.text(0) &&
                o.at("subject") == subject && o.at("parent") == b.at("parent") && o.at("occasion") == b.at("occasion") &&
                o.at("execution") == b.value("execution", json(nullptr)) &&
                o.at("provider").is_object() && o.at("provider").dump().size() <= 8192 &&
                std::holds_alternative<Abstention>(parse_outcome(o.at("outcome"))), "abstention integrity failure");
            Statement used(db, "SELECT count(*) FROM commits WHERE subject=? AND json_extract(body,'$.attempt')=?");
            used.bind(1, subject).bind(2, attempts.text(0)); used.row();
            require(used.number(0) == 0, "abstained attempt committed");
        }
    }
    if (failure_table) {
        Statement f(db, "SELECT f.attempt,f.occasion,a.occasion,a.status FROM occasion_failures f "
                        "JOIN attempts a ON a.id=f.attempt WHERE a.subject=?");
        f.bind(1, subject);
        while (f.row())
            require(f.text(1) == f.text(2) && (f.text(3) == "failed" ||
                    (f.text(3) == "committed" && disposed.count(f.text(0)) == 1)),
                    "occasion failure index mismatch");
    }
    Statement accounting(db, "SELECT accounting_wall FROM subjects WHERE id=?");
    accounting.bind(1, subject); accounting.row();
    require(accounting.number(0) == std::max(committed_wall, previous), "accounting projection mismatch");
    detail::memory_verify(db, subject, compare_memory_index);
    tx.commit();
}
json Store::record(const std::string& id) {
    Statement q(impl_->db, "SELECT body FROM commits WHERE id=?");
    q.bind(1, id);
    require(q.row(), "record not found");
    auto body = json::parse(q.text(0));
    require(hash(body) == id, "record hash mismatch");
    return body;
}
void Runtime::fail_scoped(const std::string& attempt, const std::string& reason, FailureScope scope,
                          millis now, std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    disposed_ = store_.fail(attempt, reason, scope, wall_clock_ ? wall_clock_() : add(now, elapsed));
}
std::optional<Snapshot> Runtime::step(const std::string& subject, millis now, const json& context) {
    abstention_.reset();
    disposed_.reset();
    maintenance_error_.clear();
    if (!verified_.count(subject)) {
        store_.verify(subject);
        verified_.insert(subject);
    }
    auto a = store_.admit(subject, backend_.name(), now, backend_.memory_policy(),
                          [&](const Present& p) { return backend_.context_fits(p); }, nullptr, context);
    if (!a) return std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    Snapshot result;
    try {
        auto outcome = backend_.respond_attempt(*a, 3600000, {});
        validate_backend_outcome(outcome);
        if (const auto* abstention = std::get_if<Abstention>(&outcome)) {
            store_.abstain(a->id, *abstention, backend_.telemetry());
            abstention_ = *abstention; return std::nullopt;
        }
        const auto& p = std::get<Proposal>(outcome);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        result = store_.commit(a->id, p, wall_clock_ ? wall_clock_() : add(now, elapsed), {}, elapsed);
    } catch (const Conflict&) {
        store_.fail(a->id, "stale transition");
        return std::nullopt;
    } catch (const BackendFailure& e) {
        fail_scoped(a->id, e.what(), failure_scope(e.kind), now, started); throw;
    } catch (const ContextOverflow& e) {
        // Memory pressure is cured by maintenance, never by discarding the message.
        fail_scoped(a->id, e.what(), FailureScope::transient, now, started); throw;
    } catch (const std::exception& e) {
        // Unknown backend, storage and commit failures are not evidence against
        // the occasion. Hosts may explicitly classify a known policy rejection.
        fail_scoped(a->id, e.what(), FailureScope::transient, now, started); throw;
    } catch (...) {
        fail_scoped(a->id, "backend failed", FailureScope::transient, now, started); throw;
    }
    // A cache is not part of the subject transaction. Do not turn a cache error
    // into a failed inference or conceal a successful durable commit from callers.
    try { backend_.committed(a->present, result); }
    catch (const std::exception& e) { maintenance_error_ = e.what(); }
    catch (...) { maintenance_error_ = "backend maintenance failed"; }
    return result;
}
json Store::recall(const std::string& subject, MemoryPolicy policy) {
    verify(subject);
    Transaction tx(impl_->db, false);
    Present present; present.state = impl_->snapshot(subject);
    detail::project_memory(present, detail::memory_candidates(impl_->db, subject, policy), policy, {});
    auto result = json{{"memory", present.working_memory}, {"view", present.memory_view}};
    tx.commit(); return result;
}
json Store::memory_record(const std::string& subject, const std::string& id) {
    verify(subject);
    Statement q(impl_->db, "SELECT body FROM memory_entries WHERE subject=? AND id=?");
    q.bind(1, subject).bind(2, id); require(q.row(), "unknown or foreign memory source");
    return json::parse(q.text(0));
}
void Store::rebuild_memory(const std::string& subject) {
    verify_impl(subject, false);
    Transaction tx(impl_->db);
    detail::memory_rebuild(impl_->db, subject);
    tx.commit(); verify(subject);
}
millis wall_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace cogg
