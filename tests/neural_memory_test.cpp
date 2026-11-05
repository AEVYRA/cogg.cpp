#include "cogg/neural_memory.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using nlohmann::json;
void check(bool v,const char *m) { if(!v) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"expected rejection"); }
int main() {
  try {
    // A known analytic network reads the cosine feature: tanh(dot) -> two logits.
    std::vector<float> row(23,0);row[8]=1;
    json model={{"schema","cogg:neural-memory/v1"},{"features","token-alignment/v1"},
      {"encoder","test-encoder/v1"},{"dimension",2},{"threshold",0.6},
      {"layers",json::array({{{"weight",{row}},{"bias",{0}}},
                            {{"weight",{{-2},{2}}},{"bias",{0,0}}}})}};
    cogg::NeuralMemoryScorer scorer(model);
    cogg::MemoryEncoding a{"test-encoder/v1",{{1,0},{1,0}}},b{"test-encoder/v1",{{-1,0}}};
    auto f=scorer.features(a,a);check(f.size()==23,"feature count");
    check(std::abs(f[8]-1)<1e-6,"cosine feature");
    const double expected=1/(1+std::exp(-4*std::tanh(1.0)));
    check(std::abs(scorer.probability(a,a)-expected)<1e-6,"analytic probability");
    check(scorer.score(a,a)>0 && scorer.score(a,b)==0,"threshold gating");
    auto bad=a;bad.encoder="different";rejects([&]{scorer.score(a,bad);});
    bad=a;bad.tokens.clear();rejects([&]{scorer.score(a,bad);});
    bad=a;bad.tokens[0].push_back(0);rejects([&]{scorer.score(a,bad);});
    bad=a;bad.tokens[0][0]=std::numeric_limits<float>::quiet_NaN();rejects([&]{scorer.score(a,bad);});
    bad=a;bad.tokens.assign(129,{1,0});rejects([&]{scorer.score(a,bad);});
    auto wrong=model;wrong["dimension"]=0;rejects([&]{cogg::NeuralMemoryScorer s(wrong);});
    wrong=model;wrong["threshold"]=0;rejects([&]{cogg::NeuralMemoryScorer s(wrong);});
    wrong=model;wrong["layers"][0]["weight"][0]=json::array();rejects([&]{cogg::NeuralMemoryScorer s(wrong);});
    wrong=model;wrong["features"]="different";rejects([&]{cogg::NeuralMemoryScorer s(wrong);});
    std::cout<<"PASS native neural memory analytic inference and input contracts\n";
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
