// SPDX-License-Identifier: GPL-3.0-only
// Latency governor, active control and arbiter (veyra/engine/PushGovernorControl.h) and its closed-loop Linux model
// (veyra/engine/PushGovernorSim.h). Included by CoreTests.cpp directly above main(). Every number is invented.
#include "veyra/engine/PushGovernorSim.h"
#include <cstdint>
#include <string>
#include <vector>
namespace governor_control_tests {
namespace gc=veyra::engine;
using Fg=gc::PushFgRequest;
constexpr gc::PushFgMethod kFlow=gc::PushFgMethod::PushFlow;
// One tick of a hand-written sample: latency P95 (ms, -1 unknown), a 30 fps source, a calm GPU unless loaded.
gc::PushControlSample sample(int second,double latency,double sourceFps=30,double load=0.3,double decodeMs=1){
    gc::PushControlSample s;s.nowMs=static_cast<std::int64_t>(second)*1000;s.sourceFps=sourceFps;s.latencyP95Ms=latency;s.networkJitterMs=0.3;
    auto& b=s.budget;
    if(sourceFps>0){b.sourceIntervalMs=1000.0/sourceFps;b.outputIntervalMs=1000.0/(sourceFps*3);b.gpuFrameMs=load*b.sourceIntervalMs;b.fgMs=0;}
    b.decodeQueueDelayMs=decodeMs;b.presented=90;b.missedDeadlines=0;b.skippedPairShare=0;b.power=gc::PushPowerSource::Mains;
    return s;
}
// A tick at the given level with an unplanned delay of `unplanned` ms above its planned hold.
double latencyFor(const Fg& level,double unplanned,double fps=30){return gc::pushPlannedDelayMs(level.multiplier,fps)+unplanned;}
// Runs a control with the harness's own bookkeeping; returns the trace for the invariant checker.
struct Loop{
    gc::PushGovernorControl control;
    Fg user,current;
    std::vector<gc::PushSimRecord> trace;
    explicit Loop(Fg request,gc::PushControlConfig config={}):control(config),user(request),current(request){}
    gc::PushControlTick tick(const gc::PushControlSample& s,bool refuse=false){
        gc::PushSimRecord r;r.nowMs=s.nowMs;r.before=gc::PushGpuLevel{current.tier,current.multiplier};
        r.jitterMs=s.networkJitterMs;r.rateKnown=s.sourceFps>0;r.latencyKnown=s.latencyP95Ms>=0;r.budgetAnyKnown=r.rateKnown;r.sourceFps=s.sourceFps;r.latencyMs=s.latencyP95Ms;
        const auto t=control.update(current,s);
        r.beforeIndex=t.startIndex;r.tick=t;
        if(t.apply){if(refuse)control.applyRefused();else current=t.target;}
        r.after=gc::PushGpuLevel{current.tier,current.multiplier};
        r.changed=!(r.after.multiplier==r.before.multiplier&&r.after.tier==r.before.tier);
        r.userRequest=user;trace.push_back(r);return t;
    }
    std::string violation(const gc::PushControlConfig& c={}) const {return gc::pushGovernorInvariantViolation(trace,c);}
};
}

