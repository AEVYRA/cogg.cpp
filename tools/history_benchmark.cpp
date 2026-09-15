#include "cogg/kernel.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace cogg;
using Clock=std::chrono::steady_clock;
template<class F> double seconds(F f){auto t=Clock::now();f();return std::chrono::duration<double>(Clock::now()-t).count();}
int main(int argc,char** argv){
 try{
  if(argc!=2)throw Error("usage: cogg-history-benchmark NEW_DIRECTORY");
  std::filesystem::path root=argv[1];if(std::filesystem::exists(root))throw Error("benchmark directory must not exist");std::filesystem::create_directories(root);
  for(int count:{100,1000,10000}){
   auto path=root/(std::to_string(count)+".db");Store s(path.string());s.create("agent",{1,1000000,100000000},0);
   auto build=seconds([&]{for(int i=0;i<count;++i){if(i)s.submit("agent","request-"+std::to_string(i),{{"text","current topic"},{"shared",std::string(2048,'x')}},i*10);auto a=s.admit("agent","fixture",i*10);Proposal p;p.notes={{"note-"+std::to_string(i),"A short retained observation."}};s.commit(a->id,p,i*10);}});
   json out={{"transitions",count},{"build_seconds",build}};
   out["verify_seconds"]=seconds([&]{s.verify("agent");});
   out["request_trace_seconds"]=seconds([&]{(void)s.request_trace("agent","request-"+std::to_string(count-1));});
   out["timeline_seconds"]=seconds([&]{(void)s.timeline("agent");});
   out["database_bytes"]=std::filesystem::file_size(path);std::cout<<out.dump()<<std::endl;
  }
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
