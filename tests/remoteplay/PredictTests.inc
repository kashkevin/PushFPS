// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushPredict.h (stream "predict"): the switch, per-pair eligibility, the plan of one predicted
// pair, timestamps, the flow size, the per-pixel rule and the whole-frame confidence. Values are examples.
#include "veyra/pipeline/PushPredict.h"
#include <cmath>
#include <cstdint>
#include <string_view>

namespace predict_tests {
using namespace veyra::pipeline;

TEST(predict_switch_accepts_only_onepredicted) {
    CHECK(pushFgScheduleFromSwitch(std::string_view("onepredicted")) == PushPairSchedule::OnePredicted);
    CHECK(pushFgScheduleFromSwitch(std::string_view("OnePredicted")) == PushPairSchedule::OnePredicted);
    CHECK(pushFgScheduleFromSwitch(std::wstring_view(L"onepredicted")) == PushPairSchedule::OnePredicted);
    CHECK(pushFgScheduleFromSwitch(std::string_view("")) == PushPairSchedule::Interpolate);
    CHECK(pushFgScheduleFromSwitch(std::string_view("1")) == PushPairSchedule::Interpolate);
    CHECK(pushFgScheduleFromSwitch(std::string_view("onepredicted ")) == PushPairSchedule::Interpolate);
    CHECK(pushFgScheduleFromSwitch(std::string_view("twopredicted")) == PushPairSchedule::Interpolate);
}

inline PushPredictEligibility eligible() {
    PushPredictEligibility e;
    e.requested = PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.historyValid = true;
    e.formatSupported = true; e.passReady = true;
    return e;
}

TEST(predict_pair_refusals_in_order) {
    CHECK(pushPredictPairRefusal(eligible()) == PushPredictRefusal::Ok);
    auto e = eligible(); e.requested = PushPairSchedule::Interpolate;
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::SwitchOff);
    e = eligible(); e.vfg = false;          CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NotVfg);
    e = eligible(); e.multiplier = 2;       CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NotThreeX);
    e = eligible(); e.multiplier = 4;       CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NotThreeX);
    e = eligible(); e.historyValid = false; CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NoHistory);
    e = eligible(); e.hudLock = true;       CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::HudLockOrExclusion);
    e = eligible(); e.exclusion = true;     CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::HudLockOrExclusion);
    e = eligible(); e.presentSinkFg = true; CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::PresentSinkFg);
    e = eligible(); e.formatSupported = false; CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::UnsupportedFormat);
    e = eligible(); e.passReady = false;    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::PassUnavailable);
    e = eligible(); e.confident = false;    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::LowConfidence);
    // The switch wins over everything: unset never reports another reason (nothing else is looked at).
    e = PushPredictEligibility{}; e.vfg = false; e.multiplier = 0;
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::SwitchOff);
    CHECK(std::string_view(pushPredictRefusalName(PushPredictRefusal::LowConfidence)) == "low-confidence");
}

TEST(predict_pass_is_created_only_with_the_switch) {
    for (int bits = 0; bits < 64; ++bits) {
        const bool fg = (bits & 1) != 0, vfg = (bits & 2) != 0, other = (bits & 4) != 0, sink = (bits & 8) != 0, fmt = (bits & 16) != 0;
        const unsigned m = (bits & 32) ? 3u : 2u;
        CHECK(!pushPredictCreate(PushPairSchedule::Interpolate, fg, vfg, other, sink, m, fmt));   // unset: never
        CHECK(pushPredictCreate(PushPairSchedule::OnePredicted, fg, vfg, other, sink, m, fmt) == (fg && vfg && !other && !sink && m >= 3 && fmt));
    }
    CHECK(pushPredictCreate(PushPairSchedule::OnePredicted, true, true, false, false, 4, true));   // a 4X ceiling (adaptive) still creates it
}

