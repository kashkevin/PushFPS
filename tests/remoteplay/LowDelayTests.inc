// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushLowDelay.h (Package 15, stream "lowdelay"): when the one-predicted schedule is asked for, the
// phase each schedule starts from, and admission. Values are examples (100 ns units, a 29.97 FPS game).
#include "veyra/engine/PushLowDelay.h"
#include "veyra/pipeline/PushPredict.h"

namespace lowdelay_tests {
using namespace veyra::engine;

inline PushLowDelayPair calmPair() {
    PushLowDelayPair p;
    p.wanted = true; p.eligible = true;
    return p;
}

TEST(lowdelay_enters_after_calm_pairs_and_leaves_at_once) {
    PushLowDelaySchedule s;
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
    // Other exits are as before: a skipped pair leaves at once.
    PushLowDelaySchedule t;
    t.setPolicy(PushLowDelayPolicy::StayIn);
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) t.pair(calm);
    PushLowDelayPair skipped = calm; skipped.skipped = true;
    t.pair(skipped);
    CHECK(!t.active() && t.exits(PushLowDelayExit::Skipped) == 1);
    t.reset();
    CHECK(t.policy() == PushLowDelayPolicy::StayIn);   // reset keeps the policy
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
}

TEST(lowdelay_leaves_after_three_windows_off_the_grid) {
    PushLowDelaySchedule s;
    PushLowDelayPair calm; calm.wanted = true; calm.eligible = true;
    for (unsigned i = 0; i < PushLowDelaySchedule::kCalmPairs; ++i) s.pair(calm);
    // Interpolation not measured yet (the row on from the start): bad windows do not leave (leaving to measure was
    // tried and cost overload 97.3 -> 50.8 % on the grid, SIMULATED: out of the mode, skipped pairs block the return).
    for (int i = 0; i < 6; ++i) s.gridWindow(96.0, true, false);
    CHECK(s.active() && !s.interpolatedOnGridKnown());
    // Interpolation measured worse (93 %): three bad windows do not leave (it would be less steady).
    s.gridWindow(93.0, false, true);
    CHECK(s.interpolatedOnGridKnown());
    for (int i = 0; i < 6; ++i) s.gridWindow(96.0, true, false);
    CHECK(s.active());
    // Interpolation measured good (99.5 %): three bad windows in a row while the mode ran the whole window leave.
    s.gridWindow(99.5, false, true);
    s.gridWindow(97.0, true, false); s.gridWindow(97.5, true, false);
    s.gridWindow(99.2, true, false);                                     // a good window restarts the count
    s.gridWindow(97.0, true, false); s.gridWindow(96.0, true, false);
    s.gridWindow(95.0, false, false);                                    // the mode did not run the whole window: restarts
    s.gridWindow(-1.0, true, false);                                     // no frame-generation output: restarts
    s.gridWindow(97.9, true, false); s.gridWindow(97.9, true, false);
    CHECK(s.active());
    s.gridWindow(97.9, true, false);
    CHECK(!s.active() && s.exits(PushLowDelayExit::Late) == 1 && s.calmNeeded() == PushLowDelaySchedule::kStayInCalmPairs);
    CHECK(std::string(pushLowDelayExitName(PushLowDelayExit::Late)) == "late");
}

TEST(lowdelay_exits_text_lists_every_reason) {
    PushLowDelaySchedule s;
    CHECK(pushLowDelayExitsText(s) == "off:0,ineligible:0,refused:0,unusable:0,skipped:0,held:0,late:0");
}

}  // namespace lowdelay_tests
