// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushSizes.h (stream "delay90"): the [slipstream-sizes] line, fit= / fitReason=, and part (a) of
// VEYRA_EXP_RP_FIT_DISPLAY against pipeline::ResolutionPlan. Sizes are examples; nothing here is a measurement.
#include "veyra/engine/PushSizes.h"
#include "veyra/engine/PushFgLatency.h"
#include "veyra/pipeline/ResolutionPlan.h"
#include <string>

namespace sizes_tests {
using namespace veyra::engine;
using veyra::pipeline::ResolutionPlan;
using veyra::pipeline::SrTarget;
using veyra::pipeline::NrSizePolicy;
inline bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
inline PushWxH wxh(const veyra::pipeline::Extent& e) { return {e.width, e.height}; }
// The live plan the engine builds (EngineController: measured.resolution) with the target the switches leave.
inline ResolutionPlan livePlan(std::uint32_t w, std::uint32_t h, bool sr, SrTarget asked, bool follow, std::uint32_t dw, std::uint32_t dh) {
    const auto used = SrTarget(pushFitDisplayTarget(follow, std::uint32_t(asked), dw, dh));
    return ResolutionPlan::make({w, h}, sr, NrSizePolicy::Native, false, 0, used);
}
inline PushFitVerdict verdictOf(const ResolutionPlan& p, bool sr, SrTarget asked, bool follow, std::uint32_t dw, std::uint32_t dh, bool dual = false) {
    PushFitInputs in;
    in.srInput = wxh(p.srInput);
    in.srEvaluated = wxh(p.srOutput);
    in.work = wxh(p.fg);
    in.display = {dw, dh};
    in.sr = sr;
    in.dualSr = dual;
    in.followDisplay = follow;
    in.targetAsked = sr ? int(asked) : -1;
    in.targetUsed = sr ? int(pushFitDisplayTarget(follow, std::uint32_t(asked), dw, dh)) : -1;
    return pushFitVerdict(in);
}
}

