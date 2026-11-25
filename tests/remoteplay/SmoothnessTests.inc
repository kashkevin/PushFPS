// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushSmoothness.h (stream "smooth") and the VEYRA_EXP_RP_SMOOTH_STEADY option of
// FgCostSettling (FgRecoveryBudget.h). Rates, GPU times and sizes are made up; nothing here is a measurement.
#include "veyra/engine/PushSmoothness.h"
#include "veyra/engine/FgRecoveryBudget.h"
#include "veyra/engine/PushPipelineHook.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace smooth_tests {
using namespace veyra::engine;
// VFG offers 2..8, DLSS 2/3/4/6, PushFlow 2..4 (DisplayFgBudget.h).
constexpr PushFgMask vfgMask() { PushFgMask m = 0; for (unsigned k = 2; k <= 8; ++k) m |= 1u << k; return m; }
constexpr PushFgMask dlssMask() { return (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6); }
inline bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
// A cost model whose groups cost `ms` per generated frame (one 2X group measured).
inline PushSmoothCostModel perFrame(double ms) { PushSmoothCostModel m; m.record(1, ms); return m; }
}

TEST(smooth_sr_target_follows_a_1440p_display){
    using veyra::engine::pushSmoothSrTarget;
    // 4K (1) on a 2560x1440 monitor -> 1440p (0); 8K (2) -> 1440p; 5K (3) -> 1440p.
    CHECK(pushSmoothSrTarget(1, 2560, 1440) == 0);
    CHECK(pushSmoothSrTarget(2, 2560, 1440) == 0);
    CHECK(pushSmoothSrTarget(3, 2560, 1440) == 0);
    // A 4K monitor keeps 4K; 8K on it -> 4K; 1440p asked on a 4K monitor stays 1440p (never raised).
    CHECK(pushSmoothSrTarget(1, 3840, 2160) == 1);
    CHECK(pushSmoothSrTarget(2, 3840, 2160) == 1);
    CHECK(pushSmoothSrTarget(0, 3840, 2160) == 0);
    // An ultrawide 3440x1440 is not covered by 2560x1440: 4K stays.
    CHECK(pushSmoothSrTarget(1, 3440, 1440) == 1);
    // A 1080p monitor: the smallest box (1440p) still covers it.
    CHECK(pushSmoothSrTarget(1, 1920, 1080) == 0);
    // Unknown display or target: unchanged.
    CHECK(pushSmoothSrTarget(1, 0, 0) == 1);
    CHECK(pushSmoothSrTarget(9, 2560, 1440) == 9);
}

TEST(smooth_cap_fit_picks_3x_for_59_94_on_179_95){
    using namespace smooth_tests;
    // The existing rule: ceil(179.95 / 59.94) = 4. With the 2 % tolerance 3X reaches the cap.
    CHECK(static_cast<unsigned>(std::ceil(179.95 / 59.94 - 1e-6)) == 4u);
    CHECK(pushSmoothCapMultiplier(6, 59.94, 179.95, vfgMask()) == 3u);
    CHECK(pushSmoothCapMultiplier(6, 60.0, 179.95, vfgMask()) == 3u);
    CHECK(pushSmoothCapMultiplier(6, 60.03, 179.95, vfgMask()) == 3u);
    // A 120 cap: 2X; a 30 fps game on 180: 6X (29.97 x 6 = 179.8).
    CHECK(pushSmoothCapMultiplier(6, 59.94, 120.0, vfgMask()) == 2u);
    CHECK(pushSmoothCapMultiplier(6, 29.97, 179.95, vfgMask()) == 6u);
    // DLSS has no 5X: a 36 fps game for 180 takes 6X (the smallest offered that reaches it).
    CHECK(pushSmoothCapMultiplier(6, 36.0, 180.0, dlssMask()) == 6u);
    // Never above the ceiling: nothing reaches 240 from 30 under 6X -> the largest offered (6).
    CHECK(pushSmoothCapMultiplier(6, 30.0, 240.0, vfgMask()) == 6u);
    // Unknown rates: the ceiling (the caller keeps its own rule).
    CHECK(pushSmoothCapMultiplier(6, 0.0, 180.0, vfgMask()) == 6u);
    CHECK(pushSmoothCapMultiplier(6, 60.0, 0.0, vfgMask()) == 6u);
}

