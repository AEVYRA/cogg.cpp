// Offline fictional-history replay. No provider calls; weights/cache are explicit inputs.
#include "cogg/kernel.hpp"
#include "cogg/neural_memory.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <unistd.h>
using namespace cogg;
using Clock=std::chrono::steady_clock;
namespace {
struct Temp {
  std::filesystem::path path;
  Temp():path(std::filesystem::temp_directory_path()/("cogg-neural-"+std::to_string(getpid())+"-"+
        std::to_string(Clock::now().time_since_epoch().count()))) {std::filesystem::create_directory(path);}
  ~Temp(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
std::vector<double> mean(const MemoryEncoding &e) {
  std::vector<double> v(e.tokens[0].size());
  for(const auto &t:e.tokens)for(std::size_t j=0;j<v.size();++j)v[j]+=t[j];
  double norm=0;for(double x:v)norm+=x*x;norm=std::max(std::sqrt(norm),1e-12);
  for(auto &x:v)x/=norm;return v;
}
double cosine(const std::vector<double>&a,const std::vector<double>&b) {
  double s=0;for(std::size_t i=0;i<a.size();++i)s+=a[i]*b[i];return s;
}
}
int main(int argc,char**argv) {
 try {
  if(argc!=2)throw Error("usage: cogg-neural-memory-eval INPUT.json");
  if(std::filesystem::file_size(argv[1])>512*1024*1024)throw Error("replay input exceeds 512 MiB");
  std::ifstream in(argv[1]);json input;in>>input;
  NeuralMemoryScorer scorer(input.at("model"));std::map<std::string,MemoryEncoding> cache;
  std::map<std::string,std::vector<double>> vectors;
  for(const auto &[text,tokens]:input.at("encodings").items()) {
    cache.emplace(text,MemoryEncoding{input.at("encoder"),tokens.get<std::vector<std::vector<float>>>()});
    // Validate shape/bounds before mean() or inference.
    scorer.features(cache.at(text),cache.at(text));vectors[text]=mean(cache.at(text));
  }
  Temp temp;Store store((temp.path/"history.db").string());store.create("s",{1,10000,1000000},0);
  millis now=1;
  auto write=[&](MemoryNote note) {
    store.submit("s",std::to_string(now),{{"text","fixture"}},now);
    auto a=store.admit("s","fixture",now);if(!a)throw Error("fixture admission failed");
    Proposal p;p.notes={std::move(note)};store.commit(a->id,p,now++);
  };
  const auto &data=input.at("dataset");
  for(const auto &m:data.at("memories"))write({m.at("key"),m.at("text"),"fact"});
  MemoryPolicy all;all.strategy="recent";all.max_items=256;all.max_bytes=1048576;all.candidate_limit=512;
  auto initial=store.recall("s",all);std::map<std::string,std::string> ids,texts;
  for(const auto &e:initial.at("view").at("items")) {ids[e.at("key")]=e.at("id");texts[e.at("id")]=e.at("text");}
  // Push all meaningful records beyond the old 4*64 recent scorer window.
  for(int i=0;i<360;++i) {
    write({"noise-"+std::to_string(i),input.at("noise"),"episode"});
    if(i==179) {
      const auto middle=store.recall("s",all);
      for(const auto&e:middle.at("view").at("items"))texts[e.at("id")]=e.at("text");
    }
  }
  auto recent=store.recall("s",all);
  for(const auto &e:recent.at("view").at("items"))texts[e.at("id")]=e.at("text");
  json report={{"schema","cogg:neural-memory-replay-result/v1"},{"strategies",json::array()},
               {"probabilities",json::array()},{"scope","Controlled source retrieval; no answer model or live service"}};
  for(const std::string method:{"balanced","cosine","neural"}) {
    json rows=json::array();int retrieved=0,needed=0,missing_cases=0,false_cases=0,wrong=0,exceptions=0;
    std::vector<double> latency;
    for(const auto &q:data.at("queries")) {
      const std::string query=q.at("text");const auto &qe=cache.at(query);
      const auto qv=vectors.at(query);const auto expected=q.at("expected").get<std::set<std::string>>();
      MemoryPolicy p;p.query=query;p.max_items=input.at("budgets").at("max_items");
      p.max_bytes=input.at("budgets").at("max_bytes");p.candidate_limit=input.at("budgets").at("candidate_limit");
      p.strategy=method=="balanced"?"balanced":"semantic_only";
      const auto begin=Clock::now();
      if(method!="balanced") p.semantic_candidates=[&](const std::string&,const std::string&,std::size_t limit) {
        std::vector<std::pair<double,std::string>> ranked;
        for(const auto &[id,text]:texts)ranked.emplace_back(cosine(vectors.at(text),qv),id);
        std::sort(ranked.begin(),ranked.end(),[](const auto&a,const auto&b){return a.first!=b.first?a.first>b.first:a.second<b.second;});
        if(ranked.size()>limit)ranked.resize(limit);
        if(method=="neural")for(auto &r:ranked)r.first=scorer.score(cache.at(texts.at(r.second)),qe);
        std::stable_sort(ranked.begin(),ranked.end(),[](const auto&a,const auto&b){return a.first>b.first;});
        std::vector<std::string> selected;
        const double threshold=method=="cosine"?input.at("cosine_threshold").get<double>():0.0;
        for(const auto&r:ranked)if(r.first>threshold)selected.push_back(r.second);
        return selected;
      };
      const auto view=store.recall("s",p);
      const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();latency.push_back(ms);
      std::vector<std::string> selected;int hits=0;
      for(const auto&e:view.at("view").at("items")) {
        const auto key=e.at("key").get<std::string>();selected.push_back(key);
        if(expected.count(key)) {++hits;if(key.ends_with(".1")||key.ends_with(".2"))++exceptions;}else ++wrong;
        if(!e.at("current").get<bool>()||e.at("summary_stale").get<bool>())throw Error("stale output");
      }
      if(expected.empty()){++missing_cases;if(!selected.empty())++false_cases;}
      retrieved+=hits;needed+=static_cast<int>(expected.size());
      rows.push_back({{"query",query},{"selected",selected},{"hits",hits},{"required",expected.size()},{"milliseconds",ms}});
    }
    std::sort(latency.begin(),latency.end());
    report["strategies"].push_back({{"method",method},{"source_hits",retrieved},{"required_sources",needed},
      {"counterevidence_or_condition_hits",exceptions},{"irrelevant_selected",wrong},{"no_match_cases",missing_cases},
      {"false_no_match_accepts",false_cases},{"median_ms",latency[latency.size()/2]},
      {"p95_ms",latency[static_cast<std::size_t>(0.95*static_cast<double>(latency.size()-1))]},{"rows",rows}});
  }
  for(const auto&q:data.at("queries"))for(const auto&m:data.at("memories"))
    report["probabilities"].push_back(scorer.probability(cache.at(m.at("text")),cache.at(q.at("text"))));
  // Prove the selected source view is admissible, committed and replay-verifiable.
  store.submit("s","final-probe",{{"text",data.at("queries")[0].at("text")}},now);
  auto a=store.admit("s","fixture",now);if(!a)throw Error("final admission failed");
  store.commit(a->id,Proposal{},now);store.rebuild_memory("s");store.verify("s");
  report["store_rebuild_verify"]=true;std::cout<<report.dump()<<'\n';
 } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
