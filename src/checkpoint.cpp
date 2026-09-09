#include "cogg/checkpoint.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#ifdef __unix__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cogg {
namespace {
constexpr std::uint64_t max_metadata = 1048576;
constexpr std::array<std::uint8_t, 8> magic = {'C','O','G','G','K','V',0,1};
// magic, LE metadata length, LE blob length, SHA256(metadata), SHA256(blob).
constexpr std::size_t prefix_size = 88;
using Digest = std::array<std::uint8_t, 32>;
Digest digest(std::span<const std::uint8_t> data) {
    Digest result; unsigned int n = 0;
    if (EVP_Digest(data.data(), data.size(), result.data(), &n, EVP_sha256(), nullptr) != 1 || n != result.size())
        throw Error("checkpoint hashing failed");
    return result;
}
std::span<const std::uint8_t> bytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t get_u64(const std::uint8_t* in) {
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i) result |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    return result;
}
void check(bool condition, const char* message) { if (!condition) throw Error(message); }
#ifdef __unix__
struct FD {
    int value = -1;
    explicit FD(int fd) : value(fd) {}
    ~FD() { if (value >= 0) ::close(value); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
[[noreturn]] void io_error(const char* operation) {
    throw Error(std::string("checkpoint ") + operation + ": " + std::strerror(errno));
}
void write_all(int fd, std::span<const std::uint8_t> data) {
    while (!data.empty()) {
        const auto n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) io_error("write failed");
        data = data.subspan(static_cast<std::size_t>(n));
    }
}
void read_all(int fd, std::span<std::uint8_t> data) {
    while (!data.empty()) {
        const auto n = ::read(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) io_error("read failed");
        check(n > 0, "truncated checkpoint");
        data = data.subspan(static_cast<std::size_t>(n));
    }
}
#endif
} // namespace

std::string checkpoint_path(const std::string& directory, const std::string& subject) {
    const auto d = digest(bytes(subject));
    std::string name;
    for (auto b : d) { name += "0123456789abcdef"[b >> 4]; name += "0123456789abcdef"[b & 15]; }
    return (std::filesystem::path(directory) / (name + ".coggkv")).string();
}
std::string checkpoint_path(const std::string& directory, const std::string& subject,
                            const std::string& origin, const std::string& executor) {
    check(!origin.empty() && !executor.empty(), "checkpoint namespace requires origin and executor");
    return checkpoint_path(directory, json::array({"cogg-cache/v2", subject, origin, executor}).dump());
}
std::optional<Checkpoint> read_checkpoint(const std::string& path, std::uint64_t max_bytes) {
#ifdef __unix__
    FD fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.value < 0) {
        if (errno == ENOENT) return std::nullopt;
        io_error("open failed");
    }
    struct stat info{};
    if (::fstat(fd.value, &info) != 0) io_error("stat failed");
    check(S_ISREG(info.st_mode), "checkpoint is not a regular file");
    check(info.st_size >= static_cast<off_t>(prefix_size), "truncated checkpoint header");
    std::array<std::uint8_t, prefix_size> prefix;
    read_all(fd.value, prefix);
    check(std::equal(magic.begin(), magic.end(), prefix.begin()), "unsupported checkpoint format");
    const auto header_size = get_u64(prefix.data() + 8);
    const auto state_size = get_u64(prefix.data() + 16);
    // Avoid adding attacker-controlled lengths until all have bounded values.
    check(max_bytes <= 4294967296ULL && state_size > 0 && state_size <= max_bytes,
          "checkpoint state exceeds byte limit");
    check(header_size > 0 && header_size <= max_metadata, "checkpoint metadata exceeds byte limit");
    check(static_cast<std::uint64_t>(info.st_size) == prefix_size + header_size + state_size,
          "checkpoint length mismatch");
    std::string header(static_cast<std::size_t>(header_size), '\0');
    read_all(fd.value, {reinterpret_cast<std::uint8_t*>(header.data()), header.size()});
    const auto hd = digest(bytes(header));
    check(std::equal(hd.begin(), hd.end(), prefix.begin() + 24), "checkpoint metadata checksum mismatch");
    Checkpoint result;
    result.metadata = json::parse(header);
    check(result.metadata.is_object(), "invalid checkpoint metadata");
    result.state.resize(static_cast<std::size_t>(state_size));
    read_all(fd.value, result.state);
    const auto sd = digest(result.state);
    check(std::equal(sd.begin(), sd.end(), prefix.begin() + 56), "checkpoint state checksum mismatch");
    return result;
#else
    (void)path; (void)max_bytes;
    throw Error("durable checkpoints require a POSIX filesystem implementation");
#endif
}
void write_checkpoint(const std::string& path, const Checkpoint& cp, std::uint64_t max_bytes,
                      const std::function<void(CheckpointPoint)>& hook) {
#ifdef __unix__
    check(max_bytes <= 4294967296ULL && !cp.state.empty() && cp.state.size() <= max_bytes,
          "checkpoint state exceeds byte limit");
    check(cp.metadata.is_object(), "invalid checkpoint metadata");
    const auto header = cp.metadata.dump();
    check(!header.empty() && header.size() <= max_metadata, "checkpoint metadata exceeds byte limit");
    std::array<std::uint8_t, prefix_size> prefix{};
    std::copy(magic.begin(), magic.end(), prefix.begin());
    put_u64(prefix.data() + 8, header.size()); put_u64(prefix.data() + 16, cp.state.size());
    const auto hd = digest(bytes(header)), sd = digest(cp.state);
    std::copy(hd.begin(), hd.end(), prefix.begin() + 24);
    std::copy(sd.begin(), sd.end(), prefix.begin() + 56);
    const auto parent = std::filesystem::path(path).parent_path();
    FD directory(::open((parent.empty() ? std::filesystem::path(".") : parent).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.value < 0) io_error("directory open failed");
    std::string temporary = path + ".tmp.XXXXXX";
    FD fd(::mkstemp(temporary.data())); // Unique, mode 0600, same directory as destination.
    if (fd.value < 0) io_error("temporary file creation failed");
    bool renamed = false;
    try {
        if (::fcntl(fd.value, F_SETFD, FD_CLOEXEC) != 0) io_error("close-on-exec failed");
        write_all(fd.value, prefix); write_all(fd.value, bytes(header));
        if (hook) hook(CheckpointPoint::after_header);
        write_all(fd.value, cp.state);
        if (::fsync(fd.value) != 0) io_error("file sync failed");
        if (hook) hook(CheckpointPoint::after_file_sync);
        if (::rename(temporary.c_str(), path.c_str()) != 0) io_error("rename failed");
        renamed = true;
        if (hook) hook(CheckpointPoint::after_rename);
        if (::fsync(directory.value) != 0) io_error("directory sync failed");
        if (hook) hook(CheckpointPoint::after_directory_sync);
    } catch (...) {
        if (!renamed) ::unlink(temporary.c_str());
        throw;
    }
#else
    (void)path; (void)cp; (void)max_bytes; (void)hook;
    throw Error("durable checkpoints require a POSIX filesystem implementation");
#endif
}
} // namespace cogg