TEST(sizes_fit_display_16x9_remote_play_sr_to_4k_works_at_1440p){
    using namespace sizes_tests;
    // Remote Play 1080p, SR target 4K (the default), a 2560x1440 panel. Switch off: NR, FG and the output work at 4K.
    const auto off = livePlan(1920, 1080, true, SrTarget::Uhd4K, false, 2560, 1440);
    CHECK((off.fg == veyra::pipeline::Extent{3840, 2160}) && off.output == off.fg && off.base == off.fg && off.nr == off.fg);
    const auto vOff = verdictOf(off, true, SrTarget::Uhd4K, false, 2560, 1440);
    CHECK(!vOff.fit && vOff.reason == PushFitReason::AsSet);
    // Switch on: the target is lowered to 1440p and every later stage works at the display's size.
    const auto on = livePlan(1920, 1080, true, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK((on.fg == veyra::pipeline::Extent{2560, 1440}) && on.output == on.fg && on.nr == on.fg && on.srApplied);
    const auto vOn = verdictOf(on, true, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK(vOn.fit && vOn.reason == PushFitReason::SrLowered);
    // 8K asked: lowered to 1440p as well (the smallest box that covers).
    CHECK((livePlan(1920, 1080, true, SrTarget::Uhd8K, true, 2560, 1440).fg == veyra::pipeline::Extent{2560, 1440}));
    // A target already at 1440p stays (never raised); reason within-display.
    const auto qhd = livePlan(1920, 1080, true, SrTarget::Qhd, true, 2560, 1440);
    CHECK((qhd.fg == veyra::pipeline::Extent{2560, 1440}));
    CHECK(verdictOf(qhd, true, SrTarget::Qhd, true, 2560, 1440).reason == PushFitReason::WithinDisplay);
}

TEST(sizes_fit_display_21x9_keeps_the_smallest_box_that_covers){
    using namespace sizes_tests;
    // 3440x1440: the 1440p box (2560 wide) does not cover it, so 4K stays; 8K is lowered to 4K.
    const auto p = livePlan(1920, 1080, true, SrTarget::Uhd4K, true, 3440, 1440);
    CHECK((p.fg == veyra::pipeline::Extent{3840, 2160}));
    const auto v = verdictOf(p, true, SrTarget::Uhd4K, true, 3440, 1440);
    CHECK(v.fit && v.reason == PushFitReason::SrSmallestBox);
    const auto p8 = livePlan(1920, 1080, true, SrTarget::Uhd8K, true, 3440, 1440);
    CHECK((p8.fg == veyra::pipeline::Extent{3840, 2160}));
    CHECK(verdictOf(p8, true, SrTarget::Uhd8K, true, 3440, 1440).reason == PushFitReason::SrLowered);
}

TEST(sizes_fit_display_uses_the_display_not_a_smaller_window){
    using namespace sizes_tests;
    // A 1280x720 window on a 2560x1440 monitor: the engine passes the MONITOR's size (pushSmoothMonitorSize), so the
    // target is 1440p, not lower; the client size only appears in the line.
    const auto p = livePlan(1920, 1080, true, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK((p.fg == veyra::pipeline::Extent{2560, 1440}));
    PushSizesFacts f;
    f.client = {1280, 720};
    f.display = {2560, 1440};
    f.fg = wxh(p.fg);
    f.verdict = verdictOf(p, true, SrTarget::Uhd4K, true, 2560, 1440);
    const auto line = pushSizesLine(f);
    CHECK(has(line, " client=1280x720") && has(line, " display=2560x1440") && has(line, " fit=1 fitReason=sr-lowered"));
}

TEST(sizes_fit_display_unknown_display_changes_nothing){
    using namespace sizes_tests;
    CHECK(pushFitDisplayTarget(true, 1, 0, 0) == 1);
    CHECK(pushFitDisplayTarget(true, 1, 2560, 0) == 1);
    const auto p = livePlan(1920, 1080, true, SrTarget::Uhd4K, true, 0, 0);
    CHECK((p.fg == veyra::pipeline::Extent{3840, 2160}));
    const auto v = verdictOf(p, true, SrTarget::Uhd4K, true, 0, 0);
    CHECK(!v.fit && v.reason == PushFitReason::DisplayUnknown);
    // Switch off: the user's target whatever the display.
    CHECK(pushFitDisplayTarget(false, 2, 2560, 1440) == 2);
}

TEST(sizes_fit_display_4k_capture_is_reported_not_reduced){
    using namespace sizes_tests;
    // A 4K capture without SR on a 1440p panel: nothing reduces it (4K is kept), and the line says so as a fact.
    const auto p = livePlan(3840, 2160, false, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK((p.fg == veyra::pipeline::Extent{3840, 2160}) && p.output == p.fg);
    const auto v = verdictOf(p, false, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK(!v.fit && v.reason == PushFitReason::SourceSize);
    PushSizesFacts f;
    f.source = {3840, 2160};
    f.fg = f.output = f.base = {3840, 2160};
    f.display = f.mode = {2560, 1440};
    f.modeHz = 180;
    f.fitSwitch = true;
    f.verdict = v;
    const auto line = pushSizesLine(f);
    CHECK(has(line, "source=3840x2160") && has(line, " fit=0 fitReason=source-size") && !has(line, "advice"));
    CHECK(has(line, " mode=2560x1440 modeHz=180") && has(line, " fitDisplay=1"));
    // A 1440p capture works at the display's size.
    const auto q = livePlan(2560, 1440, false, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK(verdictOf(q, false, SrTarget::Uhd4K, true, 2560, 1440).reason == PushFitReason::WithinDisplay);
    // A 4K source with SR on: SR does not enlarge it, so the same reason.
    const auto r = livePlan(3840, 2160, true, SrTarget::Uhd4K, true, 2560, 1440);
    CHECK((r.fg == veyra::pipeline::Extent{3840, 2160}));
    CHECK(verdictOf(r, true, SrTarget::Uhd4K, true, 2560, 1440).reason == PushFitReason::SourceSize);
}

TEST(sizes_fit_display_double_sr_is_left_alone){
    using namespace sizes_tests;
    const auto p = livePlan(1920, 1080, true, SrTarget::Uhd4K, false, 2560, 1440);
    const auto v = verdictOf(p, true, SrTarget::Uhd4K, true, 2560, 1440, true);
    CHECK(!v.fit && v.reason == PushFitReason::DoubleSr);
}

TEST(sizes_line_lists_every_size_and_marks_unknown_ones){
    using namespace sizes_tests;
    PushSizesFacts f;
    f.source = {1920, 1080};
    f.srInput = {1920, 1080};
    f.srEvaluated = {3840, 2160};
    f.base = f.nr = f.fg = f.output = {3840, 2160};
    f.flow = {1920, 1080};
    f.backBuffer = {2560, 1440};
    f.client = {2560, 1440};
    f.display = {2560, 1440};
    f.sr = true;
    f.srTarget = f.srTargetAsked = 1;
    f.verdict = {false, PushFitReason::AsSet};
    const auto line = pushSizesLine(f);
    CHECK(line.rfind("source=1920x1080 srInput=1920x1080 srEvaluated=3840x2160 base=3840x2160 nr=3840x2160 flow=1920x1080 fg=3840x2160 output=3840x2160 backBuffer=2560x1440 client=2560x1440 display=2560x1440 mode=- modeHz=0 sr=1 srTarget=1 srTargetAsked=1", 0) == 0);
    CHECK(has(line, " fit=0 fitReason=as-set"));
    for (const char* key : {"source=", "srInput=", "srEvaluated=", "base=", "nr=", "flow=", "fg=", "output=", "backBuffer=", "client=", "display=", "mode=", "fit=", "fitReason="})
        CHECK(has(line, key));
}

TEST(sizes_gate_looks_once_a_second_and_writes_only_changes){
    using veyra::engine::PushSizesGate;
    PushSizesGate g;
    CHECK(g.due(100));
    CHECK(!g.due(100 + 5000000));             // half a second later: not looked at
    CHECK(g.due(100 + PushSizesGate::kPeriod100ns));
    CHECK(g.changed("a"));                    // the first line is written (the graph was built)
    CHECK(!g.changed("a"));                   // the same values: nothing
    CHECK(g.changed("b"));                    // a value changed
    g.reset();
    CHECK(g.due(7) && g.changed("b"));        // a new session writes again
    CHECK(g.due(1));                          // a clock that went back looks again rather than never
}

TEST(delay90_latency_window_splits_a_real_frame_into_parts){
    using veyra::engine::PushFgLatencyWindow;
    // A 30 FPS game at 3X (example values, not measurements): T = 33.4 ms, so (M-1)/M x T = 22.267 ms. Picked up 0.6 ms
    // after its callback, ready 4.0 ms after it, first generated frame seen ready 8.5 ms after it, deadline 31.267 ms
    // (margin 0.5 ms), Present call 0.1 ms after the deadline.
    PushFgLatencyWindow w;
    const double interp = 2.0 / 3.0 * 33.4;
    for (int i = 0; i < 90; ++i) {
        const double phase = 8.5 + interp + 0.5, late = i % 10 == 0 ? 0.9 : 0.1;
        w.real(-1.0, phase - 4.0, phase, 8.5, true, 2u, false);
        w.parts(true, 0.6, 4.0, 8.5, interp, phase, late, phase + late);
        w.presented(late);
    }
    CHECK(w.splitFrames() == 90);
    CHECK(w.percentile(PushFgLatencyWindow::Metric::Base, 50) == 3.4);
    CHECK(w.percentile(PushFgLatencyWindow::Metric::Fg1, 50) == 4.5);
    const std::string f = w.fields(3.0, true);
    CHECK(f.find(" pickupWaitP50Ms=0.600 pickupWaitP95Ms=0.600 baseObsP50Ms=3.400 baseObsP95Ms=3.400 fg1ObsP50Ms=4.500 fg1ObsP95Ms=4.500 interpWaitP50Ms=22.267 marginP50Ms=0.500 realLateP50Ms=0.100") != std::string::npos);
    // The six parts of one frame add up to its Present call: 0.6 + 3.4 + 4.5 + 22.267 + 0.5 + 0.1 = 31.367.
    CHECK(f.find(" partsSumP50Ms=31.367 delayToCallP50Ms=31.367 partsFps=30.000") != std::string::npos);
    // The earlier fields stay first and unchanged in order.
    CHECK(f.find(" fallbackFps=") < f.find(" pickupWaitP50Ms="));
    // A negative margin (the phase too short for the first generated frame) is kept, not dropped.
    w.clear();
    w.parts(true, 0.5, 3.0, 9.0, 22.0, 29.0, 0.0, 29.0);
    CHECK(w.fields(1.0, false).find(" marginP50Ms=-2.000 ") != std::string::npos);
    // Not split: off the FG phase, no generated frame seen, or times out of order; then every part is unknown (-1).
    w.clear();
    w.parts(false, 0.5, 3.0, 8.0, 22.0, 31.0, 0.1, 31.1);
    w.parts(true, 0.5, 3.0, -1.0, 22.0, 31.0, 0.1, 31.1);
    w.parts(true, 0.5, 6.0, 5.0, 22.0, 31.0, 0.1, 31.1);
    w.parts(true, 0.5, 3.0, 8.0, -1.0, 31.0, 0.1, 31.1);
    CHECK(w.splitFrames() == 0);
    const std::string g = w.fields(1.0, false);
    CHECK(g.find(" partsSumP50Ms=-1.000 ") != std::string::npos && g.find(" fg1ObsP50Ms=-1.000 ") != std::string::npos);
}