TEST(smooth_preferred_never_exceeds_target_cap_or_refresh){
    using namespace smooth_tests;
    CHECK(pushSmoothPreferred(120, 59.94, 0, 179.95, 6, vfgMask()) == 2u);
    CHECK(pushSmoothPreferred(180, 59.94, 0, 179.95, 6, vfgMask()) == 3u);
    CHECK(pushSmoothPreferred(180, 59.94, 120, 179.95, 6, vfgMask()) == 2u);   // the cap wins
    CHECK(pushSmoothPreferred(180, 59.94, 0, 120.0, 6, vfgMask()) == 2u);      // the refresh wins
    CHECK(pushSmoothPreferred(120, 29.97, 0, 179.95, 6, vfgMask()) == 4u);
    CHECK(pushSmoothPreferred(90, 29.97, 0, 179.95, 6, vfgMask()) == 3u);
    CHECK(pushSmoothPreferred(90, 59.94, 0, 179.95, 6, vfgMask()) == 0u);      // 1.5X is no multiplier
    CHECK(pushSmoothPreferred(180, 29.97, 0, 179.95, 4, dlssMask()) == 4u);    // the ceiling
    CHECK(pushSmoothPreferred(180, 0.0, 0, 179.95, 6, vfgMask()) == 0u);
}

TEST(smooth_mode_starts_at_the_target_and_steps_down_only_after_sustained_trouble){
    using namespace smooth_tests;
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 180; in.contentFps = 59.94; in.refreshHz = 179.95; in.ceiling = 6; in.offered = vfgMask();
    PushSmoothCostModel costs = perFrame(3.0);  // 3 ms per generated frame: 3X = 4 + 6 = 10 ms of 16.7 (0.6)
    in.baseMs = 4.0; in.costs = &costs;
    in.now100ns = 1000000000;
    auto d = mode.update(in);
    CHECK(d.multiplier == 3u && d.changed && d.reason == PushSmoothReason::Start && !d.lowered);
    CHECK(d.load > 0.55 && d.load < 0.65);
    // GPU grows heavy: 5 ms per generated frame -> 4 + 10 = 14 ms (0.84): still under 0.92, no change.
    costs = perFrame(5.0);
    for (int i = 0; i < 40; ++i) { in.now100ns += 1666667; d = mode.update(in); CHECK(d.multiplier == 3u); }
    // One slow sample (6.5 ms -> 17 ms, load 1.02) for one tick: no step down.
    costs = perFrame(6.5); in.now100ns += 2500000; d = mode.update(in); CHECK(d.multiplier == 3u);
    costs = perFrame(5.0); in.now100ns += 2500000; d = mode.update(in); CHECK(d.multiplier == 3u);
    // Sustained (4 ticks in a row): one step down, with the reason.
    costs = perFrame(6.5);
    bool lowered = false;
    for (int i = 0; i < 5; ++i) { in.now100ns += 2500000; d = mode.update(in); lowered = lowered || d.lowered; }
    CHECK(lowered && d.multiplier == 2u && d.reason == PushSmoothReason::GpuHeadroom && mode.stepDowns() == 1u);
    // Never below 2X and never off, however heavy it stays.
    for (int i = 0; i < 40; ++i) { in.now100ns += 2500000; d = mode.update(in); }
    CHECK(d.multiplier == 2u);
}