TEST(governor_control_ladder_starts_at_the_request_and_never_exceeds_it){
    namespace t=governor_control_tests;namespace gc=t::gc;
    const auto full=gc::pushControlLadder(t::Fg{t::kFlow,3,2},false,false);
    CHECK(full.count==5);   // 3x/t2, 2x/t2, 2x/t1, 2x/t0, off: multipliers first (no rebuild), tiers last
    CHECK(full[0].multiplier==3&&full[0].tier==2);CHECK(full[1].multiplier==2&&full[1].tier==2);CHECK(full[2].tier==1);
    CHECK(full[3].multiplier==2&&full[3].tier==0);CHECK(full[full.count-1].multiplier==1);
    for(std::size_t i=1;i<full.count;++i)CHECK(gc::pushLevelWithin(full[i],full[i-1]));
    for(std::size_t i=0;i<full.count;++i)CHECK(gc::pushLevelWithin(full[i],full[0]));
    const auto tierFrozen=gc::pushControlLadder(t::Fg{t::kFlow,3,2},false,true);
    for(std::size_t i=0;i<tierFrozen.count;++i)CHECK(tierFrozen[i].multiplier<2||tierFrozen[i].tier==2);
    const auto multiplierFrozen=gc::pushControlLadder(t::Fg{t::kFlow,3,2},true,false);
    CHECK(multiplierFrozen.count==3);for(std::size_t i=0;i<multiplierFrozen.count;++i)CHECK(multiplierFrozen[i].multiplier==3);
    CHECK(gc::pushControlLadder(t::Fg{t::kFlow,1,0},false,false).count==1);
    CHECK(gc::pushControlLadder(t::Fg{t::kFlow,3,2},true,true).count==1);
    // A method with one tier: multipliers only.
    const auto fsr=gc::pushControlLadder(t::Fg{gc::PushFgMethod::Fsr,2,0},false,false);
    CHECK(fsr.count==2&&fsr[1].multiplier==1);
    CHECK(!gc::pushLevelWithin(gc::PushGpuLevel{2,3},gc::PushGpuLevel{1,3}));CHECK(gc::pushLevelWithin(gc::PushGpuLevel{2,1},gc::PushGpuLevel{0,1}));
}

TEST(governor_control_config_is_sanitized){
    namespace gc=governor_control_tests::gc;
    gc::PushControlConfig bad;bad.latencyBudgetMs=-3;bad.hysteresisMs=99;bad.minIntervalMs=0;bad.maxChangesPerMinute=0;bad.oscillationReversals=0;bad.healthyHoldMs=-5;
    const auto c=gc::pushSanitizedControlConfig(bad);
    CHECK(c.latencyBudgetMs==10);CHECK(c.hysteresisMs<c.latencyBudgetMs);CHECK(c.minIntervalMs>=1000);CHECK(c.maxChangesPerMinute==1);
    CHECK(c.oscillationReversals==2);CHECK(c.healthyHoldMs>=1000);
    CHECK(gc::pushPlannedDelayMs(1,30)==0);CHECK(gc::pushPlannedDelayMs(2,-1)==0);
    CHECK(gc::pushPlannedDelayMs(3,30)>22.2&&gc::pushPlannedDelayMs(3,30)<22.3);
}

TEST(governor_control_every_simulator_scenario_keeps_every_invariant){
    namespace gc=governor_control_tests::gc;
    for(std::size_t i=0;i<gc::kPushSimScenarioCount;++i){
        const auto s=gc::pushRunGovernorScenario(static_cast<gc::PushSimScenario>(i));
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        CHECK(s.trace.size()==300u);
    }
    // A clean LAN never changes anything; overload is answered; tighter and looser settings keep the rules too.
    CHECK(gc::pushRunGovernorScenario(gc::PushSimScenario::CleanLan).changes==0);
    CHECK(gc::pushRunGovernorScenario(gc::PushSimScenario::FlappingThreshold).changes==0);
    CHECK(gc::pushRunGovernorScenario(gc::PushSimScenario::GpuOverloadRamp).downs>=1);
    gc::PushControlConfig tight;tight.latencyBudgetMs=4;tight.hysteresisMs=1;tight.minIntervalMs=1000;tight.maxChangesPerMinute=3;tight.healthyHoldMs=5000;tight.oscillationReversals=2;
    gc::PushControlConfig loose;loose.latencyBudgetMs=30;loose.sustainMs=0;loose.evaluateMs=2000;loose.lockoutMs=5000;
    for(const auto& config:{tight,loose})
        for(std::size_t i=0;i<gc::kPushSimScenarioCount;++i){
            const auto s=gc::pushRunGovernorScenario(static_cast<gc::PushSimScenario>(i),config);
            if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+" (config): "+s.firstViolation);
        }
}

