// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushLowDelay.h (Package 15, stream "lowdelay"): when the one-predicted schedule is asked for, the
// phase each schedule starts from, and admission. Values are examples (100 ns units, a 29.97 FPS game).
#include "veyra/engine/PushLowDelay.h"
#include "veyra/pipeline/PushPredict.h"
#include "veyra/engine/PushSmoothness.h"
#include "veyra/engine/PushFgHealth.h"

namespace lowdelay_tests {
using namespace veyra::engine;

inline PushLowDelayPair calmPair() {
    PushLowDelayPair p;
    p.wanted = true; p.eligible = true;
    return p;
}

// Package 15's rule (PushLowDelayInterrupts::Strict), kept for comparison; the engine runs Tolerant since Package 16.
TEST(lowdelay_enters_after_calm_pairs_and_leaves_at_once) {
    PushLowDelaySchedule s;
    s.setInterruptRule(PushLowDelayInterrupts::Strict);
    for (unsigned i = 0; i + 1 < PushLowDelaySchedule::kCalmPairs; ++i) { s.pair(calmPair()); CHECK(!s.request()); }
    s.pair(calmPair());
    CHECK(s.request());
    CHECK(s.entries() == 1);
    auto p = calmPair(); p.ranPredicted = true; s.pair(p);
    CHECK(s.request());
    CHECK(s.predictedPairs() == 1);
    p = calmPair(); p.skipped = true; s.pair(p);
    CHECK(!s.request());
    CHECK(s.exits() == 1);
    CHECK(s.exits(PushLowDelayExit::Skipped) == 1);
    CHECK(s.switches() == 2);
    // Calm restarts from zero, and a stay of one pair doubles the run needed.
    CHECK(s.calmNeeded() == 2 * PushLowDelaySchedule::kCalmPairs);
    for (unsigned i = 0; i + 1 < 2 * PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    CHECK(!s.request());
    s.pair(calmPair());
    CHECK(s.request());
}

TEST(lowdelay_exit_reasons_in_order) {
    const auto reasonFor = [](PushLowDelayPair p) {
        PushLowDelaySchedule s;
        s.setInterruptRule(PushLowDelayInterrupts::Strict);   // one pair decides the exit (Package 15's rule)
        for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
        s.pair(p);
        for (unsigned i = static_cast<unsigned>(PushLowDelayExit::Off); i < static_cast<unsigned>(PushLowDelayExit::Count); ++i)
            if (s.exits(static_cast<PushLowDelayExit>(i))) return static_cast<PushLowDelayExit>(i);
        return PushLowDelayExit::None;
    };
    auto p = calmPair(); p.wanted = false; p.skipped = true; CHECK(reasonFor(p) == PushLowDelayExit::Off);
    p = calmPair(); p.eligible = false; p.held = true;      CHECK(reasonFor(p) == PushLowDelayExit::Ineligible);
    p = calmPair(); p.held = true; p.skipped = true;        CHECK(reasonFor(p) == PushLowDelayExit::Held);
    p = calmPair(); p.skipped = true; p.refused = true;     CHECK(reasonFor(p) == PushLowDelayExit::Skipped);
    p = calmPair(); p.refused = true;                       CHECK(reasonFor(p) == PushLowDelayExit::Refused);
    CHECK(reasonFor(calmPair()) == PushLowDelayExit::None);
}

TEST(lowdelay_unusable_frame_leaves_and_backs_off) {
    PushLowDelaySchedule s;
    s.setInterruptRule(PushLowDelayInterrupts::Strict);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    CHECK(s.request());
    s.usable(true);
    CHECK(s.request());
    s.usable(false);
    CHECK(!s.request());
    CHECK(s.exits(PushLowDelayExit::Unusable) == 1);
    // Left soon after entering: the next entry needs twice the calm run.
    CHECK(s.calmNeeded() == 2 * PushLowDelaySchedule::kCalmPairs);
    for (unsigned i = 0; i + 1 < 2 * PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    CHECK(!s.request());
    s.pair(calmPair());
    CHECK(s.request());
    // Repeated short stays: the run doubles up to the cap.
    for (int k = 0; k < 10; ++k) {
        s.usable(false);
        const unsigned need = s.calmNeeded();
        for (unsigned i = 0; i < need; ++i) s.pair(calmPair());
    }
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kMaxCalmPairs);
    CHECK(s.request());
    // A settled stay brings it back to kCalmPairs.
    for (unsigned i = 0; i < PushLowDelaySchedule::kSettledPairs; ++i) s.pair(calmPair());
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
    // A long stay that ends does not back off.
    auto p = calmPair(); p.refused = true; s.pair(p);
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
    // Off: never asks, and nothing counted as an exit when it never entered.
    PushLowDelaySchedule off;
    p = calmPair(); p.wanted = false;
    for (int i = 0; i < 100; ++i) off.pair(p);
    CHECK(!off.request());
    CHECK(off.switches() == 0);
    // Turning the row off is not a back-off.
    PushLowDelaySchedule user;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) user.pair(calmPair());
    user.pair(p);
    CHECK(user.exits(PushLowDelayExit::Off) == 1);
    CHECK(user.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
}

TEST(lowdelay_phase_args_move_one_slot_each_way) {
    constexpr std::int64_t interval = 333667, slot = interval / 3, legacy = 450000, allow = 20000;
    constexpr std::int64_t now = 100000000;
    // Predicted starts one slot before the interpolated phase learned; never above Package 14's ceiling - T/3.
    auto a = pushLowDelayPhaseArgs(true, now, interval, legacy, allow, 460000, 0, 0);
    CHECK(a.start == 460000 - slot && a.start + a.allowance == legacy + allow - slot);
    a = pushLowDelayPhaseArgs(true, now, interval, legacy, allow, 0, 0, 0);
    CHECK(a.start == legacy - slot && a.allowance == allow);
    a = pushLowDelayPhaseArgs(true, now, interval, legacy, allow, 900000, 0, 0);
    CHECK(a.start == legacy + allow - slot && a.allowance == 0);
    // Interpolate: Package 14's legacy and allowance exactly unless the predicted schedule ran within the last second.
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 420000, 300000, 0);
    CHECK(a.start == legacy && a.allowance == allow);
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 0, 300000, now - 5000000);
    CHECK(a.start == 300000 + slot && a.start + a.allowance == legacy + allow);   // one slot after P; ceiling unchanged
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 420000, 250000, now - 5000000);
    CHECK(a.start == 420000 && a.start + a.allowance == legacy + allow);          // what interpolation needed before
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 0, 400000, now - 5000000);
    CHECK(a.start == legacy + allow && a.allowance == 0);                          // capped at the ceiling
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 420000, 400000, now - 10000000);
    CHECK(a.start == legacy && a.allowance == allow);
    a = pushLowDelayPhaseArgs(false, now, interval, legacy, allow, 420000, 0, now - 1);
    CHECK(a.start == legacy);
}