TEST(smooth_mode_steps_up_after_headroom_with_hysteresis){
    using namespace smooth_tests;
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 180; in.contentFps = 60; in.refreshHz = 180; in.ceiling = 6; in.offered = vfgMask();
    PushSmoothCostModel costs = perFrame(6.5);
    in.baseMs = 4.0; in.costs = &costs; in.now100ns = 1000000000;
    mode.update(in);
    for (int i = 0; i < 8; ++i) { in.now100ns += 2500000; mode.update(in); }
    CHECK(mode.applied() == 2u);
    // Headroom returns (3X predicted 4 + 2 x 2 = 8 ms, 0.48): no step up within 10 s of the step down (it came at the 4th trouble tick, 1 s before downAt).
    costs = perFrame(2.0);
    std::int64_t downAt = in.now100ns;
    unsigned seen = 2;
    while (in.now100ns - downAt < 85000000) { in.now100ns += 2500000; seen = mode.update(in).multiplier; CHECK(seen == 2u); }
    // ... and then it does, once.
    for (int i = 0; i < 40 && seen == 2u; ++i) { in.now100ns += 2500000; seen = mode.update(in).multiplier; }
    CHECK(seen == 3u && mode.stepUps() == 1u && mode.reason() == PushSmoothReason::Recovered);
}

TEST(smooth_mode_counts_rejected_pairs_as_trouble){
    using namespace smooth_tests;
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 120; in.contentFps = 29.97; in.refreshHz = 179.95; in.ceiling = 6; in.offered = vfgMask();
    in.now100ns = 1000000000;  // no GPU times known: only the admission counts can trouble it
    CHECK(mode.update(in).multiplier == 4u);
    for (int i = 0; i < 6; ++i) { in.now100ns += 2500000; in.admitted += 5; in.rejected += 3; mode.update(in); }
    CHECK(mode.applied() == 3u && mode.reason() == PushSmoothReason::RejectedPairs);
    // A target change downwards applies at once.
    in.targetFps = 60; in.now100ns += 100000;
    auto d = mode.update(in);
    CHECK(d.multiplier == 2u && d.lowered && d.reason == PushSmoothReason::Target);
    // Off: multiplier 0 (the existing rules apply again).
    in.targetFps = 0;
    CHECK(mode.update(in).multiplier == 0u && mode.applied() == 0u);
}

TEST(smooth_mode_returns_within_the_gate_when_the_content_rate_falls_again){
    using namespace smooth_tests;
    // Review case: 30 -> 60 -> 30 content (a game that runs at 60 in a menu). With costs measured and comfortable,
    // the return to 6X must take the 0.5 s gate, not the 5 s headroom hold plus the 10 s lock-out (it took 16 s).
    PushSmoothnessMode mode;
    PushSmoothInput in;
    PushSmoothCostModel costs = perFrame(1.0);
    in.targetFps = 180; in.contentFps = 29.97; in.refreshHz = 179.95; in.ceiling = 6; in.offered = vfgMask();
    in.baseMs = 4.0; in.costs = &costs; in.now100ns = 1000000000;
    CHECK(mode.update(in).multiplier == 6u);
    in.contentFps = 59.94;
    for (int i = 0; i < 20; ++i) { in.now100ns += 166667; mode.update(in); }
    CHECK(mode.applied() == 3u && mode.reason() == PushSmoothReason::AtCeiling);
    in.contentFps = 29.97;
    const std::int64_t back = in.now100ns;
    unsigned m = 3;
    while (m != 6u && in.now100ns - back < 100000000) { in.now100ns += 333667; m = mode.update(in).multiplier; }
    CHECK(m == 6u);
    CHECK(in.now100ns - back >= 5000000 && in.now100ns - back < 10000000);   // the 0.5 s gate (one pair late at most)
    CHECK(mode.stepDowns() == 0u && mode.stepUps() == 0u);
}

TEST(smooth_mode_never_sticks_at_2x_after_one_high_reading_without_costs){
    using namespace smooth_tests;
    // Review case: one content reading of 61.5 (184.5 at 3X is above 180 x 1.02) with no costs measured yet: 2X at
    // once, and back to 3X after the gate. It stayed at 2X "hold" because the way up waited for modelled headroom.
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 180; in.contentFps = 60.0; in.refreshHz = 180; in.ceiling = 6; in.offered = vfgMask();
    in.now100ns = 1000000000;
    CHECK(mode.update(in).multiplier == 3u);
    in.contentFps = 61.5; in.now100ns += 166667;
    CHECK(mode.update(in).multiplier == 2u);
    in.contentFps = 60.0;
    for (int i = 0; i < 12; ++i) { in.now100ns += 166667; mode.update(in); }
    CHECK(mode.applied() == 2u);                       // 0.2 s: still in the gate
    for (int i = 0; i < 30; ++i) { in.now100ns += 166667; mode.update(in); }
    CHECK(mode.applied() == 3u);                       // 0.7 s: back
}