TEST(governor_control_randomized_traces_keep_every_invariant){
    namespace gc=governor_control_tests::gc;
    unsigned changes=0;
    gc::PushControlConfig fast;fast.minIntervalMs=1000;fast.healthyHoldMs=3000;fast.maxChangesPerMinute=10;fast.evaluateMs=2000;fast.lockoutMs=10000;fast.sustainMs=1000;
    for(std::uint64_t seed=1000;seed<1150;++seed){
        const auto trace=gc::pushSimRandomTrace(seed,600);
        for(const auto& config:{gc::PushControlConfig{},fast}){
            const auto s=gc::pushRunGovernorSim("random",600,[&trace](int t){return trace.worlds[static_cast<std::size_t>(t)];},seed*7u,config,trace.request,trace.actions);
            if(!s.firstViolation.empty())throw std::runtime_error("seed "+std::to_string(seed)+": "+s.firstViolation);
            changes+=s.changes;
        }
    }
    CHECK(changes>200);   // the traces do exercise the control
}

TEST(governor_control_checker_catches_broken_traces){
    namespace t=governor_control_tests;namespace gc=t::gc;
    const double hold=gc::pushPlannedDelayMs(3,30);   // the checker recomputes the unplanned delay from these
    gc::PushSimRecord a;a.nowMs=0;a.sourceFps=30;a.latencyMs=hold+2;a.userRequest=t::Fg{t::kFlow,3,2};a.before=gc::PushGpuLevel{2,3};a.after=gc::PushGpuLevel{2,3};
    std::vector<gc::PushSimRecord> trace{a};
    CHECK(gc::pushGovernorInvariantViolation(trace,{}).empty());
    auto above=a;above.after=gc::PushGpuLevel{2,4};above.changed=true;
    CHECK(!gc::pushGovernorInvariantViolation({above},{}).empty());
    auto down=a;down.nowMs=1000;down.changed=true;down.after=gc::PushGpuLevel{1,3};down.latencyMs=hold+20;
    auto again=down;again.nowMs=2000;again.before=down.after;again.after=gc::PushGpuLevel{0,3};again.beforeIndex=1;
    CHECK(gc::pushGovernorInvariantViolation({a,down},{}).empty());
    CHECK(!gc::pushGovernorInvariantViolation({a,down,again},{}).empty());   // closer than the minimum interval
    auto noReason=down;noReason.latencyMs=hold+5;
    auto lying=noReason;lying.tick.unplannedMs=20;lying.tick.cause=gc::PushControlCause::Latency;   // the controller's own figure is not trusted
    CHECK(!gc::pushGovernorInvariantViolation({a,lying},{}).empty());
    CHECK(!gc::pushGovernorInvariantViolation({a,noReason},{}).empty());
    auto up=a;up.nowMs=30000;up.changed=true;up.before=gc::PushGpuLevel{1,3};up.after=gc::PushGpuLevel{2,3};up.latencyKnown=false;
    CHECK(!gc::pushGovernorInvariantViolation({a,up},{}).empty());          // a step up with the latency unknown
    up.latencyKnown=true;up.latencyMs=hold+9;CHECK(!gc::pushGovernorInvariantViolation({a,up},{}).empty());   // near the budget
    up.latencyMs=hold+2;CHECK(gc::pushGovernorInvariantViolation({a,up},{}).empty());
    up.tick.protectionTrouble=true;CHECK(!gc::pushGovernorInvariantViolation({a,up},{}).empty());
    auto frozen=down;frozen.userFrozenTier=true;CHECK(!gc::pushGovernorInvariantViolation({a,frozen},{}).empty());
    auto blind=down;blind.latencyKnown=false;blind.budgetAnyKnown=false;CHECK(!gc::pushGovernorInvariantViolation({a,blind},{}).empty());
}

TEST(governor_control_unknown_inputs_never_act){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<200;++s){
        auto in=t::sample(s,-1,s%3==0?-1.0:30.0,s>50?2.0:0.3,-1);   // latency never known; overload with an unknown rate now and then
        in.budget.gpuFrameMs=-1;in.budget.presented=0;in.budget.missedDeadlines=0;in.budget.skippedPairShare=-1;
        CHECK(!loop.tick(in).apply);
    }
    CHECK(loop.control.changes()==0);
}

