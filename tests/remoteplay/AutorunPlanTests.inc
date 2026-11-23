// SPDX-License-Identifier: GPL-3.0-only
// Test automation plan (veyra/engine/PushAutorunPlan.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushAutorunPlan.h"
#include "veyra/engine/SlipstreamSettings.h"
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
    r.setButtonsAvailable(true);                          // VEYRA_EXP_RP_LATENCY_BUTTONS=1
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
TEST(autorunRunnerAlternativeButtonsNeedTheSwitch){
    // Without VEYRA_EXP_RP_LATENCY_BUTTONS=1 an OPTIONS / D-pad step fails before anything is pressed.
    for(const char* text:{"latency:20:options;exit","latency:500:dpad-right-left"}){
        ar::Runner r(ar::parsePlan(text));
        ar::Inputs in;in.presenting=true;
        const auto o=r.tick(in);
        CHECK(o.action!=ar::Action::StartLatency&&o.action!=ar::Action::CancelLatency);
        CHECK(o.lines.size()==3&&o.lines[0]=="step=1 name=latency state=start reason=none"
              &&o.lines[1]=="step=1 name=latency state=failed reason=buttons-experimental"&&r.finished()&&!r.ok());
        CHECK((std::string(text).find("exit")!=std::string::npos)==(o.action==ar::Action::Exit));
    }
    // The PS button needs no switch, at any count; with the switch the other buttons start.
    for(const char* text:{"latency:500","latency:12:ps"}){
        ar::Runner r(ar::parsePlan(text));ar::Inputs in;in.presenting=true;
        CHECK(r.tick(in).action==ar::Action::StartLatency);
    }
    ar::Runner on(ar::parsePlan("latency:12:dpad-right-left"));on.setButtonsAvailable(true);
    ar::Inputs in;in.presenting=true;CHECK(on.tick(in).action==ar::Action::StartLatency);
    CHECK(lst::buttonAllowed(lst::Button::Ps,false)&&!lst::buttonAllowed(lst::Button::Options,false)&&!lst::buttonAllowed(lst::Button::DpadRightLeft,false)
          &&lst::buttonAllowed(lst::Button::Options,true));
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
    auto o=r.tick(in);                                            // an invalid plan runs nothing, not even wait:0
    CHECK(o.lines.size()==2&&o.lines[0]=="step=2 name=unknown state=failed reason=unknown-step");
    CHECK(o.lines[1]=="finished ok=0 steps=2"&&o.action==ar::Action::Exit);   // the text holds a well-formed exit
    ar::Runner a(ar::parsePlan("latency:12"));
    a.tick(in);
    o=a.abandon("closing");
    CHECK(o.action==ar::Action::CancelLatency&&o.lines.at(0)=="step=1 name=latency state=failed reason=closing");
    CHECK(a.abandon("closing").lines.empty());
    ar::Runner exitOnly(ar::parsePlan("exit"));
    in={};o=exitOnly.tick(in);                                    // exit alone does not wait for a stream
    CHECK(o.action==ar::Action::Exit&&o.lines.back()=="finished ok=1 steps=1");
}
// Review B2: a malformed plan fails closed on the first tick, with no stream and without running any step.
TEST(autorunInvalidPlanRunsNothing){
    struct Case{const char* text;const char* failed;bool exit;};
    for(const Case& c:{Case{"latency:500;bogus;exit","step=2 name=unknown state=failed reason=unknown-step",true},
                       Case{"latency:500;bogus","step=2 name=unknown state=failed reason=unknown-step",false},
                       Case{"settle:1;latency:50:dpad-right-left;wait:x","step=3 name=wait state=failed reason=bad-argument",false},
                       Case{"latency:12;exit:1","step=2 name=exit state=failed reason=bad-argument",false},   // exit:1 is no exit
                       Case{"wait:x;exit","step=1 name=wait state=failed reason=bad-argument",true}}){
        ar::Runner r(ar::parsePlan(c.text));
        ar::Inputs in;                                            // no stream at all
        auto o=r.tick(in);
        CHECK(o.lines.size()==2&&o.lines[0]==c.failed&&o.lines[1].rfind("finished ok=0 steps=",0)==0);
        CHECK(r.finished()&&!r.ok()&&o.action==(c.exit?ar::Action::Exit:ar::Action::None));
        in.presenting=true;in.steadySinceMs=0;
        for(int k=1;k<100;++k){in.nowMs=k*50;o=r.tick(in);CHECK(o.lines.empty()&&o.action==ar::Action::None);}
    }
    ar::Runner tooLong(ar::parsePlan(std::string(1100,'a')+";exit"));
    const auto o=tooLong.tick(ar::Inputs{});
    CHECK(o.lines.at(0)=="step=1 name=plan state=failed reason=plan-too-long"&&o.action==ar::Action::None);
}
// Review S1: the plan ends when the stream it ran on goes away or the user cancels its latency test.
TEST(autorunPlanStopsWhenTheStreamGoes){
    // The reviewer's case: a disconnect during wait, then a reconnect (a new session): the latency step never starts.
    ar::Runner w(ar::parsePlan("settle:1;wait:10;latency:20;exit"));
    ar::Inputs in;in.presenting=true;in.steadySinceMs=0;in.session=7;
    bool pressed=false;std::vector<std::string> lines;ar::Action last=ar::Action::None;
    for(int k=0;k<1000&&!w.finished();++k){
        in.nowMs=k*50;
        const bool gone=k>60&&k<150;
        in.presenting=!gone;in.steadySinceMs=gone?-1:0;in.session=gone?0:(k>=150?8:7);
        const auto o=w.tick(in);
        for(const auto& l:o.lines)lines.push_back(l);
        if(o.action==ar::Action::StartLatency)pressed=true;
        if(o.action!=ar::Action::None)last=o.action;
    }
    CHECK(!pressed&&w.finished()&&!w.ok()&&last==ar::Action::Exit);
    CHECK(lines.size()>=2&&lines[lines.size()-2]=="step=2 name=wait state=failed reason=stream-lost");
    // Same session, frames stop for 10 s: lost; for 5 s: the plan goes on.
    ar::Runner stall(ar::parsePlan("wait:60"));
    in={};in.presenting=true;in.session=3;stall.tick(in);
    in.presenting=false;in.nowMs=5000;CHECK(stall.tick(in).lines.empty());
    in.presenting=true;in.nowMs=6000;CHECK(stall.tick(in).lines.empty());
    in.presenting=false;in.nowMs=15999;CHECK(stall.tick(in).lines.empty());
    in.nowMs=16000;auto o=stall.tick(in);
    CHECK(o.lines.at(0)=="step=1 name=wait state=failed reason=stream-lost"&&stall.finished());
    // The user's Stop while the latency test runs: the button is released (CancelLatency), the app still closes.
    ar::Runner run(ar::parsePlan("latency:500;exit"));
    in={};in.presenting=true;in.session=4;
    CHECK(run.tick(in).action==ar::Action::StartLatency);
    in.latency=ar::LatencyState::Running;in.nowMs=100;CHECK(run.tick(in).lines.empty());
    in.session=0;in.presenting=false;in.nowMs=150;o=run.tick(in);
    CHECK(o.action==ar::Action::CancelLatency&&o.lines.at(0)=="step=1 name=latency state=failed reason=stream-lost"&&run.exitAfterCancel());
    // The user cancels the test (its state goes back to idle): the step fails now, not after its 2560 s timeout.
    ar::Runner cancel(ar::parsePlan("latency:500;exit"));
    in={};in.presenting=true;in.session=4;cancel.tick(in);
    in.latency=ar::LatencyState::Running;in.nowMs=2000;cancel.tick(in);
    in.latency=ar::LatencyState::Idle;in.nowMs=2050;o=cancel.tick(in);
    CHECK(o.lines.at(0)=="step=1 name=latency state=failed reason=cancelled"&&o.action==ar::Action::Exit&&cancel.finished());
    // A hook step is cancelled (the user's settings go back) when the stream goes.
    ArHookGuard guard;
    std::vector<int> events;
    ar::registerStep("sr",[&](ar::HookEvent e,int){events.push_back(int(e));return ar::HookResult{};});
    ar::Runner hook(ar::parsePlan("sr:30"));
    in={};in.presenting=true;in.session=5;hook.tick(in);hook.tick(in);
    in.session=6;o=hook.tick(in);
    CHECK(o.lines.at(0)=="step=1 name=sr state=failed reason=stream-lost"&&(events==std::vector<int>{0,1,2}));
    // Abandon (closing) while the test runs asks for the button's release.
    ar::Runner closing(ar::parsePlan("latency:12"));
    in={};in.presenting=true;closing.tick(in);
    CHECK(closing.abandon("closing").action==ar::Action::CancelLatency);
}
// Review S2: slipstream.ini cannot switch the automation on; only the real environment can.
TEST(autorunSwitchesNeverFromTheSettingsFile){
    const auto f=veyra::engine::parseSlipstreamSettings("VEYRA_TEST_RP_AUTORUN=latency:500;exit\nVEYRA_TEST_RP_AUTOCONNECT=1\nVEYRA_EXP_RP_STATS=1\nmode=competitive\n");
    CHECK(f.valid);
    bool stats=false;
    for(const auto& sw:f.switches){CHECK(sw.name!="VEYRA_TEST_RP_AUTORUN"&&sw.name!="VEYRA_TEST_RP_AUTOCONNECT");if(sw.name=="VEYRA_EXP_RP_STATS")stats=true;}
    CHECK(stats);                                                 // other switches still work
    CHECK(f.unknownKeys.size()==2&&f.unknownKeys[0]=="VEYRA_TEST_RP_AUTORUN"&&f.unknownKeys[1]=="VEYRA_TEST_RP_AUTOCONNECT");
    for(const auto& h:f.heldOff)CHECK(h.rfind("VEYRA_TEST_RP_AUTO",0)!=0);
    const auto off=veyra::engine::parseSlipstreamSettings("VEYRA_TEST_RP_AUTOCONNECT=\n");
    CHECK(off.valid&&off.heldOff.empty()&&off.switches.empty());
    for(const char* mode:{"ultra-low-latency","competitive","balanced","maximum-smoothness","automatic"}){
        const auto m=veyra::engine::parseSlipstreamSettings(std::string("mode=")+mode+"\nlan=1\n");
        for(const auto& sw:m.switches)CHECK(sw.name.rfind("VEYRA_TEST_RP_AUTO",0)!=0);
    }
}
// Review S4: [slipstream-config] at most once per 5 s; fgActive, srActive and the window size alone never cause one.
TEST(autorunConfigLineGate){
    const std::string a="mode=none fg=1 window=1920x1080 refreshHz=180.00 trialsMax=500 modeSource=none fgActive=1 srActive=0";
    CHECK(ar::configTrigger(a)=="mode=none fg=1 refreshHz=180.00 trialsMax=500 modeSource=none");
    ar::ConfigLineGate g;
    CHECK(g.due(a,1,0));                                          // the session's first line
    int lines=0;
    for(int s=1;s<=60;++s){                                       // a minute of flapping, checked every second
        std::string flap=a;
        if(s%2){flap.replace(flap.find("fgActive=1"),10,"fgActive=0");flap.replace(flap.find("window=1920x1080"),16,"window=1919x1080");}
        if(g.due(flap,1,s*1000))++lines;
    }
    CHECK(lines==0);
    std::string changed=a;changed.replace(changed.find("fg=1"),4,"fg=0");
    CHECK(g.due(changed,1,61000));                                // a real change
    std::string back=a;
    CHECK(!g.due(back,1,62000)&&!g.due(back,1,65999)&&g.due(back,1,66000));   // a second change waits for 5 s
    CHECK(!g.due(back,2,67000)&&g.due(back,2,71000));            // a new session: written, also no sooner than 5 s
    // Every second a real change: one line per 5 s at most.
    ar::ConfigLineGate h;int n=0;
    for(int s=0;s<60;++s)if(h.due("fg="+std::to_string(s),1,s*1000))++n;
    CHECK(n==12);
}
} // namespace
