// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushCausal.h (stream "ownfg", step 4): the fully causal schedule's ladder (exits to ONE
// predicted frame only, hysteresis, stop), its learner start, admission group, timestamps and evidence. Values are examples.
#include "veyra/engine/PushCausalSchedule.h"
#include "veyra/pipeline/PushCausal.h"
#include <string>

namespace causal_tests {
using namespace veyra::pipeline;

PushCausalVerdict good() { return {true, true, true, 1.0, 1.0}; }
PushCausalVerdict badP2() { return {true, false, true, 1.0, 1.0}; }

TEST(causal_off_by_default_and_asks_only_when_chosen_and_mode_asks) {
    PushCausalLadder l;
    CHECK(!l.isChosen() && !l.ask(true));
    l.chosen(true);
    CHECK(l.ask(true) && !l.ask(false));          // the one-predicted schedule decides whether the mode runs
    CHECK(l.level() == PushCausalLevel::Causal);
    l.chosen(false);
    CHECK(!l.ask(true));
}

TEST(causal_unusable_exit_goes_to_one_predicted_not_interpolation) {
    PushCausalLadder l; l.chosen(true);
    // 44 bad of 60: stays (a short burst of unusable predictions is not sustained)
    for (unsigned i = 0; i < 60; ++i) { l.pair(true, false); l.verdict(i < 44 ? badP2() : good(), 5.56); }
    CHECK(l.level() == PushCausalLevel::Causal);
    for (unsigned i = 0; i < 60; ++i) { l.pair(true, false); l.verdict(badP2(), 5.56); }
    CHECK(l.level() == PushCausalLevel::OnePredicted);    // ONE slot back, the only lower level the ladder knows
    CHECK(l.lastExit() == PushCausalExit::Unusable && l.exits(PushCausalExit::Unusable) == 1);
    CHECK(!l.ask(true));
    CHECK(l.returnNeed() == PushCausalLadder::kReturnPairs && l.returnHave() == 0);
}

TEST(causal_p1_alone_does_not_decide_at_3x_but_does_at_2x) {
    PushCausalLadder l; l.chosen(true);
    for (unsigned i = 0; i < 120; ++i) { l.pair(true, false); l.verdict({false, true, true, 1.0, 1.0}, 5.56); }
    CHECK(l.level() == PushCausalLevel::Causal);
    CHECK(l.p1UnusableShare() > 0.99 && l.p2UnusableShare() < 0.01);    // reported separately
    PushCausalLadder two; two.chosen(true);
    for (unsigned i = 0; i < 60; ++i) { two.pair(true, false); two.verdict({false, true, false, 1.0, -1.0}, 8.33); }
    CHECK(two.level() == PushCausalLevel::OnePredicted);
}

TEST(causal_late_and_dear_exits) {
    PushCausalLadder l; l.chosen(true);
    l.outcome(true); l.outcome(true); l.outcome(false);
    CHECK(l.level() == PushCausalLevel::Causal);           // two late drops of 60: allowed
    l.outcome(true);
    CHECK(l.level() == PushCausalLevel::OnePredicted && l.lastExit() == PushCausalExit::Late);
    PushCausalLadder d; d.chosen(true);
    for (unsigned i = 0; i < 19; ++i) d.verdict({true, true, true, 6.0, 6.0}, 11.12);
    CHECK(d.level() == PushCausalLevel::Causal);           // too few samples
    d.verdict({true, true, true, 6.0, 6.0}, 11.12);
    CHECK(d.level() == PushCausalLevel::OnePredicted && d.lastExit() == PushCausalExit::Dear);
    CHECK(d.lastCostP95() > 11.0);
    PushCausalLadder cheap; cheap.chosen(true);
    for (unsigned i = 0; i < 40; ++i) cheap.verdict({true, true, true, 2.0, 2.0}, 11.12);
    CHECK(cheap.level() == PushCausalLevel::Causal);
}

TEST(causal_return_hysteresis_and_stop) {
    PushCausalLadder l; l.chosen(true);
    auto fail = [&] { for (unsigned i = 0; i < 3; ++i) l.outcome(true); };
    fail();
    CHECK(l.level() == PushCausalLevel::OnePredicted);
    for (unsigned i = 0; i < 299; ++i) l.pair(false, true);
    CHECK(l.level() == PushCausalLevel::OnePredicted && l.returnHave() == 299);
    l.pair(false, false);                                  // a pair that did not run predicted does not count
    CHECK(l.level() == PushCausalLevel::OnePredicted);
    l.pair(false, true);
    CHECK(l.level() == PushCausalLevel::Causal && l.ask(true));
    // fails again quickly: the next return needs twice as long
    for (unsigned i = 0; i < 10; ++i) l.pair(true, false);
    fail();
    CHECK(l.returnNeed() == 2 * PushCausalLadder::kReturnPairs);
    for (unsigned i = 0; i < 600; ++i) l.pair(false, true);
    CHECK(l.level() == PushCausalLevel::Causal);
    fail();                                                // third exit of the same reason: stays for the playback
    CHECK(l.stoppedFor() == PushCausalExit::Late);
    for (unsigned i = 0; i < 5000; ++i) l.pair(false, true);
    CHECK(l.level() == PushCausalLevel::OnePredicted && !l.ask(true));
    l.chosen(false); l.chosen(true);                       // switched off and on: tries afresh
    CHECK(l.ask(true) && l.stoppedFor() == PushCausalExit::None && l.entries() >= 1);
}

TEST(causal_return_after_a_long_stay_is_not_doubled) {
    PushCausalLadder l; l.chosen(true);
    for (unsigned i = 0; i < 3; ++i) l.outcome(true);
    for (unsigned i = 0; i < 300; ++i) l.pair(false, true);
    for (unsigned i = 0; i < 400; ++i) l.pair(true, false);  // stayed well beyond kQuickFailPairs
    for (unsigned i = 0; i < 3; ++i) l.outcome(true);
    CHECK(l.returnNeed() == PushCausalLadder::kReturnPairs);
}

TEST(causal_phase_starts_one_slot_below_the_predicted_phase) {
    const std::int64_t interval = 333333, slot = interval / 3, legacy = 500000, allowance = 100000;
    const auto learned = pushCausalPhaseArgs(interval, 3, legacy, allowance, 300000);
    CHECK(learned.start == 300000 - slot);
    CHECK(learned.start + learned.allowance == legacy + allowance - 2 * slot);
    const auto none = pushCausalPhaseArgs(interval, 3, legacy, allowance, 0);
    CHECK(none.start == legacy - 2 * slot);
    const auto two = pushCausalPhaseArgs(interval, 2, legacy, allowance, 300000);   // 2X: one slot of T/2, no predicted phase
    CHECK(two.start == legacy - interval / 2 && two.start + two.allowance == legacy + allowance - interval / 2);
    const auto clamped = pushCausalPhaseArgs(interval, 3, 50000, 0, 0);
    CHECK(clamped.start == 0 && clamped.allowance == 0);
}

TEST(causal_pair_refusal_reasons) {
    PushPredictEligibility e;
    e.requested = PushPairSchedule::Causal; e.vfg = false; e.multiplier = 3; e.historyValid = true; e.formatSupported = true;
    e.passReady = true; e.vfgRefused = true; e.motionReady = true;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::Ok);            // VFG's refusal and "not VFG" do not apply
    CHECK(pushCausalPairRefusal(e, false) == PushPredictRefusal::NotVfg);
    e.multiplier = 2;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::Ok);            // Smoothness Mode at 2X: [B, P(1/2)]
    e.multiplier = 4;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::NotThreeX);
    e.multiplier = 3; e.historyValid = false;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::NoHistory);
    e.historyValid = true; e.motionReady = false;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::MotionNotReady);
    e.motionReady = true; e.requested = PushPairSchedule::OnePredicted;
    CHECK(pushCausalPairRefusal(e, true) == PushPredictRefusal::SwitchOff);
}

