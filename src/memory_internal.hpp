#pragma once
#include "cogg/kernel.hpp"
struct sqlite3;
namespace cogg::detail {
void memory_schema(sqlite3 *db);
void memory_apply(sqlite3 *db, const std::string &subject, const std::string &parent, std::int64_t tick,
                  const std::string &commit, const std::vector<MemoryNote> &notes);
void memory_verify(sqlite3 *db, const std::string &subject, bool compare_index);
void memory_rebuild(sqlite3 *db, const std::string &subject);
json memory_candidates(sqlite3 *db, const std::string &subject, const MemoryPolicy &policy);
void project_memory(Present &present, const json &candidates, const MemoryPolicy &policy,
                    const std::function<bool(const Present &)> &fits);
} // namespace cogg::detail
