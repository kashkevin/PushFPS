// SPDX-License-Identifier: GPL-3.0-only
// Latency governor, sprint 4 (review 4): link stalls the mean jitter cannot see, the credit a step earns, rebuild
// spacing for generation on/off, and standing down. Included by CoreTests.cpp directly above main(). LINUX MODEL:
// every number is invented (PushGovernorSim.h); nothing here ran on a GPU, a PC or a PS5.
#include "veyra/engine/PushGovernorSim.h"
#include "veyra/engine/PushGovernorStall.h"
#include <cstdint>
#include <string>
#include <vector>
namespace governor_stall_tests {
namespace gs=veyra::engine;
using Fg=gs::PushFgRequest;
constexpr gs::PushFgMethod kFlow=gs::PushFgMethod::PushFlow;
// One hand-written tick: unplanned delay above the level's planned hold, a 30 fps source, GPU work per picture
// = base x interval + (M-1) x (2 + tier) ms of generation, a decode-queue delay.
gs::PushControlSample at(int second,const Fg& level,double unplanned,double baseLoad=0.3,double decodeMs=1,double jitterMs=0.3,double stallMs=-1){
    gs::PushControlSample s;s.nowMs=static_cast<std::int64_t>(second)*1000;s.sourceFps=30;
    s.latencyP95Ms=gs::pushPlannedDelayMs(level.multiplier,30)+unplanned;s.networkJitterMs=jitterMs;s.lossPercent=0;s.networkStallMs=stallMs;
    auto& b=s.budget;b.sourceIntervalMs=1000.0/30;b.outputIntervalMs=1000.0/90;b.gpuFrameMs=baseLoad*b.sourceIntervalMs;
    b.fgMs=level.multiplier>=2?static_cast<double>(level.multiplier-1)*(2.0+static_cast<double>(level.tier)):0.0;
    b.decodeQueueDelayMs=decodeMs;b.presented=90;b.missedDeadlines=0;b.skippedPairShare=0;b.power=gs::PushPowerSource::Mains;
    return s;
}
struct Loop{
    gs::PushGovernorControl control;Fg user,current;std::vector<gs::PushSimRecord> trace;
    explicit Loop(Fg request,gs::PushControlConfig config={}):control(config),user(request),current(request){}
    gs::PushControlTick tick(const gs::PushControlSample& s){
        gs::PushSimRecord r;r.nowMs=s.nowMs;r.before=gs::PushGpuLevel{current.tier,current.multiplier};r.jitterMs=s.networkJitterMs;r.sourceFps=s.sourceFps;
        r.latencyMs=s.latencyP95Ms;r.latencyKnown=s.latencyP95Ms>=0;
        const auto t=control.update(current,s);r.beforeIndex=t.startIndex;r.tick=t;if(t.apply)current=t.target;
        r.after=gs::PushGpuLevel{current.tier,current.multiplier};r.changed=!(r.after.multiplier==r.before.multiplier&&r.after.tier==r.before.tier);
        r.userRequest=user;trace.push_back(r);return t;
    }
};
// The bound the review asked for, stated: on the stall traces the governor's mean output frame rate is at most
// this far below no governor's (model fps), it makes at most this many changes per 6 h, and is reduced at most
// this share of the time.
constexpr double kOutputBoundFps=0.5;
constexpr unsigned kChangeBound=12;
constexpr double kReducedBoundPercent=1.0;
}

TEST(governor_stall_peak_rise_is_a_stall_seen_now){
    namespace gs=governor_stall_tests::gs;
    gs::PushStallFromPeak p;
    CHECK(p.update(-1)==-1);CHECK(p.update(std::numeric_limits<double>::quiet_NaN())==-1);
    CHECK(p.update(3)==3);      // first reading: what the window holds counts as seen (the cautious side)
    CHECK(p.update(3)==0);      // unchanged: nothing new
    CHECK(p.update(40)==40);    // a frame 40 ms off its cadence arrived since the last tick
    CHECK(p.update(40)==0);CHECK(p.update(25)==0);   // held, then a bucket replaced: nothing new is known
    CHECK(p.update(30)==30);    // a rise from the lower held value: a new stall
    p.reset();CHECK(p.update(30)==30);
}