TEST(causal_timestamps_steps_and_admission_group) {
    CHECK(pushCausalPts(0, 333333, 1, 3) == 333333 + 111111);
    CHECK(pushCausalPts(0, 333333, 2, 3) == 333333 + 222222);
    CHECK(pushCausalPts(0, 333334, 1, 2) == 333334 + 166667);
    CHECK(pushCausalStep(1, 3) > 0.333f && pushCausalStep(1, 3) < 0.334f && pushCausalStep(1, 2) == 0.5f);
    CHECK(pushCausalAdmissionGroupMs(4.0, 2.5, 7) == 4.0);
    CHECK(pushCausalAdmissionGroupMs(4.0, 2.5, 8) == 2.5);
    CHECK(pushCausalAdmissionGroupMs(4.0, -1.0, 30) == 4.0);
}

TEST(causal_frame_usable_rule_and_evidence_text) {
    PushWarpCounters c{};
    c.still = 100; c.blend = 80; c.hole = 20;
    CHECK(pushCausalFrameUsable(c));
    c.hole = 40;
    CHECK(!pushCausalFrameUsable(c));
    PushCausalLadder l;
    CHECK(pushCausalText(l, false, "causal").empty());     // never chosen: nothing added to the logs
    l.chosen(true);
    l.pair(true, false);
    l.verdict({true, false, true, 1.2, 1.5}, 5.56);
    const std::string t = pushCausalText(l, true, "causal");
    CHECK(t.find(" schedule=causal ") != std::string::npos);
    CHECK(t.find("causalP1Unusable=0.000") != std::string::npos);
    CHECK(t.find("causalP2Unusable=1.000") != std::string::npos);
    CHECK(t.find("causalExitsUnusable=0 causalExitsLate=0 causalExitsDear=0") != std::string::npos);
    CHECK(t.find(',') == std::string::npos);                // the log readers split at commas
    PushWarpCostWindow p1, p2;
    PushWarpStageMs ms{}; ms.fill(-1.0); ms[static_cast<unsigned>(PushWarpStage::Total)] = 1.5;
    p1.add(0, ms);
    const std::string cost = pushCausalCostText(p1, p2);
    CHECK(cost.find(" causalP1GpuMs=1.50/1.50 causalP2GpuMs=-1.00/-1.00 causalGpuSamples=1/0") != std::string::npos);
    CHECK(t.find("causalPairs=1 ") != std::string::npos);
}

