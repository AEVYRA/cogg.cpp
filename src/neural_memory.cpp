#include "cogg/neural_memory.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace cogg {
namespace {
using Vec = std::vector<float>;
void require(bool ok, const char *message) {
    if (!ok) throw std::invalid_argument(message);
}
double dot(const Vec &a, const Vec &b) {
    double s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) s += double(a[i]) * b[i];
    return s;
}
Vec normalized(Vec v) {
    const auto n = std::max(std::sqrt(dot(v, v)), 1e-12);
    for (auto &x : v) x = static_cast<float>(x / n);
    return v;
}
struct Text { Vec mean; std::vector<Vec> tokens; };
Text prepare(const MemoryEncoding &x, const std::string &encoder, std::size_t dim) {
    require(x.encoder == encoder, "memory encoder mismatch");
    require(!x.tokens.empty() && x.tokens.size() <= 128, "memory token count outside 1..128");
    Text out{Vec(dim, 0), {}};
    for (const auto &row : x.tokens) {
        require(row.size() == dim, "memory token dimension mismatch");
        for (std::size_t j = 0; j < dim; ++j) {
            require(std::isfinite(row[j]) && std::abs(row[j]) <= 1e6f, "invalid memory token value");
            out.mean[j] += row[j] / static_cast<float>(x.tokens.size());
        }
        out.tokens.push_back(normalized(row));
    }
    out.mean = normalized(out.mean);
    return out;
}
void append(Vec &to, const Vec &from) { to.insert(to.end(), from.begin(), from.end()); }
}
NeuralMemoryScorer::NeuralMemoryScorer(const nlohmann::json &m) {
    require(m.at("schema") == "cogg:neural-memory/v1", "unsupported neural memory schema");
    require(m.at("features") == "token-alignment/v1", "unsupported memory feature version");
    encoder_ = m.at("encoder").get<std::string>();
    require(!encoder_.empty() && encoder_.size() <= 512, "invalid encoder identity");
    require(m.at("dimension").is_number_integer(), "dimension must be an integer");
    const auto d = m.at("dimension").get<std::int64_t>();
    require(d > 0 && d <= 1024, "invalid embedding dimension");
    dim_ = static_cast<std::size_t>(d);
    threshold_ = m.at("threshold").get<double>();
    require(std::isfinite(threshold_) && threshold_ > 0 && threshold_ <= 1, "invalid neural threshold");
    const auto &ls = m.at("layers");
    require(ls.is_array() && ls.size() >= 2 && ls.size() <= 3, "expected two or three neural layers");
    auto input = 7 * dim_ + 9;
    for (std::size_t k = 0; k < ls.size(); ++k) {
        const auto &l = ls[k];
        const auto &w = l.at("weight"), &b = l.at("bias");
        require(w.is_array() && b.is_array() && !w.empty() && w.size() <= 256 &&
                w.size() == b.size(), "invalid neural layer shape");
        require(k + 1 != ls.size() || w.size() == 2, "expected two output logits");
        Layer layer;
        for (std::size_t i = 0; i < w.size(); ++i) {
            require(w[i].is_array() && w[i].size() == input, "invalid neural weight shape");
            auto row = w[i].get<Vec>();
            for (float v : row) require(std::isfinite(v) && std::abs(v) <= 1e6f, "invalid neural weight");
            const float bias = b[i].get<float>();
            require(std::isfinite(bias) && std::abs(bias) <= 1e6f, "invalid neural bias");
            layer.weight.push_back(std::move(row)); layer.bias.push_back(bias);
        }
        input = w.size(); layers_.push_back(std::move(layer));
    }
}
std::vector<float> NeuralMemoryScorer::features(const MemoryEncoding &memory, const MemoryEncoding &query) const {
    const auto p = prepare(memory, encoder_, dim_), c = prepare(query, encoder_, dim_);
    const auto np = p.tokens.size(), nc = c.tokens.size();
    std::vector<Vec> sims(nc, Vec(np));
    Vec mp(np, -1), mc(nc, -1);
    for (std::size_t j = 0; j < nc; ++j) for (std::size_t i = 0; i < np; ++i) {
        const auto s = static_cast<float>(std::clamp(dot(c.tokens[j], p.tokens[i]), -1.0, 1.0));
        sims[j][i] = s; mp[i] = std::max(mp[i], s); mc[j] = std::max(mc[j], s);
    }
    auto residual = [&](const std::vector<Vec> &tokens, const Vec &mx, double &mass) {
        Vec out(dim_, 0); mass = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const double w = std::pow(1.0 - mx[i], 3) + 1e-5; mass += w;
            for (std::size_t k = 0; k < dim_; ++k) out[k] += static_cast<float>(w * tokens[i][k]);
        }
        return normalized(out);
    };
    double mass_p = 0, mass_c = 0;
    const auto rp = residual(p.tokens, mp, mass_p), rc = residual(c.tokens, mc, mass_c);
    Vec left(dim_, 0);
    for (std::size_t j = 0; j < nc; ++j) {
        Vec weights(np); double total = 0;
        for (std::size_t i = 0; i < np; ++i) {
            weights[i] = static_cast<float>(std::exp(25.0 * (sims[j][i] - mc[j]))); total += weights[i];
        }
        const double match = std::pow(std::max(0.0f, mc[j]), 4) + 1e-5;
        for (std::size_t k = 0; k < dim_; ++k) {
            double aligned = 0;
            for (std::size_t i = 0; i < np; ++i) aligned += weights[i] / total * p.tokens[i ? i - 1 : 0][k];
            left[k] += static_cast<float>(match * (aligned - c.tokens[j ? j - 1 : 0][k]));
        }
    }
    Vec out; out.reserve(7 * dim_ + 9);
    append(out, p.mean); append(out, c.mean);
    for (std::size_t k = 0; k < dim_; ++k) out.push_back(p.mean[k] - c.mean[k]);
    for (std::size_t k = 0; k < dim_; ++k) out.push_back(p.mean[k] * c.mean[k]);
    out.push_back(static_cast<float>(dot(p.mean, c.mean)));
    append(out, rp); append(out, rc); append(out, normalized(left));
    auto stats = [&](const Vec &v) {
        double sum = 0; for (float x : v) sum += x;
        out.push_back(static_cast<float>(sum / static_cast<double>(v.size())));
        out.push_back(*std::min_element(v.begin(), v.end())); out.push_back(*std::max_element(v.begin(), v.end()));
    };
    stats(mp); stats(mc);
    out.push_back(static_cast<float>(mass_p / static_cast<double>(np)));
    out.push_back(static_cast<float>(mass_c / static_cast<double>(nc)));
    return out;
}
double NeuralMemoryScorer::probability(const MemoryEncoding &memory, const MemoryEncoding &query) const {
    auto x = features(memory, query);
    for (std::size_t k = 0; k < layers_.size(); ++k) {
        const auto &l = layers_[k]; Vec y(l.bias.size());
        for (std::size_t i = 0; i < y.size(); ++i) {
            const double z = dot(l.weight[i], x) + l.bias[i];
            y[i] = static_cast<float>(k + 1 == layers_.size() ? z : std::tanh(z));
        }
        x = std::move(y);
    }
    return 1.0 / (1.0 + std::exp(std::clamp(double(x[0]) - x[1], -80.0, 80.0)));
}
double NeuralMemoryScorer::score(const MemoryEncoding &memory, const MemoryEncoding &query) const {
    const double p = probability(memory, query);
    return p >= threshold_ ? p : 0.0;
}
}