TEST(governor_control_latency_over_budget_steps_down_then_returns_slowly){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    int firstDown=-1,firstUp=-1;unsigned downs=0;
    for(int s=0;s<200;++s){
        // 18 ms unplanned at the request until 40 s; afterwards 2 ms at any level.
        const double unplanned=s<40?(loop.current.multiplier==3?18.0:2.0):2.0;
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,unplanned)));
        if(tick.apply&&firstDown<0){firstDown=s;CHECK(tick.cause==gc::PushControlCause::Latency);CHECK(tick.target.multiplier==2);}   // fewer generated frames first
        if(tick.apply&&firstDown>=0&&s>firstDown&&tick.target.multiplier==3&&firstUp<0)firstUp=s;
        if(tick.apply&&tick.target.multiplier<3)++downs;
    }
    CHECK(downs==1);   // no return while the request's last reading (fresh) still says it is over budget
    CHECK(firstDown>=3&&firstDown<=4);   // sustained 3 s
    CHECK(firstUp>=firstDown+20);         // the healthy hold, and until the request's bad reading is forgotten
    CHECK(loop.current==(t::Fg{t::kFlow,3,2}));   // straight back to where the step came from, not via unmeasured tiers
    CHECK(loop.violation().empty());
}

TEST(governor_control_a_step_that_does_not_help_is_rolled_back_and_locked){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    std::vector<int> rollbacks,downs;
    for(int s=0;s<400;++s){
        // 15 ms unplanned at every level: the cause is not frame generation (network-like).
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,15)));
        if(tick.apply&&tick.cause==gc::PushControlCause::Rollback){rollbacks.push_back(s);CHECK(tick.target==(t::Fg{t::kFlow,3,2}));}
        else if(tick.apply)downs.push_back(s);
    }
    CHECK(downs.size()>=2&&rollbacks.size()==downs.size());
    CHECK(rollbacks[0]-downs[0]>=5&&rollbacks[0]-downs[0]<=6);   // judged after evaluateMs
    CHECK(downs[1]-rollbacks[0]>=60);                              // then that step is locked out for a minute ...
    CHECK(downs.size()<3||downs[2]-rollbacks[1]>=120);             // ... doubling
    CHECK(loop.control.rollbacks()==rollbacks.size());
    CHECK(loop.current==(t::Fg{t::kFlow,3,2}));
    CHECK(loop.violation().empty());
}

TEST(governor_control_user_override_wins_and_freezes_that_setting){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<10;++s)loop.tick(t::sample(s,t::latencyFor(loop.current,2)));
    loop.current=t::Fg{t::kFlow,4,2};loop.user=loop.current;   // the user raises the multiplier
    const auto first=loop.tick(t::sample(10,t::latencyFor(loop.current,2)));
    CHECK(first.userOverride);CHECK(!first.apply);CHECK(loop.control.frozenMultiplier());CHECK(!loop.control.frozenTier());
    CHECK(loop.control.request()==(t::Fg{t::kFlow,4,2}));
    for(int s=11;s<200;++s){
        // Over budget, but the only levels left are tiers: latency never steps a tier (a graph rebuild that
        // hardly moves the delay), so nothing changes.
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,25)));
        CHECK(!tick.apply);
    }
    CHECK(loop.current==(t::Fg{t::kFlow,4,2}));
    loop.current.tier=1;loop.user=loop.current;   // and now the tier as well: suspended
    for(int s=200;s<300;++s){const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,25)));CHECK(!tick.apply);}
    CHECK(loop.trace.back().tick.reason=="suspended-by-user");
    // A new stream lifts the freeze.
    loop.control.newStream();
    bool moved=false;
    for(int s=300;s<320;++s)moved=loop.tick(t::sample(s,t::latencyFor(loop.current,25))).apply||moved;
    CHECK(moved);
}