TEST(smooth_mode_without_costs_recovers_from_rejections_after_quiet_time){
    using namespace smooth_tests;
    // Trouble without any cost measured (admission rejected pairs): after 5 s without trouble and 10 s after the step
    // down it comes back one level; it never stays at the lower level for good.
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 180; in.contentFps = 60.0; in.refreshHz = 180; in.ceiling = 6; in.offered = vfgMask();
    in.now100ns = 1000000000;
    mode.update(in);
    for (int i = 0; i < 6; ++i) { in.now100ns += 2500000; in.admitted += 10; in.rejected += 5; mode.update(in); }
    CHECK(mode.applied() == 2u && mode.troubleCap() == 2u);
    unsigned m = 2;
    for (int i = 0; i < 80 && m == 2u; ++i) { in.now100ns += 2500000; in.admitted += 15; m = mode.update(in).multiplier; }
    CHECK(m == 3u && mode.reason() == PushSmoothReason::Recovered && mode.troubleCap() == 0u);
}

TEST(smooth_mode_does_not_retry_a_failed_level_after_each_content_flip){
    using namespace smooth_tests;
    // Review case: 6X is too heavy for a 30 fps game (4 + 5 x 5.6 = 32 ms of 33.4: load 0.96), 5X is not (0.79); the
    // game flips between 30 and 60 six times in two minutes. 6X must not come back after each flip: the limit is kept
    // for that content rate.
    PushSmoothnessMode mode;
    PushSmoothInput in;
    PushSmoothCostModel costs = perFrame(5.6);
    in.targetFps = 180; in.contentFps = 29.97; in.refreshHz = 179.95; in.ceiling = 6; in.offered = vfgMask();
    in.baseMs = 4.0; in.costs = &costs; in.now100ns = 1000000000;
    CHECK(mode.update(in).multiplier == 6u);
    for (int i = 0; i < 40; ++i) { in.now100ns += 333667; mode.update(in); }
    CHECK(mode.applied() == 5u && mode.stepDowns() == 1u && mode.troubleCap() == 5u);
    unsigned highest = 0;
    for (int flip = 0; flip < 6; ++flip) {
        in.contentFps = flip % 2 == 0 ? 59.94 : 29.97;
        for (int i = 0; i < 20 * 30; ++i) {          // 20 s per state
            in.now100ns += 333667;
            const unsigned m = mode.update(in).multiplier;
            if (in.contentFps < 40) highest = std::max(highest, m);
        }
        CHECK(mode.applied() == (in.contentFps > 40 ? 3u : 5u));
    }
    CHECK(highest == 5u && mode.stepDowns() == 1u);
}

TEST(smooth_mode_and_load_ignore_a_non_finite_content_rate){
    using namespace smooth_tests;
    // Review bounds case: contentFps = +inf must read as unmeasured, never as a load (static_cast<int64_t>(inf) is UB).
    PushSmoothnessMode mode;
    PushSmoothInput in;
    PushSmoothCostModel costs = perFrame(2.0);
    in.targetFps = 180; in.contentFps = std::numeric_limits<double>::infinity(); in.refreshHz = 180; in.ceiling = 6;
    in.offered = vfgMask(); in.baseMs = 4.0; in.costs = &costs; in.now100ns = 1000000000;
    auto d = mode.update(in);
    CHECK(d.multiplier == 0u && d.reason == PushSmoothReason::Unmeasured && d.load == -1);
    in.contentFps = std::numeric_limits<double>::quiet_NaN();
    CHECK(mode.update(in).multiplier == 0u);
    in.contentFps = 60.0;
    d = mode.update(in);
    CHECK(d.multiplier == 3u && std::isfinite(d.load));
    in.contentFps = std::numeric_limits<double>::infinity();
    d = mode.update(in);
    CHECK(d.multiplier == 3u && d.load == -1);         // keeps what it had, no load
    CHECK(pushSmoothLoad(3, std::numeric_limits<double>::infinity(), 4.0, &costs, 0.5) < 0);
    CHECK(pushSmoothLoad(3, 60.0, std::numeric_limits<double>::infinity(), &costs, 0.5) < 0);
    CHECK(pushSmoothCapMultiplier(6, std::numeric_limits<double>::infinity(), 180.0, vfgMask()) == 6u);
    CHECK(pushSmoothPreferred(180, std::numeric_limits<double>::infinity(), 0, 180, 6, vfgMask()) == 0u);
}