TEST(lowdelay_admission_checks_i_with_fg1_and_the_group_with_p) {
    // I due at 1 000 000, P two slots later.
    constexpr std::int64_t dI = 1000000, dP = dI + 2 * 111222;
    CHECK(pushLowDelayAdmissionDeadline(dI, dP, 14.0, 8.0) == dI + 60000);   // 6 ms after I (P's cost)
    CHECK(pushLowDelayAdmissionDeadline(dI, dP, 40.0, 8.0) == dP);           // never past P's deadline
    CHECK(pushLowDelayAdmissionDeadline(dI, dP, 7.0, 8.0) == dI);            // Fg1 above the group P95: I's own
    CHECK(pushLowDelayAdmissionDeadline(dI, dP, 0.0, 8.0) == dI);            // no estimate: stricter
    CHECK(pushLowDelayAdmissionDeadline(dI, dP, 14.0, 0.0) == dI);
}

TEST(lowdelay_state_for_the_row) {
    using veyra::pipeline::PushPredictRefusal;
    const auto none = PushLowDelaySuspend::None;
    // (wanted, paused, card + VFG + 3X, graph built asking for the pass, static refusal, schedule active)
    CHECK(pushLowDelayStateFor(false, none, true, true, PushPredictRefusal::Ok, true) == PushLowDelayState::Idle);
    CHECK(pushLowDelayStateFor(true, none, false, true, PushPredictRefusal::Ok, true) == PushLowDelayState::NotHere);
    CHECK(pushLowDelayStateFor(true, none, true, false, PushPredictRefusal::SwitchOff, false) == PushLowDelayState::Starting);   // the rebuild is on its way
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::SwitchOff, false) == PushLowDelayState::Unavailable); // asked, not created
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::PassUnavailable, false) == PushLowDelayState::Unavailable);
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::HudLockOrExclusion, false) == PushLowDelayState::NotHere);   // a reason
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::UnsupportedFormat, false) == PushLowDelayState::NotHere);
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::Ok, false) == PushLowDelayState::Calm);
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::Ok, true) == PushLowDelayState::Active);
    CHECK(pushLowDelayStateFor(true, none, true, false, PushPredictRefusal::Ok, true) == PushLowDelayState::Active);   // a kit-built pass counts
    CHECK(pushLowDelayStateFor(true, PushLowDelaySuspend::HudDetection, true, true, PushPredictRefusal::Ok, false) == PushLowDelayState::Suspended);
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::VfgRefused, false) == PushLowDelayState::Unavailable);   // review 2, SHOULD 9
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::NotThreeX, false) == PushLowDelayState::NotHere);
    CHECK(pushLowDelayStateFor(true, none, true, true, PushPredictRefusal::SmoothnessMode, false) == PushLowDelayState::NotHere);
    CHECK(std::string(pushLowDelayStateName(PushLowDelayState::Starting)) == "starting");
    CHECK(std::string(pushLowDelayStateName(PushLowDelayState::Unavailable)) == "unavailable");
    CHECK(std::string(pushLowDelayStateName(PushLowDelayState::NotHere)) == "not-here");
    CHECK(std::string(pushLowDelayStateName(PushLowDelayState::Suspended)) == "suspended");
}

// Scheduling review C: the mode pauses while a measurement or a comparison clip needs the ordinary frames.
TEST(lowdelay_pauses_for_measurements_and_comparison_clips) {
    CHECK(pushLowDelaySuspendFor(false, false, false, false) == PushLowDelaySuspend::None);
    CHECK(pushLowDelaySuspendFor(true, true, true, true) == PushLowDelaySuspend::QualityTest);
    CHECK(pushLowDelaySuspendFor(false, true, false, false) == PushLowDelaySuspend::Benchmark);
    CHECK(pushLowDelaySuspendFor(false, false, true, false) == PushLowDelaySuspend::HudDetection);
    CHECK(pushLowDelaySuspendFor(false, false, false, true) == PushLowDelaySuspend::ComparisonClip);
    CHECK(std::string(pushLowDelaySuspendName(PushLowDelaySuspend::ComparisonClip)) == "comparison-clip");
    // Paused = not wanted for the schedule: it leaves as for "off", without back-off.
    PushLowDelaySchedule s;
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    PushLowDelayPair paused; paused.wanted = false;
    s.pair(paused);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Off) == 1 && s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
}

// The row changes the graph's shape only where the predicted-frame pass can exist (pushPredictCreate: VFG, 3X or more).
TEST(lowdelay_row_shapes_the_graph_only_with_vfg_at_3x) {
    CHECK(pushLowDelayShapesGraph(true, true, 3));
    CHECK(pushLowDelayShapesGraph(true, true, 4));
    CHECK(!pushLowDelayShapesGraph(false, true, 3));   // off: no pass
    CHECK(!pushLowDelayShapesGraph(true, true, 2));
    CHECK(!pushLowDelayShapesGraph(true, true, 1));
    CHECK(!pushLowDelayShapesGraph(true, false, 3));
    // Wherever the row shapes the graph, the graph creates the pass for it (same conditions, other inputs fixed).
    for (unsigned m = 1; m <= 6; ++m)
        for (int vfg = 0; vfg < 2; ++vfg)
            CHECK(pushLowDelayShapesGraph(true, vfg != 0, m) ==
                  veyra::pipeline::pushPredictCreate(veyra::pipeline::pushPredictAsked(veyra::pipeline::PushPairSchedule::Interpolate, true),
                                                     true, vfg != 0, false, false, m, true));
}

