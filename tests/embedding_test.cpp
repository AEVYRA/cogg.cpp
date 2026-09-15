#include "cogg/kernel.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
using namespace cogg;
namespace {
void check(bool b,const char* why){if(!b)throw Error(why);}
template<class F> void rejects(F f){bool failed=false;try{f();}catch(const Error&){failed=true;}check(failed,"expected rejection");}
struct Temp {
 std::filesystem::path path=std::filesystem::temp_directory_path()/("cogg-embedding-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".db");
 ~Temp(){for(auto suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);}
};
void exercise(){
 Temp t; Store s(t.path.string());s.create("agent",{1,1000,1000},0);
 auto boot=s.admit("agent","fixture",0);s.commit(boot->id,Proposal{},0);
 MemoryPolicy policy;policy.max_items=2;
 s.submit("agent","request-1",{{"text","remember"}},10);
 auto a=s.admit("agent","fixture",10,policy);check(bool(a),"admission missing");
 auto projected=prompt_temporal(a->present);
 check(projected["admission"]["occasion"]["payload_ref"]=="/occasion/payload","duplicate payload not referenced");
 check(!projected["admission"]["occasion"].contains("payload") && a->present.temporal["admission"]["occasion"].contains("payload"),"prompt projection changed receipt");
 Proposal p;p.notes={{"a","first task","task","open"},{"b","second task","task","open"},{"c","third task","task","open"}};
 auto head=s.snapshot("agent").head;
 rejects([&]{s.commit(a->id,p,10);});check(s.snapshot("agent").head==head,"capacity rejection advanced head");
 check(s.open_tasks("agent")["items"].empty(),"capacity rejection leaked notes");
 s.fail(a->id,"capacity");
 auto plain=s.admit("agent","trusted-import",11);s.commit(plain->id,p,11); // Simulates an old unbounded writer.
 s.submit("agent","maintenance",{{"text","Explicitly retract the first task"}},12);
 rejects([&]{s.admit("agent","fixture",12,policy);});
 auto tasks=s.open_tasks("agent","",1);check(tasks["has_more"]==true,"task pagination missing");
 auto id=tasks["items"][0]["id"].get<std::string>();
 policy.strategy="task_maintenance";policy.maintenance_ids={id};
 auto m=s.admit("agent","fixture",12,policy);check(m->present.memory_view["items"].size()==1,"maintenance not bounded");
 Proposal bad;bad.notes={{"z","new task","task","open"}};rejects([&]{s.commit(m->id,bad,12);});
 Proposal settle;settle.notes={{"a","first task","task","retracted"}};s.commit(m->id,settle,12);
 s.verify("agent");check(s.open_tasks("agent")["items"].size()==2,"maintenance dropped unnamed tasks");
 s.submit("agent","normal",{{"text","continue"}},13);policy.strategy="balanced";policy.maintenance_ids.clear();
 auto n=s.admit("agent","fixture",13,policy);check(bool(n),"recovery did not restore admission");s.commit(n->id,Proposal{},13);s.verify("agent");
 // A smaller executor can also recover a byte overflow without editing history.
 s.submit("agent","bytes",{{"text","store"}},14);auto raw=s.admit("agent","trusted-import",14);
 Proposal large;large.notes={{"large",std::string(1500,'x'),"task","open"}};s.commit(raw->id,large,14);
 s.submit("agent","small",{{"text","settle"}},15);policy.max_items=16;policy.max_bytes=1000;
 rejects([&]{s.admit("agent","small",15,policy);});
 auto all=s.open_tasks("agent");std::string large_id;
 for(const auto& item:all["items"])if(item["note"]["key"]=="large")large_id=item["id"];
 policy.strategy="task_maintenance";policy.max_bytes=4096;policy.maintenance_ids={large_id};
 auto repair=s.admit("agent","maintenance-executor",15,policy);
 Proposal closed;closed.notes={{"large",std::string(1500,'x'),"task","closed"}};s.commit(repair->id,closed,15);
 s.verify("agent");
 auto trace=s.request_trace("agent","request-1",1);
 check(trace["attempt_count"]==2 && trace["truncated"]==true && trace["commits"].size()==1,"request reconciliation incomplete");
 check(s.request_trace("agent","unknown")["occasions"].empty(),"unknown request invented");
 auto page=s.commits_page("agent",-1,2);check(page["items"].size()==2 && page["has_more"]==true,"commit page bounds");
 auto next=s.commits_page("agent",page["next_after_tick"],2,page["head"]);check(next["items"][0]["body"]["tick"]==2,"cursor skipped commit");
 rejects([&]{s.commits_page("agent",0,2,std::string(64,'0'));});
 Store ro(t.path.string(),OpenMode::read_only);ro.verify("agent");check(ro.request_trace("agent","request-1")["commits"]==s.request_trace("agent","request-1")["commits"],"read-only trace differs");
 rejects([&]{ro.submit("agent","forbidden",json::object(),20);});
 rejects([&]{Store missing(t.path.string()+".missing",OpenMode::read_only);});
 check(!std::filesystem::exists(t.path.string()+".missing"),"read-only created database");
}
}
int main(){try{exercise();std::cout<<"embedding recovery passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