TEST(smooth_cost_model_has_a_fixed_part_and_a_per_frame_part){
    using veyra::engine::PushSmoothCostModel;
    PushSmoothCostModel c;
    CHECK(c.predict(2) < 0);
    // One level measured: all per-frame (2X group of 1 frame at 6 ms -> 2 frames 12 ms): too high, which only delays.
    c.record(1, 6.0);
    CHECK(std::abs(c.predict(2) - 12.0) < 1e-9 && c.predict(0) == 0.0);
    // Two levels: flow once per pair (4 ms) + 2 ms per frame: 1 frame 6 ms, 3 frames 10 ms -> 2 frames 8, 5 frames 14.
    c.record(3, 10.0);
    CHECK(std::abs(c.predict(2) - 8.0) < 1e-9);
    CHECK(std::abs(c.predict(5) - 14.0) < 1e-9);
    // A measured level is used as measured; a later sample moves it by a tenth.
    c.record(3, 20.0);
    CHECK(std::abs(c.measured(3) - 11.0) < 1e-9);
    // Out of range and invalid samples are ignored.
    c.record(0, 1.0); c.record(40, 1.0); c.record(2, -1.0);
    CHECK(c.measured(2) < 0 && c.predict(40) < 0);
    // The load: base 4 + group(2 frames) 8.something + blit 0.5 over 16.67 ms.
    const double load = veyra::engine::pushSmoothLoad(3, 60.0, 4.0, &c, 0.5);
    CHECK(load > 0.5 && load < 1.2);
    CHECK(veyra::engine::pushSmoothLoad(3, 60.0, -1.0, &c, 0.5) < 0);
    CHECK(veyra::engine::pushSmoothLoad(3, 60.0, 4.0, nullptr, 0.5) < 0);
}

TEST(smooth_window_line_carries_sizes_rates_and_stage_times){
    using namespace smooth_tests;
    PushSmoothWindow w;
    PushSmoothCounters c;
    w.restart(0, c);
    CHECK(!w.due(49999999) && w.due(50000000));
    for (int i = 0; i < 300; ++i) {
        w.pair(i < 150 ? 3u : 4u, 59.94);
        w.stage(PushSmoothStage::Span, 12.0 + (i % 10));
        w.stage(PushSmoothStage::Fg, 8.0);
        w.stage(PushSmoothStage::Sr, 2.0);
    }
    c.received = 300; c.unique = 300; c.evaluated = 280; c.rejected = 20; c.generated = 700; c.dropCap = 250;
    PushSmoothConfigInfo info;
    info.backend = "vfg"; info.ceiling = 6; info.sourceW = 1920; info.sourceH = 1080; info.workW = 3840; info.workH = 2160;
    info.displayW = 2560; info.displayH = 1440; info.srTarget = 1; info.srTargetUser = 1;
    PushSmoothLineInputs x;
    x.capFps = 179.95; x.refreshHz = 179.95; x.vrr = 1; x.why = "cap";
    const std::string line = pushSmoothLine(w, info, c, 50000000, x);
    CHECK(contains(line, "window=5.0 backend=vfg ceiling=6X chosen=4X chosenMin=3X chosenMax=4X changes=1 why=cap"));
    CHECK(contains(line, " work=3840x2160 ") && contains(line, " display=2560x1440 "));
    CHECK(contains(line, " uniqueFps=60.00 ") && contains(line, " rejectedPairsFps=4.00 ") && contains(line, " dropCapFps=50.00 "));
    CHECK(contains(line, " gpuFgP50Ms=8.000 ") && contains(line, " gpuSrP95Ms=2.000 ") && contains(line, " gpuCleanP50Ms=-1.000 "));
    CHECK(contains(line, " gpuSpanP95Ms=21.000 ") && contains(line, " gpuFrames=300 "));
    CHECK(contains(line, " pairBudgetMs=16.683 "));
    // 300 spans averaging 16.5 ms over 5 s = 99 %.
    CHECK(contains(line, " gpuLoadPct=99.0 "));
    CHECK(contains(line, " smooth=0 smoothReason=off "));
}

