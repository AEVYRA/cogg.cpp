#include "protocol.hpp"
#include "cogg/routing.hpp"
#ifdef COGG_HTTP
#include "cogg/http_backend.hpp"
#endif
#ifdef COGG_SELF
#include "cogg/self_state.hpp"
#endif
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
using namespace cogg;
namespace ui = cogg::ui;
namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
void require(bool condition, const std::string& message) { if (!condition) throw Error(message); }
bool message_payload(const json& p) {
    return p.is_object() && p.contains("text") && p.at("text").is_string() &&
           (!p.contains("type") || p.at("type") == "message");
}
class Demo final : public Backend {
public:
    std::string name() const override { return "cogg:tui-demo/v1"; }
    Proposal propose(const Present& p) override {
        Proposal out;
        if (p.occasion.kind == "external" && message_payload(p.occasion.payload)) {
            out.kind = "speech"; out.text = "Demo echo: " + p.occasion.payload.value("text", "");
            out.notes.push_back({"demo.last-message", p.occasion.payload.value("text", ""), "episode", "active", {}, {}});
        }
        return out;
    }
};
struct Options {
    std::string db, subject, socket, config, executor;
    bool create = false, demo = false, remote = false;
};
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--create") o.create = true;
        else if (a == "--demo") o.demo = true;
        else if (a == "--allow-remote") o.remote = true;
        else {
            require(i + 1 < argc, "missing argument for " + a); std::string v = argv[++i];
            if (a == "--db") o.db = v; else if (a == "--subject") o.subject = v;
            else if (a == "--socket") o.socket = v; else if (a == "--config") o.config = v;
            else if (a == "--executor") o.executor = v; else throw Error("unknown option: " + a);
        }
    }
    require(!o.db.empty() && !o.subject.empty() && !o.socket.empty(), "required: --db FILE --subject ID --socket PATH");
    require(o.demo ? o.config.empty() && o.executor.empty() : !o.config.empty() && !o.executor.empty(),
            "choose --demo OR --config FILE --executor ID [--allow-remote]");
    o.db = fs::weakly_canonical(fs::absolute(o.db)).string();
    o.socket = fs::absolute(o.socket).string();
    return o;
}
void current_database_only(const Options& o) {
    if (!fs::exists(o.db)) { require(o.create, "database missing; use --create for a new subject"); return; }
    require(!o.create, "--create refuses an existing database");
    sqlite3* raw = nullptr;
    require(sqlite3_open_v2(o.db.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "cannot read database header");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    auto pragma = [&](const char* sql) {
        sqlite3_stmt* q = nullptr;
        require(sqlite3_prepare_v2(db.get(), sql, -1, &q, nullptr) == SQLITE_OK, "cannot inspect database schema");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(q, sqlite3_finalize);
        require(sqlite3_step(q) == SQLITE_ROW, "cannot inspect database schema"); return sqlite3_column_int(q, 0);
    };
    require(pragma("PRAGMA application_id") == 1129269063 && pragma("PRAGMA user_version") == 5,
            "host requires cogg schema 5; older databases must be migrated explicitly on a separate copy");
}
struct Control {
    std::mutex mutex;
    bool paused = true, running = false;
    std::string last = "started paused", error;
};
json commit_summary(const json& row) {
    const auto& b = row.at("body");
    return {{"id", row.at("id")}, {"tick", b.at("tick")},
            {"kind", b.contains("proposal") ? b.at("proposal").at("kind") : json("genesis")},
            {"occasion", b.value("occasion", json(nullptr))}, {"at", b.value("committed_at", b.value("created_at", json(nullptr)))}};
}
json projection(Store& store, const std::string& subject, Control& control, const std::string& executor) {
    const auto t = store.timeline(subject);
    json conversation = json::array(), timeline = json::array();
    // A partial window is explicitly labelled; full immutable records remain inspectable.
    for (const auto& e : t.at("occasions")) {
        const auto& b = e.at("body"); const auto& p = b.at("payload");
        if (b.at("kind") == "external" && message_payload(p))
            conversation.push_back({{"id", e.at("id")}, {"role", "user"}, {"text", ui::excerpt(p.value("text", ""))},
                {"at", b.at("received_at")}, {"status", e.at("consumed") == "" ? "pending" : "consumed"}});
    }
    for (const auto& c : t.at("commits")) {
        timeline.push_back(commit_summary(c)); const auto& b = c.at("body");
        if (b.contains("proposal") && b.at("proposal").at("kind") == "speech")
            conversation.push_back({{"id", c.at("id")}, {"role", "subject"},
                {"text", ui::excerpt(b.at("proposal").at("text").get<std::string>())},
                {"at", b.at("committed_at")}, {"tick", b.at("tick")}, {"status", "committed"}});
    }
    std::stable_sort(conversation.begin(), conversation.end(), [](const json& a, const json& b) { return a.at("at") < b.at("at"); });
    const auto conversation_total = conversation.size(), commits_total = timeline.size();
    if (conversation.size() > 60) conversation.erase(conversation.begin(), conversation.end() - 60);
    if (timeline.size() > 80) timeline.erase(timeline.begin(), timeline.end() - 80);
    json attempts = json::array();
    for (std::size_t i = t.at("attempts").size() > 20 ? t.at("attempts").size() - 20 : 0; i < t.at("attempts").size(); ++i) {
        const auto& a = t.at("attempts").at(i);
        json row = {{"id", a.at("id")}, {"status", a.at("status")}, {"backend", a.at("body").at("backend")},
                    {"occasion", a.at("body").at("occasion")}, {"error", ui::excerpt(a.at("error").get<std::string>(), 512)}};
        for (const auto& r : t.at("abstentions")) if (r.at("body").at("attempt") == a.at("id")) row["abstention"] = r.at("body").at("outcome");
        attempts.push_back(row);
    }
    json host;
    { std::lock_guard lock(control.mutex); host = {{"paused", control.paused}, {"running", control.running},
        {"last", control.last}, {"error", control.error}, {"executor", executor}}; }
    return {{"subject", subject}, {"tick", t.at("tick")}, {"head", t.at("head")}, {"lifecycle", t.at("lifecycle")},
        {"wake_at", t.at("wake_at")}, {"host", host}, {"schedule", store.schedule(subject, wall_now())},
        {"conversation", conversation}, {"conversation_total", conversation_total}, {"commits", timeline},
        {"commits_total", commits_total}, {"attempts", attempts}, {"sampled_at", wall_now()}};
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "cogg-host --db FILE --subject ID --socket PATH [--create] (--demo | --config FILE --executor ID [--allow-remote])\n"
                         "Linux, one subject per host. Starts PAUSED; use cogg-tui /resume. SIGTERM stops host.\n"; return 0;
        }
        auto o = options(argc, argv);
        ::umask(0077);
        auto dir = fs::path(o.socket).parent_path(); fs::create_directories(dir);
        struct stat st{};
        require(::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 0077) == 0,
                "socket directory must be owned by you, mode 0700");
        // Separate ownership locks cover the socket and database, including different socket paths.
        ui::Fd socket_lock(::open((o.socket + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        require(socket_lock.value >= 0 && ::flock(socket_lock.value, LOCK_EX | LOCK_NB) == 0, "socket already owned by another host");
        ui::Fd db_lock(::open((o.db + ".host.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        require(db_lock.value >= 0 && ::flock(db_lock.value, LOCK_EX | LOCK_NB) == 0, "database already owned by another host");
        current_database_only(o);
        std::function<std::unique_ptr<Backend>()> factory;
        Capabilities caps;
        std::string executor = o.demo ? "demo" : o.executor;
        if (o.demo) factory = [] { return std::make_unique<Demo>(); };
        else {
#ifdef COGG_HTTP
            std::ifstream in(o.config); require(bool(in), "cannot open executor config"); json config; in >> config;
            auto selected = config.at("executors").at(o.executor);
            HttpBackend probe(selected); caps = probe.capabilities();
            require(!caps.remote || o.remote, "remote executor requires --allow-remote");
            factory = [selected] { return std::make_unique<HttpBackend>(selected); };
#else
            throw Error("HTTP support not built; configure with -DCOGG_HTTP=ON");
#endif
        }
        Store store(o.db);
        if (o.create) store.create(o.subject, Limits{}, wall_now());
        store.verify(o.subject);
        const bool guarded = store.snapshot(o.subject).memory.contains("self.profile");
#ifdef COGG_SELF
        if (guarded) inspect_self(store, o.subject);
#else
        require(!guarded, "self-state subject requires a COGG_SELF build");
#endif
        // Only remove a stale socket after both ownership locks; never replace a regular file.
        if (::lstat(o.socket.c_str(), &st) == 0) {
            require(S_ISSOCK(st.st_mode) && st.st_uid == ::getuid(), "socket path is not an owned socket");
            ui::Fd check(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
            sockaddr_un a{}; a.sun_family = AF_UNIX;
            require(o.socket.size() < sizeof(a.sun_path), "socket path too long"); std::strcpy(a.sun_path, o.socket.c_str());
            require(::connect(check.value, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 && errno == ECONNREFUSED, "socket is active or cannot be checked");
            require(::unlink(o.socket.c_str()) == 0, "cannot remove stale socket");
        }
        ui::Fd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)); require(listener.value >= 0, "cannot create host socket");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        require(o.socket.size() < sizeof(address.sun_path), "socket path too long"); std::strcpy(address.sun_path, o.socket.c_str());
        require(::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(listener.value, 16) == 0, "cannot bind/listen on host socket");
        require(::chmod(o.socket.c_str(), 0600) == 0, "cannot restrict socket permissions");
        struct Unlink { std::string path; ~Unlink() { ::unlink(path.c_str()); } } unlink{o.socket};
        ui::Fd audit(::open((o.socket + ".operators.jsonl").c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
        require(audit.value >= 0, "cannot open operator audit");
        auto log_operator = [&](const std::string& command) {
            auto line = json({{"schema", "cogg:host-control/v1"}, {"session", std::to_string(::getpid())},
                {"subject", o.subject}, {"db", o.db}, {"at", wall_now()}, {"command", command}}).dump() + '\n';
            require(::write(audit.value, line.data(), line.size()) == static_cast<ssize_t>(line.size()) && ::fsync(audit.value) == 0, "operator audit write failed");
        };
        log_operator("start-paused");
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::signal(SIGPIPE, SIG_IGN);
        Control control;
        std::jthread worker([&](std::stop_token token) {
            try {
                Store engine(o.db); Registry registry; registry.add(executor, caps, factory);
                RoutedRuntime runtime(engine, registry, wall_now);
#ifdef COGG_SELF
                auto backend = guarded ? factory() : nullptr;
                auto execution = guarded ? json({{"schema", "cogg:execution/v1"}, {"executor", executor}, {"backend", backend->name()},
                    {"session", ui::nonce()}, {"capabilities", capabilities_json(caps)}}) : json(nullptr);
#endif
                while (!stopped && !token.stop_requested()) {
                    bool enabled;
                    { std::lock_guard lock(control.mutex); enabled = !control.paused; }
                    if (enabled && engine.schedule(o.subject, wall_now()).at("status") == "ready") {
                        { std::lock_guard lock(control.mutex); if (control.paused) continue; control.running = true; }
                        auto cancelled = [&] { return stopped || token.stop_requested(); };
                        std::string status, maintenance;
#ifdef COGG_SELF
                        if (guarded) {
                            SelfRuntime self(engine, *backend, execution);
                            auto result = self.step(o.subject, wall_now(), nullptr, json::array(), 60000, cancelled);
                            status = result.status; maintenance = result.maintenance_error;
                        } else
#endif
                        {
                            Route route; route.executors = {executor}; route.allow_remote = o.remote;
                            route.timeout_ms = route.attempt_timeout_ms = 60000; route.cancelled = cancelled;
                            auto result = runtime.step(o.subject, route, wall_now()); status = result.status; maintenance = result.maintenance_error;
                        }
                        std::lock_guard lock(control.mutex); control.running = false; control.last = status; control.error = maintenance;
                        // No invisible retries/billing loop on a still-pending occasion.
                        if (status != "committed" && status != "waiting") control.paused = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(control.mutex); control.running = false; control.paused = true;
                control.last = "worker stopped"; control.error = ui::excerpt(e.what(), 512);
            }
        });
        std::cout << "cogg-host ready: " << o.socket << " (paused, executor " << executor << ")" << std::endl;
        while (!stopped) {
            pollfd p{listener.value, POLLIN, 0}; if (::poll(&p, 1, 200) <= 0) continue;
            ui::Fd client(::accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC)); if (client.value < 0) continue;
            timeval timeout{1, 0}; ::setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            json reply = {{"protocol", "cogg:host/v1"}, {"ok", true}};
            try {
                const auto req = ui::receive_frame(client.value, 65536);
                require(req.is_object() && req.value("protocol", "") == "cogg:host/v1", "unsupported protocol");
                const auto op = req.at("op").get<std::string>(); json data;
                if (op == "snapshot") data = projection(store, o.subject, control, executor);
                else if (op == "send") {
                    const auto text = req.at("text").get<std::string>(), key = req.at("key").get<std::string>();
                    require(!text.empty() && text.size() <= 8192 && !key.empty() && key.size() <= 128, "message/key length invalid");
                    data = {{"occasion", store.submit(o.subject, "tui:" + key, {{"type", "message"}, {"text", text}}, wall_now())}};
                } else if (op == "pause" || op == "resume") {
                    std::lock_guard lock(control.mutex);
                    require(control.last != "worker stopped", "worker stopped; restart host after inspecting error");
                    log_operator(op); control.paused = op == "pause";
                    data = {{"paused", control.paused}, {"running", control.running}, {"note", "pause takes effect before next admission; an in-flight attempt may commit"}};
                } else if (op == "inspect") {
                    auto id = req.value("id", ""); if (id.empty()) id = store.snapshot(o.subject).head;
                    auto record = store.record(id); require(record.at("subject") == o.subject, "record belongs to another subject"); data = {{"id", id}, {"body", record}};
                } else if (op == "memory") {
                    MemoryPolicy policy; policy.query = req.value("query", ""); require(policy.query.size() <= 1024, "query too long");
                    policy.max_bytes = 32768; policy.max_items = 32; policy.strategy = policy.query.empty() ? "recent" : "lexical";
                    data = {{"view", store.recall(o.subject, policy).at("view")}, {"working_memory", store.snapshot(o.subject).memory},
                        {"note", "bounded recent/search view; not necessarily the executor's admitted context"}};
#ifdef COGG_SELF
                    if (guarded) data["self"] = inspect_self(store, o.subject);
#endif
                } else if (op == "why-awake") {
                    const auto t = store.timeline(o.subject); const auto& last = t.at("commits").back().at("body");
                    data = {{"schedule", store.schedule(o.subject, wall_now())}, {"last_wake_plan", last.value("wake_plan", json(nullptr))}};
                } else if (op == "since") {
                    const auto tick = req.at("tick").get<std::int64_t>(); const auto t = store.timeline(o.subject);
                    require(tick >= 0 && tick <= t.at("tick").get<std::int64_t>(), "tick outside subject history");
                    json rows = json::array(); for (const auto& c : t.at("commits")) if (c.at("body").at("tick").get<std::int64_t>() > tick && rows.size() < 100) rows.push_back(commit_summary(c));
                    data = {{"after_tick", tick}, {"through_tick", rows.empty() ? json(tick) : rows.back().at("tick")},
                        {"head_tick", t.at("tick")}, {"commits", rows}, {"more", t.at("tick").get<std::int64_t>() - tick > 100}};
                } else throw Error("unknown host operation");
                reply["data"] = data;
                if (reply.dump().size() + 1 > ui::frame_limit) throw Error("record exceeds IPC limit; use offline cogg-cli inspection");
            } catch (const std::exception& e) { reply = {{"protocol", "cogg:host/v1"}, {"ok", false}, {"error", ui::excerpt(e.what(), 512)}}; }
            try { ui::send_frame(client.value, reply); } catch (const std::exception&) { /* disconnected client has no runtime effect */ }
        }
        worker.request_stop(); worker.join(); log_operator("stop"); return 0;
    } catch (const std::exception& e) { std::cerr << "cogg-host: " << e.what() << '\n'; return 2; }
}
