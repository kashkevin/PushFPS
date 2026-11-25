// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "fglat": include/veyra/engine/PushFgLatency.h (Ultra Low Latency Frame Generation, VEYRA_EXP_RP_FG_ULL). Every
// rate and time here is made up: a model of the code's decisions, not a measurement of a GPU, a card or a PS5.
#include "veyra/engine/PushFgLatency.h"
#include "veyra/engine/PushSmoothness.h"
#include <cstdint>
#include <string>
#include <vector>

namespace fglat_tests {
using veyra::engine::PushFgFallbackHold;
using veyra::engine::PushUllParts;
using veyra::engine::pushUllParse;
constexpr std::int64_t kFglatMs = 10000;   // 100 ns ticks per millisecond
}  // namespace fglat_tests

TEST(fglat_switch_parse_all_parts_and_lists) {
    using namespace fglat_tests;
    const PushUllParts all = pushUllParse(L"1");
    CHECK(all.valid && all.hold && all.phase && all.headroom && all.grace);
    CHECK(std::string(veyra::engine::pushUllPartsName(all)) == "all");
    const PushUllParts word = pushUllParse(L"all");
    CHECK(word.hold && word.phase && word.headroom && word.grace);
    const PushUllParts list = pushUllParse(L"hold,phase");
    CHECK(list.valid && list.hold && list.phase && !list.headroom && !list.grace);
    CHECK(std::string(veyra::engine::pushUllPartsName(list)) == "hold,phase");
    const PushUllParts one = pushUllParse(L"grace");
    CHECK(one.valid && one.grace && !one.hold && one.any());
    CHECK(std::string(veyra::engine::pushUllPartsName(one)) == "grace");
}

TEST(fglat_switch_unset_or_not_understood_turns_nothing_on) {
    using namespace fglat_tests;
    const PushUllParts unset = pushUllParse(L"");
    CHECK(unset.valid && !unset.any());
    // "0", a typo, an empty part, upper case, spaces, a value too long: nothing on, and marked not understood.
    for (const wchar_t* bad : {L"0", L"hodl", L"hold,", L",hold", L"HOLD", L"hold, phase", L"yes", L"2"}) {
        const PushUllParts p = pushUllParse(bad);
        CHECK(!p.any());
        CHECK(!p.valid);
    }
    const std::wstring tooLong(64, L'a');
    CHECK(!pushUllParse(tooLong).any() && !pushUllParse(tooLong).valid);
    CHECK(std::string(veyra::engine::pushUllPartsName(unset)) == "none");
}

TEST(fglat_hold_enters_at_once_while_settling_and_resumes_after_eight_admitted) {
    using namespace fglat_tests;
    PushFgFallbackHold hold;
    std::int64_t t = 1000 * kFglatMs;
    const std::int64_t pair = 333400;   // 30 fps content
    // Steady FG: never held.
    for (int i = 0; i < 30; ++i, t += pair) CHECK(!hold.observe(t, true, false));
    // A FgCostSettling hold begins: the first skipped pair already shows its real frame when ready.
    CHECK(hold.observe(t, false, true));
    CHECK(hold.held() && hold.why() == PushFgFallbackHold::Why::Settling && hold.entries() == 1);
    t += pair;
    for (int i = 0; i < 60; ++i, t += pair) CHECK(hold.observe(t, false, true));   // 2 s of hold
    // The probe after the hold: admitted pairs stay held until 8 in a row were admitted.
    for (unsigned i = 1; i < PushFgFallbackHold::kResumePairs; ++i, t += pair) CHECK(hold.observe(t, true, false));
    CHECK(!hold.observe(t, true, false));
    CHECK(!hold.held() && hold.why() == PushFgFallbackHold::Why::None && hold.entries() == 1);
}

