#include "cogg/checkpoint.hpp"
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace cogg;
namespace {
void check(bool ok, const char* msg) { if (!ok) throw Error(msg); }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw Error("invalid checkpoint accepted");
}
struct Temp {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("cogg-checkpoint-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(dir); }
    ~Temp() { std::filesystem::remove_all(dir); }
};
std::vector<char> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void replace(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void envelope() {
    Temp temp;
    const auto path = checkpoint_path(temp.dir.string(), "../../subject");
    check(std::filesystem::path(path).parent_path() == temp.dir, "subject escaped cache directory");
    check(!read_checkpoint(path, 1024), "missing file not distinguished");
    Checkpoint cp{{{"subject", "s"}, {"head", "h1"}}, {1, 2, 3, 4}};
    write_checkpoint(path, cp, 1024);
    auto loaded = read_checkpoint(path, 1024);
    check(loaded && loaded->metadata == cp.metadata && loaded->state == cp.state, "checkpoint roundtrip lost data");
    const auto valid = read_bytes(path);
    for (std::size_t offset : {std::size_t{0}, std::size_t{8}, std::size_t{16},
                              std::size_t{24}, std::size_t{56}, std::size_t{88}, valid.size() - 1}) {
        auto bytes = valid; bytes[offset] ^= 1; replace(path, bytes);
        rejects([&] { read_checkpoint(path, 1024); });
    }
    for (auto n : {std::size_t{0}, std::size_t{30}, valid.size() - 1}) {
        auto bytes = valid; bytes.resize(n); replace(path, bytes);
        rejects([&] { read_checkpoint(path, 1024); });
    }
    auto extra = valid; extra.push_back(0); replace(path, extra);
    rejects([&] { read_checkpoint(path, 1024); });
    replace(path, valid);
    rejects([&] { read_checkpoint(path, 2); });
    rejects([&] { write_checkpoint(path, cp, 2); });
    const auto link = (temp.dir / "symlink").string();
    std::filesystem::create_symlink(path, link);
    rejects([&] { read_checkpoint(link, 1024); });
    cp.metadata["head"] = "h2";
    rejects([&] { write_checkpoint(path, cp, 1024, [](CheckpointPoint point) {
        if (point == CheckpointPoint::after_file_sync) throw Error("injected disk publication failure");
    }); });
    check(read_checkpoint(path, 1024)->metadata.at("head") == "h1", "failure replaced complete checkpoint");
    for (const auto& f : std::filesystem::directory_iterator(temp.dir))
        check(f.path().filename().string().find(".tmp.") == std::string::npos, "handled failure leaked temporary file");
    std::cout << "PASS envelope integrity, bounded reads, atomic replacement and failed-write cleanup\n";
}
class CacheBackend : public Backend {
public:
    std::string path;
    std::function<void(CheckpointPoint)> hook;
    std::string name() const override { return "checkpoint-fixture/v1"; }
    Proposal propose(const Present&) override {
        Proposal p; p.memory = {{"preserved", true}}; p.wake_after_ms = 1; return p;
    }
    void committed(const Present&, const Snapshot& s) override {
        write_checkpoint(path, {{{"head", s.head}, {"tick", s.tick}}, {4, 5, 6}}, 1024, hook);
    }
};
void crash(CheckpointPoint point) {
    Temp temp; const auto db = (temp.dir / "subject.db").string();
    const auto path = checkpoint_path(temp.dir.string(), "s");
    std::string old_head;
    {
        Store s(db); s.create("s", {1, 100, 10000}, 0); old_head = s.snapshot("s").head;
        write_checkpoint(path, {{{"head", old_head}}, {1, 2, 3}}, 1024);
    }
    const pid_t child = fork(); check(child >= 0, "fork failed");
    if (child == 0) {
        try {
            Store s(db); CacheBackend b; b.path = path;
            b.hook = [point](CheckpointPoint at) { if (at == point) raise(SIGSTOP); };
            Runtime runtime(s, b); runtime.step("s", 0); _exit(3);
        } catch (...) { _exit(2); }
    }
    int status = 0;
    check(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status), "child missed checkpoint boundary");
    check(kill(child, SIGKILL) == 0, "SIGKILL failed");
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status), "child did not die");
    Store store(db); store.verify("s");
    auto s = store.snapshot("s");
    check(s.tick == 1 && s.memory.at("preserved") == true && s.wake_at, "checkpoint interruption lost committed subject");
    auto cp = read_checkpoint(path, 1024); check(cp.has_value(), "complete checkpoint disappeared");
    const bool published = point == CheckpointPoint::after_rename || point == CheckpointPoint::after_directory_sync;
    check(cp->metadata.at("head") == (published ? s.head : old_head), "checkpoint replacement was not atomic");
    CacheBackend next; next.path = path; Runtime runtime(store, next);
    check(runtime.step("s", *s.wake_at)->tick == 2, "restart could not continue durable chain");
    store.verify("s");
    check(read_checkpoint(path, 1024)->metadata.at("head") == store.snapshot("s").head, "next checkpoint is not current");
    std::cout << "PASS SIGKILL checkpoint boundary " << static_cast<int>(point) << '\n';
}
void concurrent_publication() {
    Temp temp; const auto path = checkpoint_path(temp.dir.string(), "shared");
    write_checkpoint(path, {{{"writer", 0}}, {0, 0, 0}}, 1024);
    pid_t children[2];
    for (int i = 0; i < 2; ++i) {
        children[i] = fork(); check(children[i] >= 0, "fork failed");
        if (children[i] == 0) {
            try {
                const auto value = static_cast<std::uint8_t>(i + 1);
                for (int n = 0; n < 20; ++n)
                    write_checkpoint(path, {{{"writer", value}}, {value, value, value}}, 1024);
                _exit(0);
            } catch (...) { _exit(2); }
        }
    }
    for (int i = 0; i < 100; ++i) {
        auto cp = read_checkpoint(path, 1024);
        check(cp && cp->state.size() == 3 && cp->metadata.at("writer") == cp->state[0] &&
              cp->state[0] == cp->state[1] && cp->state[1] == cp->state[2], "concurrent publication mixed files");
    }
    for (auto child : children) {
        int status = 0; check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                              "concurrent writer failed");
    }
    std::cout << "PASS concurrent readers/writers see complete envelopes\n";
}
}
int main() {
    try {
        envelope();
        concurrent_publication();
        for (auto point : {CheckpointPoint::after_header, CheckpointPoint::after_file_sync,
                           CheckpointPoint::after_rename, CheckpointPoint::after_directory_sync}) crash(point);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
