// SPDX-License-Identifier: GPL-3.0-only
// Snapshot glue for the latency governor and the GPU budget (veyra/engine/PushGovernorFeed.h).
// Included by CoreTests.cpp directly above main(). Every number below is made up, not a measurement.
#include "veyra/engine/PushGovernorFeed.h"
#include <optional>
#include <string>
#include <string_view>
namespace governor_feed_tests {
namespace ge=veyra::engine;
constexpr std::int64_t kFeedSec=10000000;
// A healthy 30 fps PS5 stream at 2X: one second of counters per call.
ge::PushFeedInput healthy(std::int64_t second,std::uint64_t session=1){
    ge::PushFeedInput in;
    in.now100ns=second*kFeedSec;in.sessionId=session;
    in.sourceFps=30;in.outputFps=60;in.enhancementP95Ms=6;in.fgBatchP95Ms=2;in.decodeQueueP95Ms=1;in.decodeP95Ms=3;
    in.jitterMs=0.3;in.lossPercent=0;in.queueDepth=0;in.gpuUtilizationPercent=40;
    in.countersKnown=true;in.presented=static_cast<std::uint64_t>(second)*60u;in.pairsCandidate=static_cast<std::uint64_t>(second)*30u;
    in.framesLostKnown=true;in.multiplier=2;in.power=ge::PushPowerSource::Mains;
    return in;
}
ge::GovernorKnobs knobs(){return ge::pushGovernorKnobs(ge::PushGovernorSwitches{},2,0);}
}

TEST(governor_feed_switch_values){
    using governor_feed_tests::ge::PushExperimentMode;namespace ge=governor_feed_tests::ge;
    CHECK(ge::pushExperimentMode(std::nullopt,true)==PushExperimentMode::Off);
    CHECK(ge::pushExperimentMode(std::string_view(""),true)==PushExperimentMode::Off);
    CHECK(ge::pushExperimentMode(std::string_view("0"),true)==PushExperimentMode::Off);
    CHECK(ge::pushExperimentMode(std::string_view("1"),true)==PushExperimentMode::Observe);
    CHECK(ge::pushExperimentMode(std::string_view("Apply"),true)==PushExperimentMode::Apply);
    CHECK(ge::pushExperimentMode(std::string_view("apply"),false)==PushExperimentMode::Observe); // the governor never applies
    CHECK(ge::pushExperimentMode(std::string_view("applyx"),true)==PushExperimentMode::Observe);
}

TEST(governor_feed_first_sample_has_no_counter_deltas){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    const auto out=feed.update(t::healthy(1),t::knobs());
    CHECK((out.budgetUnknown&b::kMissedDeadlines)!=0);CHECK((out.budgetUnknown&b::kSkippedPairs)!=0);
    CHECK(out.budget.skippedPairShare<0);CHECK(out.budget.presented==0&&out.budget.missedDeadlines==0);
    CHECK((out.governorUnknown&b::kFramesLost)!=0);
    CHECK(out.governorValid);
}

TEST(governor_feed_counter_deltas_become_shares){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    (void)feed.update(t::healthy(1),t::knobs());
    auto in=t::healthy(2);in.expired=3;in.pairsSkipped=15;in.framesLost=2;
    const auto out=feed.update(in,t::knobs());
    CHECK(out.budget.presented==60u&&out.budget.missedDeadlines==3u);
    CHECK(out.budget.skippedPairShare>0.49&&out.budget.skippedPairShare<0.51);
    CHECK(out.governor.framesLostPerSecond>1.99&&out.governor.framesLostPerSecond<2.01);
    CHECK((out.budgetUnknown&(b::kMissedDeadlines|b::kSkippedPairs|b::kSourceFps|b::kGpuFrame|b::kFgCost|b::kDecodeQueue))==0);
    // Thermal state has no source: always listed.
    CHECK((out.budgetUnknown&b::kThermal)!=0);
    // Intervals, and the per-frame cost without the FG batch (6 - 2).
    CHECK(out.budget.sourceIntervalMs>33.3&&out.budget.sourceIntervalMs<33.4);
    CHECK(out.budget.outputIntervalMs>16.6&&out.budget.outputIntervalMs<16.7);
    CHECK(out.budget.gpuFrameMs>3.99&&out.budget.gpuFrameMs<4.01&&out.budget.fgMs>1.99&&out.budget.fgMs<2.01);
    CHECK(out.governor.gpuHeadroomPercent>59.9&&out.governor.gpuHeadroomPercent<60.1);
}