TEST(fglat_hold_enters_after_250_ms_of_skipped_pairs_not_before) {
    using namespace fglat_tests;
    PushFgFallbackHold hold;
    std::int64_t t = 50 * kFglatMs;
    const std::int64_t pair = 166800;   // 59.94 fps
    // Skipped pairs only, no settling hold: the phase stays for the first 250 ms (LiveFgRejectionHold: 1 s).
    const std::int64_t first = t;
    bool entered = false;
    std::int64_t enteredAt = 0;
    for (int i = 0; i < 40; ++i, t += pair) {
        const bool held = hold.observe(t, false, false);
        if (held && !entered) { entered = true; enteredAt = t; }
    }
    CHECK(entered);
    CHECK(enteredAt - first >= PushFgFallbackHold::kEnter100ns);
    CHECK(enteredAt - first < PushFgFallbackHold::kEnter100ns + pair);
    CHECK(hold.why() == PushFgFallbackHold::Why::Rejected);
}

TEST(fglat_hold_never_enters_on_mixed_admission) {
    using namespace fglat_tests;
    // One pair in three skipped (the 90-100 FPS case of FPS90.md P1): a single skipped pair keeps the phase, so the
    // presentation stays on one timeline (no early / late real frames alternating).
    PushFgFallbackHold hold;
    std::int64_t t = 0;
    for (int i = 0; i < 600; ++i, t += 166800) CHECK(!hold.observe(t + 1, i % 3 != 0, false));
    CHECK(hold.entries() == 0);
    // Also two skipped in a row, then an admitted one, for 10 s.
    for (int i = 0; i < 600; ++i, t += 166800) CHECK(!hold.observe(t + 1, i % 3 == 0, false));
}

TEST(fglat_hold_a_skip_while_resuming_starts_the_count_again) {
    using namespace fglat_tests;
    PushFgFallbackHold hold;
    std::int64_t t = kFglatMs;
    CHECK(hold.observe(t, false, true));
    for (int i = 0; i < 5; ++i) CHECK(hold.observe(t += 333400, true, false));
    CHECK(hold.observe(t += 333400, false, false));   // one skip: still held, count back to zero
    for (unsigned i = 1; i < PushFgFallbackHold::kResumePairs; ++i) CHECK(hold.observe(t += 333400, true, false));
    CHECK(!hold.observe(t += 333400, true, false));
    hold.reset();
    CHECK(!hold.held() && hold.entries() == 0);
}

TEST(fglat_hold_model_shows_two_uneven_intervals_per_hold) {
    using namespace fglat_tests;
    // The pacing cost of the hold, on a model of a capture card's real frames: 30 fps content (T = 33.4 ms), 4X, the
    // learned phase 29.5 ms after the callback (25.0 + 3.5 first generated frame + 1 ms margins), base work 2 ms. Real
    // frames are shown at callback + phase while FG runs, at callback + base while held. Generated frames are left out:
    // what matters is the shift of the real-frame timeline, which the eye sees as one short and one long interval.
    const double T = 33.4, phase = 29.5, base = 2.0;
    PushFgFallbackHold hold;
    std::vector<double> shown;
    for (int i = 0; i < 120; ++i) {
        const double callback = 1000.0 + i * T;
        const bool admitted = i < 30 || i >= 60;   // 30 admitted, a 1 s settling hold (30 pairs), then admitted again
        const bool settling = i >= 30 && i < 60;
        const bool held = hold.observe(std::int64_t(callback * kFglatMs), admitted, settling);
        shown.push_back(callback + (held ? base : phase));
    }
    int uneven = 0;
    double shortest = 1e9, longest = 0;
    for (std::size_t i = 1; i < shown.size(); ++i) {
        const double gap = shown[i] - shown[i - 1];
        if (gap < T - 0.01 || gap > T + 0.01) ++uneven;
        if (gap < shortest) shortest = gap;
        if (gap > longest) longest = gap;
    }
    CHECK(uneven == 2);                                    // entering and leaving, nothing else
    CHECK(shortest > T - (phase - base) - 0.01 && shortest < T - (phase - base) + 0.01);   // 33.4 - 27.5 = 5.9 ms
    CHECK(longest > T + (phase - base) - 0.01 && longest < T + (phase - base) + 0.01);     // 60.9 ms
    // While held (pairs 31-59 and the 7 resuming ones) every real frame is 27.5 ms earlier than on the FG phase.
    CHECK(shown[45] - (1000.0 + 45 * T) < base + 0.01);
    CHECK(shown[70] - (1000.0 + 70 * T) > phase - 0.01);
}

