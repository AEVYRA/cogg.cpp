#include "cogg/llama_backend.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace cogg;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw Error("model path required");
        auto dir = std::filesystem::temp_directory_path() / ("cogg-llama-outcome-" + std::to_string(getpid()));
        std::filesystem::create_directory(dir);
        struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove_all(p); } } cleanup{dir};
        std::filesystem::create_directory(dir / "kv");
        Store store((dir / "s.db").string()); store.create("s", {1,100,1000000}, 0);
        store.commit(store.admit("s", "seed", 0)->id, Proposal{}, 0);
        store.submit("s", "format-probe", {{"text", "Return exactly this JSON object, with no other fields: {\"kind\":\"abstain\",\"reason\":\"missing_input\",\"detail\":\"No observation supplied\"}"}}, 1);
        LlamaOptions options; options.allow_abstention = true; options.model_path = argv[1]; options.timeout_ms = 120000;
        options.checkpoint_directory = (dir / "kv").string();
        LlamaBackend backend(options); Runtime runtime(store, backend);
        const auto head = store.snapshot("s").head;
        auto result = runtime.step("s", 1);
        if (result || !runtime.last_abstention() || runtime.last_abstention()->reason != "missing_input")
            throw Error("native Outcome grammar did not produce the requested abstention");
        if (store.snapshot("s").head != head || !std::filesystem::is_empty(dir / "kv"))
            throw Error("abstention advanced head or published checkpoint");
        store.verify("s");
        std::cout << "PASS native typed abstention, no transition or checkpoint; format probe, not calibration proof\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
