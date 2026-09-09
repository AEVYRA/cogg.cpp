#pragma once
#include "cogg/http_backend.hpp"
#include <vector>

namespace cogg {
// Explicit local file import; no URLs or automatic camera access. Original bytes
// are retained in a private content-addressed directory next to the host database.
inline constexpr std::size_t max_image_bytes = 5 * 1024 * 1024;
json import_image(const std::string& path, const std::string& directory);
std::vector<std::uint8_t> image_bytes(const std::string& directory, const json& image);

// A host operation, not a subject transition. One vision request on a receipt
// miss; successful receipts survive restart and are bound to head + occasion.
// The subsequent actor admission freezes these inputs in the ordinary ledger.
json image_context(Store&, const std::string& subject, HttpBackend& vision,
                   const std::string& directory, millis now, millis timeout,
                   const std::function<bool()>& cancelled = {});

// A scoped host acceptance check: image answers must preserve the explicitly
// supplied observation note. It checks provenance retention, not visual truth.
class ImageActor final : public Backend {
public:
    explicit ImageActor(std::unique_ptr<Backend> actor) : actor_(std::move(actor)) {}
    std::string name() const override { return "cogg-image-actor/v1:" + actor_->name(); }
    Proposal propose(const Present& p) override { return actor_->propose(p); }
    Outcome respond_attempt(const Attempt&, millis, const std::function<bool()>&) override;
    std::optional<MemoryPolicy> memory_policy() const override { return actor_->memory_policy(); }
    bool context_fits(const Present& p) const override { return actor_->context_fits(p); }
    json telemetry() const override { return actor_->telemetry(); }
    void committed(const Present& p, const Snapshot& s) override { actor_->committed(p,s); }
private:
    std::unique_ptr<Backend> actor_;
};
}
