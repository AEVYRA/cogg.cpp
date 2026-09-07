#include "cogg/kernel.hpp"
#include <csignal>
#include <iostream>
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
    std::cout << "cogg-cli init DB SUBJECT [min-interval-ms max-attempts period-ms]\n"
                 "cogg-cli send DB SUBJECT IDEMPOTENCY-KEY TEXT\n"
                 "cogg-cli run DB SUBJECT [steps=0 idle-base-ms=0]\n"
                 "cogg-cli inspect DB SUBJECT\n"
                 "cogg-cli verify DB SUBJECT\n"
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
        if (command == "init" && (argc == 4 || argc == 7)) {
            cogg::Limits limits;
            if (argc == 7) limits = {number(argv[4]), number(argv[5]), number(argv[6])};
            store.create(subject, limits, cogg::wall_now());
            std::cout << "created " << subject << " tick=0\n";
        } else if (command == "send" && argc == 6) {
            std::cout << store.submit(subject, argv[4], {{"text", argv[5]}}, cogg::wall_now()) << '\n';
        } else if (command == "inspect" && argc == 4) {
            std::cout << store.timeline(subject).dump(2) << '\n';
        } else if (command == "verify" && argc == 4) {
            store.verify(subject);
            std::cout << "verified " << subject << " tick=" << store.snapshot(subject).tick << '\n';
        } else if (command == "run" && argc >= 4 && argc <= 6) {
            const auto steps = argc >= 5 ? number(argv[4]) : 0;
            const auto interval = argc >= 6 ? number(argv[5]) : 0;
            if (interval > 86400000) throw cogg::Error("demo interval exceeds one day");
            store.verify(subject); // Refuse corrupt history before admission or inference.
            Counter backend(interval);
            cogg::Runtime runtime(store, backend);
            std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
            std::int64_t done = 0;
            while (!stopping && (steps == 0 || done < steps)) {
                if (auto s = runtime.step(subject, cogg::wall_now())) {
                    ++done;
                    std::cout << cogg::json({{"subject", subject}, {"tick", s->tick}, {"head", s->head},
                        {"lifecycle", s->lifecycle}, {"memory", s->memory}}).dump() << std::endl;
                } else std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        } else { usage(); return 2; }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cogg: " << e.what() << '\n'; return 1;
    }
}