TEST(governor_stall_review_stall_traces_cost_nothing_bounded){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    // Review 4, finding 1: stall-only, and Wi-Fi stalls (20-80 ms every 2-8 s, mean jitter 2.5 ms) whether the
    // interarrival peak sees them or not. 6 h of model time each (the tool runs 24 h).
    for(int kind:{3,9,10}){
        const auto s=gs::pushRunReviewTrace(kind,6*3600);
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        const auto f=gs::pushSimTraceFacts(s);
        if(!(s.outputGovernor>=s.outputNone-t::kOutputBoundFps))throw std::runtime_error(s.scenario+": output below no governor");
        CHECK(s.changes<=t::kChangeBound);
        CHECK(f.reducedPercent<=t::kReducedBoundPercent);
        CHECK(f.lastHourReducedPercent==0);   // not left reduced
        CHECK(s.overBudgetGovernor<=s.overBudgetNone);
    }
}

TEST(governor_stall_gpu_evidence_stands_beside_link_stalls){
    namespace gs=governor_stall_tests::gs;
    // The same Wi-Fi stalls on a slow GPU: the GPU's own trouble is still answered (model: over budget nearly all
    // day without the governor).
    const auto s=gs::pushRunReviewTrace(11,2*3600);
    if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
    CHECK(s.changes>=1);CHECK(s.overBudgetGovernor<s.overBudgetNone/2);
    const auto slow=gs::pushRunReviewTrace(4,2*3600);
    CHECK(slow.firstViolation.empty());CHECK(slow.changes>=1&&slow.changes<=6);CHECK(slow.overBudgetGovernor<slow.overBudgetNone/10);
}

TEST(governor_stall_review_traces_keep_every_invariant_and_rebuild_spacing){
    namespace gs=governor_stall_tests::gs;
    for(int kind=0;kind<gs::kPushSimReviewTraceCount;++kind){
        const auto s=gs::pushRunReviewTrace(kind,4*3600);
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        CHECK(gs::pushSimTraceFacts(s).maxRebuildsPerMinute<=1);
    }
}

TEST(governor_stall_a_decode_wait_longer_than_the_gpu_work_is_not_stepped){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    // 25 ms decode-queue waits with the GPU at 0.3 + generation (18 ms of work): no frame generation level can
    // cause or cure that wait.
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<120;++s){
        const auto tick=loop.tick(t::at(s,loop.current,2,0.3,25));
        CHECK(!tick.apply);
        if(s>5){CHECK(!tick.gpuExplains);CHECK(!tick.gpuTrouble);}
    }
    CHECK(loop.trace.back().tick.reason=="trouble-not-gpu");
    // The same wait with the GPU busy enough to explain it (0.6 x 33 + 8 = 28 ms of work) is answered.
    t::Loop busy(t::Fg{t::kFlow,3,2});
    bool stepped=false;
    for(int s=0;s<20;++s)stepped=busy.tick(t::at(s,busy.current,2,busy.current.multiplier==3?0.6:0.3,busy.current.multiplier==3?25.0:1.0)).apply||stepped;
    CHECK(stepped);
    CHECK(gs::pushGovernorInvariantViolation(loop.trace,{}).empty());CHECK(gs::pushGovernorInvariantViolation(busy.trace,{}).empty());
}

TEST(governor_stall_latency_longer_than_all_the_gpu_work_is_not_stepped){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    // 40 ms unplanned with 18 ms of GPU work and no jitter: even generation off cannot bring it under the budget.
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<60;++s)CHECK(!loop.tick(t::at(s,loop.current,40)).apply);
    CHECK(loop.trace.back().tick.reason=="over-budget-not-gpu");
}

TEST(governor_stall_a_seen_stall_makes_the_network_burst_and_holds){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    // A 30 ms stall in the interarrival peak with a quiet mean jitter: decode waits and latency in the next
    // stallLingerMs are the link's.
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<60;++s){
        const bool stall=s%2==0;
        const auto tick=loop.tick(t::at(s,loop.current,stall?16.0:3.0,0.6,stall?25.0:1.0,0.5,stall?30.0:0.0));
        CHECK(!tick.apply);
        if(s>0)CHECK(tick.networkBurst);
    }
    // A slow GPU under the same stalls is still seen: load 1.2 is the GPU's whatever the link does.
    t::Loop gpu(t::Fg{t::kFlow,3,2});
    bool stepped=false;
    for(int s=0;s<30;++s){const bool stall=s%2==0;stepped=gpu.tick(t::at(s,gpu.current,3,gpu.current.multiplier==3?1.0:0.3,stall?25.0:1.0,0.5,stall?30.0:0.0)).apply||stepped;}
    CHECK(stepped);CHECK(gs::pushGovernorInvariantViolation(gpu.trace,{}).empty());
}