struct FakeOnePredicted { int late = 0, shown = 0; void predictedOutcome(bool dropped) { dropped ? ++late : ++shown; } };

TEST(causal_run_routes_outcomes_and_logs_changes) {
    veyra::engine::PushCausalRun run;
    FakeOnePredicted one;
    CHECK(!run.ask(false, true, false, true));                       // not chosen: nothing asked
    CHECK(run.ask(true, true, false, true) && run.ask(true, false, true, true));   // 3X asked, or 2X with the mode on
    CHECK(!run.ask(true, true, false, false));                       // the graph cannot run it
    veyra::engine::pushCausalOutcome(one, run, false, true);
    CHECK(one.late == 1);                                            // not a causal frame: the one-predicted schedule's
    for (int i = 0; i < 3; ++i) veyra::engine::pushCausalOutcome(one, run, true, true);
    CHECK(one.late == 1 && run.ladder.level() == PushCausalLevel::OnePredicted);   // never the other schedule's exits
    const auto text = run.pair(true, false, true);                   // the change is logged with the next pair
    CHECK(text.find("left for ONE predicted frame") != std::string::npos && text.find("reason=late") != std::string::npos);
    CHECK(run.pair(true, false, true).empty());
    CHECK(!run.ask(true, true, false, true));                        // on the one-predicted level
    const auto e = run.evidence("onepredicted", true);
    CHECK(e.find(" schedule=onepredicted causalChosen=1 causalLevel=onepredicted") == 0);
    CHECK(pushCausalBudgetMs(0, 333333, 3) > 22.2 && pushCausalBudgetMs(0, 333333, 3) < 22.3);
    CHECK(pushCausalBudgetMs(0, 333333, 2) > 16.6 && pushCausalBudgetMs(0, 333333, 2) < 16.7);
    CHECK(pushCausalBudgetMs(0, 333333, 1) == 0.0);
}

TEST(causal_run_phase_restarts_on_entry) {
    veyra::engine::PushCausalRun run;
    run.ask(true, true, false, true);
    const auto p = run.select(100000000, 333333, 3, 500000, 100000, 300000);
    CHECK(p == 300000 - 111111);                                     // no samples: the start
    run.pair(true, true, false);
    for (int i = 0; i < 10; ++i) run.latency.observe(100000000 + i, 90000);
    const auto q = run.select(100000001, 333333, 3, 500000, 100000, 300000);
    CHECK(q < p);                                                    // learned from the causal pairs' readiness
}

}  // namespace causal_tests
