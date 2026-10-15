#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace cogg {
// A type is an interpretation, not a truth certificate. Source IDs refer to
// earlier deposits in this subject; covers makes an explicit lossy summary.
// Types: note, fact, episode, task, summary, procedure, prospective.
// Active procedure notes require explicit non-summary sources in the subject chain.
struct MemoryNote {
    std::string key;
    std::string text;
    std::string type = "note";
    std::string status = "active";
    std::vector<std::string> sources;
    std::vector<std::string> covers;
};
struct MemoryPolicy {
    std::string query; // Empty in Runtime means derive from the current occasion.
    std::size_t max_bytes = 8192;
    std::size_t max_items = 16;
    std::size_t candidate_limit = 64;
    bool history = false;
    // 'balanced' and 'hybrid' interleave lexical, semantic/synonym, procedure and
    // recent lanes while preserving open tasks. 'semantic' prioritizes semantic/synonym
    // and procedure lanes after open tasks. 'lexical' and 'recent' are baselines.
    std::string strategy = "balanced";
    // Explicit, bounded recovery from task pressure. Only these current deposit
    // IDs are shown as tasks; all remaining obligations stay open in storage.
    // Set strategy="task_maintenance" and supply 1..8 IDs from open_tasks().
    std::vector<std::string> maintenance_ids;
    // Optional synonym/concept expansion map (term -> expanded terms) for the semantic lane.
    std::map<std::string, std::vector<std::string>> synonyms;
    // Optional host-supplied semantic scorer returning > 0.0 for relevant entries.
    std::function<double(const nlohmann::json &entry, const std::string &query)> semantic_scorer;
};
nlohmann::json memory_note_json(const MemoryNote &note);
MemoryNote parse_memory_note(const nlohmann::json &note);
void validate_memory_notes(const std::vector<MemoryNote> &notes);
} // namespace cogg