TEST(lowdelay_resets_only_at_a_switch) {
    auto r = pushLowDelayResets(false, false); CHECK(!r.interpolate && !r.predicted);   // off: never
    r = pushLowDelayResets(true, false);       CHECK(!r.interpolate && r.predicted);    // entering
    r = pushLowDelayResets(true, true);        CHECK(r.interpolate && !r.predicted);    // staying: the idle learner stays empty
    r = pushLowDelayResets(false, true);       CHECK(r.interpolate && !r.predicted);    // leaving
}

TEST(lowdelay_wanted_from_the_row_or_the_kit) {
    PushLowDelayShared shared;
    CHECK(!pushLowDelayWanted(false, shared));
    CHECK(pushLowDelayWanted(true, shared));             // the kit's switch, set before PushFPS started
    shared.wanted.store(true);
    CHECK(pushLowDelayWanted(false, shared));            // the row alone
    CHECK(pushLowDelayWanted(true, shared));
    shared.wanted.store(false);
    CHECK(!pushLowDelayWanted(false, shared));           // the row off: at once, no restart
}

// Scheduling review item A: the stay-in policy. An unusable prediction no longer leaves; a sustained failure (45 of the
// last 60) does, once, and then a long calm run (300 pairs, doubled if it fails again soon) is needed to return.
TEST(lowdelay_stay_in_keeps_the_schedule_on_isolated_bad_predictions) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    CHECK(s.active());
    PushLowDelayPair ran = calm; ran.ranPredicted = true;
    for (int i = 0; i < 200; ++i) { s.pair(ran); s.usable(i % 4 != 0); }   // 25 % unusable, spread out
    CHECK(s.active() && s.exits() == 0 && s.unusableShown() == 50);
    for (int i = 0; i < 44; ++i) { s.pair(ran); s.usable(false); }        // 44 bad + 4 of the earlier window...
    // ...the window of 60 now holds the last 44 bad and 16 earlier verdicts (4 of them bad): 48 >= 45 -> left by now.
    CHECK(!s.active() && s.exits(PushLowDelayExit::Unusable) == 1);
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    for (unsigned i = 0; i + 1 < PushLowDelaySchedule::kStayInCalmPairs; ++i) s.pair(calm);
    CHECK(!s.active());
    s.pair(calm);
    CHECK(s.active() && s.entries() == 2);
    // Failing again soon: twice the wait.
    for (int i = 0; i < 45; ++i) { s.pair(ran); s.usable(false); }
    CHECK(!s.active() && s.calmNeeded() == 2 * PushLowDelaySchedule::kStayInCalmPairs);
    // Under Package 15's rule a skipped pair leaves at once (Package 16's Tolerant rule: lowdelay_tolerant_* below).
    PushLowDelaySchedule t;
    t.setPolicy(PushLowDelayPolicy::StayIn);
    t.setInterruptRule(PushLowDelayInterrupts::Strict);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) t.pair(calm);
    PushLowDelayPair skipped = calm; skipped.skipped = true;
    t.pair(skipped);
    CHECK(!t.active() && t.exits(PushLowDelayExit::Skipped) == 1);
    t.reset();
    CHECK(t.policy() == PushLowDelayPolicy::StayIn && t.interruptRule() == PushLowDelayInterrupts::Strict);   // reset keeps both
}

TEST(lowdelay_engine_policy_is_stay_in_unless_the_rollback_switch) {
    CHECK(pushLowDelayPolicyFor(false) == PushLowDelayPolicy::StayIn);
    CHECK(pushLowDelayPolicyFor(true) == PushLowDelayPolicy::LeaveOnUnusable);
    CHECK(std::string(pushLowDelayPolicyName(PushLowDelayPolicy::StayIn)) == "stay-in");
    CHECK(std::string(pushLowDelayPolicyName(PushLowDelayPolicy::LeaveOnUnusable)) == "leave-on-unusable");
}

TEST(lowdelay_leave_policy_is_unchanged_by_default) {
    PushLowDelaySchedule s;
    CHECK(s.policy() == PushLowDelayPolicy::LeaveOnUnusable);
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    s.usable(false);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Unusable) == 1 && s.calmNeeded() == 2 * PushLowDelaySchedule::kCalmPairs);
}

// Review 2, SHOULD 2: steadiness first. More than 2 of the last 60 predicted frames dropped late leaves; so do three
// one-second windows in a row below 98 % on the grid while the mode ran the whole window. Both with the long back-off.
TEST(lowdelay_leaves_when_predicted_frames_come_late) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    CHECK(s.active());
    for (int i = 0; i < 100; ++i) s.predictedOutcome(i % 40 == 0);   // 1 late in 40: at most 2 in any 60
    CHECK(s.active());
    s.predictedOutcome(true); s.predictedOutcome(true);                  // now 3 in the last 60
    CHECK(!s.active() && s.exits(PushLowDelayExit::Late) == 1);
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    s.predictedOutcome(true);                                            // not in the mode: ignored
    CHECK(s.exits() == 1);
    CHECK(s.predictedLateSeen() == 3 + 3 && s.predictedOutcomesSeen() == 103 && s.gridExits() == 0);   // the log counts every outcome
}

TEST(lowdelay_leaves_after_three_windows_off_the_grid) {
    // Review 1 SHOULD 10: against the MEDIAN of interpolation's own whole windows minus a margin.
    PushLowDelaySchedule s;
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    s.gridWindow(96.0, false, true);
    CHECK(!s.interpolatedOnGridKnown());                                // fewer than kBaselineMin windows: no grid exit
    s.gridWindow(95.0, false, true); s.gridWindow(96.0, false, true);  // his steadiness: 95-96 %
    CHECK(s.interpolatedOnGridKnown() && s.interpolatedOnGridBaseline() == 96.0);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    CHECK(s.active());
    // His numbers while predicted: 95-96 % -> no exit (a fixed 98 % left every 3 s).
    for (int i = 0; i < 30; ++i) s.gridWindow(i % 2 ? 95.2 : 96.1, true, false);
    CHECK(s.active() && s.exits() == 0);
    // Three whole windows below the median minus the margin leave; a good window or a partial one restarts the count.
    s.gridWindow(94.5, true, false); s.gridWindow(94.0, true, false);
    s.gridWindow(95.5, true, false);                                    // within the margin: restarts
    s.gridWindow(94.0, true, false); s.gridWindow(93.0, true, false);
    s.gridWindow(80.0, false, false);                                   // the mode did not run the whole window: restarts
    s.gridWindow(-1.0, true, false);                                    // no frame-generation output: restarts
    s.gridWindow(94.9, true, false); s.gridWindow(94.9, true, false);
    CHECK(s.active());
    s.gridWindow(94.9, true, false);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Late) == 1 && s.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    CHECK(s.gridExits() == 1);                                          // the log tells the grid's "late" from dropped frames'
    CHECK(std::string(pushLowDelayExitName(PushLowDelayExit::Late)) == "late");
    // Median of an even count; at most kBaselineWindows kept.
    PushLowDelaySchedule m;
    for (double v : {90.0, 99.0, 97.0, 98.0}) m.gridWindow(v, false, true);
    CHECK(m.interpolatedOnGridBaseline() == 97.5);
    for (int i = 0; i < 20; ++i) m.gridWindow(99.0, false, true);
    CHECK(m.interpolatedOnGridBaseline() == 99.0);
    // Entry does not wait for the baseline; without it, windows far off the grid do not leave.
    PushLowDelaySchedule u;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) u.pair(calm);
    CHECK(u.active());
    for (int i = 0; i < 5; ++i) u.gridWindow(50.0, true, false);
    CHECK(u.active() && u.exits() == 0);
}

