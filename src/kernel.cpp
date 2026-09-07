#include "cogg/kernel.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <limits>
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
            l.max_wake_ms >= l.min_wake_ms && l.max_wake_ms <= max_delay,
            "invalid runtime limits");
    return {{"min_wake_ms", l.min_wake_ms}, {"max_attempts", l.max_attempts},
            {"period_ms", l.period_ms}, {"max_wake_ms", l.max_wake_ms}};
}
void validate(const Proposal& p) {
    require(p.kind == "null" || p.kind == "reflection" || p.kind == "speech",
            "unsupported transition kind");
    require(p.text.size() <= max_payload, "speech too large");
    require(p.kind == "speech" || p.text.empty(), "only speech may emit text");
    require(p.memory.size() <= 64, "too many memory writes");
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

json proposal_json(const Proposal& p) {
    validate(p);
    json writes = json::array();
    for (const auto& w : p.memory) writes.push_back({{"key", w.key}, {"value", w.value}});
    return {{"kind", p.kind}, {"text", p.text}, {"memory", writes},
            {"wake_after_ms", optional_time(p.wake_after_ms)}};
}
Proposal parse_proposal(const json& j) {
    require(j.is_object(), "transition must be an object");
    const std::set<std::string> fields = {"kind", "text", "memory", "wake_after_ms"};
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
    validate(p); return p;
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
                std::int64_t through = std::numeric_limits<std::int64_t>::max()) {
        const auto period = limits.at("period_ms").get<millis>();
        const auto lower = now >= period ? now - period : -1;
        Statement q(db, "SELECT admitted_at,seq FROM attempts WHERE subject=? AND seq<=? ORDER BY seq");
        q.bind(1, subject).bind(2, through);
        std::vector<millis> active;
        std::optional<millis> last;
        std::int64_t watermark = 0;
        while (q.row()) {
            last = q.number(0); watermark = q.number(1);
            if (*last > lower) active.push_back(*last);
        }
        millis floor_at = now, quota_at = now;
        if (last) floor_at = std::max(now, add(*last, limits.at("min_wake_ms").get<millis>()));
        const auto cap = limits.at("max_attempts").get<std::size_t>();
        if (active.size() >= cap) quota_at = add(active[active.size() - cap], period);
        return {{"used", active.size()}, {"limit", cap}, {"period_ms", period},
                {"through_seq", watermark}, {"floor_at", floor_at}, {"quota_at", quota_at},
                {"eligible_at", std::max(floor_at, quota_at)}};
    }
    json wake_plan(const std::string& subject, const json& limits, const Proposal& p,
                   millis now, std::int64_t through = std::numeric_limits<std::int64_t>::max()) {
        const auto b = budget(subject, limits, now, through);
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

Store::Store(const std::string& path) : impl_(std::make_unique<Impl>()) {
    require(path != ":memory:" && !path.empty(), "a durable database path is required");
    if (sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
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
    sql(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    Transaction tx(db);
    {
        Statement q(db, "PRAGMA user_version"); q.row();
        require(q.number(0) == 0 || q.number(0) == 1 || q.number(0) == 2, "unsupported database version");
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
CREATE INDEX IF NOT EXISTS attempts_budget ON attempts(subject,admitted_at);
CREATE TABLE IF NOT EXISTS commits(
 id TEXT PRIMARY KEY, subject TEXT NOT NULL REFERENCES subjects(id),
 tick INTEGER NOT NULL, body TEXT NOT NULL, UNIQUE(subject,tick));
PRAGMA application_id=1129269063;
PRAGMA user_version=2;
)SQL");
    tx.commit();
}
Store::~Store() = default;

void Store::create(const std::string& subject, Limits limits, millis now, const json& initial_memory) {
    require(initial_memory.is_object() && initial_memory.dump().size() <= max_state, "invalid initial memory");
    check_text(subject, 128, "subject"); check_time(now);
    auto l = limits_json(limits);
    Transaction tx(impl_->db);
    json genesis = {{"schema", 2}, {"subject", subject}, {"tick", 0}, {"parent", nullptr},
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
std::optional<Attempt> Store::admit(const std::string& subject, const std::string& backend, millis now) {
    check_time(now); check_text(backend, 200, "backend");
    auto* db = impl_->db;
    Transaction tx(db);
    auto s = impl_->snapshot(subject);
    const auto decision = impl_->schedule(subject, now);
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
    json body = {{"schema", 2}, {"nonce", random_id()}, {"subject", subject},
                 {"parent", s.head}, {"tick", s.tick}, {"occasion", o.id},
                 {"backend", backend}, {"admitted_at", now}, {"prior_unsettled", unsettled},
                 {"wall_observed_at", observed}, {"temporal", temporal}};
    const auto id = hash(body);
    Statement insert(db, "INSERT INTO attempts(id,subject,parent,occasion,admitted_at,body,status) "
                         "VALUES(?,?,?,?,?,?,'reserved')");
    insert.bind(1, id).bind(2, subject).bind(3, s.head).bind(4, o.id).bind(5, now)
          .bind(6, body.dump()).done();
    tx.commit();
    return Attempt{id, Present{s, o, unsettled, temporal}};
}

Snapshot Store::commit(const std::string& attempt, const Proposal& proposal, millis now,
                       const std::function<void(CommitPoint)>& hook, std::optional<millis> inference_elapsed_ms) {
    if (inference_elapsed_ms) check_time(*inference_elapsed_ms);
    check_time(now); const auto p = proposal_json(proposal);
    auto* db = impl_->db;
    Transaction tx(db);
    Statement aq(db, "SELECT body,status FROM attempts WHERE id=?");
    aq.bind(1, attempt); require(aq.row(), "attempt missing");
    if (aq.text(1) != "reserved") throw Conflict("attempt already settled");
    const auto a = json::parse(aq.text(0));
    require(hash(a) == attempt, "attempt hash mismatch");
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
    json body = {{"schema", 2}, {"subject", subject}, {"tick", tick}, {"parent", s.head},
                 {"occasion", oid}, {"attempt", attempt}, {"proposal", p},
                 {"memory", memory}, {"wake_at", optional_time(wake)}, {"committed_at", now},
                 {"wall_observed_at", observed}, {"wake_plan", plan},
                 {"inference_elapsed_ms", optional_time(inference_elapsed_ms)}};
    auto id = hash(body);
    Statement insert(db, "INSERT INTO commits VALUES(?,?,?,?)");
    insert.bind(1, id).bind(2, subject).bind(3, tick).bind(4, body.dump()).done();
    Statement head(db, "UPDATE subjects SET tick=?,head=?,memory=?,wake=?,accounting_wall=? WHERE id=? AND head=?");
    head.bind(1, tick).bind(2, id).bind(3, memory.dump()).bind(4, optional_time(wake).dump())
        .bind(5, now).bind(6, subject).bind(7, s.head).done();
    require(sqlite3_changes(db) == 1, "head update failed");
    Statement consumed(db, "UPDATE occasions SET consumed=? WHERE id=? AND consumed IS NULL");
    consumed.bind(1, id).bind(2, oid).done();
    require(sqlite3_changes(db) == 1, "occasion update failed");
    Statement settled(db, "UPDATE attempts SET status='committed' WHERE id=?");
    settled.bind(1, attempt).done();
    Statement obsolete(db, "UPDATE attempts SET status='superseded' WHERE subject=? AND parent=? AND status='reserved'");
    obsolete.bind(1, subject).bind(2, s.head).done();
    if (hook) hook(CommitPoint::before_sql_commit);
    tx.commit();
    if (hook) hook(CommitPoint::after_sql_commit);
    return Snapshot{subject, tick, id, memory, wake, wake ? "sleeping" : "waiting_external"};
}
void Store::fail(const std::string& attempt, const std::string& reason) {
    Statement q(impl_->db, "UPDATE attempts SET status='failed',error=? WHERE id=? AND status='reserved'");
    q.bind(1, reason.substr(0, 1024)).bind(2, attempt).done();
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
    Statement e(db, "SELECT id,body,consumed FROM occasions WHERE subject=? ORDER BY seq"); e.bind(1, subject);
    while (e.row()) result["occasions"].push_back({{"id", e.text(0)}, {"body", json::parse(e.text(1))},
                                                {"consumed", e.text(2)}});
    tx.commit(); return result;
}
void Store::verify(const std::string& subject) {
    auto* db = impl_->db;
    Transaction tx(db, false);
    Statement integrity(db, "PRAGMA integrity_check");
    require(integrity.row() && integrity.text(0) == "ok", "SQLite integrity failure");
    Statement fk(db, "PRAGMA foreign_key_check"); require(!fk.row(), "foreign key failure");
    auto s = impl_->snapshot(subject);
    json memory = json::object();
    std::optional<millis> wake;
    std::string head;
    std::int64_t tick = 0;
    std::set<std::string> chain;
    millis committed_wall = 0;
    Statement q(db, "SELECT id,tick,body FROM commits WHERE subject=? ORDER BY tick"); q.bind(1, subject);
    while (q.row()) {
        auto b = json::parse(q.text(2));
        require(q.number(1) == tick && b.at("tick") == tick && b.at("subject") == subject &&
                (b.at("schema") == 1 || b.at("schema") == 2) && hash(b) == q.text(0), "commit integrity failure");
        if (tick == 0) {
            committed_wall = b.at("created_at").get<millis>();
            check_time(committed_wall);
            memory = b.at("memory");
            require(memory.is_object() && memory.dump().size() <= max_state, "invalid genesis memory");
            require(b.at("parent").is_null() && b.at("limits") == impl_->config(subject) &&
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
            auto p = parse_proposal(b.at("proposal"));
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
                    impl_->config(subject).at("min_wake_ms").get<millis>()))) : std::nullopt;
            } else {
                const auto& saved = b.at("wake_plan");
                const auto replay = impl_->wake_plan(subject, impl_->config(subject), p, at,
                    saved.at("budget").at("through_seq").get<std::int64_t>());
                require(saved == replay, "wake decision replay mismatch");
                wake = read_time(replay.at("granted_at"));
            }
            require(b.at("wake_at") == optional_time(wake), "wake replay mismatch");
        }
        head = q.text(0); chain.insert(head); ++tick;
    }
    require(tick > 0 && s.tick == tick - 1 && s.head == head && s.memory == memory && s.wake_at == wake,
            "subject projection differs from committed history");
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
        const auto event = impl_->event(attempts.text(3));
        require(event.at("subject") == subject, "foreign admitted occasion");
        if (b.at("schema") == 2) {
            const auto at = attempts.number(4);
            const auto expected = impl_->budget(subject, impl_->config(subject), at, attempts.number(6) - 1);
            const auto& temporal = b.at("temporal");
            require(expected == temporal.at("admission").at("budget") && expected.at("eligible_at").get<millis>() <= at,
                    "admission budget replay mismatch");
            require(temporal.at("clock") == impl_->clock(subject) && temporal.at("coordinate") == b.at("tick"),
                    "admission clock mismatch");
            require(record(attempts.text(2)).at("tick") == b.at("tick"), "admission coordinate mismatch");
            if (event.at("kind") == "scheduled")
                require(b.at("wall_observed_at").get<millis>() >= event.at("payload").at("due_at").get<millis>(),
                        "scheduled wake admitted before physical deadline");
        }
        previous = attempts.number(4);
        const auto status = attempts.text(5);
        require(status == "reserved" || status == "committed" || status == "failed" || status == "superseded",
                "unknown attempt status");
    }
    Statement accounting(db, "SELECT accounting_wall FROM subjects WHERE id=?");
    accounting.bind(1, subject); accounting.row();
    require(accounting.number(0) == std::max(committed_wall, previous), "accounting projection mismatch");
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
std::optional<Snapshot> Runtime::step(const std::string& subject, millis now) {
    maintenance_error_.clear();
    if (!verified_.count(subject)) {
        store_.verify(subject);
        verified_.insert(subject);
    }
    auto a = store_.admit(subject, backend_.name(), now);
    if (!a) return std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    Snapshot result;
    try {
        auto p = backend_.propose(a->present);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        result = store_.commit(a->id, p, wall_clock_ ? wall_clock_() : add(now, elapsed), {}, elapsed);
    } catch (const Conflict&) {
        store_.fail(a->id, "stale transition");
        return std::nullopt;
    } catch (const std::exception& e) {
        store_.fail(a->id, e.what()); throw;
    }
    // A cache is not part of the subject transaction. Do not turn a cache error
    // into a failed inference or conceal a successful durable commit from callers.
    try { backend_.committed(a->present, result); }
    catch (const std::exception& e) { maintenance_error_ = e.what(); }
    catch (...) { maintenance_error_ = "backend maintenance failed"; }
    return result;
}
millis wall_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace cogg