TEST(governor_feed_new_session_or_clock_step_restarts_deltas){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    (void)feed.update(t::healthy(5),t::knobs());
    auto other=t::healthy(6,2);other.presented=10;   // a new session's counters start again
    CHECK((feed.update(other,t::knobs()).budgetUnknown&b::kMissedDeadlines)!=0);
    CHECK((feed.update(t::healthy(3,2),t::knobs()).budgetUnknown&b::kMissedDeadlines)!=0); // clock went back
    auto back=t::healthy(4,2);back.presented=0;     // counters went back
    CHECK((feed.update(back,t::knobs()).budgetUnknown&b::kMissedDeadlines)!=0);
}

TEST(governor_feed_generation_off_costs_nothing_for_fg){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    auto in=t::healthy(1);in.multiplier=1;in.fgBatchP95Ms=-1;
    const auto out=feed.update(in,t::knobs());
    CHECK(out.budget.fgMs==0.0);CHECK(out.budget.gpuFrameMs>5.99&&out.budget.gpuFrameMs<6.01);
    CHECK((out.budgetUnknown&b::kFgCost)==0);
}

TEST(governor_feed_unknown_inputs_are_neutral_for_the_governor){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;using G=t::ge::PushLatencyGovernor;
    t::ge::PushGovernorFeed feed;
    t::ge::PushFeedInput in;in.now100ns=governor_feed_tests::kFeedSec;in.sourceFps=30;
    const auto k=t::ge::pushGovernorKnobs(t::ge::PushGovernorSwitches{},1,0);
    const auto out=feed.update(in,k);
    CHECK(out.governorValid);
    CHECK((out.governorUnknown&(b::kJitter|b::kLoss|b::kFramesLost|b::kGpuHeadroom|b::kQueueDepth|b::kScheduling|b::kPresentLate))==
          (b::kJitter|b::kLoss|b::kFramesLost|b::kGpuHeadroom|b::kQueueDepth|b::kScheduling|b::kPresentLate));
    // Between every "bad" and "clear" threshold, and never calm.
    CHECK(out.governor.gpuHeadroomPercent>=G::kHeadroomBad&&out.governor.gpuHeadroomPercent<=G::kHeadroomClear);
    CHECK(out.governor.presentLateMs<G::kPresentLateBad&&out.governor.presentLateMs>=G::kPresentLateClear);
    CHECK(out.governor.queueDepth<G::kQueueBad&&out.governor.queueDepth>=G::kQueueClear);
    CHECK(!(out.governor.networkJitterMs<G::kCalmJitterMs));
    CHECK(out.governor.cpuSchedulingDelayMs==static_cast<double>(k.controllerPollMs));
    // Fed for a minute of unknowns, the governor recommends nothing.
    G governor;bool any=false;
    for(std::int64_t s=1;s<=60;++s){
        in.now100ns=s*t::kFeedSec;
        const auto o=feed.update(in,k);
        if(governor.update(o.nowMs,o.governor,k))any=true;
    }
    CHECK(!any);
    // Without a source rate it is not fed at all.
    in.sourceFps=-1;
    CHECK(!feed.update(in,k).governorValid);
}

TEST(governor_feed_knobs_from_switches){
    namespace ge=governor_feed_tests::ge;
    ge::PushGovernorSwitches s;
    CHECK(ge::pushGovernorKnobs(s,1,0).reorder==ge::ReorderProfile::Long);
    s.lanProfile=true;CHECK(ge::pushGovernorKnobs(s,1,0).reorder==ge::ReorderProfile::Short);
    s.reorderAdaptive=true;CHECK(ge::pushGovernorKnobs(s,1,0).reorder==ge::ReorderProfile::Adaptive);
    s.backlog=true;s.padPollMs=2;
    const auto k=ge::pushGovernorKnobs(s,3,2);
    CHECK(k.backlog&&k.controllerPollMs==2&&k.fgStep==2&&k.qualityTier==2);
    CHECK(ge::pushGovernorKnobs(s,1,0).fgStep==0);
}