TEST(lowdelay_exits_text_lists_every_reason) {
    PushLowDelaySchedule s;
    CHECK(pushLowDelayExitsText(s) == "off:0,ineligible:0,refused:0,unusable:0,skipped:0,held:0,late:0,dear:0");
}

// Package 16, item 1 (the owner's log of 24 Nov: `lowDelay state=not-here refusal=smoothness-mode predictedPairs=0`):
// Smoothness Mode is no longer a refusal. The engine reports not-vfg off a capture card with VFG, else the graph's own.
TEST(lowdelay_smoothness_mode_is_no_longer_a_refusal) {
    using veyra::pipeline::PushPredictRefusal;
    for (unsigned r = 0; r <= static_cast<unsigned>(PushPredictRefusal::SmoothnessMode); ++r) {
        const auto graph = static_cast<PushPredictRefusal>(r);
        CHECK(pushLowDelayStaticRefusal(true, graph) == graph);
        CHECK(pushLowDelayStaticRefusal(false, graph) == PushPredictRefusal::NotVfg);
    }
    // The graph's static refusal for a 3X VFG pair with Smoothness Mode's target set: Ok (nothing in it reads the target).
    veyra::pipeline::PushPredictEligibility e;
    e.requested = veyra::pipeline::PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.formatSupported = true; e.passReady = true;
    CHECK(pushLowDelayStaticRefusal(true, veyra::pipeline::pushPredictStaticRefusal(e)) == PushPredictRefusal::Ok);
    e.multiplier = 2;   // Smoothness Mode stepped down to 2X: the pair's own multiplier is the reason
    CHECK(pushLowDelayStaticRefusal(true, veyra::pipeline::pushPredictStaticRefusal(e)) == PushPredictRefusal::NotThreeX);
}

// Package 16, item 1: below 3X the schedule pauses (no back-off) and resumes only after kMultiplierResumePairs calm pairs
// at 3X; a multiplier that changes back and forth faster than that never makes the schedule switch at its pace.
TEST(lowdelay_multiplier_pause_resumes_after_a_calm_second_without_flapping) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    CHECK(s.active() && s.entries() == 1);
    PushLowDelayPair twoX = calmPair(); twoX.eligible = false; twoX.multiplierPause = true;
    for (int i = 0; i < 40; ++i) s.pair(twoX);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Ineligible) == 1 && s.multiplierPauses() == 1);
    CHECK(s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);   // a pause is not a back-off
    for (unsigned i = 0; i + 1 < PushLowDelaySchedule::kMultiplierResumePairs; ++i) { s.pair(calmPair()); CHECK(!s.active()); }
    s.pair(calmPair());
    CHECK(s.active() && s.entries() == 2 && s.resumeNeeded() == 0);
    // Flapping: 20 pairs at 3X, 1 at 2X, again and again: one exit, no re-entry, so two switches in all.
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 20; ++i) s.pair(calmPair());
        s.pair(twoX);
    }
    CHECK(!s.active() && s.entries() == 2 && s.exits() == 2 && s.multiplierPauses() == 51);
    // Any other ineligibility keeps the ordinary calm run (a settings change, not a multiplier).
    PushLowDelaySchedule t;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) t.pair(calmPair());
    PushLowDelayPair other = calmPair(); other.eligible = false;
    t.pair(other);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) t.pair(calmPair());
    CHECK(t.active() && t.entries() == 2 && t.multiplierPauses() == 0);
}

// Package 16, item 1: Smoothness Mode's own decisions (the real class) driving the schedule. Target 90 at 30 FPS: 3X;
// sustained admission rejections (one pair in 7) step it down to 2X, headroom brings it back after its own hold and lock-out. The
// schedule pauses once and resumes once, a second after 3X is back.
TEST(lowdelay_with_smoothness_mode_pauses_and_resumes_once_per_step) {
    PushSmoothnessMode mode;
    PushSmoothCostModel costs;
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    const PushFgMask offered = (1u << 2) | (1u << 3) | (1u << 4);
    std::uint64_t admitted = 0, rejected = 0;
    std::int64_t now = 1000000000;
    const std::int64_t T = 333667;
    unsigned stepDownAt = 0, resumedAt = 0, threeXAgainAt = 0;
    for (unsigned k = 0; k < 30 * 60; ++k, now += T) {
        const bool trouble = k >= 300 && k < 420 && k % 7 == 0;   // 4 s with one pair in 7 rejected (above 10 %, below 7 of 30)
        (trouble ? rejected : admitted) += 1;
        costs.preferPredicted(s.active());
        PushSmoothInput in;
        in.targetFps = 90; in.contentFps = 29.97; in.refreshHz = 179.952; in.ceiling = 3; in.offered = offered;
        in.baseMs = 4.7; in.costs = &costs; in.blitMs = 0.1; in.admitted = admitted; in.rejected = rejected; in.now100ns = now;
        const auto d = mode.update(in);
        const unsigned m = d.multiplier >= 2 ? d.multiplier : 3u;
        if (m == 3) { if (s.active()) costs.recordPredicted(2, 6.0); else costs.record(2, 4.4); }
        else costs.record(1, 2.2);
        if (m == 2 && !stepDownAt) stepDownAt = k;
        if (stepDownAt && m == 3 && !threeXAgainAt) threeXAgainAt = k;
        PushLowDelayPair p = calmPair();
        p.eligible = m == 3; p.multiplierPause = m != 3; p.ranPredicted = s.request() && m == 3;
        p.skipped = trouble && m == 3;
        const bool was = s.active();
        s.pair(p);
        if (!was && s.active() && threeXAgainAt && !resumedAt) resumedAt = k;
    }
    CHECK(mode.stepDowns() == 1 && mode.stepUps() == 1);
    CHECK(stepDownAt > 300 && stepDownAt < 420 && threeXAgainAt > stepDownAt);
    CHECK(s.multiplierPauses() == 1);
    CHECK(resumedAt >= threeXAgainAt + PushLowDelaySchedule::kMultiplierResumePairs - 1);
    CHECK(s.active());
    // Package 16, item 3: the skipped pairs of the trouble did not end the mode; the step down paused it once.
    CHECK(s.exits() == 1 && s.exits(PushLowDelayExit::Ineligible) == 1 && s.entries() == 2);
}