TEST(governor_control_refused_setter_is_not_a_user_change){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    int asked=-1;
    for(int s=0;s<10&&asked<0;++s)if(loop.tick(t::sample(s,t::latencyFor(loop.current,20)),true).apply)asked=s;
    CHECK(asked>=0);CHECK(loop.current==(t::Fg{t::kFlow,3,2}));
    const auto next=loop.tick(t::sample(asked+1,t::latencyFor(loop.current,20)));
    CHECK(next.refused);CHECK(!next.userOverride);CHECK(!loop.control.frozenMultiplier());CHECK(loop.control.index()==0);
    // The same level is not asked again within the hold.
    for(int s=asked+2;s<asked+55;++s)CHECK(!loop.tick(t::sample(s,t::latencyFor(loop.current,20))).apply);
}

TEST(governor_control_rate_limits_hold_under_a_flapping_trigger){
    namespace t=governor_control_tests;namespace gc=t::gc;
    gc::PushControlConfig c;c.sustainMs=0;c.healthyHoldMs=1000;c.minIntervalMs=1000;c.maxChangesPerMinute=4;c.evaluateMs=1000;c.oscillationReversals=3;c.lockoutMs=30000;
    t::Loop loop(t::Fg{t::kFlow,4,2},c);
    for(int s=0;s<600;++s)loop.tick(t::sample(s,t::latencyFor(loop.current,(s/2)%2==0?30.0:0.0)));
    CHECK(loop.violation(c).empty());
    unsigned changes=0;for(const auto& r:loop.trace)changes+=r.changed?1u:0u;
    CHECK(changes>0);CHECK(changes<=4u*10u);
    CHECK(loop.control.lockouts()+loop.control.rollbacks()>0);
}

TEST(governor_control_protection_steps_first_and_restore_returns_the_request){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    int firstProtect=-1;
    for(int s=0;s<60&&firstProtect<0;++s){
        // From 10 s decode waits 25 ms of 33 while the GPU runs the request; one tier less is enough.
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,2),30,0.3,s>=10&&loop.current.multiplier==3?25.0:1.0));
        if(tick.apply&&firstProtect<0){firstProtect=s;CHECK(tick.cause==gc::PushControlCause::Decode);CHECK(tick.target==(t::Fg{t::kFlow,2,2}));}
    }
    CHECK(firstProtect>=10);CHECK(loop.violation().empty());
    CHECK(!(loop.current==(t::Fg{t::kFlow,3,2})));
    const auto back=loop.control.pendingRestore(loop.current);
    CHECK(back.has_value());CHECK(*back==(t::Fg{t::kFlow,3,2}));
    loop.control.restored();
    CHECK(!loop.control.pendingRestore(*back).has_value());
    // Something the user set is never "restored" away.
    CHECK(!loop.control.pendingRestore(t::Fg{t::kFlow,4,0}).has_value());
}

TEST(governor_control_clock_step_back_is_ignored){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    loop.tick(t::sample(100,t::latencyFor(loop.current,2)));
    const auto tick=loop.tick(t::sample(50,t::latencyFor(loop.current,40)));
    CHECK(!tick.apply);CHECK(tick.reason=="bad-input");
}

TEST(governor_control_user_change_keeps_the_request_of_the_other_setting){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<10;++s)loop.tick(t::sample(s,t::latencyFor(loop.current,loop.current.multiplier==3?20.0:2.0)));
    CHECK(loop.current==(t::Fg{t::kFlow,2,2}));   // reduced by the governor (the tier kept)
    loop.current.tier=1;loop.user=t::Fg{t::kFlow,3,1};   // the user changes only the tier
    const auto tick=loop.tick(t::sample(10,t::latencyFor(loop.current,2)));
    CHECK(tick.userOverride);CHECK(loop.control.frozenTier());CHECK(!loop.control.frozenMultiplier());
    CHECK(loop.control.request()==(t::Fg{t::kFlow,3,1}));   // not 2x: the multiplier the user chose stays the ceiling
    CHECK(loop.control.index()==1);
    const auto back=loop.control.pendingRestore(loop.current);
    CHECK(back.has_value()&&*back==(t::Fg{t::kFlow,3,1}));
    // The multiplier still comes back after a healthy stretch, at the user's tier.
    for(int s=11;s<200;++s)loop.tick(t::sample(s,t::latencyFor(loop.current,2)));
    CHECK(loop.current==(t::Fg{t::kFlow,3,1}));
    CHECK(loop.violation().empty());
}