TEST(governor_feed_recommendation_text){
    namespace ge=governor_feed_tests::ge;
    CHECK(ge::pushGovernorRecommendationText(std::nullopt)=="none");
    ge::GovernorRecommendation r;r.knob=ge::GovernorKnob::FrameGeneration;r.from=2;r.to=1;r.reason="x";
    const std::string text=ge::pushGovernorRecommendationText(r);
    CHECK(text.find("frame-generation-step 2->1")==0);CHECK(text.find("effect=lower")!=std::string::npos);
    CHECK(ge::pushFeedUnavailable(0)=="none");
    CHECK(ge::pushFeedUnavailable(ge::feed_input::kThermal|ge::feed_input::kPresentLate)=="thermal,presentLate");
}

TEST(governor_feed_log_gate_every_ten_seconds_or_on_change){
    governor_feed_tests::ge::PushFeedLogGate gate;
    CHECK(gate.due(0,"a"));CHECK(!gate.due(5*governor_feed_tests::kFeedSec,"a"));CHECK(gate.due(6*governor_feed_tests::kFeedSec,"b"));
    CHECK(!gate.due(15*governor_feed_tests::kFeedSec,"b"));CHECK(gate.due(16*governor_feed_tests::kFeedSec,"b"));
}

// --- the GPU budget driver ----------------------------------------------------------------------------------
namespace governor_feed_tests {
using Req=ge::PushFgRequest;
// One second of a GPU that cannot keep up (load 1.0 of a 33 ms source interval) or of a calm one.
ge::PushGpuBudgetSample busy(std::int64_t s){ge::PushGpuBudgetSample x;x.now100ns=s*kFeedSec;x.sourceIntervalMs=33.3;x.outputIntervalMs=8.3;x.gpuFrameMs=25;x.fgMs=8;x.decodeQueueDelayMs=1;x.presented=120;x.power=ge::PushPowerSource::Mains;return x;}
ge::PushGpuBudgetSample calm(std::int64_t s){auto x=busy(s);x.gpuFrameMs=4;x.fgMs=2;return x;}
}

TEST(governor_feed_driver_observe_never_asks_for_a_change){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,4,2};
    bool apply=false;std::size_t lowest=0;
    for(std::int64_t s=1;s<=60;++s){
        const auto step=driver.update(user,t::busy(s),false);
        apply=apply||step.apply;
        if(step.decision.index>lowest)lowest=step.decision.index;
        CHECK(!(step.restarted&&s>1));   // the settings never moved, so it never starts again
    }
    CHECK(!apply);CHECK(lowest>0);   // it did recommend lower levels
}

TEST(governor_feed_driver_apply_steps_down_then_recovers_to_the_request_only){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,3,1};
    t::Req current=user;
    unsigned changes=0;
    for(std::int64_t s=1;s<=40;++s){
        const auto step=driver.update(current,t::busy(s),true);
        if(step.apply){
            ++changes;
            // One step at a time, never above the request.
            CHECK(step.target.multiplier<=user.multiplier);CHECK(step.target.tier<=user.tier);
            current=step.target;
        }
    }
    CHECK(changes>=1);CHECK(!(current==user));
    const t::Req lowest=current;
    // Healthy for long enough: back up step by step, never past the request. (It stays below the levels whose
    // last measured load predicts trouble: that is the budget's own rule, PushGpuBudget.h.)
    unsigned ups=0;
    for(std::int64_t s=41;s<=2000;++s){
        const auto step=driver.update(current,t::calm(s),true);
        if(step.apply){++ups;CHECK(step.target.multiplier<=user.multiplier);CHECK(step.target.tier<=user.tier);current=step.target;}
    }
    CHECK(ups>=1);CHECK(current.multiplier>lowest.multiplier);
}

TEST(governor_feed_driver_user_change_becomes_the_new_request){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,4,2};
    t::Req current=user;
    for(std::int64_t s=1;s<=10;++s){const auto step=driver.update(current,t::busy(s),true);if(step.apply)current=step.target;}
    CHECK(!(current==user));
    // The user picks 2X Fast themselves: that is the new ceiling.
    const t::Req mine{t::ge::PushFgMethod::PushFlow,2,0};
    const auto step=driver.update(mine,t::calm(11),true);
    CHECK(step.restarted);CHECK(step.request==mine);CHECK(!step.apply);
    for(std::int64_t s=12;s<=2000;++s){const auto x=driver.update(mine,t::calm(s),true);CHECK(!x.apply);}
}

