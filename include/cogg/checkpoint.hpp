#pragma once

#include "cogg/kernel.hpp"

namespace cogg {
struct Checkpoint {
    json metadata;
    std::vector<std::uint8_t> state;
};
enum class CheckpointPoint { after_header, after_file_sync, after_rename, after_directory_sync };
// One atomic, checksummed cache file. Local POSIX filesystem, trusted directory.
// max_bytes bounds the state blob; metadata is separately bounded to 1 MiB.
std::optional<Checkpoint> read_checkpoint(const std::string& path, std::uint64_t max_bytes);
void write_checkpoint(const std::string& path, const Checkpoint&, std::uint64_t max_bytes,
                      const std::function<void(CheckpointPoint)>& hook = {});
std::string checkpoint_path(const std::string& directory, const std::string& subject);
} // namespace cogg