// Review 2, SHOULD 9: after VFG refused or failed a FrameIndex-2-only request, the mode is unavailable for the session:
// a static reason (never asked again), checked after the pass exists, before any per-pair reason.
TEST(predict_vfg_refusal_is_static_for_the_session) {
    PushPredictEligibility e;
    e.requested = PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.formatSupported = true; e.passReady = true;
    e.vfgRefused = true;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::VfgRefused);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::VfgRefused);
    e.historyValid = true;
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::VfgRefused);
    e.passReady = false;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::PassUnavailable);   // no pass at all comes first
    CHECK(std::string_view(pushPredictRefusalName(PushPredictRefusal::VfgRefused)) == "vfg-refused");
}

// Review 2, SHOULD 4: Smoothness Mode is an engine-side static reason with its own name; the graph never returns it.
TEST(predict_smoothness_mode_reason_is_named_and_engine_only) {
    CHECK(std::string_view(pushPredictRefusalName(PushPredictRefusal::SmoothnessMode)) == "smoothness-mode");
    PushPredictEligibility e;
    e.requested = PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.formatSupported = true; e.passReady = true;
    for (int bits = 0; bits < 16; ++bits) {
        e.historyValid = (bits & 1) != 0; e.confident = (bits & 4) != 0; e.hudLock = (bits & 8) != 0;
        CHECK(pushPredictPairRefusal(e) != PushPredictRefusal::SmoothnessMode);
    }
}

// Scheduling review B: static refusals are known before asking; per-pair reasons are not static.
TEST(predict_static_refusal_ignores_per_pair_reasons) {
    PushPredictEligibility e;
    e.requested = PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.formatSupported = true; e.passReady = true;
    e.historyValid = false; e.confident = false;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::Ok);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NoHistory);
    e.hudLock = true;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::HudLockOrExclusion);
    e.hudLock = false; e.formatSupported = false;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::UnsupportedFormat);
    e.formatSupported = true; e.passReady = false;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::PassUnavailable);
    e.requested = PushPairSchedule::Interpolate;
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::SwitchOff);
}

// Package 15: the "Lower delay" row asks for the pass when the graph is built (desc lowDelay), so switching it creates or
// releases the pass in the running session; the kit's switch asks on its own; neither: nothing is asked or created.
TEST(predict_asked_by_the_row_or_the_kit_switch) {
    CHECK(pushPredictAsked(PushPairSchedule::Interpolate, false) == PushPairSchedule::Interpolate);
    CHECK(pushPredictAsked(PushPairSchedule::Interpolate, true) == PushPairSchedule::OnePredicted);
    CHECK(pushPredictAsked(PushPairSchedule::OnePredicted, false) == PushPairSchedule::OnePredicted);
    CHECK(pushPredictAsked(PushPairSchedule::OnePredicted, true) == PushPairSchedule::OnePredicted);
    // Row off, no switch: never created, whatever the rest (Package 14's graph).
    for (unsigned m = 1; m <= 4; ++m)
        CHECK(!pushPredictCreate(pushPredictAsked(PushPairSchedule::Interpolate, false), true, true, false, false, m, true));
}

TEST(predict_plan_uses_vfg_index_2_of_3_and_the_free_slot) {
    for (unsigned parity = 0; parity < 2; ++parity) {
        const auto plan = pushOnePredictedPlan(parity);
        CHECK(plan.vfgMultiplier == 3 && plan.vfgFrameIndex == 2);
        CHECK(plan.interpolatedSlot == parity + 2);   // the slot VFG's FrameIndex 2 already writes today
        CHECK(plan.predictedSlot == parity);          // FrameIndex 1's slot, unused in this pair
        CHECK(plan.interpolatedSlot != plan.predictedSlot);
        CHECK(plan.interpolatedSlot < 4 && plan.predictedSlot < 4);   // kGeneratedPoolSlots at 3X = 2 x (3 - 1)
    }
}

