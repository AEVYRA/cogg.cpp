#include "cogg/kernel.hpp"
#include <csignal>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace cogg;
void check(bool ok, const char* msg) { if (!ok) throw Error(msg); }
void crash_case(CommitPoint point) {
    const auto path = (std::filesystem::temp_directory_path() /
        ("cogg-crash-" + std::to_string(getpid()) + "-" + std::to_string(static_cast<int>(point)))).string();
    std::filesystem::create_directory(path);
    const auto db = path + "/state.db";
    { Store s(db); s.create("s", {1, 10, 1000}, 0); }
    const pid_t child = fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        try {
            Store s(db); auto a = s.admit("s", "crash-test", 0);
            if (!a) _exit(3);
            Proposal p; p.memory = {{"durable", "yes"}}; p.wake_after_ms = 20;
            s.commit(a->id, p, 0, [&](CommitPoint stage) { if (stage == point) raise(SIGSTOP); });
            _exit(0);
        } catch (...) { _exit(2); }
    }
    int status = 0;
    check(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status), "child missed crash boundary");
    check(kill(child, SIGKILL) == 0, "SIGKILL failed");
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status), "child did not die");
    {
        Store s(db); s.verify("s");
        const auto snap = s.snapshot("s");
        if (point == CommitPoint::before_sql_commit) {
            check(snap.tick == 0 && snap.memory.empty() && !snap.wake_at, "crash exposed partial state");
            auto retry = s.admit("s", "replacement", 1);
            check(retry && retry->present.prior_unsettled_attempt, "crash inference uncertainty erased");
            s.commit(retry->id, Proposal{}, 1);
        } else {
            check(snap.tick == 1 && snap.memory.at("durable") == "yes" && snap.wake_at == 20,
                  "durable commit lost after SIGKILL");
            auto next = s.admit("s", "replacement", 20);
            check(next && next->present.occasion.kind == "scheduled", "scheduled wake lost");
            s.commit(next->id, Proposal{}, 20);
        }
        s.verify("s");
    }
    std::filesystem::remove_all(path);
}
int main() {
    try {
        crash_case(CommitPoint::before_sql_commit); std::cout << "PASS SIGKILL before durable commit\n";
        crash_case(CommitPoint::after_sql_commit); std::cout << "PASS SIGKILL after durable commit\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