TEST(smooth_steady_settling_needs_two_failed_probes_before_a_hold){
    using veyra::engine::FgCostSettling;
    const std::int64_t pair = 166667;
    // Default: one failed probe after an idle second holds generation off for 2 s (unchanged rule).
    {
        FgCostSettling s; std::int64_t t = 100000000;
        CHECK(!s.observe(false, true, t));
        t += FgCostSettling::kIdle100ns;
        CHECK(s.observe(false, true, t)); CHECK(s.observe(false, true, t += pair));
        s.observe(true, false, t += pair);
        CHECK(s.holding(t + 1) && s.holdUntil() == t + FgCostSettling::kRetry100ns);
    }
    // Steady: the first failure starts a second probe at once; the second failure in a row holds.
    {
        FgCostSettling s; s.setSteady(true); std::int64_t t = 100000000;
        CHECK(!s.observe(false, true, t));
        t += FgCostSettling::kIdle100ns;
        CHECK(s.observe(false, true, t)); CHECK(s.observe(false, true, t += pair));
        s.observe(true, false, t += pair);
        CHECK(!s.holding(t + 1));
        CHECK(s.observe(false, true, t += pair));   // the next evaluation is a warm-up again (a new probe)
        CHECK(s.observe(false, true, t += pair));
        s.observe(true, false, t += pair);
        CHECK(s.holding(t + 1));
        // reset() keeps the option (a settings revision must not turn the experiment off).
        s.reset();
        t += FgCostSettling::kIdle100ns * 4;
        CHECK(!s.observe(false, true, t));
        t += FgCostSettling::kIdle100ns;
        s.observe(false, true, t); s.observe(false, true, t += pair);
        s.observe(true, false, t += pair);
        CHECK(!s.holding(t + 1));
    }
}

TEST(smooth_mode_names_a_target_that_is_not_a_whole_multiple){
    using namespace smooth_tests;
    // Stream "fps": 90 for a 60 fps game is between 1X and 2X; whole multipliers give 120 (2X). The mode runs 2X and
    // says the target is not a multiple, instead of "the game is already close to the target".
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 90; in.contentFps = 59.94; in.refreshHz = 119.998; in.ceiling = 4; in.offered = vfgMask();
    in.now100ns = 1000000000;
    auto d = mode.update(in);
    CHECK(d.multiplier == 2u && d.preferred == 0u && d.reason == PushSmoothReason::TargetNotMultiple);
    CHECK(pushSmoothReasonName(d.reason) == "target-not-a-multiple");
    // A 30 fps game at 90: 3X, even.
    PushSmoothnessMode thirty;
    in.contentFps = 29.97;
    d = thirty.update(in);
    CHECK(d.multiplier == 3u && d.reason == PushSmoothReason::Start);
    // A 60 target for a 60 fps game: the game already fills it (the old reason stays).
    PushSmoothnessMode full;
    in.targetFps = 60; in.contentFps = 59.94;
    d = full.update(in);
    CHECK(d.multiplier == 2u && d.reason == PushSmoothReason::TargetBelow2x);
    // A 90 cap with a 120 target and a 60 fps game: the cap bounds the target, still not a multiple.
    PushSmoothnessMode capped;
    in.targetFps = 120; in.capFps = 90;
    d = capped.update(in);
    CHECK(d.multiplier == 2u && d.reason == PushSmoothReason::TargetNotMultiple);
}