TEST(predict_timestamps_put_p_one_slot_after_b) {
    const std::int64_t a = 1000000, b = a + 333667;   // 33.37 ms
    CHECK(pushInterpolatedPts(a, b, 2, 3) == a + 222445);   // FrameBatch::interpolate's rounding
    CHECK(pushPredictedPts(a, b, 3) == b + 111222);
    const auto after = pushPredictedPts(a, b, 3) - b, before = b - pushInterpolatedPts(a, b, 2, 3);
    CHECK(after - before <= 1 && before - after <= 1);   // even within 100 ns rounding: I, B, P one slot apart
    CHECK(std::abs(pushPredictStep(3) - 1.0f / 3.0f) < 1e-7f);
}

TEST(predict_flow_extent_is_a_quarter_within_limits) {
    const auto k = pushPredictFlowExtent(3840, 2160);
    CHECK(k.width == 960 && k.height == 540);
    const auto q = pushPredictFlowExtent(2560, 1440);
    CHECK(q.width == 640 && q.height == 360);
    const auto t = pushPredictFlowExtent(64, 40);
    CHECK(t.width == 32 && t.height == 32);
    const auto odd = pushPredictFlowExtent(1921, 1081);
    CHECK(odd.width == 481 && odd.height == 271);
}

TEST(predict_pixel_rule) {
    const float w = 3840;
    // Still (HUD, letterbox): B exactly, whatever the flow says.
    CHECK(pushPredictPixelRule(0.0f, 40.0f, 0.0f, false, 1.0f, w) == PushPredictPixel::Still);
    CHECK(pushPredictPixelRule(1.0f / 1023.0f, 40.0f, 1.0f, true, 0.0f, w) == PushPredictPixel::Still);
    // Changed but not moving: held (B).
    CHECK(pushPredictPixelRule(0.1f, 0.1f, 1.0f, true, 0.0f, w) == PushPredictPixel::Held);
    // Trusted motion: warped.
    CHECK(pushPredictPixelRule(0.1f, 12.0f, 0.9f, true, 0.01f, w) == PushPredictPixel::Warped);
    // Each untrusted condition: fallback to B.
    CHECK(pushPredictPixelRule(0.1f, 12.0f, 0.3f, true, 0.01f, w) == PushPredictPixel::Fallback);
    CHECK(pushPredictPixelRule(0.1f, 12.0f, 0.9f, false, 0.01f, w) == PushPredictPixel::Fallback);
    CHECK(pushPredictPixelRule(0.1f, 12.0f, 0.9f, true, 0.5f, w) == PushPredictPixel::Fallback);
    CHECK(pushPredictPixelRule(0.1f, 500.0f, 0.9f, true, 0.01f, w) == PushPredictPixel::Fallback);   // > w / 8
    // NaN motion never warps.
    CHECK(pushPredictPixelRule(0.1f, std::nanf(""), 0.9f, true, 0.01f, w) == PushPredictPixel::Held);
    CHECK(pushPredictPixelRule(0.1f, 12.0f, std::nanf(""), true, 0.01f, w) == PushPredictPixel::Fallback);
    // MUST 2 (review of 24 Nov): a failed flow (trust 0, so GpuDisValidate wrote motion 0) on a changed pixel is a
    // fallback, not held; held needs a trusted "no motion".
    CHECK(pushPredictPixelRule(0.1f, 0.0f, 0.0f, true, 0.0f, w) == PushPredictPixel::Fallback);
    CHECK(pushPredictPixelRule(0.1f, 0.1f, 0.3f, true, 0.0f, w) == PushPredictPixel::Fallback);
    CHECK(pushPredictPixelRule(0.1f, 0.0f, 0.9f, true, 0.0f, w) == PushPredictPixel::Held);
}