TEST(governor_feed_driver_ignores_a_tier_on_methods_without_tiers){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    // DLSS with a stale PushFlow tier of 2 in the settings: one tier only, so nothing to "correct".
    const t::Req user{t::ge::PushFgMethod::Dlss,4,2};
    const auto step=driver.update(user,t::calm(1),true);
    CHECK(!step.apply);CHECK(step.request.tier==0);
    // Its first reduction is a multiplier step (4 -> 3), not a tier.
    t::Req current=user;bool stepped=false;
    for(std::int64_t s=2;s<=20&&!stepped;++s){
        const auto x=driver.update(current,t::busy(s),true);
        if(x.apply){stepped=true;CHECK(x.target.multiplier==3);CHECK(x.target.tier==0);}
    }
    CHECK(stepped);
}

TEST(governor_feed_driver_new_source_returns_to_the_request){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,3,2};
    t::Req current=user;
    for(std::int64_t s=1;s<=10;++s){const auto step=driver.update(current,t::busy(s),true);if(step.apply)current=step.target;}
    CHECK(!(current==user));
    driver.newSource();
    const auto step=driver.update(current,t::calm(11),true);
    CHECK(step.restarted);CHECK(step.apply);CHECK(step.target==user);CHECK(step.request==user);
    // Observe mode after a new source asks for nothing.
    t::ge::PushGpuBudgetDriver watcher;
    (void)watcher.update(user,t::calm(1),false);watcher.newSource();
    const auto seen=watcher.update(user,t::calm(2),false);
    CHECK(seen.restarted);CHECK(!seen.apply);
    // A user change while paused wins over the remembered request.
    driver.newSource();
    const t::Req mine{t::ge::PushFgMethod::PushFlow,2,1};
    const auto other=driver.update(mine,t::calm(12),true);
    CHECK(other.request==mine);CHECK(!other.apply);
}

TEST(governor_feed_driver_restore_gives_back_the_request_only_while_ours){
    namespace t=governor_feed_tests;
    t::ge::PushGpuBudgetDriver driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,3,2};
    CHECK(!driver.pendingRestore(user));   // nothing started
    t::Req current=user;
    (void)driver.update(current,t::calm(1),true);
    CHECK(!driver.pendingRestore(current));   // at the request: nothing to give back
    for(std::int64_t s=2;s<=10;++s){const auto step=driver.update(current,t::busy(s),true);if(step.apply)current=step.target;}
    CHECK(!(current==user));
    const auto back=driver.pendingRestore(current);
    CHECK(back.has_value());CHECK(*back==user);
    // Someone else changed the settings: not ours to restore.
    CHECK(!driver.pendingRestore(t::Req{t::ge::PushFgMethod::PushFlow,2,1}));
    driver.restored();
    CHECK(!driver.pendingRestore(user));
    // Restored and stopped: the next stream starts at the request without a change.
    driver.newSource();
    const auto next=driver.update(user,t::calm(11),true);
    CHECK(!next.apply);CHECK(next.request==user);
}

TEST(governor_feed_driver_refused_level_waits_a_minute){
    namespace t=governor_feed_tests;using D=t::ge::PushGpuBudgetDriver;
    D driver;
    const t::Req user{t::ge::PushFgMethod::PushFlow,3,2};
    std::int64_t asked=0;
    for(std::int64_t s=1;s<=10&&!asked;++s)if(driver.update(user,t::busy(s),true).apply)asked=s;
    CHECK(asked>0);
    // The setter refused: the settings are still the user's. That is no new request, and no new attempt
    // for 60 s.
    const auto refused=driver.update(user,t::busy(asked+1),true);
    CHECK(refused.refused);CHECK(!refused.restarted);CHECK(refused.request==user);CHECK(!refused.apply);
    unsigned again=0;
    for(std::int64_t s=asked+2;s<asked+1+60;++s){const auto x=driver.update(user,t::busy(s),true);if(x.apply)++again;CHECK(!x.apply||!x.held);}
    CHECK(again==0);
    CHECK(driver.update(user,t::busy(asked+1+60),true).apply);
}

TEST(governor_feed_apply_needs_the_decode_queue){
    namespace ge=governor_feed_tests::ge;
    CHECK(ge::pushGpuBudgetCanApply(0));
    CHECK(ge::pushGpuBudgetCanApply(ge::feed_input::kThermal|ge::feed_input::kPower));
    CHECK(!ge::pushGpuBudgetCanApply(ge::feed_input::kDecodeQueue));
}