TEST(fglat_headroom_target_keeps_three_percent_of_the_vrr_ceiling) {
    using veyra::engine::pushUllHeadroomTarget;
    // The owner's panel (179.95 Hz with VRR): 120, so a 60 fps game runs 2X and a 30 fps game 4X, never at the ceiling.
    CHECK(pushUllHeadroomTarget(179.95, 1) == 120);
    CHECK(pushUllHeadroomTarget(180.0, 1) == 120);
    CHECK(pushUllHeadroomTarget(240.0, 1) == 180);
    CHECK(pushUllHeadroomTarget(144.0, 1) == 120);
    CHECK(pushUllHeadroomTarget(165.0, 1) == 120);
    CHECK(pushUllHeadroomTarget(119.998, 1) == 90);
    CHECK(pushUllHeadroomTarget(60.0, 1) == 0);      // nothing fits under a 60 Hz ceiling: the existing rules
    // Fixed refresh or VRR unknown: no automatic target.
    CHECK(pushUllHeadroomTarget(179.95, 0) == 0);
    CHECK(pushUllHeadroomTarget(179.95, -1) == 0);
    CHECK(pushUllHeadroomTarget(0.0, 1) == 0);
    CHECK(pushUllHeadroomTarget(std::nan(""), 1) == 0);
    // Every target it gives is one Smoothness Mode accepts.
    for (const double hz : {75.0, 100.0, 120.0, 144.0, 165.0, 180.0, 200.0, 240.0, 360.0}) {
        const int t = pushUllHeadroomTarget(hz, 1);
        CHECK(t == 0 || veyra::engine::pushSmoothTargetValid(t));
        CHECK(double(t) <= hz * veyra::engine::kPushUllHeadroom);
    }
}

TEST(fglat_headroom_target_through_smoothness_mode_picks_2x_and_4x_on_180hz) {
    // What Smoothness Mode then does with the target (its own rule, PushSmoothness.h): the largest offered multiplier
    // whose output is at or under min(target, cap, refresh) x 1.02. Model inputs, no costs (no step-down).
    using namespace veyra::engine;
    const auto run = [](double contentFps) {
        PushSmoothnessMode mode;
        PushSmoothCostModel costs;
        PushSmoothInput in;
        in.targetFps = pushUllHeadroomTarget(179.95, 1);
        in.contentFps = contentFps;
        in.capFps = 179.95;
        in.refreshHz = 179.95;
        in.ceiling = 6;
        in.offered = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6);
        in.costs = &costs;
        unsigned m = 0;
        for (int i = 0; i < 30; ++i) {
            in.now100ns = 1000000 + std::int64_t(i) * 166800;
            m = mode.update(in).multiplier;
        }
        return m;
    };
    CHECK(run(59.94) == 2);   // 120, not 180 (3X at the ceiling)
    CHECK(run(29.97) == 4);   // 120, not 180 (6X)
}

TEST(fglat_grace_is_half_an_output_interval_and_keeps_the_anchor_part) {
    using veyra::engine::pushUllGeneratedGrace;
    constexpr std::int64_t def = 100000;   // kLiveGeneratedGrace100ns, 10 ms
    // 30 fps at 4X: an output interval of 8.35 ms, half of it 4.17 ms.
    CHECK(pushUllGeneratedGrace(def, def, 333667, 4) == 41708);
    // 60 fps at 2X: 4.17 ms; at 6X of 60: 1.39 ms; a 1 ms floor at 360 fps output.
    CHECK(pushUllGeneratedGrace(def, def, 166833, 2) == 41708);
    CHECK(pushUllGeneratedGrace(def, def, 166833, 6) == 13902);
    CHECK(pushUllGeneratedGrace(def, def, 166833, 8) == 10427);
    CHECK(pushUllGeneratedGrace(def, def, 100000, 8) == 10000);
    // Never more than the default (slow content), and the callback-to-anchor part is kept on top.
    CHECK(pushUllGeneratedGrace(def, def, 2000000, 2) == def);
    CHECK(pushUllGeneratedGrace(def + 7000, def, 333667, 4) == 41708 + 7000);
    // Unknown interval or no generation: the default, unchanged.
    CHECK(pushUllGeneratedGrace(def + 7000, def, 0, 4) == def + 7000);
    CHECK(pushUllGeneratedGrace(def, def, 333667, 1) == def);
}