// Package 16, item 3 (forensics F1): the decision code alone, every pair eligible and usable, i.i.d. skipped pairs (a
// fixed LCG). Share = pairs that ran predicted. Package 15's Strict rule collapses at 2 %; the Tolerant rule does not.
inline double predictedShareWithSkips(PushLowDelayInterrupts rule, double skipShare, unsigned pairs = 18000, std::uint32_t seed = 12345) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    s.setInterruptRule(rule);
    std::uint32_t x = seed;
    unsigned ran = 0;
    for (unsigned k = 0; k < pairs; ++k) {
        x = x * 1664525u + 1013904223u;
        PushLowDelayPair p = calmPair();
        p.skipped = double(x >> 8) / double(1u << 24) < skipShare;
        p.ranPredicted = s.request() && !p.skipped;
        p.refused = s.request() && p.skipped;   // as the engine: an asked pair that did not run predicted
        ran += p.ranPredicted;
        s.pair(p);
    }
    return double(ran) / double(pairs);
}
TEST(lowdelay_tolerant_rule_runs_through_2_percent_skips) {
    for (const std::uint32_t seed : {1u, 7u, 12345u}) {
        CHECK(predictedShareWithSkips(PushLowDelayInterrupts::Tolerant, 0.02, 18000, seed) >= 0.95);
        CHECK(predictedShareWithSkips(PushLowDelayInterrupts::Tolerant, 0.05, 18000, seed) >= 0.90);
        CHECK(predictedShareWithSkips(PushLowDelayInterrupts::Strict, 0.02, 18000, seed) < 0.2);   // Package 15 (about 1 %)
    }
    CHECK(predictedShareWithSkips(PushLowDelayInterrupts::Tolerant, 0.0) >= 0.999);
    CHECK(PushLowDelaySchedule{}.interruptRule() == PushLowDelayInterrupts::Tolerant);   // the default
    CHECK(std::string(pushLowDelayInterruptsName(PushLowDelayInterrupts::Tolerant)) == "tolerant");
}

TEST(lowdelay_tolerant_isolated_interruptions_neither_end_nor_double) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    CHECK(s.active());
    PushLowDelayPair reset = calmPair(); reset.refused = true;   // a history reset: the graph ran today's pair
    PushLowDelayPair skipped = calmPair(); skipped.skipped = true;
    PushLowDelayPair held = calmPair(); held.held = true;
    for (int round = 0; round < 50; ++round) {
        s.pair(reset); for (int i = 0; i < 9; ++i) s.pair(calmPair());
        s.pair(skipped); for (int i = 0; i < 9; ++i) s.pair(calmPair());
        s.pair(held); for (int i = 0; i < 9; ++i) s.pair(calmPair());
    }
    CHECK(s.active() && s.exits() == 0 && s.entries() == 1 && s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
    CHECK(s.interruptions() == 150 && s.recentInterruptions() == 3);
}

TEST(lowdelay_tolerant_leaves_on_frequent_interruptions_and_returns_with_hysteresis) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    PushLowDelayPair skipped = calmPair(); skipped.skipped = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kLeaveInterrupts; ++i) { s.pair(skipped); CHECK(s.active()); }
    s.pair(skipped);   // the 7th of the last 30
    CHECK(!s.active() && s.exits(PushLowDelayExit::Skipped) == 1 && s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
    // Back only once at most 3 of the last 30 were interrupted: 7 skips in the window, so 27 calm pairs.
    for (unsigned k = 1; k < 27; ++k) { s.pair(calmPair()); CHECK(!s.active()); }
    s.pair(calmPair());
    CHECK(s.active() && s.entries() == 2);
    // A hold (frame generation off for a second): it leaves within 7 pairs, without back-off.
    PushLowDelayPair held = calmPair(); held.held = true;
    for (int i = 0; i < 40; ++i) s.pair(held);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Held) == 1 && s.calmNeeded() == PushLowDelaySchedule::kCalmPairs);
    // The sustained back-off after a late-frame exit counts calm pairs; interruptions do not restart it.
    PushLowDelaySchedule t;
    t.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) t.pair(calmPair());
    for (int i = 0; i < 3; ++i) t.predictedOutcome(true);
    CHECK(!t.active() && t.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    unsigned pairs = 0;
    while (!t.active() && pairs < 2000) { t.pair(pairs % 50 == 49 ? skipped : calmPair()); ++pairs; }
    CHECK(t.active() && pairs < PushLowDelaySchedule::kStayInCalmPairs + PushLowDelaySchedule::kStayInCalmPairs / 40);
}

TEST(lowdelay_previous_ran_counts_only_pairs_that_generated) {
    CHECK(pushLowDelayPreviousRan(true, false, false));    // a skipped / reset pair after a predicted one: still predicted
    CHECK(!pushLowDelayPreviousRan(false, false, false));
    CHECK(!pushLowDelayPreviousRan(true, false, true));    // an interpolated pair that generated
    CHECK(pushLowDelayPreviousRan(false, true, true));
    // So an isolated skip inside the mode resets neither learner differently from a predicted pair.
    const auto r = pushLowDelayResets(true, pushLowDelayPreviousRan(true, false, false));
    CHECK(r.interpolate && !r.predicted);
}

