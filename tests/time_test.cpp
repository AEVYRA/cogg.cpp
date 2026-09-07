#include "cogg/kernel.hpp"
#include <filesystem>
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <limits>
#include <chrono>
using namespace cogg;
namespace {
void check(bool ok, const char* msg) { if (!ok) throw Error(msg); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const Error&) { caught=true; } check(caught,"expected rejection"); }
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("cogg-time-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    ~Temp() { std::filesystem::remove(path); std::filesystem::remove(path.string()+"-wal"); std::filesystem::remove(path.string()+"-shm"); }
};
Attempt admit(Store& s, millis now) { auto a=s.admit("s","time-test",now); check(a.has_value(),"missing admission"); return *a; }
void quota_and_provenance() {
    Temp t; Store s(t.path.string()); s.create("s",{10,2,100,50},1000,{{"goal_activation",1}});
    auto a=admit(s,1000); s.fail(a.id,"deliberate failure");
    auto b=admit(s,1010);
    check(b.present.temporal.at("coordinate")==0 && b.present.temporal.at("admission").at("budget").at("used")==1,"attempt mistaken for tick");
    Proposal p; p.wake_after_ms=0;
    auto out=s.commit(b.id,p,1010);
    check(out.wake_at==1100,"quota was not included in granted wake");
    auto plan=s.record(out.head).at("wake_plan");
    check(plan.at("requested_after_ms")==0 && plan.at("granted_after_ms")==90,"request overwritten by grant");
    check(plan.at("reasons")==json::array({"minimum_interval","attempt_budget"}),"grant reasons missing");
    auto before=s.timeline("s");
    for(int i=0;i<100;++i) check(s.schedule("s",1099).at("status")=="sleeping","wake too early");
    check(s.timeline("s")==before,"polling mutated history");
    { Store resumed(t.path.string()); resumed.verify("s"); check(!resumed.admit("s","test",1099),"restart refunded quota"); }
    auto c=admit(s,1117);
    check(c.present.temporal.at("wake_lateness_ms")==17,"physical lateness missing");
    check(c.present.temporal.at("previous_wake")==plan,"wake provenance lost on admission");
    p.wake_after_ms=500;
    out=s.commit(c.id,p,1117);
    check(out.wake_at==1167,"maximum requested sleep not bounded");
    check(s.record(out.head).at("wake_plan").at("reasons")==json::array({"maximum_interval"}),"maximum clamp reason");
    s.verify("s");
}
void rollback_and_elapsed() {
    Temp t; Store s(t.path.string()); s.create("s",{10,1,100},1000);
    auto a=admit(s,1000); Proposal p; p.wake_after_ms=10;
    s.commit(a.id,p,1000); // wake at 1100 because quota is spent
    check(!s.admit("s","test",1100-1),"early budget wake");
    auto due=admit(s,1100); s.fail(due.id,"retry later");
    check(!s.admit("s","test",900),"queued wake fired after wall rollback");
    auto d=s.schedule("s",900);
    check(d.at("status")=="sleeping" && d.at("wall_rollback_ms")==200,"wall and accounting merged");
    auto again=admit(s,1200);
    auto out=s.commit(again.id,p,800,{},250); // elapsed is steady even when physical clock rolled back
    check(s.record(out.head).at("committed_at")==1450 && out.wake_at==1460,"inference duration lost in rollback");
    check(s.record(out.head).at("wall_observed_at")==800,"raw observation lost");
    check(s.snapshot("s").tick==2,"failed attempt advanced time"); s.verify("s");
}
void windows_and_wait() {
    Temp t; Store s(t.path.string()); s.create("s",{1,100,1000},0);
    auto origin=s.snapshot("s").head;
    auto a=admit(s,0); Proposal p; p.wake_after_ms=5;
    auto one=s.commit(a.id,p,0).head;
    s.submit("s","hello",{{"text","external preempts wake"}},1);
    auto ext=admit(s,1); check(ext.present.occasion.kind=="external","external did not preempt sleep");
    auto two=s.commit(ext.id,Proposal{},1).head;
    check(s.schedule("s",1000000).at("status")=="waiting_external","null wake resurrected sleep");
    check(s.duration("s",origin,two).at("duration")==2,"wrong duration");
    check(s.duration("s",one,one).at("duration")==0,"nonzero self duration");
    check(s.duration("s",origin,one).at("duration").get<int>()+s.duration("s",one,two).at("duration").get<int>()==2,"nonadditive duration");
    rejects([&]{s.duration("s",two,one);});
    s.create("other",{},0); rejects([&]{s.duration("s",origin,s.snapshot("other").head);});
    rejects([&]{s.duration("s",origin,a.id);});
    auto clock=s.clock("s"); check(clock.at("origin")==origin,"clock origin missing");
    { Store restarted(t.path.string()); check(restarted.clock("s")==clock,"clock changed on restart"); }
}
void virtual_48_hours() {
    Temp t; Store s(t.path.string()); s.create("s",{60000,12,3600000,900000},0);
    millis now=0; std::int64_t expected=0; std::string head=s.snapshot("s").head;
    const auto origin=head;
    while(now<48LL*3600000) {
        const auto d=s.schedule("s",now); check(d.at("status")=="ready","virtual schedule stalled");
        auto a=admit(s,now); Proposal p;
        const auto active=expected%3;
        p.memory={{"goal_activation",active}}; p.wake_after_ms=active==0?0:active==1?300000:900000;
        auto out=s.commit(a.id,p,now); head=out.head; ++expected;
        check(out.tick==expected && *out.wake_at>now,"zero loop or wrong tick");
        check(!s.admit("s","test",*out.wake_at-1),"early virtual wake");
        now=*out.wake_at;
        if(expected%25==0) { Store reopened(t.path.string()); reopened.verify("s"); }
    }
    check(expected>200,"insufficient virtual coverage");
    check(s.duration("s",origin,head).at("duration")==expected,"sleep counted as subject time");
    s.verify("s");
    std::cout<<"virtual 48h commits="<<expected<<" (scheduler test, not model evidence)\n";
}
void legacy_continuation() {
    Temp t;
    std::ifstream source(COGG_LEGACY_FIXTURE); std::ostringstream sql; sql<<source.rdbuf();
    check(!sql.str().empty(), "legacy fixture missing");
    sqlite3* db=nullptr; check(sqlite3_open(t.path.c_str(),&db)==SQLITE_OK,"fixture open");
    const auto rc=sqlite3_exec(db,sql.str().c_str(),nullptr,nullptr,nullptr);
    sqlite3_close(db); check(rc==SQLITE_OK,"fixture import");
    Store s(t.path.string()); s.verify("legacy"); auto old=s.timeline("legacy");
    check(old.at("tick")==1 && old.at("commits").at(1).at("body").at("schema")==1,"wrong legacy fixture");
    const auto now=old.at("commits").at(1).at("body").at("committed_at").get<millis>()+1000;
    s.submit("legacy","phase3",{{"text","continue"}},now);
    auto a=s.admit("legacy","test",now); check(a.has_value(),"legacy admission");
    Proposal p; p.wake_after_ms=0;
    auto next=s.commit(a->id,p,now); s.verify("legacy");
    check(next.tick==2 && next.memory.at("counter")==1,"legacy continuity lost");
    auto updated=s.timeline("legacy");
    check(updated.at("commits").at(0)==old.at("commits").at(0) && updated.at("commits").at(1)==old.at("commits").at(1),"legacy history rewritten");
}
void time_overflow() {
    Temp t; Store s(t.path.string());
    const auto high=std::numeric_limits<millis>::max()-5;
    s.create("s",{1,10,100},high); auto a=admit(s,high); Proposal p; p.wake_after_ms=10;
    rejects([&]{s.commit(a.id,p,high);}); check(s.snapshot("s").tick==0,"overflow committed");
    s.commit(a.id,Proposal{},high); s.verify("s");
}
}
int main() { try { quota_and_provenance(); rollback_and_elapsed(); windows_and_wait(); virtual_48_hours(); legacy_continuation(); time_overflow(); std::cout<<"PASS temporal invariants\n"; }
catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; } }
