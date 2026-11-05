#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace cogg {
// Derived version-bound token features; padding removed, special tokens retained.
struct MemoryEncoding {
    std::string encoder;
    std::vector<std::vector<float>> tokens;
};
// Optional CPU inference. The host owns encoding/cache/index; no torch dependency.
// Probability of applicability is neither truth nor authorization.
class NeuralMemoryScorer {
public:
    explicit NeuralMemoryScorer(const nlohmann::json &model);
    double probability(const MemoryEncoding &memory, const MemoryEncoding &query) const;
    double score(const MemoryEncoding &memory, const MemoryEncoding &query) const;
    std::vector<float> features(const MemoryEncoding &memory, const MemoryEncoding &query) const;
    const std::string &encoder() const { return encoder_; }
    double threshold() const { return threshold_; }
    std::size_t dimension() const { return dim_; }
private:
    struct Layer { std::vector<std::vector<float>> weight; std::vector<float> bias; };
    std::string encoder_;
    std::size_t dim_ = 0;
    double threshold_ = 0.5;
    std::vector<Layer> layers_;
};
}