TEST(governor_stall_trouble_that_returns_at_the_lower_level_withdraws_the_credit){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    // 15 ms bursts for 4 s every 15 s at EVERY level: the first step looks credited (the burst ended during its
    // evaluation); the same burst at the level it went to withdraws that credit; after two such steps the governor
    // stands down with the request in force.
    t::Loop loop(t::Fg{t::kFlow,3,2});
    unsigned withdrawn=0;
    for(int s=0;s<1500;++s){
        const auto tick=loop.tick(t::at(s,loop.current,s%15<4?15.0:2.0));
        if(tick.apply&&tick.reason=="rollback-trouble-returned")++withdrawn;
    }
    CHECK(withdrawn>=1);CHECK(loop.control.stoodDown());
    CHECK(loop.current==(t::Fg{t::kFlow,3,2}));
    CHECK(gs::pushGovernorInvariantViolation(loop.trace,{}).empty());
    unsigned reduced=0;for(std::size_t i=600;i<loop.trace.size();++i)reduced+=loop.trace[i].after.multiplier!=3?1u:0u;
    CHECK(reduced==0);
}

TEST(governor_stall_standing_down_restores_the_request){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    gs::PushControlConfig c;c.tierIntervalMs=0;
    t::Loop loop(t::Fg{t::kFlow,3,2},c);
    bool wasReduced=false;int restoredAt=-1;
    for(int s=0;s<1500;++s){
        // A GPU overload at 3x for the first 40 s (a credited protection step to 2x), then 15 ms unplanned at every
        // level below the request: each latency step from 2x is useless.
        const bool overload=s<40&&loop.current.multiplier==3;
        const double unplanned=loop.current.multiplier==3?2.0:15.0;
        const auto tick=loop.tick(t::at(s,loop.current,unplanned,overload?0.95:0.3));
        wasReduced=wasReduced||loop.current.multiplier!=3;
        if(tick.apply&&tick.cause==gs::PushControlCause::StandDown&&restoredAt<0){restoredAt=s;CHECK(tick.target==(t::Fg{t::kFlow,3,2}));}
    }
    CHECK(wasReduced);CHECK(loop.control.stoodDown());CHECK(restoredAt>0);
    CHECK(loop.current==(t::Fg{t::kFlow,3,2}));CHECK(loop.trace.back().tick.reason=="stood-down");
    CHECK(gs::pushGovernorInvariantViolation(loop.trace,c).empty());
}

TEST(governor_stall_checker_catches_rebuilds_and_changes_after_standing_down){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    const double hold=gs::pushPlannedDelayMs(2,30);
    gs::PushSimRecord a;a.nowMs=0;a.sourceFps=30;a.latencyMs=hold+2;a.jitterMs=0.3;a.userRequest=t::Fg{t::kFlow,2,2};a.before=gs::PushGpuLevel{2,2};a.after=a.before;
    // Generation off then on again 10 s later: two rebuilds closer than the tier interval (finding 2).
    gs::PushSimRecord off=a;off.nowMs=1000;off.changed=true;off.after=gs::PushGpuLevel{0,1};off.latencyMs=hold+20;off.tick.cause=gs::PushControlCause::Latency;
    gs::PushSimRecord on=a;on.nowMs=11000;on.changed=true;on.before=off.after;on.after=a.before;on.latencyMs=2;on.tick.cause=gs::PushControlCause::Rollback;
    on.tick.reason="rollback-target-not-improved";on.tick.apply=true;off.tick.apply=true;
    const auto v=gs::pushGovernorInvariantViolation({a,off,on},{});
    CHECK(v.find("graph rebuilds closer")!=std::string::npos);
    gs::PushControlConfig spaced;spaced.tierIntervalMs=5000;
    CHECK(gs::pushGovernorInvariantViolation({a,off,on},spaced).empty());
    // Three useless verdicts in one stream (the stand-down after two was skipped).
    std::vector<gs::PushSimRecord> trace{a};
    for(int i=0;i<3;++i){
        auto d=off;d.nowMs=100000+200000*i;trace.push_back(d);
        auto r=on;r.nowMs=d.nowMs+100000;trace.push_back(r);
    }
    gs::PushControlConfig noSpacing;noSpacing.tierIntervalMs=0;
    const auto w=gs::pushGovernorInvariantViolation(trace,noSpacing);
    if(w.find("standing down")==std::string::npos&&w.find("rolled back as useless")==std::string::npos)throw std::runtime_error("checker said: "+w);
    CHECK(w.find("standing down")!=std::string::npos||w.find("rolled back as useless")!=std::string::npos);
    // A stand-down that does not put the request back.
    gs::PushSimRecord bad=a;bad.nowMs=900000;bad.changed=true;bad.before=gs::PushGpuLevel{0,1};bad.after=gs::PushGpuLevel{0,2};bad.tick.cause=gs::PushControlCause::StandDown;
    std::vector<gs::PushSimRecord> two{a,trace[1],trace[2],trace[3],trace[4],bad};
    CHECK(gs::pushGovernorInvariantViolation(two,noSpacing).find("restor")!=std::string::npos);
    auto good=bad;good.after=gs::PushGpuLevel{2,2};good.before=gs::PushGpuLevel{2,2};good.before.multiplier=1;
    CHECK(gs::pushGovernorInvariantViolation({a,trace[1],trace[2],trace[3],trace[4],good},noSpacing).empty());
}