TEST(governor_control_returns_after_a_protection_step_under_the_budget_itself){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    bool reduced=false;
    for(int s=0;s<200;++s){
        // 8 ms unplanned everywhere: under the 10 ms budget but over budget minus hysteresis (7 ms). A decode
        // overload at the request from 10 to 40 s makes protection step down once.
        const bool overload=s>=10&&s<40&&loop.current.tier==2;
        loop.tick(t::sample(s,t::latencyFor(loop.current,8),30,0.3,overload?25.0:1.0));
        reduced=reduced||!(loop.current==(t::Fg{t::kFlow,3,2}));
    }
    CHECK(reduced);
    CHECK(loop.current==(t::Fg{t::kFlow,3,2}));   // back: the latency never asked for that step
    CHECK(loop.violation().empty());
}

TEST(governor_control_probes_back_up_when_short_bursts_keep_resetting_the_hold){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    int probes=0,probeRollbacks=0,firstDown=-1;
    for(int s=0;s<1500;++s){
        // 15 ms unplanned for 4 s every 15 s at every level: the 20 s healthy hold never completes.
        const bool burst=s%15<4;
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,burst?15.0:2.0)));
        if(tick.apply&&firstDown<0)firstDown=s;
        if(tick.apply&&tick.cause==gc::PushControlCause::Probe)++probes;
        if(tick.apply&&tick.reason=="rollback-probe-over-budget")++probeRollbacks;
    }
    CHECK(firstDown>=0);
    CHECK(probes>=2);              // capped: one per probe interval (5 min, doubling after a failed one)
    CHECK(probes<=1500/300+1);
    CHECK(probeRollbacks<=probes);
    CHECK(loop.violation().empty());
}

TEST(governor_control_periodic_short_bursts_bound_the_time_reduced){
    namespace gc=governor_control_tests::gc;
    const auto b=gc::pushRunBurstSim(6*3600);
    if(!b.firstViolation.empty())throw std::runtime_error("bursts: "+b.firstViolation);
    const double reduced=1.0-static_cast<double>(b.secondsAtIndex[0])/(6.0*3600.0);
    CHECK(reduced<0.6);   // Linux model: about 46 % over 24 h (it was ~100 % before the back-off decayed)
}

TEST(governor_control_pure_network_bursts_change_nothing){
    namespace t=governor_control_tests;namespace gc=t::gc;
    // The reviewer's trace: jitter explains the whole excess, at every level: no step is frame generation's to take.
    const auto b=gc::pushRunBurstSim(6*3600);
    CHECK(b.firstViolation.empty());
    CHECK(b.changes==0);CHECK(b.rebuilds==0);
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<600;++s){auto in=t::sample(s,t::latencyFor(loop.current,s%40<8?16.0:2.0));in.networkJitterMs=s%40<8?15.0:0.3;CHECK(!loop.tick(in).apply);}
    // With the jitter unknown the cause cannot be told either: no latency step.
    t::Loop blind(t::Fg{t::kFlow,3,2});
    for(int s=0;s<60;++s){auto in=t::sample(s,t::latencyFor(blind.current,20));in.networkJitterMs=-1;CHECK(!blind.tick(in).apply);}
}
TEST(governor_control_latency_never_steps_a_tier_and_tier_steps_are_spaced){
    namespace t=governor_control_tests;namespace gc=t::gc;
    gc::PushGovernorControl c;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    loop.control.update(loop.current,t::sample(0,30));
    // Multiplier frozen by the user: only tiers left, so latency has nothing to step.
    loop.current=t::Fg{t::kFlow,4,2};loop.user=loop.current;
    for(int s=1;s<100;++s)CHECK(!loop.tick(t::sample(s,t::latencyFor(loop.current,25))).apply);
    CHECK(loop.trace.back().tick.reason=="at-floor");
    // GPU-cost protection may step tiers, at most one per tierIntervalMs.
    gc::PushControlConfig spaced;spaced.tierIntervalMs=200000;spaced.healthyHoldMs=5000;
    t::Loop gpu(t::Fg{t::kFlow,3,2},spaced);
    std::vector<int> tierSteps;
    for(int s=0;s<600;++s){
        // Overloaded above 2x/t1 (each step down still helps), fine below.
        const double load=s>=100?0.3:gpu.current.multiplier==3?0.95:gpu.current.tier==2?0.88:0.3;   // the overload ends at 100 s
        gpu.tick(t::sample(s,t::latencyFor(gpu.current,2),30,load));
    }
    // Every tier change (protection, recovery, probe or rollback alike) at least 60 s after the last one.
    for(const auto& r:gpu.trace)if(r.changed&&r.before.multiplier>=2&&r.after.multiplier>=2&&r.before.tier!=r.after.tier)tierSteps.push_back(static_cast<int>(r.nowMs/1000));
    CHECK(tierSteps.size()>=1);
    for(std::size_t i=1;i<tierSteps.size();++i)CHECK(tierSteps[i]-tierSteps[i-1]>=200);
    CHECK(gpu.violation(spaced).empty());
    (void)c;
}

