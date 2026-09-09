#include "cogg/http_backend.hpp"
#include "cogg/output_limits.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace cogg {
namespace {
void need(bool ok, const char* message) { if (!ok) throw Error(message); }
struct CurlGlobal {
    CurlGlobal() { need(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "HTTP initialization failed"); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
bool safe_string(const std::string& s, std::size_t max) {
    return !s.empty() && s.size() <= max && std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string credential(const json& config) {
    const auto name = config.value("api_key_env", std::string{});
    if (name.empty()) return {};
    std::string key;
    if (const auto* value = std::getenv(name.c_str())) key = value;
    if (key.empty() && config.contains("credential_file")) {
        std::ifstream in(config.at("credential_file").get<std::string>());
        need(bool(in), "credential file unavailable");
        std::string line;
        while (std::getline(in, line)) {
            need(line.size() <= 65536, "credential line too large");
            line = trim(line);
            if (line.starts_with("export ")) line = trim(line.substr(7));
            auto equals = line.find('=');
            if (equals == std::string::npos || trim(line.substr(0, equals)) != name) continue;
            key = trim(line.substr(equals + 1));
            if (!key.empty() && (key.front() == '\'' || key.front() == '"')) {
                auto close = key.find(key.front(), 1);
                need(close != std::string::npos && (trim(key.substr(close + 1)).empty() || trim(key.substr(close + 1)).starts_with('#')), "invalid credential entry");
                key = key.substr(1, close - 1);
            } else {
                auto comment = key.find(" #"); if (comment != std::string::npos) key = trim(key.substr(0, comment));
            }
            break;
        }
    }
    need(safe_string(key, 8192), "credential missing or invalid"); return key;
}
constexpr auto instruction = R"(Return one cogg outcome: either an abstention or a transition. Return ONLY a JSON object.
Abstain when required evidence is missing, the task is unsupported, a conclusion is uncertain, or you decline to respond.
An abstention has exactly this form: {"kind":"abstain","reason":"missing_input","detail":"No observation supplied"}.
Choose one reason: missing_input, unsupported, uncertain, refused. Abstention has no text, memory, notes or wake fields and leaves the occasion pending.
inputs are host assertions. Observation/inference labels do not certify truth. Sources preserve attribution; an inference is not a new observation. An empty inputs list supplies no observation. Never claim camera or image access without supplied evidence.
If you choose a TRANSITION instead, follow these transition rules:
A valid no-action example is {"kind":"null","text":"","memory":[],"wake_after_ms":null}.
Transition kind values: null, reflection, speech. Only speech may have nonempty text. Use reflection to update memory without speaking, null for no action.
Each memory write has exactly key (the actual key to change) and value (its new value). [] preserves memory. Never copy illustrative placeholders as actual keys.
Optional notes: [{"key":"name","text":"content","type":"note|fact|episode|task|summary","status":"active|open|closed|retracted","sources":[],"covers":[]}].
Note sources/covers must name existing deposit IDs from this subject, not input packet IDs. Omit notes when unnecessary.
Memory/deposits are selected views, not the entire history. Do not invent facts or claim omitted memory is absent.
Subject, tick, parent and timing are host-owned. No tools or authority to change history.
For a created occasion, preserve initial memory; no speech is needed. For a supported external request, put the answer in speech text.
Preserve memory unless explicitly asked to change it. Request no autonomous wake unless needed.)";
// Ollama enforces the output shape; kernel validation still owns semantic invariants.
json proposal_schema() {
    auto schema = json::parse(R"JSON({
  "type": "object",
  "additionalProperties": false,
  "required": [
    "kind",
    "text",
    "memory",
    "wake_after_ms"
  ],
  "properties": {
    "kind": {
      "type": "string",
      "enum": [
        "null",
        "reflection",
        "speech"
      ]
    },
    "text": {
      "type": "string"
    },
    "memory": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": [
          "key",
          "value"
        ],
        "properties": {
          "key": {
            "type": "string"
          },
          "value": {}
        }
      }
    },
    "wake_after_ms": {
      "anyOf": [
        {
          "type": "null"
        },
        {
          "type": "integer"
        }
      ]
    },
    "notes": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": [
          "key",
          "text",
          "type",
          "status",
          "sources",
          "covers"
        ],
        "properties": {
          "key": {
            "type": "string"
          },
          "text": {
            "type": "string"
          },
          "type": {
            "type": "string",
            "enum": [
              "note",
              "fact",
              "episode",
              "task",
              "summary"
            ]
          },
          "status": {
            "type": "string",
            "enum": [
              "active",
              "open",
              "closed",
              "retracted"
            ]
          },
          "sources": {
            "type": "array",
            "items": {
              "type": "string"
            }
          },
          "covers": {
            "type": "array",
            "items": {
              "type": "string"
            }
          }
        }
      }
    }
  }
})JSON");
    auto& properties = schema["properties"];
    properties["memory"]["maxItems"] = output_limits::memory_writes;
    properties["notes"]["maxItems"] = output_limits::notes;
    for (const auto* key : {"sources", "covers"})
        properties["notes"]["items"]["properties"][key]["maxItems"] = output_limits::sources;
    // JSON Schema character lengths cannot express the core's UTF-8 byte limits.
    properties["wake_after_ms"]["anyOf"][1]["minimum"] = 0;
    properties["wake_after_ms"]["anyOf"][1]["maximum"] = 365LL * 24 * 60 * 60 * 1000;
    return schema;
}
struct Transfer {
    std::string response;
    const std::function<bool()>* cancelled;
    static std::size_t write(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
        auto& self = *static_cast<Transfer*>(opaque);
        if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
        auto n = size * count;
        if (n > 2 * 1048576 - self.response.size()) return 0;
        try { self.response.append(data, n); return n; } catch (...) { return 0; }
    }
    static int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        try { const auto& f = *static_cast<Transfer*>(opaque)->cancelled; return f && f() ? 1 : 0; }
        catch (...) { return 1; }
    }
};
}
struct HttpBackend::Impl {
    json config, receipt = json::object();
    std::string url, identity;
    bool ollama = false, remote = true;
    std::size_t max_prompt = 12288;
    int output = 512, context = 4096, threads = 2;
    explicit Impl(json c) : config(std::move(c)) {
        static CurlGlobal global; (void)global;
        need(config.is_object(), "invalid HTTP configuration");
        const std::set<std::string> fields = {"kind", "base_url", "model", "api_key_env", "credential_file", "context_tokens", "threads", "max_output_tokens", "max_prompt_bytes", "request_options", "remote"};
        for (const auto& [key, value] : config.items()) { (void)value; need(fields.count(key) != 0, "unknown HTTP configuration field"); }
        const auto kind = config.at("kind").get<std::string>();
        need(kind == "ollama" || kind == "chat_completions", "unsupported HTTP adapter"); ollama = kind == "ollama";
        auto base = config.at("base_url").get<std::string>();
        need(safe_string(base, 2048) && safe_string(config.at("model").get<std::string>(), 200), "invalid HTTP endpoint or model");
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        need(bool(parsed) && curl_url_set(parsed.get(), CURLUPART_URL, base.c_str(), 0) == CURLUE_OK, "invalid HTTP URL");
        auto part = [&](CURLUPart field) {
            char* value = nullptr; const auto rc = curl_url_get(parsed.get(), field, &value, 0);
            std::string result = rc == CURLUE_OK ? value : ""; curl_free(value); return result;
        };
        auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
        need((scheme == "https" || scheme == "http") && part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty() &&
            part(CURLUPART_QUERY).empty() && part(CURLUPART_FRAGMENT).empty(), "unsupported HTTP URL components");
        // Only numeric loopback addresses count as local; DNS aliases remain remote.
        remote = !(host == "127.0.0.1" || host == "[::1]");
        if (config.contains("remote")) {
            need(config.at("remote").is_boolean(), "invalid remote declaration");
            remote = remote || config.at("remote").get<bool>();
        }
        if (config.contains("api_key_env")) {
            const auto key_name = config.at("api_key_env").get<std::string>();
            need(!key_name.empty() && key_name.size() <= 128 && std::all_of(key_name.begin(), key_name.end(), [](unsigned char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
            }), "invalid credential reference");
            need(scheme == "https", "credentials require HTTPS");
        }
        need(!config.contains("credential_file") || config.contains("api_key_env"), "credential file requires a key name");
        while (!base.empty() && base.back() == '/') base.pop_back();
        url = base + (ollama ? "/api/chat" : "/chat/completions");
        auto integer = [&](const char* key, int fallback, int minimum, int maximum) {
            if (!config.contains(key)) return fallback;
            const auto& value = config.at(key);
            need(value.is_number_integer() && value >= minimum && value <= maximum, "invalid HTTP integer limit");
            return value.get<int>();
        };
        output = integer("max_output_tokens", 512, 32, 16384);
        context = integer("context_tokens", 4096, 1024, 1048576);
        threads = integer("threads", 2, 1, 256);
        max_prompt = static_cast<std::size_t>(integer("max_prompt_bytes", 12288, 2048, 1048576));
        need(!ollama || output + 256 < context, "output exceeds Ollama context");
        auto options = config.value("request_options", json::object());
        need(options.is_object(), "invalid request options");
        for (const auto& [key, value] : options.items()) {
            (void)value;
            need(!ollama && (key == "thinking" || key == "reasoning_effort" || key == "temperature" || key == "response_format"), "unsupported request option");
        }
        need(options.dump().size() <= 2048, "request options too large");
        auto manifest = config; manifest.erase("api_key_env"); manifest.erase("credential_file");
        identity = "cogg-http/v1:" + execution_hash({{"configuration", manifest}, {"adapter", COGG_HTTP_ADAPTER_HASH}});
    }
    json messages(const Present& p) const {
        auto view = p.memory_view;
        json deposits = view.is_null() ? json::array() : view.at("items");
        if (!view.is_null()) view.erase("items");
        nlohmann::ordered_json data = {
            {"memory", p.working_memory.is_null() ? p.state.memory : p.working_memory},
            {"deposits", deposits}, {"memory_view", view},
            {"subject", p.state.subject}, {"tick", p.state.tick}, {"parent", p.state.head},
            {"temporal", p.temporal}, {"prior_unsettled_attempt", p.prior_unsettled_attempt}, {"inputs", p.inputs},
            {"occasion", {{"id", p.occasion.id}, {"kind", p.occasion.kind}, {"payload", p.occasion.payload}}}};
        return json::array({{{"role", "system"}, {"content", instruction}}, {{"role", "user"}, {"content", data.dump()}}});
    }
};
HttpBackend::HttpBackend(json config) : impl_(std::make_unique<Impl>(std::move(config))) {}
HttpBackend::~HttpBackend() = default;
std::string HttpBackend::name() const { return impl_->identity; }
Capabilities HttpBackend::capabilities() const { return {impl_->remote, false, false, true}; }
json HttpBackend::telemetry() const { return impl_->receipt; }
std::optional<MemoryPolicy> HttpBackend::memory_policy() const {
    MemoryPolicy p; p.max_bytes = std::min<std::size_t>(8192, impl_->max_prompt / 2); return p;
}
bool HttpBackend::context_fits(const Present& p) const { return impl_->messages(p).dump().size() <= impl_->max_prompt; }
Proposal HttpBackend::propose(const Present&) { throw BackendFailure("HTTP inference requires an admitted attempt"); }
Proposal HttpBackend::propose_attempt(const Attempt& a, millis timeout, const std::function<bool()>& cancelled) {
    auto outcome = respond_attempt(a, timeout, cancelled);
    if (auto p = std::get_if<Proposal>(&outcome)) return *p;
    throw BackendFailure("abstention requires the Outcome API");
}
Outcome HttpBackend::respond_attempt(const Attempt& a, millis timeout, const std::function<bool()>& cancelled) {
    impl_->receipt = json::object();
    need(timeout > 0 && timeout <= 3600000 && context_fits(a.present), "HTTP inference exceeds limits");
    json request = {{"model", impl_->config.at("model")}, {"messages", impl_->messages(a.present)}, {"stream", false}};
    if (impl_->ollama) {
        request["format"] = {{"anyOf", json::array({proposal_schema(), {
            {"type", "object"}, {"additionalProperties", false},
            {"required", json::array({"kind", "reason", "detail"})},
            {"properties", {{"kind", {{"const", "abstain"}}},
                {"reason", {{"type", "string"}, {"enum", json::array({"missing_input", "unsupported", "uncertain", "refused"})}}},
                {"detail", {{"type", "string"}}}}}}})}}; request["keep_alive"] = "2m";
        request["options"] = {{"num_ctx", impl_->context}, {"num_predict", impl_->output}, {"num_thread", impl_->threads}, {"temperature", 0}};
    } else {
        request["max_tokens"] = impl_->output;
        const auto options = impl_->config.value("request_options", json::object());
        for (const auto& [key, value] : options.items()) request[key] = value;
    }
    const auto body = request.dump();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    need(bool(handle), "HTTP handle allocation failed");
    curl_slist* raw_headers = nullptr;
    auto append = [&](const std::string& value) {
        auto* next = curl_slist_append(raw_headers, value.c_str());
        if (!next) { curl_slist_free_all(raw_headers); raw_headers = nullptr; throw BackendFailure("HTTP header allocation failed"); }
        raw_headers = next;
    };
    std::string key;
    try { key = credential(impl_->config); }
    catch (...) { throw BackendFailure("credentials unavailable", FailureKind::credentials); }
    append("Content-Type: application/json"); append("X-Cogg-Attempt: " + a.id);
    if (!key.empty()) append("Authorization: Bearer " + key);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    Transfer transfer{{}, &cancelled};
    auto set = [&](CURLoption option, auto value) { need(curl_easy_setopt(handle.get(), option, value) == CURLE_OK, "HTTP option rejected"); };
    set(CURLOPT_URL, impl_->url.c_str()); set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_USERAGENT, "cogg.cpp/0.7.0"); set(CURLOPT_POST, 1L);
    set(CURLOPT_POSTFIELDS, body.c_str()); set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout)); set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<millis>(timeout, 5000)));
    set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    // Local model requests must not accidentally leave through an environment proxy.
    if (!impl_->remote) set(CURLOPT_PROXY, "");
    set(CURLOPT_WRITEFUNCTION, &Transfer::write); set(CURLOPT_WRITEDATA, &transfer);
    set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, &Transfer::progress); set(CURLOPT_XFERINFODATA, &transfer);
    if (cancelled && cancelled()) throw BackendFailure("HTTP cancelled");
    auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) throw BackendFailure("HTTP transfer failed", code == CURLE_OPERATION_TIMEDOUT ? FailureKind::timeout : FailureKind::transport);
    long status = 0; curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw BackendFailure("HTTP status rejected", FailureKind::transport);
    // Deliberately do not forward provider error bodies or parser excerpts to logs.
    try {
        need(key.empty() || transfer.response.find(key) == std::string::npos, "credential echoed by provider");
        auto response = json::parse(transfer.response); std::string content; bool refused = false;
        json receipt = {{"adapter", impl_->ollama ? "ollama" : "chat_completions"}, {"usage", json::object()}};
        if (response.contains("model") && response.at("model").is_string()) {
            auto model = response.at("model").get<std::string>(); need(safe_string(model, 200), "invalid model receipt"); receipt["reported_model"] = model;
        }
        auto usage = [&](const json& source, const char* field, const char* target) {
            if (!source.contains(field)) return;
            const auto& v = source.at(field);
            need(v.is_number_integer() && v >= 0 && v <= 1000000000, "invalid usage receipt"); receipt["usage"][target] = v;
        };
        if (impl_->ollama) {
            need(response.at("done") == true && response.at("done_reason") == "stop", "incomplete Ollama response");
            const auto& message = response.at("message");
            need(message.value("role", "assistant") == "assistant" && (!message.contains("tool_calls") || message.at("tool_calls").empty()), "unexpected Ollama output");
            content = message.at("content").get<std::string>();
            usage(response, "prompt_eval_count", "prompt_tokens"); usage(response, "eval_count", "completion_tokens");
            if (receipt["usage"].contains("prompt_tokens"))
                need(receipt["usage"]["prompt_tokens"].get<int>() + impl_->output <= impl_->context, "provider context overflow");
        } else {
            need(response.at("choices").is_array() && response.at("choices").size() == 1, "ambiguous completion");
            const auto& choice = response.at("choices").at(0); const auto& message = choice.at("message");
            need((choice.at("finish_reason") == "stop" || choice.at("finish_reason") == "content_filter") && message.at("role") == "assistant" &&
                (!message.contains("tool_calls") || message.at("tool_calls").empty()), "incomplete completion");
            refused = choice.at("finish_reason") == "content_filter";
            if (message.contains("refusal") && !message.at("refusal").is_null()) {
                need(message.at("refusal").is_string(), "invalid refusal field");
                refused = refused || !message.at("refusal").get_ref<const std::string&>().empty();
            }
            if (!refused) content = message.at("content").get<std::string>();
            if (response.contains("usage")) {
                usage(response.at("usage"), "prompt_tokens", "prompt_tokens"); usage(response.at("usage"), "completion_tokens", "completion_tokens");
            }
        }
        json value = refused ? outcome_json(Abstention{"refused", "Provider declined to respond"}) : json::parse(content);
        // A narrowly defined wire alias, never inference from free-text speech.
        if (value.is_object() && value.value("kind", "") == "abstention") {
            value["kind"] = "abstain"; receipt["normalization"] = "abstention_to_abstain";
        }
        Outcome outcome = parse_outcome(value);
        impl_->receipt = receipt; return outcome;
    } catch (...) { throw BackendFailure("invalid or incomplete provider proposal", FailureKind::invalid_output); }
}
void register_http(Registry& registry, const json& config) {
    need(config.is_object() && config.size() == 1 && config.contains("executors") && config.at("executors").is_object(), "invalid executor configuration");
    for (const auto& [id, entry] : config.at("executors").items()) {
        HttpBackend checked(entry); const auto caps = checked.capabilities();
        registry.add(id, caps, [entry] { return std::make_unique<HttpBackend>(entry); });
    }
}
} // namespace cogg
