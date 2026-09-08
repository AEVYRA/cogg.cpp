#include "cogg/routing.hpp"
#include <set>
namespace cogg {
namespace {
void need(bool ok, const char* why) { if (!ok) throw Error(why); }
bool bounded(const json& v, std::size_t max, bool empty = false) {
    return v.is_string() && (empty || !v.get_ref<const std::string&>().empty()) &&
        v.get_ref<const std::string&>().size() <= max && v.get_ref<const std::string&>().find('\0') == std::string::npos;
}
bool digest(const json& v) {
    return bounded(v, 64) && v.get_ref<const std::string&>().size() == 64 &&
        v.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
}
}
Outcome parse_outcome(const json& v) {
    if (!v.is_object() || v.value("kind", "") != "abstain") return parse_proposal(v);
    need(v.size() == 3 && v.contains("reason") && v.contains("detail"), "invalid abstention shape");
    need(v.at("reason") == "missing_input" || v.at("reason") == "unsupported" ||
         v.at("reason") == "uncertain" || v.at("reason") == "refused", "invalid abstention reason");
    need(bounded(v.at("detail"), 512, true), "invalid abstention detail");
    return Abstention{v.at("reason").get<std::string>(), v.at("detail").get<std::string>()};
}
json outcome_json(const Outcome& v) {
    if (auto p = std::get_if<Proposal>(&v)) return proposal_json(*p);
    const auto& a = std::get<Abstention>(v);
    json result = {{"kind", "abstain"}, {"reason", a.reason}, {"detail", a.detail}};
    (void)parse_outcome(result); return result;
}
json make_input(const std::string& kind, const std::string& producer, const json& content,
                const std::vector<std::string>& sources) {
    json body = {{"schema", "cogg:input/v1"}, {"kind", kind}, {"producer", producer},
                 {"content", content}, {"sources", sources}};
    return {{"id", execution_hash(body)}, {"body", body}};
}
void validate_inputs(const json& inputs) {
    need(inputs.is_array() && inputs.size() <= 16 && inputs.dump().size() <= 32768, "input budget exceeded");
    std::set<std::string> seen;
    for (const auto& input : inputs) {
        need(input.is_object() && input.size() == 2 && input.contains("id") && input.contains("body"), "invalid input envelope");
        const auto& b = input.at("body");
        need(b.is_object() && b.size() == 5 && b.value("schema", "") == "cogg:input/v1" &&
             b.contains("producer") && bounded(b.at("producer"), 200) && b.contains("content") &&
             b.contains("sources") && b.at("sources").is_array(), "invalid input body");
        need(b.at("kind") == "observation" || b.at("kind") == "inference", "invalid input kind");
        need((b.at("kind") == "observation") == b.at("sources").empty(), "input provenance required");
        std::set<std::string> links;
        for (const auto& source : b.at("sources")) {
            need(digest(source), "invalid input source");
            const auto id = source.get<std::string>();
            need(seen.count(id) && links.insert(id).second, "missing, forward or duplicate input source");
        }
        need(digest(input.at("id")) && input.at("id") == execution_hash(b) &&
             seen.insert(input.at("id").get<std::string>()).second, "input hash mismatch or duplicate");
    }
}
void validate_admission_context(const json& c) {
    if (c.is_null()) return;
    need(c.is_object() && c.size() == 3 && c.contains("head") && c.contains("occasion") &&
         digest(c.at("head")) && digest(c.at("occasion")) && c.contains("inputs"), "invalid admission context");
    validate_inputs(c.at("inputs"));
}
} // namespace cogg