TEST(governor_control_a_failing_probe_is_rolled_back_and_backs_off){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    std::vector<int> probes,probeRollbacks;
    for(int s=0;s<4000;++s){
        // 3x is always over budget (attributable: no jitter); 2x is fine except short spikes that keep the
        // healthy hold from completing, so only probes try 3x again.
        const double unplanned=loop.current.multiplier==3?20.0:(s%15<3?8.5:2.0);
        const bool lockedBefore=loop.control.lockedOut(static_cast<std::int64_t>(s)*1000);
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,unplanned)));
        if(tick.apply&&tick.cause==gc::PushControlCause::Probe){probes.push_back(s);CHECK(!lockedBefore);CHECK(tick.unplannedMs<7);}
        if(tick.apply&&tick.reason=="rollback-probe-over-budget")probeRollbacks.push_back(s);
    }
    CHECK(probes.size()>=3);
    CHECK(probeRollbacks.size()==probes.size());   // never kept over the budget
    for(std::size_t i=0;i<probes.size();++i)CHECK(probeRollbacks[i]>probes[i]&&probeRollbacks[i]-probes[i]<=7);
    CHECK(probes[1]-probes[0]>=2*300-10);           // doubling ...
    CHECK(probes[2]-probes[1]>=4*300-10);           // ... capped at 4x
    if(probes.size()>3)CHECK(probes[3]-probes[2]<=4*300+60);
    CHECK(loop.violation().empty());
}

TEST(governor_control_no_probe_while_not_healthy){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<1500;++s){
        // 20 ms at 3x (a latency step down), then 8.5 ms at 2x: under the budget, over budget minus hysteresis.
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,loop.current.multiplier==3?20.0:8.5)));
        CHECK(!(tick.apply&&tick.cause==gc::PushControlCause::Probe));
    }
    CHECK(loop.current.multiplier==2);
}