// --- limits a mutation would remove ---------------------------------------------------------------------------
TEST(governor_feed_driver_never_corrects_a_request_the_ladder_cannot_hold){
    namespace t=governor_feed_tests;
    // PushFlow has no 5X and DLSS no 5X: the ladder's top is 4X, but level 0 is the user's own choice.
    for(const auto method:{t::ge::PushFgMethod::PushFlow,t::ge::PushFgMethod::Dlss}){
        t::ge::PushGpuBudgetDriver driver;
        const t::Req user{method,5,0};
        for(std::int64_t s=1;s<=30;++s)CHECK(!driver.update(user,t::calm(s),true).apply);
    }
}

TEST(governor_feed_loss_over_100_percent_is_not_a_measurement){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    auto in=t::healthy(1);in.lossPercent=150;
    const auto out=feed.update(in,t::knobs());
    CHECK((out.governorUnknown&b::kLoss)!=0);CHECK(out.governor.lossPercent==t::ge::PushLatencyGovernor::kCalmLossPercent);
    in=t::healthy(2);in.lossPercent=100;
    const auto full=feed.update(in,t::knobs());
    CHECK((full.governorUnknown&b::kLoss)==0);CHECK(full.governor.lossPercent==100.0);
}

TEST(governor_feed_utilization_limits){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    auto in=t::healthy(1);in.gpuUtilizationPercent=100;
    const auto busy=feed.update(in,t::knobs());
    CHECK((busy.governorUnknown&b::kGpuHeadroom)==0);CHECK(busy.governor.gpuHeadroomPercent==0.0);
    in=t::healthy(2);in.gpuUtilizationPercent=150;
    const auto wrong=feed.update(in,t::knobs());
    CHECK((wrong.governorUnknown&b::kGpuHeadroom)!=0);
    CHECK(wrong.governor.gpuHeadroomPercent>t::ge::PushLatencyGovernor::kHeadroomBad);
}

TEST(governor_feed_more_skipped_than_seen_is_not_a_share){
    namespace t=governor_feed_tests;namespace b=t::ge::feed_input;
    t::ge::PushGovernorFeed feed;
    (void)feed.update(t::healthy(1),t::knobs());
    auto in=t::healthy(2);in.pairsSkipped=100;   // 100 skipped, 30 seen
    const auto out=feed.update(in,t::knobs());
    CHECK((out.budgetUnknown&b::kSkippedPairs)!=0);CHECK(out.budget.skippedPairShare<0);
    in=t::healthy(3);in.pairsSkipped=100+30;     // all 30 seen were skipped
    const auto all=feed.update(in,t::knobs());
    CHECK((all.budgetUnknown&b::kSkippedPairs)==0);CHECK(all.budget.skippedPairShare==1.0);
}

TEST(governor_feed_unknown_frames_lost_never_calls_the_link_clean){
    namespace t=governor_feed_tests;using G=t::ge::PushLatencyGovernor;
    // A calm link by jitter and loss, on the long reorder wait, but the frames lost are not measured: the
    // governor must not recommend the short wait on that.
    const auto k=t::ge::pushGovernorKnobs(t::ge::PushGovernorSwitches{},1,0);
    CHECK(k.reorder==t::ge::ReorderProfile::Long);
    t::ge::PushGovernorFeed feed;G governor;bool reorder=false;
    for(std::int64_t s=1;s<=60;++s){
        auto in=t::healthy(s);in.framesLostKnown=false;in.multiplier=1;
        const auto o=feed.update(in,k);
        CHECK(o.governor.framesLostPerSecond>0);
        const auto r=governor.update(o.nowMs,o.governor,k);
        if(r&&r->knob==t::ge::GovernorKnob::Reorder)reorder=true;
    }
    CHECK(!reorder);
    // The same link with frames lost measured as 0 does get the recommendation (the test above is not vacuous).
    t::ge::PushGovernorFeed measured;G control;bool recommended=false;
    for(std::int64_t s=1;s<=60;++s){
        auto in=t::healthy(s);in.multiplier=1;
        const auto o=measured.update(in,k);
        const auto r=control.update(o.nowMs,o.governor,k);
        if(r&&r->knob==t::ge::GovernorKnob::Reorder)recommended=true;
    }
    CHECK(recommended);
}