// Package 16, item 4: a predicted frame whose measured GPU time is above one output slot on most pairs leaves (sustained
// back-off, exit "dear"); below it nothing happens. Admission uses the measured P once 8 are timed.
TEST(lowdelay_leaves_when_the_predicted_frame_is_too_dear) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calmPair());
    const double slot = 11.12;
    for (int i = 0; i < 190; ++i) s.predictedCost(i % 40 == 39 ? 14.0 : 3.0, slot);   // at most 1 of any 30 above the slot (none in the first 30)
    CHECK(s.active() && s.exits() == 0 && s.lastCostP95() < slot);
    s.predictedCost(12.0, slot);
    CHECK(s.active());                                                   // 1 of the last 30 above: the P95 (29th of 30) is not
    s.predictedCost(12.0, slot);                                         // 2 of the last 30: the P95 is above the slot
    CHECK(!s.active() && s.exits(PushLowDelayExit::Dear) == 1 && s.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    CHECK(std::string(pushLowDelayExitName(PushLowDelayExit::Dear)) == "dear");
    s.predictedCost(-1.0, slot);   // unknown: ignored
    // One slow first P after an entry is not a P95 (gate round 2: 10-15 ms once on WARP, then 0.3 ms).
    PushLowDelaySchedule f;
    f.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) f.pair(calmPair());
    f.predictedCost(14.0, slot);
    for (int i = 0; i < 40; ++i) f.predictedCost(0.3, slot);
    CHECK(f.active() && f.exits() == 0);
    CHECK(pushLowDelayAdmissionGroupMs(4.4, 2.2, 3.0, 8) == 5.2);    // Fg1 + measured P95
    CHECK(pushLowDelayAdmissionGroupMs(4.4, 2.2, 3.0, 7) == 4.4);    // too few measured: the interpolated group
    CHECK(pushLowDelayAdmissionGroupMs(4.4, 0.0, 3.0, 20) == 4.4);   // no Fg1: unchanged
}

// Package 16, item 6 (forensics F5): the row's live counters, once a second.
TEST(lowdelay_row_facts_from_the_last_second) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    // Waiting from the start: progress of the entry, no stop yet, seconds without predicted pairs counted.
    for (int i = 0; i < 5; ++i) s.pair(calmPair());
    auto f = pushLowDelayRowFacts(s, true, 0, 30, 100000000, 0, 3);
    CHECK(f.predicted == 0 && f.pairs == 30 && f.calmHave == 5 && f.calmNeed == PushLowDelaySchedule::kCalmPairs && f.lastStopAgeS == -1 &&
          f.lastStop == PushLowDelayExit::None && f.zeroSeconds == 4);
    for (int i = 0; i < 3; ++i) s.pair(calmPair());
    CHECK(s.active());
    f = pushLowDelayRowFacts(s, true, 28, 30, 110000000, 0, 4, 26);   // 2 of the 28 predicted frames dropped late
    CHECK(f.shown == 26);
    { PushLowDelayShared sh; pushLowDelayPublish(sh, f); CHECK(sh.shownPerSecond.load() == 26 && sh.predictedPerSecond.load() == 28); }
    CHECK(f.predicted == 28 && f.zeroSeconds == 0 && f.calmHave == 0 && f.calmNeed == 0);   // running: no entry progress
    for (int i = 0; i < 3; ++i) s.predictedOutcome(true);                                     // a late exit at t = 11 s
    f = pushLowDelayRowFacts(s, true, 0, 30, 160000000, 110000000, 0);
    CHECK(f.lastStop == PushLowDelayExit::Late && f.lastStopAgeS == 5 && f.calmNeed == PushLowDelaySchedule::kStayInCalmPairs);
    CHECK(pushLowDelayRowFacts(s, false, 0, 30, 160000000, 110000000, 9).zeroSeconds == 0);   // row off: not counted
    PushLowDelayShared shared;
    pushLowDelayPublish(shared, f);
    CHECK(shared.lastStop.load() == static_cast<std::uint8_t>(PushLowDelayExit::Late) && shared.lastStopAgeS.load() == 5 && shared.pairsPerSecond.load() == 30);
    pushLowDelayPublish(shared, PushLowDelayRowFacts{});
    CHECK(shared.lastStopAgeS.load() == -1 && shared.pairsPerSecond.load() == 0);
}

// Package 16 (the "picture" part itemised): [fg-health] reports the new stages and the submit -> GPU start delay.
// The display branch's capture stalls (P15_DISPLAY_AND_BASE_PATH.md: one sample 92-158 ms late, then a burst): the stall is
// one interruption and never an exit; its late or unusable predicted frames are not counted.
TEST(lowdelay_capture_stall_ride) {
    PushCaptureStallRide r;
    constexpr std::int64_t T = 333333;   // 30 FPS content
    CHECK(!r.pair(T, T) && !r.pair(T + 20000, T));
    CHECK(r.pair(1200000, T));                       // 120 ms
    CHECK(r.pair(40000, T) && r.pair(30000, T) && r.pair(30000, T));   // the burst
    CHECK(r.pair(T, T));                             // one pair more after it
    CHECK(!r.pair(T, T) && r.stalls() == 1);
    CHECK(r.pair(4 * T, T) && r.pair(T, T) && !r.pair(T, T));   // a gap without a burst: the gap pair and one more
    CHECK(!r.pair(0, T) && !r.pair(T, 0));
    CHECK(!r.pair(3 * T, T));                        // exactly 3 intervals is not a stall
    // The engine's own stall: a real frame decided on more than an interval after it arrived.
    CHECK(r.pair(T, T, T + 1) && r.lastWasEngine() && r.engineStalls() == 1);
    CHECK(!r.pair(T, T, 1000) && !r.lastWasEngine());  // the pair after it is normal (late outcomes: the schedule's tail)
    CHECK(!r.pair(T, T, T) && !r.lastWasEngine());      // a lag of one interval is not a stall

    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    PushLowDelayPair calm; calm.wanted = calm.eligible = true;
    for (int i = 0; i < 40 && !s.active(); ++i) s.pair(calm);
    CHECK(s.active());
    const auto interruptionsBefore = s.interruptions();
    PushLowDelayPair stalled = calm; stalled.stall = true;
    for (int stall = 0; stall < 6; ++stall) {        // six stalls in a row of pairs: each one interruption, no exit
        for (int i = 0; i < 5; ++i) {
            PushLowDelayPair p = stalled;
            p.skipped = i == 1; p.held = i == 2; p.refused = i == 3;
            s.pair(p);
            s.predictedOutcome(true);
            s.usable(false);
        }
        for (int i = 0; i < 4; ++i) { s.pair(calm); s.predictedOutcome(false); }
    }
    CHECK(s.active() && s.exits() == 0);
    CHECK(s.stallRides() == 6 && s.interruptions() == interruptionsBefore + 6);
    // Engine stalls (several a second on WARP) are not interruptions at all, and late outcomes right after them are not
    // counted (they arrive a few pairs later).
    {
        const auto before = s.interruptions();
        for (int k = 0; k < 40; ++k) {
            PushLowDelayPair e = calm; e.stall = e.engineStall = (k % 5 == 0);
            s.pair(e);
            s.predictedOutcome(k % 5 == 1);   // a late drop one pair after each engine stall
        }
        CHECK(s.active() && s.exits() == 0 && s.interruptions() == before);
    }
    // Outside a stall the same interruptions still leave (the tolerant rule is unchanged).
    PushLowDelayPair refused = calm; refused.refused = true;
    for (int i = 0; i < 8 && s.active(); ++i) s.pair(refused);
    CHECK(!s.active());
}

