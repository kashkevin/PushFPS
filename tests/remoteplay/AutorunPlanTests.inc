// SPDX-License-Identifier: GPL-3.0-only
// Test automation plan (veyra/engine/PushAutorunPlan.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushAutorunPlan.h"
#include <string>
#include <vector>
namespace {
namespace ar=veyra::engine::autorun;
struct ArHookGuard{~ArHookGuard(){ar::registerStep("sr",nullptr);ar::registerStep("fg",nullptr);ar::registerStep("quality",nullptr);}};
TEST(autorunPlanGrammar){
    const auto p=ar::parsePlan(" settle:20 ; latency:200:dpad-right-left;wait:5;sr:30;fg:15;quality;latency:12; exit ;");
    CHECK(p.valid()&&p.steps.size()==8&&p.hasExit());
    CHECK(p.steps[0].kind==ar::Kind::Settle&&p.steps[0].seconds==20&&p.steps[0].name=="settle");
    CHECK(p.steps[1].kind==ar::Kind::Latency&&p.steps[1].trials==200&&p.steps[1].button==veyra::engine::latencystats::Button::DpadRightLeft);
    CHECK(p.steps[2].kind==ar::Kind::Wait&&p.steps[2].seconds==5);
    CHECK(p.steps[3].kind==ar::Kind::Hook&&p.steps[3].name=="sr"&&p.steps[3].seconds==30);
    CHECK(p.steps[4].kind==ar::Kind::Hook&&p.steps[4].name=="fg"&&p.steps[4].seconds==15);
    CHECK(p.steps[5].kind==ar::Kind::Hook&&p.steps[5].name=="quality");
    CHECK(p.steps[6].button==veyra::engine::latencystats::Button::Ps&&p.steps[6].trials==12);
    CHECK(p.steps[7].kind==ar::Kind::Exit);
    CHECK(!ar::parsePlan("").valid());
    CHECK(ar::parsePlan("wait:0").valid());
}
TEST(autorunPlanLimitsAndUnknownSteps){
    for(const char* bad:{"settle:0","settle:-1","settle:3601","settle","settle:1:2","wait:x","latency:2","latency:501","latency:12:cross",
                         "latency:12:","sr:0","sr:601","fg","quality:5","exit:1","latency: 12"}){
        const auto p=ar::parsePlan(bad);
        CHECK(!p.valid()&&p.steps.size()==1&&p.steps[0].reason=="bad-argument");
    }
    const auto u=ar::parsePlan("wait:1;jump:3;exit");
    CHECK(!u.valid()&&u.steps.size()==2&&u.steps[1].name=="unknown"&&u.steps[1].reason=="unknown-step");
    CHECK(ar::parsePlan("latency:500").valid()&&ar::parsePlan("latency:3").valid());
    std::string many;for(int i=0;i<33;++i)many+="wait:1;";
    const auto m=ar::parsePlan(many);
    CHECK(m.steps.size()==33&&m.steps.back().reason=="too-many-steps");
    CHECK(ar::parsePlan(std::string(1100,'a')).steps.at(0).reason=="plan-too-long");
}
TEST(autorunRunnerWaitsForTheStreamThenSettles){
    ar::Runner r(ar::parsePlan("settle:10;wait:2"));
    ar::Inputs in;
    CHECK(r.tick(in).lines.empty());                      // no stream yet: nothing starts
    in.nowMs=1000;in.presenting=true;in.steadySinceMs=1000;
    auto o=r.tick(in);
    CHECK(o.lines.size()==1&&o.lines[0]=="step=1 name=settle state=start reason=none");
    in.nowMs=10999;CHECK(r.tick(in).lines.empty());
    in.nowMs=11000;o=r.tick(in);
    CHECK(o.lines.size()==1&&o.lines[0]=="step=1 name=settle state=done reason=ok");
    o=r.tick(in);CHECK(o.lines.at(0)=="step=2 name=wait state=start reason=none");
    in.nowMs=13000;o=r.tick(in);
    CHECK(o.lines.size()==2&&o.lines[1]=="finished ok=1 steps=2"&&o.action==ar::Action::None&&r.ok());
}
TEST(autorunRunnerSettleTimesOutAndStillExits){
    ar::Runner r(ar::parsePlan("settle:5;exit"));
    ar::Inputs in;in.presenting=true;in.steadySinceMs=-1;
    r.tick(in);
    in.nowMs=(5+120)*1000;
    const auto o=r.tick(in);
    CHECK(o.lines.size()==2&&o.lines[0]=="step=1 name=settle state=failed reason=timeout"&&o.lines[1]=="finished ok=0 steps=2");
    CHECK(o.action==ar::Action::Exit&&r.finished()&&!r.ok());
}
TEST(autorunRunnerNoStreamFailsTheFirstStep){
    ar::Runner r(ar::parsePlan("latency:12"));
    ar::Inputs in;r.tick(in);
    in.nowMs=ar::kStreamWaitMs;
    const auto o=r.tick(in);
    CHECK(o.lines.size()==3&&o.lines[1]=="step=1 name=latency state=failed reason=no-stream"&&o.action==ar::Action::None);
}
TEST(autorunRunnerLatencyStep){
    ar::Runner r(ar::parsePlan("latency:20:options;exit"));
    ar::Inputs in;in.presenting=true;
    auto o=r.tick(in);CHECK(o.action==ar::Action::StartLatency);
    in.latency=ar::LatencyState::Done;in.nowMs=100;
    CHECK(r.tick(in).lines.empty());                      // a Done left over from an earlier test does not count
    in.latency=ar::LatencyState::Running;in.nowMs=200;CHECK(r.tick(in).lines.empty());
    in.latency=ar::LatencyState::Done;in.nowMs=300;o=r.tick(in);
    CHECK(o.lines.at(0)=="step=1 name=latency state=done reason=ok");
    o=r.tick(in);
    CHECK(o.lines.size()==3&&o.lines[1]=="step=2 name=exit state=done reason=ok"&&o.lines[2]=="finished ok=1 steps=2"&&o.action==ar::Action::Exit);
    // Busy (optimiser, quality test, calibration): refused, never started.
    ar::Runner busy(ar::parsePlan("latency:12"));
    in={};in.presenting=true;in.measuring=true;
    o=busy.tick(in);CHECK(o.action==ar::Action::None&&o.lines.at(1)=="step=1 name=latency state=failed reason=busy");
    // Refused by the bridge, and a timeout cancels.
    ar::Runner refused(ar::parsePlan("latency:12"));
    in={};in.presenting=true;refused.tick(in);refused.latencyRefused("no stream!");
    o=refused.tick(in);CHECK(o.lines.at(0)=="step=1 name=latency state=failed reason=nostream");
    ar::Runner slow(ar::parsePlan("latency:3"));
    in={};in.presenting=true;slow.tick(in);in.latency=ar::LatencyState::Running;slow.tick(in);
    in.nowMs=3*5000+60000;o=slow.tick(in);
    CHECK(o.action==ar::Action::CancelLatency&&o.lines.at(0)=="step=1 name=latency state=failed reason=timeout");
    ar::Runner failed(ar::parsePlan("latency:3"));
    in={};in.presenting=true;failed.tick(in);in.latency=ar::LatencyState::Running;failed.tick(in);in.latency=ar::LatencyState::Failed;
    CHECK(failed.tick(in).lines.at(0)=="step=1 name=latency state=failed reason=test-failed");
}
TEST(autorunRunnerHookSteps){
    ArHookGuard guard;
    ar::Runner none(ar::parsePlan("sr:5;wait:1"));
    ar::Inputs in;in.presenting=true;
    auto o=none.tick(in);
    CHECK(o.lines.size()==3&&o.lines[1]=="step=1 name=sr state=failed reason=unsupported"&&o.lines[2]=="finished ok=0 steps=2");
    std::vector<int> events;int polls=0;
    ar::registerStep("fg",[&](ar::HookEvent e,int seconds){
        events.push_back(int(e));CHECK(seconds==7);
        if(e==ar::HookEvent::Poll&&++polls==2)return ar::HookResult{ar::HookState::Done,{}};
        return ar::HookResult{};
    });
    ar::registerStep("quality",[&](ar::HookEvent e,int){
        if(e==ar::HookEvent::Cancel){events.push_back(9);return ar::HookResult{};}
        return ar::HookResult{ar::HookState::Running,{}};
    });
    ar::Runner r(ar::parsePlan("fg:7;quality;exit"));
    in={};in.presenting=true;
    o=r.tick(in);CHECK(o.lines.size()==1);                       // start (Start is sent on this tick's poll)
    o=r.tick(in);o=r.tick(in);
    CHECK(o.lines.at(0)=="step=1 name=fg state=done reason=ok");
    CHECK((events==std::vector<int>{0,1,1}));
    in.nowMs=1000;r.tick(in);r.tick(in);
    in.nowMs=1000+600000;o=r.tick(in);
    CHECK(o.lines.at(0)=="step=2 name=quality state=failed reason=timeout"&&events.back()==9&&o.action==ar::Action::Exit);
    ar::registerStep("sr",[](ar::HookEvent,int){return ar::HookResult{ar::HookState::Failed,"Not Available"};});
    ar::Runner f(ar::parsePlan("sr:5"));
    in={};in.presenting=true;o=f.tick(in);                       // Start answered Failed on the first tick
    CHECK(o.lines.at(1)=="step=1 name=sr state=failed reason=notavailable");
}
TEST(autorunRunnerUnknownStepAndAbandon){
    ar::Runner r(ar::parsePlan("wait:0;dance;exit"));
    ar::Inputs in;in.presenting=true;
    auto o=r.tick(in);                                            // wait:0 starts and ends on one tick
    CHECK(o.lines.at(1)=="step=1 name=wait state=done reason=ok");
    o=r.tick(in);
    CHECK(o.lines.size()==3&&o.lines[0]=="step=2 name=unknown state=start reason=none"&&o.lines[1]=="step=2 name=unknown state=failed reason=unknown-step");
    CHECK(o.lines[2]=="finished ok=0 steps=2"&&o.action==ar::Action::Exit);   // stopped before its exit, still closes
    ar::Runner a(ar::parsePlan("latency:12"));
    a.tick(in);
    o=a.abandon("closing");
    CHECK(o.action==ar::Action::CancelLatency&&o.lines.at(0)=="step=1 name=latency state=failed reason=closing");
    CHECK(a.abandon("closing").lines.empty());
    ar::Runner exitOnly(ar::parsePlan("exit"));
    in={};o=exitOnly.tick(in);                                    // exit alone does not wait for a stream
    CHECK(o.action==ar::Action::Exit&&o.lines.back()=="finished ok=1 steps=1");
}
} // namespace
