#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace cogg::ui {
using json = nlohmann::json;
constexpr std::size_t frame_limit = 2 * 1024 * 1024;
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
std::string nonce();
void send_frame(int fd, const json&);
json receive_frame(int fd, std::size_t limit = frame_limit);
json request(const std::string& socket, json body);
// Model/user text is untrusted terminal content. Keep UTF-8, replace controls.
std::string display_text(const std::string&);
std::string excerpt(const std::string&, std::size_t bytes = 8192);
}
