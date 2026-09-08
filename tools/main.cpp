#include "cogg/kernel.hpp"
#ifdef COGG_LLAMA
#include "cogg/llama_backend.hpp"
#endif
#include <csignal>
#include <iostream>
#include <fstream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
std::int64_t number(const char* arg) {
    std::size_t used = 0;
    const std::string s(arg);
    const auto n = std::stoll(s, &used);
    if (used != s.size() || n < 0) throw cogg::Error("expected a nonnegative integer");
    return n;
}
// Deliberately deterministic: demonstrates runtime mechanics, not intelligence.
class Counter final : public cogg::Backend {
public:
    explicit Counter(cogg::millis interval) : interval_(interval) {}
    std::string name() const override { return "demo-counter/v1"; }
    cogg::Proposal propose(const cogg::Present& p) override {
        const auto n = p.state.memory.value("counter", std::int64_t{0}) + 1;
        cogg::Proposal result;
        result.kind = p.occasion.kind == "external" ? "speech" : "reflection";
        if (result.kind == "speech") result.text = "Demo received: " + p.occasion.payload.dump();
        result.memory.push_back({"counter", n});
        if (interval_ > 0) result.wake_after_ms = interval_ * (1 + n % 3);
        return result;
    }
private:
    cogg::millis interval_;
};
void usage() {
    std::cout << "cogg-cli init DB SUBJECT [min-interval-ms max-attempts period-ms [max-wake-ms [seed-memory.json]]]\n"
                 "cogg-cli send DB SUBJECT IDEMPOTENCY-KEY TEXT\n"
                 "cogg-cli run DB SUBJECT [steps=0 idle-base-ms=0]\n"
                 "cogg-cli run-model DB SUBJECT MODEL.gguf [--steps N] [--ctx N]\n"
                 "    [--tokens N] [--threads N] [--timeout-ms N] [--template NAME]\n"
                 "    [--release-context] [--once] [--internal-only]\n"
                 "    [--checkpoint-dir DIRECTORY] [--checkpoint-mib N]\n"
                 "cogg-cli schedule DB SUBJECT\n"
                 "cogg-cli duration DB SUBJECT FROM-COMMIT TO-COMMIT\n"
                 "cogg-cli inspect DB SUBJECT\n"
                 "cogg-cli verify DB SUBJECT\n"
                 "cogg-cli recall DB SUBJECT QUERY [max-bytes strategy history]\n"
                 "cogg-cli rebuild-memory DB SUBJECT\n"
                 "cogg-cli memory-record DB SUBJECT NOTE-ID\n"
                 "run uses the deterministic demo backend; 0 steps runs until interrupted.\n"
                 "idle-base-ms=0 requests no autonomous wake. Positive values enable demo wakes.\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
        if (argc < 4) { usage(); return 2; }
        const std::string command = argv[1], subject = argv[3];
        cogg::Store store(argv[2]);
        if (command == "init" && (argc == 4 || argc == 7 || argc == 8 || argc == 9)) {
            cogg::Limits limits;
            if (argc >= 7) limits = {number(argv[4]), number(argv[5]), number(argv[6])};
            if (argc >= 8) limits.max_wake_ms = number(argv[7]);
            cogg::json seed = cogg::json::object();
            if (argc == 9) {
                std::ifstream in(argv[8]);
                if (!in) throw cogg::Error("cannot open seed memory");
                in >> seed;
            }
            store.create(subject, limits, cogg::wall_now(), seed);
            std::cout << "created " << subject << " tick=0\n";
        } else if (command == "send" && argc == 6) {
            std::cout << store.submit(subject, argv[4], {{"text", argv[5]}}, cogg::wall_now()) << '\n';
        } else if (command == "schedule" && argc == 4) {
            std::cout << store.schedule(subject, cogg::wall_now()).dump() << '\n';
        } else if (command == "duration" && argc == 6) {
            std::cout << store.duration(subject, argv[4], argv[5]).dump() << '\n';
        } else if (command == "inspect" && argc == 4) {
            std::cout << store.timeline(subject).dump(2) << '\n';
        } else if (command == "recall" && (argc == 5 || argc == 8)) {
            cogg::MemoryPolicy policy; policy.query = argv[4];
            if (argc == 8) { policy.max_bytes = static_cast<std::size_t>(number(argv[5])); policy.strategy = argv[6]; policy.history = number(argv[7]) != 0; }
            std::cout << store.recall(subject, policy).dump(2) << '\n';
        } else if (command == "memory-record" && argc == 5) {
            std::cout << store.memory_record(subject, argv[4]).dump(2) << '\n';
        } else if (command == "rebuild-memory" && argc == 4) {
            store.rebuild_memory(subject); std::cout << "rebuilt memory index; subject unchanged\n";
        } else if (command == "verify" && argc == 4) {
            store.verify(subject);
            std::cout << "verified " << subject << " tick=" << store.snapshot(subject).tick << '\n';
        } else if (command == "run-model" && argc >= 5) {
#ifdef COGG_LLAMA
            cogg::LlamaOptions options;
            options.model_path = argv[4];
            std::int64_t steps = 0;
            bool release = false, once = false;
            for (int i = 5; i < argc; ++i) {
                const std::string flag = argv[i];
                if (flag == "--release-context") { release = true; continue; }
                if (flag == "--once") { once = true; continue; }
                if (flag == "--internal-only") { options.internal_only = true; continue; }
                if (++i >= argc) throw cogg::Error("missing value for " + flag);
                if (flag == "--template") { options.chat_template = argv[i]; continue; }
                if (flag == "--checkpoint-dir") { options.checkpoint_directory = argv[i]; continue; }
                const auto n = number(argv[i]);
                if (flag == "--steps") steps = n;
                else if (flag == "--timeout-ms") options.timeout_ms = n;
                else if (flag == "--checkpoint-mib") {
                    if (n < 1 || n > 4096) throw cogg::Error("checkpoint limit must be 1..4096 MiB");
                    options.max_checkpoint_bytes = static_cast<std::uint64_t>(n) * 1048576;
                }
                else {
                    if (n > 131072) throw cogg::Error("inference option too large");
                    if (flag == "--ctx") options.context_tokens = static_cast<std::uint32_t>(n);
                    else if (flag == "--tokens") options.max_output_tokens = static_cast<std::uint32_t>(n);
                    else if (flag == "--threads") options.threads = static_cast<int>(n);
                    else throw cogg::Error("unknown option: " + flag);
                }
            }
            store.verify(subject);
            std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
            options.cancelled = [] { return stopping != 0; };
            cogg::LlamaBackend backend(options);
            cogg::Runtime runtime(store, backend, cogg::wall_now);
            std::cerr << "backend=" << backend.name() << '\n';
            std::int64_t done = 0;
            while (!stopping && (steps == 0 || done < steps)) {
                if (auto s = runtime.step(subject, cogg::wall_now())) {
                    ++done;
                    const auto stats = backend.stats();
                    std::cout << cogg::json({{"subject", subject}, {"tick", s->tick}, {"head", s->head},
                        {"lifecycle", s->lifecycle}, {"memory", s->memory},
                        {"proposal", store.record(s->head).at("proposal")},
                        {"wake_plan", store.record(s->head).at("wake_plan")},
                        {"wake_at", s->wake_at ? cogg::json(*s->wake_at) : cogg::json(nullptr)},
                        {"inference", {{"prompt_tokens", stats.prompt_tokens},
                            {"reused_tokens", stats.reused_tokens}, {"generated_tokens", stats.generated_tokens}}},
                        {"checkpoint", {{"read", stats.checkpoint_read}, {"write", stats.checkpoint_write},
                            {"detail", stats.checkpoint_detail}}},
                        {"maintenance_error", runtime.maintenance_error()}}).dump() << std::endl;
                    if (release) backend.release_context();
                } else if (!once) std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (once) break;
            }
#else
            throw cogg::Error("run-model requires a build with -DCOGG_LLAMA=ON");
#endif
        } else if (command == "run" && argc >= 4 && argc <= 6) {
            const auto steps = argc >= 5 ? number(argv[4]) : 0;
            const auto interval = argc >= 6 ? number(argv[5]) : 0;
            if (interval > 86400000) throw cogg::Error("demo interval exceeds one day");
            store.verify(subject); // Refuse corrupt history before admission or inference.
            Counter backend(interval);
            cogg::Runtime runtime(store, backend, cogg::wall_now);
            std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
            std::int64_t done = 0;
            while (!stopping && (steps == 0 || done < steps)) {
                if (auto s = runtime.step(subject, cogg::wall_now())) {
                    ++done;
                    std::cout << cogg::json({{"subject", subject}, {"tick", s->tick}, {"head", s->head},
                        {"lifecycle", s->lifecycle}, {"memory", s->memory}}).dump() << std::endl;
                } else std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        } else { usage(); return 2; }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cogg: " << e.what() << '\n'; return 1;
    }
}