// Review 5 (20 November 2026). LINUX MODEL traces (PushGovernorSim.h pushRunReview5Trace), 24 h each.
namespace governor_stall_tests {
// Blocker 1's bound: over-budget seconds no worse than the Wi-Fi stalls alone cost (what review 4's code reached on
// these traces: the reviewer's 17,256 = its stall-only count), plus this margin per day.
constexpr unsigned kStallMarginS=60;
// Blocker 2's bounds on a steady GPU at scale 2.0: at most this many changes a day, and over-budget seconds within
// this margin of generation off all day.
constexpr unsigned kLoopChangeBound=30;
constexpr unsigned kLoopMarginS=60;
}

TEST(governor_review5_gpu_overload_on_a_stalling_link_is_answered){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    const unsigned stallsAlone=gs::pushRunReviewTrace(10).overBudgetNone;   // the same stall schedule, GPU healthy
    for(int kind=0;kind<=5;++kind){
        const auto s=gs::pushRunReview5Trace(kind);
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        if(!(s.overBudgetGovernor<=stallsAlone+t::kStallMarginS))
            throw std::runtime_error(s.scenario+": "+std::to_string(s.overBudgetGovernor)+" s over budget, bound "+std::to_string(stallsAlone+t::kStallMarginS));
        CHECK(s.overBudgetGovernor<s.overBudgetNone/2);
        CHECK(s.changes>=1&&s.changes<=4);
    }
}

TEST(governor_review5_a_failed_return_is_not_repeated){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    const auto s=gs::pushRunReview5Trace(6);
    if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
    // Generation off all day (the user's own choice of 1x on the same world).
    gs::PushSimWorld w;w.gpuScale=2.0;
    const auto off=gs::pushRunGovernorSim("off",86400,[w](int){return w;},7u,{},t::Fg{t::kFlow,1,0});
    if(!(s.changes<=t::kLoopChangeBound&&s.overBudgetGovernor<=off.overBudgetGovernor+t::kLoopMarginS))
        throw std::runtime_error(std::to_string(s.changes)+" changes, "+std::to_string(s.overBudgetGovernor)+" s over budget (off: "+std::to_string(off.overBudgetGovernor)+")");
    // No return into over-budget latency in a loop: after the first failed return, at most one return an hour.
    unsigned ups=0;
    for(const auto& r:s.trace)if(r.changed&&r.nowMs>=3600000&&gs::pushLevelWithin(r.before,r.after))++ups;
    CHECK(ups<=23);
    // A new stream (or a user change) forgets it: one failed return per stream, the request put back at each.
    const auto streams=gs::pushRunReview5Trace(11);
    if(!streams.firstViolation.empty())throw std::runtime_error(streams.scenario+": "+streams.firstViolation);
    CHECK(streams.changes<=t::kLoopChangeBound+4*4);CHECK(streams.overBudgetGovernor<=off.overBudgetGovernor+4*t::kLoopMarginS);
}