TEST(fglat_latency_window_fields_and_counts) {
    using veyra::engine::PushFgLatencyWindow;
    PushFgLatencyWindow w;
    // A model of 5 s of a 60 fps stream at 2X: 300 real frames; every 10th pair skipped (no generated frame shown, the
    // real frame still on the phase: a dead phase); real frames held 12 ms (P50), 20 ms in one frame of 20.
    for (int i = 0; i < 300; ++i) {
        const bool skipped = i % 10 == 0;
        const double hold = i % 20 == 0 ? 20.0 : 12.0;
        w.real(18.0 + hold * 0.1, hold, 18.4, skipped ? -1.0 : 6.2, true, skipped ? 0u : 1u, false);
        w.presented(0.2);
        if (!skipped) w.presented(0.4);
    }
    CHECK(w.realFrames() == 300 && w.deadPhase() == 30 && w.fallback() == 0);
    CHECK(w.percentile(PushFgLatencyWindow::Metric::Hold, 50) == 12.0);
    CHECK(w.percentile(PushFgLatencyWindow::Metric::Hold, 99) == 20.0);
    CHECK(w.samples(PushFgLatencyWindow::Metric::First) == 270);   // -1 (no generated frame) is not a sample
    const std::string f = w.fields(5.0, false);
    CHECK(f.find(" addedEstMs=12.000 ") != std::string::npos);
    CHECK(f.find(" realHoldP99Ms=20.000 ") != std::string::npos);
    CHECK(f.find(" phaseMs=18.400 phaseLearned=0 ") != std::string::npos);
    CHECK(f.find(" firstReadyP50Ms=6.200 ") != std::string::npos);
    CHECK(f.find(" presentLateP99Ms=0.400 ") != std::string::npos);
    CHECK(f.find(" deadPhaseFps=6.000 ") != std::string::npos);
    CHECK(f.find(" fallbackFps=0.000") != std::string::npos);
    CHECK(f.front() == ' ');
    // A card writes no age (its [slipstream-capture] realLat is that measure): -1, and held frames count as fallback.
    w.clear();
    for (int i = 0; i < 30; ++i) w.real(-1.0, 0.1, 0.0, -1.0, false, 0, true);
    const std::string g = w.fields(1.0, true);
    CHECK(g.find(" realAgeP50Ms=-1.000 ") != std::string::npos && g.find(" phaseLearned=1 ") != std::string::npos);
    CHECK(g.find(" deadPhaseFps=0.000 ") != std::string::npos && g.find(" fallbackFps=30.000") != std::string::npos);
    // Bounded: never more than kMax samples, never a NaN or a negative value.
    for (std::size_t i = 0; i < PushFgLatencyWindow::kMax + 50; ++i) w.presented(std::nan(""));
    CHECK(w.samples(PushFgLatencyWindow::Metric::Late) == 0);
    for (std::size_t i = 0; i < PushFgLatencyWindow::kMax + 50; ++i) w.presented(1.0);
    CHECK(w.samples(PushFgLatencyWindow::Metric::Late) == PushFgLatencyWindow::kMax);
}

TEST(fglat_smooth_line_carries_the_latency_fields_before_its_note) {
    using namespace veyra::engine;
    PushSmoothWindow window;
    PushSmoothCounters c;
    window.restart(0, c);
    window.pair(2, 59.94);
    PushSmoothConfigInfo info;
    info.backend = "vfg";
    info.ceiling = 2;
    PushSmoothLineInputs x;
    PushFgLatencyWindow latency;
    latency.real(20.0, 12.0, 18.0, 6.0, true, 1, false);
    x.extra = latency.fields(5.0, true);
    const std::string line = pushSmoothLine(window, info, c, 50000000, x);
    const auto fields = line.find(" addedEstMs=12.000 ");
    const auto note = line.find(" (counts before Present");
    CHECK(fields != std::string::npos && note != std::string::npos && fields < note);
    CHECK(line.find(" steady=0 realAgeP50Ms=20.000 ") != std::string::npos);
    // Without the extra fields the line is as before.
    x.extra.clear();
    CHECK(pushSmoothLine(window, info, c, 50000000, x).find("steady=0 (counts before Present") != std::string::npos);
}