TEST(smooth_cap_reduction_only_lowers_the_existing_rule){
    using namespace smooth_tests;
    // The owner's panel at 119.998 Hz (Package 9 logs) with a measured 59.94 content rate: the existing rule generates
    // 3X for a 120 cap; the default since stream "fps" runs 2X.
    CHECK(pushSmoothCapCeilRule(4, 59.94, 119.998) == 3u);
    CHECK(pushSmoothCapReduction(4, 59.94, 119.998, vfgMask()) == 2u);
    CHECK(pushSmoothCapCeilRule(6, 59.94, 179.95) == 4u);
    CHECK(pushSmoothCapReduction(6, 59.94, 179.95, vfgMask()) == 3u);
    // Where the old rule was already right, nothing changes.
    CHECK(pushSmoothCapReduction(4, 60.0, 119.998, vfgMask()) == pushSmoothCapCeilRule(4, 60.0, 119.998));
    CHECK(pushSmoothCapReduction(6, 29.97, 179.95, vfgMask()) == pushSmoothCapCeilRule(6, 29.97, 179.95));
    CHECK(pushSmoothCapReduction(4, 59.94, 90.0, vfgMask()) == 2u);
    // Never above the existing rule, also where the backend lacks the fitting multiplier (DLSS: no 5X).
    CHECK(pushSmoothCapCeilRule(6, 36.0, 180.0) == 5u && pushSmoothCapReduction(6, 36.0, 180.0, dlssMask()) == 5u);
    for (double content = 20.0; content <= 70.0; content += 0.37)
        for (double cap : {60.0, 90.0, 119.998, 143.9, 179.95, 239.8})
            for (unsigned ceiling = 2; ceiling <= 8; ++ceiling) {
                CHECK(pushSmoothCapReduction(ceiling, content, cap, vfgMask()) <= pushSmoothCapCeilRule(ceiling, content, cap));
                CHECK(pushSmoothCapReduction(ceiling, content, cap, vfgMask()) >= 2u || ceiling < 2);
            }
    // Unknown rates: the ceiling (the engine then keeps the user's multiplier, as before).
    CHECK(pushSmoothCapReduction(4, 0.0, 120.0, vfgMask()) == 4u);
    CHECK(pushSmoothCapReduction(4, std::numeric_limits<double>::infinity(), 120.0, vfgMask()) == 4u);
}

TEST(pipeline_hook_cap_key_for_the_fps_arms){
    // Stream "fps": the kit's 90 FPS arm sets the output cap through VEYRA_TEST_RP_PIPELINE (cap=display|off|30..360).
    using namespace veyra::engine;
    CHECK(pipeHookBaseKey("cap"));
    CHECK(pipeHookBaseValueOk("cap", "90") && pipeHookBaseValueOk("cap", "display") && pipeHookBaseValueOk("cap", "off"));
    CHECK(pipeHookBaseValueOk("cap", "30") && pipeHookBaseValueOk("cap", "360"));
    CHECK(!pipeHookBaseValueOk("cap", "29") && !pipeHookBaseValueOk("cap", "361") && !pipeHookBaseValueOk("cap", "fast") && !pipeHookBaseValueOk("cap", ""));
}

TEST(smooth_cap_reduction_under_vrr_stays_within_the_cap){
    using namespace smooth_tests;
    // Stream sync's finding (Package 9): the console sent 57.5-59.9 FPS. Under a 120 cap with VRR, 2X (115, shown as it
    // comes) instead of 3X (172.5 generated, 52.5 thrown away); without VRR the tolerant rule (3X) stands.
    CHECK(pushSmoothCapReduction(4, 57.5, 119.998, vfgMask(), true) == 2u);
    CHECK(pushSmoothCapReduction(4, 57.5, 119.998, vfgMask(), false) == 3u);
    CHECK(pushSmoothCapReduction(4, 59.94, 119.998, vfgMask(), true) == 2u);
    CHECK(pushSmoothCapReduction(6, 57.5, 179.95, vfgMask(), true) == 3u);
    CHECK(pushSmoothCapReduction(6, 29.97, 179.95, vfgMask(), true) == 6u);
    CHECK(pushSmoothCapReduction(6, 36.0, 180.0, dlssMask(), true) == 4u);   // DLSS 4X = 144 within 180; 6X = 216 is not
    // Never above the existing rule, never below 2X.
    for (double content = 20.0; content <= 70.0; content += 0.37)
        for (double cap : {60.0, 90.0, 119.998, 179.95})
            for (unsigned ceiling = 2; ceiling <= 8; ++ceiling) {
                const unsigned m = pushSmoothCapReduction(ceiling, content, cap, vfgMask(), true);
                CHECK(m <= pushSmoothCapCeilRule(ceiling, content, cap) && m >= 2u);
            }
}