TEST(governor_review5_credit_is_withdrawn_when_the_trouble_returns_below){
    namespace gs=governor_stall_tests::gs;
    // A decoder stall that stops whenever a level changes and returns 9, 10 or 11 min later: the step is withdrawn
    // when it returns at the lower level, and after two such steps the governor stands down at the request.
    for(int kind:{7,8,9}){
        const auto s=gs::pushRunReview5Trace(kind);
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        const auto f=gs::pushSimTraceFacts(s);
        if(!(f.reducedPercent<3.0))throw std::runtime_error(s.scenario+": reduced "+std::to_string(f.reducedPercent)+" %");
        CHECK(s.rollbacks>=1);CHECK(f.lastHourReducedPercent==0);CHECK(s.overBudgetGovernor<=s.overBudgetNone);
        CHECK(s.trace.back().tick.reason=="stood-down");
    }
}

TEST(governor_review5_stalls_episodes_and_new_streams){
    namespace gs=governor_stall_tests::gs;
    // Link stalls short enough for the GPU work to "explain": only the stall detector keeps the governor still.
    const auto runs=gs::pushRunReview5Trace(10);
    if(!runs.firstViolation.empty())throw std::runtime_error(runs.scenario+": "+runs.firstViolation);
    CHECK(runs.changes==0);
    // Latency episodes that end on their own: no step from that level is credited; two useless steps, then none.
    const auto quiet=gs::pushRunReview5Trace(13);
    if(!quiet.firstViolation.empty())throw std::runtime_error(quiet.scenario+": "+quiet.firstViolation);
    CHECK(quiet.changes<=6);
    for(const auto& r:quiet.trace)CHECK(!r.tick.credited);
    // New streams while reduced: the request is put back before each (the checker's restore rule).
    for(int kind:{11,12}){
        const auto s=gs::pushRunReview5Trace(kind);
        if(!s.firstViolation.empty())throw std::runtime_error(s.scenario+": "+s.firstViolation);
        unsigned starts=0;
        for(const auto& r:s.trace)if(r.restoreDue){++starts;CHECK(r.before.multiplier==3&&r.before.tier==2);}
        CHECK(starts>=3);
    }
}

TEST(governor_review5_checker_catches_the_new_rules){
    namespace t=governor_stall_tests;namespace gs=t::gs;
    const double hold=gs::pushPlannedDelayMs(3,30);
    gs::PushSimRecord a;a.sourceFps=30;a.latencyMs=hold+2;a.jitterMs=0.3;a.userRequest=t::Fg{t::kFlow,3,2};a.before=gs::PushGpuLevel{2,3};a.after=a.before;
    a.load=0.4;a.latencyRequestMs=hold+2;
    // A step down for latency 2 s after the peak window rose by a stall.
    std::vector<gs::PushSimRecord> tr;
    for(int i=0;i<10;++i){auto r=a;r.nowMs=i*1000;r.stallPeakMs=i==7?30.0:5.0;tr.push_back(r);}
    auto d=a;d.nowMs=9000;d.stallPeakMs=30;d.latencyMs=hold+25;d.changed=true;d.after=gs::PushGpuLevel{2,2};d.tick.cause=gs::PushControlCause::Latency;d.tick.apply=true;
    tr.back()=d;
    CHECK(gs::pushGovernorInvariantViolation(tr,{}).find("link stall")!=std::string::npos);
    // A new stream that starts at the governor's level, not the request.
    auto ns=a;ns.nowMs=20000;ns.restoreDue=true;ns.newStream=true;ns.priorRequest=gs::PushGpuLevel{2,3};ns.before=gs::PushGpuLevel{2,2};ns.after=ns.before;
    ns.userRequest=t::Fg{t::kFlow,2,2};
    CHECK(gs::pushGovernorInvariantViolation({a,ns},{}).find("put back")!=std::string::npos);
    // Two hours below the request with no latency gain.
    std::vector<gs::PushSimRecord> held;
    for(int i=0;i<7300;++i){auto r=a;r.nowMs=i*1000LL;r.before=r.after=gs::PushGpuLevel{2,2};r.latencyMs=gs::pushPlannedDelayMs(2,30)+12;r.latencyRequestMs=hold+12;held.push_back(r);}
    CHECK(gs::pushGovernorInvariantViolation(held,{}).find("no latency gain")!=std::string::npos);
    for(auto& r:held)r.latencyMs=gs::pushPlannedDelayMs(2,30)+6;   // 6 ms better: a gain
    CHECK(gs::pushGovernorInvariantViolation(held,{}).empty());
}

// Phase 2 (SmartLink link state): its own file, included here so CoreTests.cpp keeps one line for this stream.
#include "GovernorLinkTests.inc"
