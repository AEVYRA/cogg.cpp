#include "cogg/self_state.hpp"
#include "cogg/http_backend.hpp"
#include <fstream>
#include <iostream>
#include <random>
#include <csignal>
using namespace cogg;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
json read(const char* path) {
    std::ifstream in(path); if (!in) throw Error("cannot open JSON file"); json j; in >> j; return j;
}
std::string session() {
    std::random_device random;
    return execution_hash(json::array({random(), random(), random(), random(), wall_now()}));
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 4) throw Error("usage: cogg-self-example init|view|grant|run DB SUBJECT [arguments]; see docs/PHASE-7.md");
        const std::string command = argv[1], subject = argv[3]; Store store(argv[2]);
        if (command == "init" && argc == 5) {
            store.create(subject, {1, 100, 3600000}, wall_now(), {{"self.profile", self_profile(read(argv[4]))}});
            // Initialization consumes only the created event, with no model or self mutation.
            auto a = store.admit(subject, "self-example:bootstrap", wall_now());
            if (!a) throw Error("bootstrap not admitted"); store.commit(a->id, Proposal{}, wall_now());
            std::cout << inspect_self(store, subject).dump(2) << '\n'; return 0;
        }
        if (command == "view" && argc == 4) { std::cout << inspect_self(store, subject).dump(2) << '\n'; return 0; }
        if (command == "grant" && argc == 5) {
            const auto request = read(argv[4]);
            for (const auto& [key, value] : request.items())
                if (key != "create" && key != "settle" && key != "profile") throw Error("unknown grant request field");
            if (!request.is_object()) throw Error("grant request must be an object");
            const auto s = store.schedule(subject, wall_now());
            if (s.at("occasion").is_null()) throw Error("submit a request before issuing its grant");
            std::cout << self_grant(inspect_self(store, subject), s.at("occasion").at("id"),
                request.value("create", json::object()), request.value("settle", json::object()), request.value("profile", json(nullptr))).dump(2) << '\n'; return 0;
        }
        if (command == "run" && (argc == 7 || argc == 8)) {
            // Choosing an executor in this example is the host's explicit transport authorization.
            const auto configuration = read(argv[4]); const std::string executor = argv[5];
            HttpBackend backend(configuration.at("executors").at(executor));
            json execution = {{"schema", "cogg:execution/v1"}, {"executor", executor}, {"backend", backend.name()},
                {"session", session()}, {"capabilities", capabilities_json(backend.capabilities())}};
            const auto grant = std::string(argv[6]) == "-" ? json(nullptr) : read(argv[6]);
            const auto timeout = argc == 8 ? std::stoll(argv[7]) : 60000;
            std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
            SelfRuntime runtime(store, backend, execution);
            const auto result = runtime.step(subject, wall_now(), grant, json::array(), timeout, [] { return stopped != 0; });
            std::cout << json({{"status", result.status}, {"attempt", result.attempt}, {"outcome", result.outcome},
                {"maintenance_error", result.maintenance_error}, {"view", inspect_self(store, subject)}}).dump(2) << '\n';
            return result.status == "committed" || result.status == "waiting" ? 0 : result.status == "abstained" ? 4 : 3;
        }
        throw Error("invalid arguments; see docs/PHASE-7.md");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
