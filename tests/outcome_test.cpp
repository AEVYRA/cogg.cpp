#include "cogg/routing.hpp"
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace cogg;
namespace {
void check(bool ok, const char* why) { if (!ok) throw Error(why); }
template<class F> void rejects(F f) { bool bad=false; try { f(); } catch(const std::exception&) { bad=true; } check(bad,"expected rejection"); }
struct Temp {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("cogg-outcome-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(dir); }
    ~Temp() { std::filesystem::remove_all(dir); }
    std::string db() const { return (dir / "s.db").string(); }
};
void mutate(const std::string& path, const std::string& sql) {
    sqlite3* db=nullptr; check(sqlite3_open(path.c_str(), &db)==SQLITE_OK,"open failed");
    const auto rc=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr); sqlite3_close(db); check(rc==SQLITE_OK,"mutation failed");
}
struct Organ : Backend {
    std::function<Outcome(const Present&)> fn;
    explicit Organ(std::function<Outcome(const Present&)> f) : fn(std::move(f)) {}
    std::string name() const override { return "test-organ/v1"; }
    Proposal propose(const Present&) override { throw Error("legacy entry must not be called"); }
    Outcome respond_attempt(const Attempt& a, millis, const std::function<bool()>&) override { return fn(a.present); }
};
void shapes() {
    for (const auto* reason : {"missing_input","unsupported","uncertain","refused"}) {
        auto a=outcome_json(Abstention{reason,"detail"}); check(outcome_json(parse_outcome(a))==a,"abstention roundtrip");
        for (const auto* field : {"memory","text","notes","wake_after_ms"}) {
            auto bad=a; bad[field]=nullptr; rejects([&]{ parse_outcome(bad); });
        }
    }
    rejects([]{ outcome_json(Abstention{"other",""}); });
    rejects([]{ outcome_json(Abstention{"uncertain",std::string(513,'x')}); });
    check(std::holds_alternative<Proposal>(parse_outcome(proposal_json(Proposal{}))),"null became abstention");
    auto observation=make_input("observation","fixture:frame-1",{{"cat","lying on cushion"}});
    auto inference=make_input("inference","fixture:reader",{{"claim","possibly resting"}},{observation.at("id")});
    json inputs=json::array({observation,inference}); validate_inputs(inputs);
    rejects([&]{ validate_inputs(json::array({inference,observation})); });
    rejects([&]{ validate_inputs(json::array({observation,observation})); });
    auto bad=inputs; bad[0]["body"]["content"]="edited"; rejects([&]{validate_inputs(bad);});
    auto foreign=make_input("inference","reader","text",{std::string(64,'a')});
    rejects([&]{validate_inputs(json::array({observation,foreign}));});
    rejects([&]{validate_inputs(json::array({make_input("inference","reader","text")}));});
    rejects([&]{validate_inputs(json::array({make_input("observation","camera",std::string(33000,'x'))}));});
}
void persistence_and_resume() {
    Temp t; std::string attempt, receipt, parent, occasion;
    {
        Store s(t.db()); s.create("s",{10,10,1000},0,{{"promise","keep"}});
        auto a=*s.admit("s","reader",0); attempt=a.id;parent=a.present.state.head;occasion=a.present.occasion.id;
        receipt=s.abstain(a.id,{"missing_input","frame required"});
        check(s.abstain(a.id,{"missing_input","frame required"})==receipt,"replay not idempotent");
        rejects([&]{s.abstain(a.id,{"refused",""});});
        rejects([&]{s.commit(a.id,Proposal{},1);});
        s.fail(a.id,"late error");
        check(!s.admit("s","reader",9),"abstention refunded rate budget");
        check(s.snapshot("s").head==parent && s.snapshot("s").tick==0 && !s.snapshot("s").wake_at,"abstention changed state");
        s.verify("s");
    }
    {
        Store s(t.db()); s.verify("s");
        auto observation=make_input("observation","fixture:camera/frame-1",{{"description","cat lies on a cushion"}});
        auto interpretation=make_input("inference","fixture:vision",{{"description","possibly resting"}},{observation.at("id")});
        json context={{"head",parent},{"occasion",occasion},{"inputs",json::array({observation,interpretation})}};
        Organ reader([](const Present& p)->Outcome {
            check(p.inputs.size()==2 && !p.prior_unsettled_attempt,"resume input or settlement lost");
            check(p.inputs[1]["body"]["sources"][0]==p.inputs[0]["id"],"shared observation lost");
            Proposal out; out.kind="speech"; out.text="According to the supplied observation, the cat is lying on a cushion; it may be resting."; return out;
        });
        Runtime runtime(s,reader); auto result=runtime.step("s",10,context);
        check(result && result->tick==1 && result->memory.at("promise")=="keep","resume did not commit once");
        check(s.timeline("s")["abstentions"][0]["id"]==receipt,"receipt lost on restart");
        check(s.timeline("s")["attempts"][1]["body"]["inputs"]==context["inputs"],"admission did not freeze inputs");
        check(s.abstain(attempt,{"missing_input","frame required"})==receipt,"idempotence lost after head advanced");
        rejects([&]{runtime.step("s",20,context);});
        check(s.timeline("s")["attempts"].size()==2,"stale context spent quota");
        s.verify("s");
    }
    mutate(t.db(),"UPDATE abstentions SET body=json_set(body,'$.outcome.reason','unsupported')");
    Store corrupted(t.db()); rejects([&]{corrupted.verify("s");});
}
void all_abstentions_stop_and_count() {
    for (const auto* reason : {"missing_input","unsupported","uncertain","refused"}) {
        Temp t; Store s(t.db()); s.create("s",{1,1,1000},0);
        Registry registry; int next=0;
        registry.add("first",{},[&]{return std::make_unique<Organ>([&](const Present&)->Outcome{return Abstention{reason,""};});});
        registry.add("next",{},[&]{++next;return std::make_unique<Organ>([](const Present&)->Outcome{return Proposal{};});});
        Route route;route.executors={"first","next"}; RoutedRuntime runtime(s,registry);
        auto result=runtime.step("s",route,0);
        check(result.status=="abstained" && !result.snapshot && next==0,"abstention triggered implicit fallback");
        check(s.snapshot("s").tick==0 && s.timeline("s")["abstentions"].size()==1,"local null advanced subject");
        check(runtime.step("s",route,1).status=="waiting","quota refunded"); s.verify("s");
    }
    Temp t; Store s(t.db());s.create("s",{1,10,1000},0);
    Organ organ([](const Present&)->Outcome{return Abstention{"uncertain",""};}); Runtime runtime(s,organ);
    check(!runtime.step("s",0) && runtime.last_abstention()->reason=="uncertain","single-model runtime lost abstention");
    check(!runtime.step("s",0) && !runtime.last_abstention(),"last abstention leaked to waiting step");s.verify("s");
}
void routed_late_abstention() {
    Temp t; Store s(t.db());s.create("s",{1,100,1000},0);Registry registry;
    registry.add("late",{},[&]{return std::make_unique<Organ>([&](const Present&)->Outcome{
        Store other(t.db());auto winner=*other.admit("s","winner",10);other.commit(winner.id,Proposal{},10);
        return Abstention{"missing_input",""};
    });});
    Route route;route.executors={"late"};RoutedRuntime runtime(s,registry);
    auto result=runtime.step("s",route,0);
    check(result.status=="conflict" && s.timeline("s")["abstentions"].empty(),"late abstention survived competing commit");s.verify("s");
}
void races_and_capacity() {
    Temp t; Store s(t.db());s.create("s",{1,100,1000},0);
    auto first=*s.admit("s","one",0);auto second=*s.admit("s","two",1);
    s.commit(second.id,Proposal{},1);rejects([&]{s.abstain(first.id,{"uncertain",""});});s.verify("s");
    s.submit("s","new",{},2);auto head=s.snapshot("s").head;
    json context={{"head",head},{"occasion",s.schedule("s",2)["occasion"]["id"]},{"inputs",json::array({make_input("observation","fixture","large")})}};
    const auto count=s.timeline("s")["attempts"].size();
    rejects([&]{s.admit("s","small",2,{},[](const Present& p){return p.inputs.empty();},nullptr,context);});
    check(s.timeline("s")["attempts"].size()==count,"context overflow spent quota");
    auto good=*s.admit("s","other",2,{}, {},nullptr,context);s.abstain(good.id,{"uncertain",""});s.verify("s");
    mutate(t.db(),"DELETE FROM abstentions");rejects([&]{s.verify("s");});
}
}
int main(){try{shapes();persistence_and_resume();all_abstentions_stop_and_count();routed_late_abstention();races_and_capacity();std::cout<<"outcome and provenance invariants passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