// Review 1 SHOULD 9: an asked pair is admitted with the same group its deadline was shifted with (Fg1 + P's P95), not with
// the interpolated group the window learned.
TEST(lowdelay_admission_uses_the_shifted_group) {
    FgRecoveryBudget b;
    const std::int64_t t = 100000000;
    for (int i = 0; i < 20; ++i) b.complete(std::optional<double>(4.0 + 6.0), true, false, t + i * 333333, std::optional<double>(6.0));   // base 4, interpolated FG 6
    const std::int64_t now = t + 20 * 333333;
    const std::int64_t deadline = now + 80000;   // 8 ms (+10 ms grace inside canAdmit)
    CHECK(b.canAdmit(now, deadline, 0.0, 0.0));                                   // learned: base 4 + FG 6 = 10 <= 18
    CHECK(!b.canAdmit(now, deadline, 0.0, 0.0, false, 0, 15.0));                  // a dear group: 4 + 15 = 19 > 18
    const std::int64_t tight = now - 10000;                                       // -1 ms: 9 ms with the grace
    CHECK(b.canAdmit(now, tight, 0.0, 0.0, false, 0, 3.0) && !b.canAdmit(now, tight, 0.0, 0.0));   // cheap P: 4 + 3 = 7 fits, the learned 10 does not
    CHECK(b.canAdmit(now, deadline, 0.0, 0.0, true, 0, 15.0) == b.canAdmit(now, deadline, 0.0, 0.0, true));   // warming history: the override is not used
}

// Review 1 SHOULD 4: three sustained exits of the same reason stop the mode for the playback; switching the row off
// lets a later switch-on try again.
TEST(lowdelay_stops_for_the_playback_after_three_dear_exits) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    const double slot = 11.12;
    unsigned dearExits = 0;
    for (int pair = 0; pair < 20000 && s.stoppedFor() == PushLowDelayExit::None; ++pair) {
        s.pair(calmPair());
        if (s.active()) s.predictedCost(14.0, slot);   // every P above the slot
        dearExits = unsigned(s.exits(PushLowDelayExit::Dear));
    }
    CHECK(s.stoppedFor() == PushLowDelayExit::Dear && dearExits == PushLowDelaySchedule::kStopAfterSustained);
    for (int pair = 0; pair < 5000; ++pair) s.pair(calmPair());
    CHECK(!s.active() && s.entries() == PushLowDelaySchedule::kStopAfterSustained);   // no entry for the rest of the playback
    PushLowDelayPair off; off.wanted = false;
    s.pair(off);
    CHECK(s.stoppedFor() == PushLowDelayExit::None);
    for (int pair = 0; pair < 2000 && !s.active(); ++pair) s.pair(calmPair());
    CHECK(s.active());                                                                // the row switched on again: it tries again
}

// Review 1 SHOULD 4: the entry guard from the motion source's self-check (its cost for a real frame and P on this card).
TEST(lowdelay_entry_guard_from_the_self_check) {
    using R = veyra::pipeline::PushPredictRefusal;
    const double slot = 11.12;
    CHECK(pushLowDelayEntryCostRefusal(R::Ok, 3.0, slot) == R::Ok);
    CHECK(pushLowDelayEntryCostRefusal(R::Ok, 0.75 * slot + 0.01, slot) == R::FlowTooSlow);
    CHECK(pushLowDelayEntryCostRefusal(R::Ok, -1.0, slot) == R::Ok);              // unknown (test sources): no guard
    CHECK(pushLowDelayEntryCostRefusal(R::NotThreeX, 50.0, slot) == R::NotThreeX);   // another refusal stays
    CHECK(std::string_view(veyra::pipeline::pushPredictRefusalName(R::FlowTooSlow)) == "flow-too-slow");
    CHECK(pushLowDelayStateFor(true, PushLowDelaySuspend::None, true, true, R::FlowTooSlow, false) == PushLowDelayState::NotHere);
}

// Review 1 SHOULD 5: interruptions the mode causes itself (skips only while predicted) take the sustained back-off and
// stop it after three; interruptions as frequent while interpolating (his case) do not.
TEST(lowdelay_self_caused_interruptions_back_off) {
    {
        PushLowDelaySchedule s;
        s.setPolicy(PushLowDelayPolicy::StayIn);
        unsigned n = 0;
        for (int pair = 0; pair < 60000 && s.stoppedFor() == PushLowDelayExit::None; ++pair) {
            PushLowDelayPair p = calmPair();
            p.skipped = s.active() && (n++ % 4 == 0);   // 25 % skipped, only while predicted
            s.pair(p);
        }
        CHECK(s.stoppedFor() == PushLowDelayExit::Skipped);
        CHECK(s.activeInterruptShare() > s.idleInterruptShare() + PushLowDelaySchedule::kSelfMargin);
        CHECK(s.switches() < 120);   // the review's simulation without the back-off: 70 a minute at 25 %
    }
    {
        PushLowDelaySchedule s;
        s.setPolicy(PushLowDelayPolicy::StayIn);
        for (int pair = 0; pair < 60000; ++pair) {
            PushLowDelayPair p = calmPair();
            p.skipped = pair % 50 == 7;               // 2 % skipped whether predicted or not
            s.pair(p);
        }
        CHECK(s.stoppedFor() == PushLowDelayExit::None && s.active());
        CHECK(!s.selfCaused());
    }
}

