#include "protocol.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>
namespace cogg::ui {
Fd::~Fd() { if (value >= 0) ::close(value); }
std::string nonce() {
    std::random_device r; std::ostringstream out;
    for (int i = 0; i < 4; ++i) out << std::hex << std::setw(8) << std::setfill('0') << r();
    return out.str();
}
void send_frame(int fd, const json& j) {
    auto bytes = j.dump() + '\n';
    if (bytes.size() > frame_limit) throw std::runtime_error("response exceeds protocol limit");
    std::size_t at = 0;
    while (at < bytes.size()) {
        auto n = ::send(fd, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("host connection lost while sending");
        at += static_cast<std::size_t>(n);
    }
}
json receive_frame(int fd, std::size_t limit) {
    std::string bytes; char chunk[4096];
    while (bytes.size() < limit) {
        auto n = ::recv(fd, chunk, std::min(sizeof(chunk), limit - bytes.size()), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("host disconnected or timed out");
        bytes.append(chunk, static_cast<std::size_t>(n));
        auto end = bytes.find('\n');
        if (end != std::string::npos) return json::parse(bytes.substr(0, end));
    }
    throw std::runtime_error("protocol frame too large");
}
json request(const std::string& path, json body) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("socket path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (fd.value < 0) throw std::runtime_error("cannot create client socket");
    timeval timeout{2, 0};
    ::setsockopt(fd.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("host unavailable: " + std::string(std::strerror(errno)));
    body["protocol"] = "cogg:host/v1";
    send_frame(fd.value, body);
    auto reply = receive_frame(fd.value);
    if (reply.value("protocol", "") != "cogg:host/v1") throw std::runtime_error("unsupported host protocol");
    if (!reply.value("ok", false)) throw std::runtime_error(reply.value("error", "host rejected request"));
    return reply.at("data");
}
std::string excerpt(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) return s;
    while (bytes && (static_cast<unsigned char>(s[bytes]) & 0xc0) == 0x80) --bytes;
    return s.substr(0, bytes) + " … [truncated]";
}
std::string display_text(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        // C0/DEL and UTF-8 encoded C1 (including CSI/OSC) cannot reach the terminal.
        if (c == 0xc2 && i + 1 < s.size() && static_cast<unsigned char>(s[i+1]) >= 0x80 && static_cast<unsigned char>(s[i+1]) <= 0x9f) {
            out += '?'; ++i;
        } else if (c < 32 || c == 127) out += c == '\n' ? '\n' : ' ';
        else out += s[i];
    }
    return out;
}
}