// MUST 1 (review of 24 Nov): the flow as GpuDisValidate stores it (v x trust) gives back v, not v x trust.
TEST(predict_trusted_motion_undoes_the_trust_scaling) {
    const float v = -0.75f;   // flow pixels (3 work pixels at a quarter size)
    {   // every texel trust 0.6: exactly v, confidence 0.6 (the old sampling moved 60 % of the third)
        const float x[4] = {v * 0.6f, v * 0.6f, v * 0.6f, v * 0.6f}, y[4] = {0, 0, 0, 0}, t[4] = {0.6f, 0.6f, 0.6f, 0.6f};
        const auto r = pushPredictTrustedMotion(x, y, t, 0.3f, 0.7f);
        CHECK(std::abs(r.mx - v) < 1e-6f && r.my == 0.0f && std::abs(r.confidence - 0.6f) < 1e-6f);
    }
    {   // mixed trust 1 / 0.6 / 0 / 0.6: the zero-trust texel does not pull the vector toward 0
        const float t[4] = {1.0f, 0.6f, 0.0f, 0.6f}, x[4] = {v * t[0], v * t[1], 0.0f, v * t[3]}, y[4] = {0, 0, 0, 0};
        for (const float tx : {0.0f, 0.25f, 0.5f, 0.9f})
            for (const float ty : {0.0f, 0.5f, 0.75f}) {
                const auto r = pushPredictTrustedMotion(x, y, t, tx, ty);
                if (r.confidence > 1e-6f) CHECK(std::abs(r.mx - v) < 1e-5f);
            }
    }
    {   // trust 0 everywhere (a failed flow, a cut): motion 0, confidence 0 -> every changed pixel falls back
        const float x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0}, t[4] = {0, 0, 0, 0};
        const auto r = pushPredictTrustedMotion(x, y, t, 0.5f, 0.5f);
        CHECK(r.mx == 0.0f && r.my == 0.0f && r.confidence == 0.0f);
        CHECK(pushPredictPixelRule(0.2f, 0.0f, r.confidence, true, 0.0f, 3840.0f) == PushPredictPixel::Fallback);
    }
}

// MUST 2: the reviewer's cut model: 90 % of the flow texels with trust 0, the rest trusted with a real motion. The frame
// must be reported unusable (before the fix 89,951 of 100,000 pixels were "held" and the verdict was "usable").
TEST(predict_a_cut_with_failed_flow_is_unusable) {
    PushPredictCounters c;
    std::uint32_t seed = 12345;
    for (int i = 0; i < 100000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const bool failed = (seed >> 8) % 100 < 90;
        const float trustValue = failed ? 0.0f : 0.9f, len = failed ? 0.0f : 6.0f;
        switch (pushPredictPixelRule(0.3f, len, trustValue, true, 0.01f, 3840.0f)) {
        case PushPredictPixel::Still: ++c.still; break;
        case PushPredictPixel::Held: ++c.held; break;
        case PushPredictPixel::Fallback: ++c.fallback; break;
        case PushPredictPixel::Warped: ++c.warped; break;
        }
    }
    CHECK(c.held == 0 && c.fallback > 85000);
    CHECK(!pushPredictFrameUsable(c));
}

// Scheduling review F: counters belong to the P they were measured on, even when the engine reads them late.
TEST(predict_counter_readback_ring_attributes_each_p) {
    PushPredictReadbackRing ring;
    CHECK(!ring.valid(0) && !ring.valid(1));
    const auto first = ring.record(), second = ring.record(), third = ring.record();   // P of pairs n, n+1, n+2
    CHECK(first == 1 && second == 2 && third == 3);
    CHECK(ring.valid(first));                                    // n+2 (same parity as n) did not overwrite n's numbers
    CHECK(PushPredictReadbackRing::slotOf(first) != PushPredictReadbackRing::slotOf(third));
    for (unsigned i = 0; i < PushPredictReadbackRing::kSlots - 3; ++i) ring.record();
    CHECK(ring.valid(first));                                    // 8 recorded: the first is still there
    ring.record();
    CHECK(!ring.valid(first) && ring.valid(second));             // the 9th reuses its slot: a late read fails, never misattributes
}

