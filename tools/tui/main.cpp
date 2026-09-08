#include "protocol.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
using namespace ftxui;
namespace ui = cogg::ui;
using ui::json;
namespace {
const std::string help =
    "cogg — a window into a durable subject\n\n"
    "F1 Help   F2 Timeline   F3 Memory   F4 Inspect head   F5 Talk / Watch\n"
    "PgUp / PgDn scroll; Ctrl+Q disconnects; Ctrl+C clears input.\n"
    "Enter sends a message to the subject's durable inbox.\n\n"
    "/talk       conversation, committed speech only\n"
    "/watch      disable sending and host controls in this client\n"
    "/state      scheduler and host state\n"
    "/timeline   accepted transitions + attempt diagnostics\n"
    "/memory [query]   bounded deposits and working memory\n"
    "/inspect [full commit ID]   verified record, default current head\n"
    "/since [tick]   commits since connection, or an explicit tick\n"
    "/why-awake  actual occasion and last committed wake plan\n"
    "/resume     allow execution (also explicitly retries a pending failure)\n"
    "/pause      stop new admissions; current inference may still commit\n"
    "/quit       disconnect; the host continues running\n\n"
    "The host starts PAUSED. Send a message, then /resume.\n"
    "Abstention / failure pauses execution; the message remains pending.\n"
    "Watch is a client mode, not an access-control boundary.\n"
    "Demo echoes text; it is not an LLM. Reflection and null are not speech.";
std::string safe(const std::string& s) { return ui::display_text(s); }
std::string value(const json& j) { return j.is_string() ? safe(j.get<std::string>()) : safe(j.dump()); }
struct View {
    json snapshot = nullptr, detail = nullptr;
    std::string mode = "talk", notice = "Connecting…", error;
    bool connected = false, watch = false, sending = false;
    int scroll = 1000000;
    long long anchor = -1;
};
Element document(const std::string& content) {
    Elements lines; std::istringstream in(safe(content)); std::string line;
    while (std::getline(in, line)) lines.push_back(line.empty() ? text(" ") : paragraph(line));
    if (lines.empty()) lines.push_back(text(" "));
    return vbox(std::move(lines));
}
Element layout(const View& v, Element input, int width, Box& content_box) {
    auto accent = color(Color::Cyan);
    std::string subject = v.snapshot.is_null() ? "connecting" : value(v.snapshot.at("subject"));
    std::string runtime = "DISCONNECTED";
    if (v.connected) {
        const auto& h = v.snapshot.at("host");
        runtime = h.at("running") == true ? "INFERENCE" : h.at("paused") == true ? "HOST PAUSED" : value(v.snapshot.at("schedule").at("status"));
    }
    Elements title = {text(" cogg ") | bold | accent, text(subject) | bold, filler(), text(runtime) | (v.connected ? accent : color(Color::Red)), text(" ")};
    Elements rows;
    if (v.mode == "help") rows.push_back(document(help));
    else if (v.mode == "talk") {
        if (v.snapshot.is_null() || v.snapshot.at("conversation").empty())
            rows.push_back(document("No conversation yet. Write a message, then /resume.\nF1 explains host controls and the history views."));
        else {
            const auto& s = v.snapshot;
            if (s.at("conversation_total").get<std::size_t>() > s.at("conversation").size()) rows.push_back(text("Recent 60 messages; older records remain in the database.") | dim);
            for (const auto& m : s.at("conversation")) {
                bool user = m.at("role") == "user";
                rows.push_back(text((user ? "YOU" : subject) + "  ·  " + value(m.at("status")) + (m.contains("tick") ? "  ·  tick " + value(m.at("tick")) : "")) | bold | (user ? color(Color::Yellow) : accent));
                rows.push_back(document(m.at("text").get<std::string>())); rows.push_back(text(" "));
            }
        }
    } else if (v.mode == "timeline" && !v.snapshot.is_null()) {
        rows.push_back(text("COMMITS  (latest 80; /inspect FULL_ID opens a record)") | bold | accent);
        for (const auto& c : v.snapshot.at("commits")) {
            rows.push_back(paragraph("tick " + value(c.at("tick")) + "  " + value(c.at("kind")) + "  " + value(c.at("id"))));
        }
        rows.push_back(separator()); rows.push_back(text("ATTEMPTS  (latest 20; diagnostics, not subject speech)") | bold | accent);
        for (const auto& a : v.snapshot.at("attempts")) {
            rows.push_back(paragraph(value(a.at("status")) + "  " + value(a.at("backend"))));
            if (a.contains("abstention")) rows.push_back(document(a.at("abstention").dump(2)));
            if (a.at("error") != "") rows.push_back(document(value(a.at("error"))));
        }
    } else if (v.mode == "memory" && !v.detail.is_null()) {
        const auto& memory = v.detail.at("view");
        rows.push_back(text("DEPOSITS · " + value(memory.at("strategy"))) | bold | accent);
        rows.push_back(document(v.detail.at("note").get<std::string>()) | dim);
        rows.push_back(text(" "));
        for (const auto& n : memory.at("items")) {
            rows.push_back(paragraph(value(n.at("key")) + " · " + value(n.at("type")) + " · " + value(n.at("status"))) | bold | accent);
            rows.push_back(document(n.at("text").get<std::string>()));
            rows.push_back(paragraph("tick " + value(n.at("tick")) + " · source " + value(n.at("id"))) | dim);
            rows.push_back(text(" "));
        }
        if (memory.at("items").empty()) rows.push_back(text("No deposits selected."));
        rows.push_back(separator()); rows.push_back(text("WORKING MEMORY") | bold | accent);
        rows.push_back(document(v.detail.at("working_memory").dump(2)));
        if (v.detail.contains("self")) {
            rows.push_back(separator()); rows.push_back(text("GUARDED SELF STATE · read-only") | bold | accent);
            rows.push_back(document(v.detail.at("self").dump(2)));
        }
    } else if (v.mode == "state" && !v.snapshot.is_null()) {
        auto s = v.snapshot; s.erase("conversation"); s.erase("commits"); s.erase("attempts"); rows.push_back(document(s.dump(2)));
    } else if (!v.detail.is_null()) rows.push_back(document(v.detail.dump(2)));
    else rows.push_back(text("Loading…"));
    // Focus a line, not a percentage: long records remain readable one page at a time.
    auto body = vbox(std::move(rows)) | reflect(content_box) | focusPosition(0, v.scroll) | vscroll_indicator | yframe | flex;
    std::string caption = " " + v.mode + (v.watch ? " · WATCH" : "") + (v.connected ? "" : " · STALE VIEW") + " ";
    auto content = vbox({text(caption) | bold | accent, separator(), body}) | border | flex;
    Element middle = content;
    if (width >= 100 && !v.snapshot.is_null()) {
        const auto& s = v.snapshot; const auto& h = s.at("host");
        Elements state = {text(" SUBJECT STATE ") | bold | accent, separator(), text("tick  " + value(s.at("tick"))),
            paragraph("head  " + value(s.at("head")).substr(0, 16)), text(" "), text("Committed lifecycle") | dim,
            text(value(s.at("lifecycle"))), text(" "), text("Executor") | dim, paragraph(value(h.at("executor"))),
            text(" "), text("Scheduler") | dim, paragraph(value(s.at("schedule").at("status"))),
            paragraph("wake_at: " + value(s.at("wake_at"))), text(" "), text("Last host result") | dim,
            paragraph(value(h.at("last"))), paragraph(value(h.at("error"))), filler(), paragraph("Host state and committed subject state are separate.") | dim};
        middle = hbox({content, vbox(std::move(state)) | size(WIDTH, EQUAL, 28) | border});
    }
    std::string tick = v.snapshot.is_null() ? "" : "tick " + value(v.snapshot.at("tick")) + " · " + value(v.snapshot.at("lifecycle")) + " · ";
    return vbox({hbox(std::move(title)), text(" " + tick + (v.watch ? "watching" : "talk") + " · F1 help") | dim,
        middle | flex, paragraph(" " + safe(v.error.empty() ? v.notice : v.error)) | (v.error.empty() ? dim : color(Color::Red)),
        hbox({text(v.watch ? " watch > " : " > ") | accent, input | flex}) | border,
        text(" F2 timeline  F3 memory  F4 inspect  F5 talk/watch  PgUp/Dn  Ctrl+Q exit") | dim});
}
struct Job { json request; bool sending = false; std::string mode; };
struct Result { Job job; json data; std::string error; };
}
int main(int argc, char** argv) {
    try {
        std::string socket; bool watch = false; json single = nullptr; int width = 0, height = 0;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--help") { std::cout << "cogg-tui --socket PATH [--watch] [--request JSON | --snapshot WIDTH HEIGHT]\n" << help << '\n'; return 0; }
            if (a == "--watch") watch = true;
            else if (a == "--socket" && i + 1 < argc) socket = argv[++i];
            else if (a == "--request" && i + 1 < argc) single = json::parse(argv[++i]);
            else if (a == "--snapshot" && i + 2 < argc) { width = std::stoi(argv[++i]); height = std::stoi(argv[++i]); }
            else throw std::runtime_error("invalid arguments; use --help");
        }
        if (socket.empty()) throw std::runtime_error("--socket PATH is required");
        if (!single.is_null()) {
            const auto op = single.value("op", "");
            if (watch && (op == "send" || op == "pause" || op == "resume")) throw std::runtime_error("watch mode is read-only");
            std::cout << ui::request(socket, single).dump(2) << '\n'; return 0;
        }
        View view; view.watch = watch; Box content_box;
        if (width || height) {
            if (width < 40 || width > 300 || height < 12 || height > 100) throw std::runtime_error("snapshot size must be 40..300 by 12..100");
            view.snapshot = ui::request(socket, {{"op", "snapshot"}}); view.connected = true; view.notice = "Connected";
            auto screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
            Render(screen, layout(view, text(""), width, content_box)); std::cout << screen.ToString() << '\n'; return 0;
        }
        auto screen = ScreenInteractive::Fullscreen();
        screen.ForceHandleCtrlC(false); // Ctrl+C belongs to the local input, Ctrl+Q disconnects.
        std::mutex mutex; std::condition_variable cv; std::deque<Job> jobs; std::deque<Result> results;
        std::string input, retry_text, retry_key; bool pending = false;
        auto enqueue = [&](Job job) { std::lock_guard lock(mutex); jobs.push_back(std::move(job)); cv.notify_one(); };
        std::jthread network([&](std::stop_token token) {
            while (!token.stop_requested()) {
                Job job{{{"op", "snapshot"}}, false, ""};
                { std::lock_guard lock(mutex); if (!jobs.empty()) { job = jobs.front(); jobs.pop_front(); } }
                Result result{job, nullptr, ""};
                try { result.data = ui::request(socket, job.request); }
                catch (const std::exception& e) { result.error = e.what(); }
                { std::lock_guard lock(mutex); results.push_back(std::move(result)); }
                screen.PostEvent(Event::Custom);
                std::unique_lock lock(mutex); cv.wait_for(lock, std::chrono::milliseconds(700), [&] { return token.stop_requested() || !jobs.empty(); });
            }
        });
        auto command = [&] {
            if (pending || input.empty()) return;
            if (input[0] != '/') {
                if (view.watch) { view.error = "Watch mode: use /talk before sending."; return; }
                if (input.size() > 8192) { view.error = "Message exceeds 8192 bytes."; return; }
                if (input != retry_text) { retry_text = input; retry_key = ui::nonce(); }
                pending = true; view.notice = "Saving to inbox…"; view.error.clear();
                enqueue({{{"op", "send"}, {"text", input}, {"key", retry_key}}, true, "talk"}); return;
            }
            auto split = input.find(' '); auto op = input.substr(1, split == std::string::npos ? split : split - 1);
            auto arg = split == std::string::npos ? "" : input.substr(split + 1);
            if (op == "quit") { screen.ExitLoopClosure()(); return; }
            if (op == "help") view.mode = "help";
            else if (op == "talk" || op == "watch") { view.watch = op == "watch"; view.mode = "talk"; }
            else if (op == "timeline" || op == "state") view.mode = op;
            else if (op == "memory" || op == "inspect" || op == "since" || op == "why-awake" || op == "pause" || op == "resume") {
                if (view.watch && (op == "pause" || op == "resume")) { view.error = "Watch mode: host controls disabled."; return; }
                json req = {{"op", op}};
                if (op == "memory") req["query"] = arg;
                if (op == "inspect") req["id"] = arg;
                if (op == "since") {
                    try {
                        std::size_t end = 0; auto tick = arg.empty() ? view.anchor : std::stoll(arg, &end);
                        if (tick < 0 || (!arg.empty() && end != arg.size())) throw std::runtime_error("invalid tick");
                        req["tick"] = tick;
                    } catch (...) { view.error = "Use /since or /since NONNEGATIVE_TICK after connecting."; return; }
                }
                enqueue({req, false, op}); view.notice = "Requesting " + op + "…";
            } else { view.error = "Unknown command. F1 lists available commands."; return; }
            view.scroll = view.mode == "talk" ? 1000000 : 0; input.clear(); view.error.clear();
        };
        InputOption option; option.multiline = false; option.on_enter = command;
        auto entry = Input(&input, "message or /command", option);
        auto renderer = Renderer(entry, [&] { return layout(view, entry->Render(), Terminal::Size().dimx, content_box); });
        renderer |= CatchEvent([&](Event event) {
            if (event == Event::Custom) {
                std::deque<Result> batch; { std::lock_guard lock(mutex); batch.swap(results); }
                for (auto& r : batch) {
                    const auto op = r.job.request.at("op");
                    if (op == "snapshot") {
                        bool was_connected = view.connected; view.connected = r.error.empty();
                        if (view.connected) { if (!was_connected) { view.error.clear(); view.notice = "Connected; F1 for help"; } view.snapshot = std::move(r.data); if (view.anchor < 0) view.anchor = view.snapshot.at("tick").get<long long>(); }
                        else view.error = r.error + " · displayed data is stale";
                    } else if (!r.error.empty()) { view.error = r.error + (r.job.sending ? " · input retained; retry uses the same message key" : ""); }
                    else {
                        view.error.clear(); view.notice = r.job.sending ? "Saved to durable inbox" : "Host: " + r.job.mode;
                        if (r.job.sending) { input.clear(); retry_key.clear(); retry_text.clear(); view.mode = "talk"; view.scroll = 1000000; }
                        else { view.detail = std::move(r.data); view.mode = r.job.mode; view.scroll = 0; }
                    }
                    if (r.job.sending) pending = false;
                }
                return true;
            }
            if (event == Event::CtrlQ) { screen.ExitLoopClosure()(); return true; }
            if (event == Event::CtrlC) { if (!pending) input.clear(); return true; }
            if (event == Event::F1) { view.mode = "help"; view.scroll = 0; return true; }
            if (event == Event::F2) { view.mode = "timeline"; view.scroll = 0; return true; }
            if (event == Event::F3 || event == Event::F4) { auto op = event == Event::F3 ? "memory" : "inspect"; enqueue({{{"op", op}}, false, op}); return true; }
            if (event == Event::F5) { view.watch = !view.watch; view.mode = "talk"; view.scroll = 1000000; return true; }
            if (event == Event::PageUp || event == Event::PageDown) {
                auto maximum = std::max(0, content_box.y_max - content_box.y_min);
                view.scroll = std::clamp(std::min(view.scroll, maximum) + (event == Event::PageUp ? -1 : 1) * std::max(1, Terminal::Size().dimy - 12), 0, maximum); return true;
            }
            return pending; // Avoid editing/duplicating an in-flight message; disconnect still works.
        });
        screen.Loop(renderer); network.request_stop(); cv.notify_all(); network.join(); return 0;
    } catch (const std::exception& e) { std::cerr << "cogg-tui: " << e.what() << '\n'; return 2; }
}