TEST(smooth_cap_fit_state_forgets_on_revision_and_skip){
    using veyra::engine::PushCapFitState;
    PushCapFitState s;
    std::int64_t t = 1000000000;
    // First pair: the lowered value at once.
    CHECK(s.apply(7, 3, 2, t) == 2u);
    // The fit rises (VRR lost, say): held for 0.5 s, never above the existing rule.
    CHECK(s.apply(7, 3, 3, t += 1000000) == 2u);
    CHECK(s.apply(7, 3, 3, t += 3000000) == 2u);
    CHECK(s.apply(7, 3, 3, t += 2000000) == 3u);
    // The existing rule drops under the held value: the minimum, at once.
    CHECK(s.apply(7, 2, 3, t += 100000) == 2u);
    // A settings revision: the held value is forgotten, the new pair's value applies at once.
    CHECK(s.apply(7, 4, 4, t += 100000) == 3u);   // still held at 3 (the rise to 4 is pending)
    CHECK(s.apply(8, 4, 4, t += 100000) == 4u);   // new revision: at once
    // The cap block skipped for a pair (no cap, adaptive, ...): forgotten; the next pair applies at once.
    s.skipped();
    CHECK(!s.held());
    CHECK(s.apply(8, 3, 2, t += 100000) == 2u);
}

TEST(smooth_cap_fit_switch_never_undoes_the_default){
    using veyra::engine::pushSmoothCapFitSwitch;
    // Review case: 57.5 FPS under a 120 cap with VRR: the default chose 2X, the old switch's gate 3X -> 2X stays.
    CHECK(pushSmoothCapFitSwitch(2, 3) == 2u);
    CHECK(pushSmoothCapFitSwitch(3, 2) == 2u);
    CHECK(pushSmoothCapFitSwitch(4, 4) == 4u);
}

TEST(smooth_mode_close_target_is_not_called_a_non_multiple){
    using namespace smooth_tests;
    // Review: a 60 target with a 57.5 FPS source is "close to the target", not "not a multiple".
    PushSmoothnessMode mode;
    PushSmoothInput in;
    in.targetFps = 60; in.contentFps = 57.5; in.refreshHz = 119.998; in.ceiling = 4; in.offered = vfgMask();
    in.now100ns = 1000000000;
    CHECK(mode.update(in).reason == PushSmoothReason::TargetBelow2x);
    PushSmoothnessMode ninety;
    in.targetFps = 90;
    CHECK(ninety.update(in).reason == PushSmoothReason::TargetNotMultiple);
}

// Package 16 ("Lower delay" with Smoothness Mode): a predicted pair (one VFG run + the predicted frame) is its own level,
// never blended into the interpolated groups, and is used only while the predicted schedule runs.
TEST(smooth_cost_model_keeps_predicted_pairs_apart) {
    using namespace veyra::engine;
    PushSmoothCostModel m;
    m.record(2, 4.4);
    m.recordPredicted(2, 9.0);
    CHECK(m.measured(2) == 4.4 && m.measuredPredicted(2) == 9.0);
    CHECK(m.predict(2) == 4.4);            // the schedule does not run: interpolated groups
    m.preferPredicted(true);
    CHECK(m.predict(2) == 9.0);            // it runs: what 3X costs now
    CHECK(m.predict(1) > 0 && m.measuredPredicted(1) < 0);   // other levels: unchanged rules
    PushSmoothCostModel n;
    n.preferPredicted(true);
    n.record(2, 5.0);
    CHECK(n.predict(2) == 5.0);            // nothing predicted measured yet: the interpolated time
    m.reset();
    CHECK(m.measuredPredicted(2) < 0 && m.measured(2) < 0);
}