TEST(governor_control_checker_catches_probe_and_rate_violations){
    namespace t=governor_control_tests;namespace gc=t::gc;
    const double hold=gc::pushPlannedDelayMs(3,30);
    gc::PushSimRecord a;a.nowMs=0;a.sourceFps=30;a.latencyMs=hold+2;a.jitterMs=0.3;a.userRequest=t::Fg{t::kFlow,3,2};a.before=gc::PushGpuLevel{2,3};a.after=a.before;
    // A probe over the line the budget allows.
    gc::PushSimRecord down=a;down.nowMs=1000;down.changed=true;down.after=gc::PushGpuLevel{0,2};down.latencyMs=hold+20;down.tick.cause=gc::PushControlCause::Latency;
    gc::PushSimRecord probe=a;probe.nowMs=400000;probe.changed=true;probe.before=down.after;probe.after=a.before;probe.tick.cause=gc::PushControlCause::Probe;
    probe.latencyMs=gc::pushPlannedDelayMs(2,30)+2;
    CHECK(gc::pushGovernorInvariantViolation({a,down,probe},{}).empty());
    auto hot=probe;hot.latencyMs=gc::pushPlannedDelayMs(2,30)+8;
    CHECK(!gc::pushGovernorInvariantViolation({a,down,hot},{}).empty());
    auto soon=probe;soon.nowMs=100000;
    CHECK(!gc::pushGovernorInvariantViolation({a,down,soon},{}).empty());
    // The per-minute cap alone (other rules relaxed).
    gc::PushControlConfig c;c.maxChangesPerMinute=2;c.oscillationReversals=16;
    std::vector<gc::PushSimRecord> trace{a};
    gc::PushGpuLevel level=a.before;
    for(int i=0;i<3;++i){
        gc::PushSimRecord r=a;r.nowMs=10000+4000*i;r.changed=true;r.before=level;r.after=gc::PushGpuLevel{level.tier>0?level.tier-1:0u,level.tier>0?level.multiplier:level.multiplier-1};
        r.tick.cause=gc::PushControlCause::GpuLoad;r.tick.protectIndex=9;r.beforeIndex=0;level=r.after;trace.push_back(r);
    }
    const auto v=gc::pushGovernorInvariantViolation(trace,c);
    CHECK(v.find("more changes in a minute")!=std::string::npos);
    trace.pop_back();CHECK(gc::pushGovernorInvariantViolation(trace,c).empty());
}

TEST(governor_control_probes_obey_the_rate_gates){
    namespace t=governor_control_tests;namespace gc=t::gc;
    gc::PushControlConfig c;c.probeIntervalMs=10000;c.maxChangesPerMinute=1;
    t::Loop loop(t::Fg{t::kFlow,3,2},c);
    int firstDown=-1,firstProbe=-1;
    for(int s=0;s<300;++s){
        const double unplanned=loop.current.multiplier==3?(s<20?20.0:2.0):(s%15<3?8.5:2.0);
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,unplanned)));
        if(tick.apply&&firstDown<0)firstDown=s;
        if(tick.apply&&tick.cause==gc::PushControlCause::Probe&&firstProbe<0)firstProbe=s;
    }
    CHECK(firstDown>=0&&firstProbe>firstDown);
    CHECK(firstProbe-firstDown>=60);   // one change a minute: the probe waits for the cap like every other step
    CHECK(loop.violation(c).empty());
}

TEST(governor_control_bursts_with_decode_stall_or_loss_change_nothing){
    namespace gc=governor_control_tests::gc;
    for(int kind:{1,2}){
        const auto b=gc::pushRunBurstSim(6*3600,{},kind);
        if(!b.firstViolation.empty())throw std::runtime_error(b.scenario+": "+b.firstViolation);
        CHECK(b.rebuilds==0);
        const double reduced=1.0-static_cast<double>(b.secondsAtIndex[0])/(6.0*3600.0);
        CHECK(reduced<0.05);
    }
}
TEST(governor_control_stands_down_after_two_uncredited_steps){
    namespace t=governor_control_tests;namespace gc=t::gc;
    t::Loop loop(t::Fg{t::kFlow,3,2});
    unsigned downs=0;
    for(int s=0;s<1200;++s){
        // 15 ms unplanned at every level (no jitter to explain it): each step is judged useless.
        const auto tick=loop.tick(t::sample(s,t::latencyFor(loop.current,15)));
        if(tick.apply&&tick.cause==gc::PushControlCause::Latency)++downs;
    }
    CHECK(downs==2);CHECK(loop.control.stoodDown());CHECK(loop.current==(t::Fg{t::kFlow,3,2}));
    CHECK(loop.trace.back().tick.reason=="stood-down");
    loop.control.newStream();CHECK(!loop.control.stoodDown()||!loop.control.started());
}