// Package 15 review SHOULD 8: P (b + d/3) never lies beyond the next pair's first frame.
TEST(predict_p_never_beyond_the_next_pairs_first_frame) {
    constexpr std::int64_t ms = 10000, a = 0;
    // Even 30 FPS: P at b + 11.1 ms; the next pair's I(2/3) at b + 22.2 ms (stays in): kept.
    std::int64_t b = 333 * ms + 3667, p = pushPredictedPts(a, b, 3);
    std::int64_t next = b + 333 * ms + 3667;
    CHECK(!pushPredictedBeyondNext(p, pushInterpolatedPts(b, next, 2, 3)));
    // Leaving the schedule: the next I(1/3) carries exactly P's time (the designed repeat): kept.
    CHECK(pushInterpolatedPts(b, next, 1, 3) == p && !pushPredictedBeyondNext(p, pushInterpolatedPts(b, next, 1, 3)));
    // A badly paced game, 50 ms then 16.7 ms: P at b + 16.7 ms is beyond the next I(2/3) (b + 11.1) and I(1/3): dropped.
    b = 500 * ms; p = pushPredictedPts(a, b, 3); next = b + 167 * ms;
    CHECK(pushPredictedBeyondNext(p, pushInterpolatedPts(b, next, 2, 3)));
    CHECK(pushPredictedBeyondNext(p, pushInterpolatedPts(b, next, 1, 3)));
    CHECK(!pushPredictedBeyondNext(p, next));      // a next pair without generated frames: its real frame (b + 16.7) is after P
}

// Review 2, SHOULD 3: whole-frame P = B when more of the moving pixels fell back than were moved.
TEST(predict_whole_frame_held_when_most_motion_falls_back) {
    PushPredictCounters c;
    CHECK(!pushPredictWholeFrameHeld(c));                       // nothing moved: P is already B where it changed
    c.warped = 400; c.fallback = 400;
    CHECK(!pushPredictWholeFrameHeld(c));                       // exactly half: kept
    c.fallback = 401;
    CHECK(pushPredictWholeFrameHeld(c));
    c.still = 1000000; c.held = 1000000;                        // still and held pixels do not count
    CHECK(pushPredictWholeFrameHeld(c));
    CHECK(!pushPredictFrameUsable(c));                          // a whole-held frame is always unusable for the verdict
}

// Review 2, SHOULD 8 by construction in presentation time.
TEST(predict_p_never_presented_at_or_after_the_next_pairs_first_slot) {
    constexpr std::int64_t ms = 10000;
    CHECK(!pushPredictedDropBeforeNext(100 * ms, 100 * ms, 0));            // next pair not queued: nothing known, kept
    CHECK(!pushPredictedDropBeforeNext(100 * ms, 99 * ms, 111 * ms));      // due before the next first frame, time left: kept
    CHECK(pushPredictedDropBeforeNext(111 * ms, 99 * ms, 111 * ms));       // due at the next first frame's slot: dropped
    CHECK(pushPredictedDropBeforeNext(115 * ms, 99 * ms, 111 * ms));       // due after it: dropped
    CHECK(pushPredictedDropBeforeNext(100 * ms, 111 * ms, 111 * ms));      // its time has come and passed: dropped
}

TEST(predict_frame_confidence) {
    PushPredictCounters c;
    CHECK(pushPredictFallbackShare(c) == 0.0 && pushPredictWarpedShare(c) == 0.0 && pushPredictFrameUsable(c));
    c.still = 600; c.held = 100; c.warped = 250; c.fallback = 50;
    CHECK(std::abs(pushPredictFallbackShare(c) - 50.0 / 300.0) < 1e-12);
    CHECK(std::abs(pushPredictWarpedShare(c) - 0.25) < 1e-12);
    CHECK(pushPredictFrameUsable(c));
    c.fallback = 150;   // 150 / 400 moving fell back
    CHECK(!pushPredictFrameUsable(c));
    CHECK(pushPredictFrameUsable(c, 0.5));
}

}  // namespace predict_tests