// Review 1 MUST 3: the row switched off once at the first start of this version.
TEST(lowdelay_pref_reset_once_for_this_version) {
    auto m = pushLowDelayMigrate(false, true);    // first start, saved on: off, flag written, logged
    CHECK(!m.rowOn && m.writeFlag && m.logReset);
    m = pushLowDelayMigrate(false, false);        // first start, off or unset: flag written, nothing logged
    CHECK(!m.rowOn && m.writeFlag && !m.logReset);
    m = pushLowDelayMigrate(true, true);          // later starts: what the owner chose since
    CHECK(m.rowOn && !m.writeFlag && !m.logReset);
    m = pushLowDelayMigrate(true, false);
    CHECK(!m.rowOn && !m.writeFlag);
    CHECK(std::string_view(kPushLowDelayResetFlag) == "pushLowDelayReset16");
    m = pushLowDelayMigrate(false, true, true);   // rollback part LowDelayMigrate16: the saved value as it is, nothing written
    CHECK(m.rowOn && !m.writeFlag && !m.logReset);
}

// Rollback part LowDelaySchedule16 (PushLegacy.h): Package 15's row-on schedule.
TEST(lowdelay_legacy_package15_schedule) {
    PushLowDelaySchedule s;
    s.setPolicy(PushLowDelayPolicy::StayIn);
    s.setLegacyPackage15(true);
    CHECK(s.legacyPackage15() && s.interruptRule() == PushLowDelayInterrupts::Strict);
    PushLowDelayPair calm; calm.wanted = calm.eligible = true;
    for (int i = 0; i < 40 && !s.active(); ++i) s.pair(calm);
    CHECK(s.active());
    PushLowDelayPair stalled = calm; stalled.stall = true; stalled.skipped = true;
    s.pair(stalled);                                    // no stall ride: a skipped pair in a stall leaves (strict)
    CHECK(!s.active() && s.exits(PushLowDelayExit::Skipped) == 1 && s.stallRides() == 0);
    // No stop for the playback: three sustained late exits still try again.
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4000 && !s.active(); ++i) s.pair(calm);
        CHECK(s.active());
        for (int i = 0; i < 3; ++i) s.predictedOutcome(true);
        CHECK(!s.active());
    }
    CHECK(s.stoppedFor() == PushLowDelayExit::None);
    // The grid exit at a fixed 98 %, only when interpolation's own median is at 98 % or better.
    PushLowDelaySchedule g;
    g.setLegacyPackage15(true);
    for (int i = 0; i < 3; ++i) g.gridWindow(96.0, false, true);   // interpolation itself at 96 %: no grid exit
    for (int i = 0; i < 40 && !g.active(); ++i) g.pair(calm);
    for (int i = 0; i < 5; ++i) g.gridWindow(97.0, true, false);
    CHECK(g.active());
    PushLowDelaySchedule h;
    h.setLegacyPackage15(true);
    for (int i = 0; i < 3; ++i) h.gridWindow(99.5, false, true);
    for (int i = 0; i < 40 && !h.active(); ++i) h.pair(calm);
    for (int i = 0; i < 3; ++i) h.gridWindow(97.9, true, false);   // within today's margin of 1 point it would stay (98.5)
    CHECK(!h.active() && h.gridExits() == 1);
}

// Review 1 MUST 2: the clock calibration after the submission, at most once a second (QPC ticks; 10 MHz here).
TEST(clock_calibration_at_most_once_a_second) {
    constexpr std::int64_t f = 10000000;
    CHECK(pushClockCalibrationDue(5, 0, f));                       // none yet: take one
    CHECK(!pushClockCalibrationDue(1000 + f - 1, 1000, f));        // 0.9999999 s later: not yet
    CHECK(pushClockCalibrationDue(1000 + f, 1000, f));             // 1 s later: due
    CHECK(pushClockCalibrationDue(1000, 1000, 0));                 // no frequency: never stale-forever
    int taken = 0; std::int64_t last = 0;
    for (std::int64_t t = 1; t <= 3 * f; t += f / 90) if (pushClockCalibrationDue(t, last, f)) { ++taken; last = t; }   // 90 base lists a second for 3 s
    CHECK(taken == 3);
}

TEST(fg_health_reports_the_picture_parts) {
    veyra::engine::PushFgHealth h;
    (void)h.second(10000000, {}, {});
    for (int i = 0; i < 20; ++i) { h.gpu(35, 1.5); h.gpu(36, 0.4); h.gpu(37, 0.2); h.submitToGpuStart(0.3); }
    const std::string line = h.second(10000000 + veyra::engine::PushFgHealth::kWindow100ns, {}, {});
    CHECK(line.find("gpu_baseTailP50Ms=1.500") != std::string::npos && line.find("gpu_vfgCaptureP95Ms=0.400") != std::string::npos &&
          line.find("gpu_realComposeP99Ms=0.200") != std::string::npos && line.find("submitToGpuStartP50Ms=0.300") != std::string::npos);
    CHECK(line.find("gpu_fg1P50Ms=-1") != std::string::npos);   // the earlier stages keep their names and places
}

// Package 16, item 7: the pass is kept after the first switch-on of a run, so later toggles rebuild nothing.
TEST(lowdelay_pass_is_kept_after_the_first_switch_on) {
    CHECK(!pushLowDelayKeepPass(false, false));   // never on this run: no pass (the row-off path is Package 14's)
    CHECK(pushLowDelayKeepPass(true, false));     // the first switch-on: the graph shape changes once
    CHECK(pushLowDelayKeepPass(false, true));     // off again: kept, no rebuild
    CHECK(pushLowDelayKeepPass(true, true));
    // The graph shape therefore changes at most once in a run of toggles (VFG at 3X).
    bool everOn = false, shape = false; unsigned changes = 0;
    for (const bool row : {false, true, false, true, false, true, true, false}) {
        const bool next = pushLowDelayShapesGraph(pushLowDelayKeepPass(row, everOn), true, 3);
        changes += next != shape; shape = next; everOn = everOn || row;
    }
    CHECK(changes == 1);
}

}  // namespace lowdelay_tests
