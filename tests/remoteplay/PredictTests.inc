// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushPredict.h (stream "predict"): the switch, per-pair eligibility, the plan of one predicted
// pair, timestamps, the flow size, the per-pixel rule and the whole-frame confidence. Values are examples.
#include "veyra/pipeline/PushPredict.h"
#include "veyra/pipeline/PushPredictCheck.h"
#include "veyra/engine/PushLowDelay.h"
#include <cmath>
#include <cstring>
#include <vector>
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
    // Package 16, item 2: Live HUD Lock and exclusion boxes no longer refuse (P composes from B).
    e = eligible(); e.hudLock = true;       CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::Ok);
    e = eligible(); e.exclusion = true;     CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::Ok);
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
    e.hudLock = true; e.exclusion = true;   // Package 16: not a refusal any more
    CHECK(pushPredictStaticRefusal(e) == PushPredictRefusal::Ok);
    e.hudLock = false; e.exclusion = false; e.formatSupported = false;
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

// e2e scenario m2: the row on at a configured 2X reads not-here / not-3x, not unavailable / switch-off.
TEST(predict_create_refusal_names_the_shape) {
    using S = PushPairSchedule; using R = PushPredictRefusal;
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, false, false, 2, true) == R::NotThreeX);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, false, false, 3, true) == R::Ok);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, false, false, 4, true) == R::Ok);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, false, false, false, 3, true) == R::NotVfg);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, true, false, 3, true) == R::NotVfg);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, false, true, 3, true) == R::PresentSinkFg);
    CHECK(pushPredictCreateRefusal(S::OnePredicted, true, true, false, false, 3, false) == R::UnsupportedFormat);
    CHECK(pushPredictCreateRefusal(S::Interpolate, true, true, false, false, 3, true) == R::SwitchOff);
    // It matches pushPredictCreate: Ok exactly when the pass is created.
    for (unsigned m : {1u, 2u, 3u, 4u}) for (int bits = 0; bits < 16; ++bits) {
        const bool fg = bits & 1, vfg = bits & 2, other = bits & 4, sink = bits & 8;
        CHECK((pushPredictCreateRefusal(S::OnePredicted, fg, vfg, other, sink, m, true) == R::Ok) == pushPredictCreate(S::OnePredicted, fg, vfg, other, sink, m, true));
    }
    // And the row's state: not here (with its reason), not unavailable.
    CHECK(veyra::engine::pushLowDelayStateFor(true, veyra::engine::PushLowDelaySuspend::None, true, true, R::NotThreeX, false) == veyra::engine::PushLowDelayState::NotHere);
}

TEST(predict_flow_extent_is_a_quarter_within_limits) {
    const auto k = pushPredictFlowExtent(3840, 2160);
    CHECK(k.width == 960 && k.height == 540);
    const auto q = pushPredictFlowExtent(2560, 1440);
    CHECK(q.width == 640 && q.height == 360);
    const auto t = pushPredictFlowExtent(64, 40);
    CHECK(t.width == 64 && t.height == 40 && !pushPredictFidelityExtentOk(t));   // an axis under 64 keeps its own size
    const auto e2e = pushPredictFlowExtent(160, 90);                             // the e2e fast loop's size
    CHECK(e2e.width == 64 && e2e.height == 64 && pushPredictFidelityExtentOk(e2e));
    CHECK(pushPredictFidelityExtentOk(pushPredictFlowExtent(3840, 2160)) && pushPredictFidelityExtentOk(pushPredictFlowExtent(256, 144)));
    // FidelityFX's search needs waves of at least 32 lanes (WARP: 4; NVIDIA: 32; AMD: 32 or 64).
    CHECK(!pushPredictFidelityWaveOk(4) && !pushPredictFidelityWaveOk(16) && pushPredictFidelityWaveOk(32) && pushPredictFidelityWaveOk(64) && !pushPredictFidelityWaveOk(0));
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


// Package 16, item 2: where a predicted pair's frames take their locked / excluded pixels from.
TEST(predict_hud_lock_and_boxes_compose_from_the_frame_followed) {
    for (unsigned parity = 0; parity < 2; ++parity) {
        const auto i = pushPredictComposeSources(false, parity);   // I(2/3) and every interpolated frame: A
        CHECK(i.hudHeld == 1 - parity && i.exclusionRaw == 1 - parity);
        const auto p = pushPredictComposeSources(true, parity);    // P: B (the copy of it for HUD Lock, raw B for boxes)
        CHECK(p.hudHeld == kPushPredictHeldCopy && p.exclusionRaw == parity);
    }
}

// Package 16, item 2: the test-only check's CPU side (PushPredictCheck.h), on made-up pictures.
TEST(predict_check_counts_fully_locked_and_excluded_pixels_only) {
    constexpr std::uint32_t W = 16, H = 8, pitch = W * 4;
    std::vector<std::uint8_t> state(std::size_t(pitch) * H, 0), p(std::size_t(pitch) * H, 0), b(std::size_t(pitch) * H, 0);
    const auto put = [&](std::vector<std::uint8_t>& v, std::uint32_t x, std::uint32_t y, std::uint32_t w) { std::memcpy(v.data() + y * pitch + x * 4, &w, 4); };
    const auto half = [](float f) -> std::uint16_t { return f >= 1.0f ? 0x3C00 : f >= 0.96f ? 0x3BAE : f >= 0.5f ? 0x3800 : f >= 0.2f ? 0x3266 : 0; };
    // Columns 2..5 locked (still 1.0, contrast 0.2: above 2.5 x 0.04); column 6 still 0.5 (feathered, not counted).
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint16_t v[2] = {half(x >= 2 && x <= 5 ? 1.0f : x == 6 ? 0.5f : 0.0f), half(x >= 2 && x <= 5 ? 0.2f : 0.0f)};
            std::memcpy(state.data() + y * pitch + x * 4, v, 4);
            put(b, x, y, 0xFF102030u);
            put(p, x, y, 0xFF102030u);
        }
    CHECK(std::fabs(pushHalfToFloat(0x3BAE) - 0.95996f) < 1e-4f && pushHalfToFloat(0x3C00) == 1.0f && pushHalfToFloat(0x3266) > 0.199f);
    PushHudLockStateView view{state.data(), pitch, W, H, 0};
    PushPredictBoxes none;
    auto c = pushPredictCheck(p.data(), pitch, b.data(), pitch, W, H, false, view, none);
    CHECK(c.hudLocked == 4 * H && c.hudLockMismatch == 0 && c.excluded == 0);
    put(p, 3, 2, 0x00102030u);   // alpha differs only: not a mismatch
    put(p, 4, 2, 0xFF102031u);   // a locked pixel differs
    put(p, 6, 2, 0xFF999999u);   // a feathered pixel differs: not counted
    put(p, 12, 2, 0xFF999999u);  // a moving pixel differs: not counted
    c = pushPredictCheck(p.data(), pitch, b.data(), pitch, W, H, false, view, none);
    CHECK(c.hudLocked == 4 * H && c.hudLockMismatch == 1);
    // A box over x 8..16 (normalised), feather 2: fully inside where at least 2 px from every edge -> x 10..13, y 2..5.
    PushPredictBoxes box; box.enabled = true; box.feather = 2; box.regions[0] = {0.5f, 0.0f, 1.0f, 1.0f};
    c = pushPredictCheck(p.data(), pitch, b.data(), pitch, W, H, false, PushHudLockStateView{}, box);
    CHECK(c.hudLocked == 0 && c.excluded == 4 * 4 && c.exclusionMismatch == 1);   // (12, 2)
    // An ellipse is sent with left and right swapped.
    PushPredictBoxes ellipse; ellipse.enabled = true; ellipse.feather = 0; ellipse.regions[0] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pushExclusionFull(ellipse, 8, 4, W, H) && !pushExclusionFull(ellipse, 0, 0, W, H));
    // 10-bit: the two alpha bits are ignored, every colour bit counts.
    std::vector<std::uint8_t> p10(std::size_t(pitch) * H, 0), b10(std::size_t(pitch) * H, 0);
    put(p10, 3, 3, 0xC0000001u); put(b10, 3, 3, 0x00000001u); put(p10, 4, 3, 0x20000000u);
    c = pushPredictCheck(p10.data(), pitch, b10.data(), pitch, W, H, true, view, none);
    CHECK(c.hudLockMismatch == 1);
    // Due: switch, a predicted pair, nothing in flight, the 1st, 33rd, ... predicted pair.
    CHECK(pushPredictCheckDue(true, true, false, 1) && pushPredictCheckDue(true, true, false, 1 + kPushPredictCheckEvery));
    CHECK(!pushPredictCheckDue(false, true, false, 1) && !pushPredictCheckDue(true, false, false, 1) && !pushPredictCheckDue(true, true, true, 1) &&
          !pushPredictCheckDue(true, true, false, 2));
}

// Package 16, item 4: stage times from the pass's five timestamps, the 10 s window's percentiles and its log text.
TEST(predict_stage_times_from_timestamps) {
    const std::uint64_t ticks[kPushPredictStamps] = {1000000, 1100000, 3100000, 3600000, 3700000};   // a 1 MHz counter
    const auto ms = pushPredictStagesFromTicks(ticks, 1000000);
    CHECK(std::fabs(ms[0] - 100.0) < 1e-9 && std::fabs(ms[1] - 2000.0) < 1e-9 && std::fabs(ms[2] - 500.0) < 1e-9 &&
          std::fabs(ms[3] - 100.0) < 1e-9 && std::fabs(ms[4] - 2700.0) < 1e-9);
    CHECK(pushPredictStagesFromTicks(ticks, 0)[4] < 0);
    const std::uint64_t backwards[kPushPredictStamps] = {5, 4, 6, 7, 8};
    CHECK(pushPredictStagesFromTicks(backwards, 1000)[0] < 0 && pushPredictStagesFromTicks(backwards, 1000)[4] >= 0);
    PushPredictCostWindow w;
    CHECK(w.percentile(PushPredictStage::Total, 0.5) < 0);
    for (int i = 1; i <= 100; ++i) { PushPredictStageMs m; m.fill(-1.0); m[4] = double(i); m[1] = double(i) / 2; w.add(1000000000LL + i * 333667LL, m); }
    CHECK(w.size() == 100 && w.percentile(PushPredictStage::Total, 0.5) == 50.0 && w.percentile(PushPredictStage::Total, 0.95) == 95.0 &&
          w.percentile(PushPredictStage::Total, 0.99) == 99.0 && w.percentile(PushPredictStage::Flow, 0.95) == 47.5);
    CHECK(w.percentile(PushPredictStage::Warp, 0.5) < 0);   // never measured
    PushPredictStageMs late; late.fill(1.0);
    w.add(1000000000LL + 100 * 333667LL + PushPredictCostWindow::kWindow100ns + 1, late);   // 10 s later: the old samples go
    CHECK(w.size() == 1);
    CHECK(pushPredictCostText(w) == "samples:1,downsample:1.00/1.00/1.00,flow:1.00/1.00/1.00,warp:1.00/1.00/1.00,verdict:1.00/1.00/1.00,total:1.00/1.00/1.00");
}

// Package 16, item 4 (d): the float-only flow's start-up check (CPU side) and the choice.
TEST(predict_float_flow_self_check_and_choice) {
    constexpr std::uint32_t W = 8, H = 4;
    const auto half = [](float f) -> std::uint16_t {   // exact for the small values used here
        if (f == 0.0f) return 0;
        std::uint16_t sign = f < 0 ? 0x8000 : 0; float a = std::fabs(f); int e = 0;
        while (a >= 2.0f) { a /= 2; ++e; } while (a < 1.0f) { a *= 2; --e; }
        return std::uint16_t(sign | ((e + 15) << 10) | std::uint16_t(std::lround((a - 1.0f) * 1024.0f)));
    };
    struct Flow { std::vector<std::uint8_t> motion, trust; };
    const auto make = [&](float vx, float vy, std::uint8_t t, int untrustedColumns) {
        Flow f; f.motion.resize(W * H * 4); f.trust.resize(W * H);
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::uint8_t tt = int(x) < untrustedColumns ? 0 : t;
                const float scale = float(tt) / 255.0f;
                const std::uint16_t h[2] = {half(vx * scale), half(vy * scale)};
                std::memcpy(f.motion.data() + (y * W + x) * 4, h, 4);
                f.trust[y * W + x] = tt;
            }
        return f;
    };
    const auto view = [&](const Flow& f) { return PushFlowView{f.motion.data(), W * 4, f.trust.data(), W}; };
    const Flow strict = make(-3.0f, -1.0f, 255, 1), same = make(-3.0f, -1.0f, 255, 1);
    auto a = pushFlowAgreement(view(strict), view(same), W, H, -3.0f, -1.0f);
    CHECK(a.pass && a.trustDisagree == 0.0 && a.meanDiffPx < 1e-3 && a.strictErrorPx < 1e-3 && a.strictTrusted == 7.0 / 8.0);
    const Flow off = make(-2.5f, -1.0f, 255, 1);   // half a pixel off everywhere: fails the mean difference
    a = pushFlowAgreement(view(strict), view(off), W, H, -3.0f, -1.0f);
    CHECK(!a.pass && a.meanDiffPx > 0.4);
    const Flow lessTrusted = make(-3.0f, -1.0f, 255, 2);   // one column more untrusted: 1 / 8 of the pixels disagree
    a = pushFlowAgreement(view(strict), view(lessTrusted), W, H, -3.0f, -1.0f);
    CHECK(!a.pass && std::fabs(a.trustDisagree - 0.125) < 1e-9);
    const Flow nothing = make(0.0f, 0.0f, 0, 0);
    CHECK(!pushFlowAgreement(view(nothing), view(nothing), W, H, -3.0f, -1.0f).pass);   // the strict flow found nothing: no float
    CHECK(pushPredictFlowChoiceFrom(std::string_view("float")) == PushPredictFlowChoice::Float &&
          pushPredictFlowChoiceFrom(std::wstring_view(L"strict")) == PushPredictFlowChoice::Strict &&
          pushPredictFlowChoiceFrom(std::string_view("Float")) == PushPredictFlowChoice::Auto);
}

// Crash containment of the flow self-check (gate run 45526571434): what may run, and what is used.
TEST(predict_flow_check_plan_and_use) {
    using C = PushPredictFlowChoice; using U = PushFlowUse; using B = PushFlowCheckBlock;
    // Default: FidelityFX only (never GPU DIS); GPU DIS only when chosen, the strict build only with FP64.
    auto p = pushFlowCheckPlan(C::Auto, true, true, B::None);
    CHECK(p.fidelity && !p.strict && !p.flt && p.block == B::None);
    p = pushFlowCheckPlan(C::Auto, true, false, B::None);
    CHECK(!p.fidelity && !p.strict && !p.flt);                    // a build without FidelityFX: nothing (no GPU DIS fallback)
    p = pushFlowCheckPlan(C::FidelityFx, false, true, B::None);
    CHECK(p.fidelity && !p.strict && !p.flt);
    p = pushFlowCheckPlan(C::Strict, true, true, B::None);
    CHECK(p.strict && !p.flt && !p.fidelity);
    p = pushFlowCheckPlan(C::Strict, false, true, B::None);
    CHECK(!p.strict && !p.flt && !p.fidelity);                    // forced strict without FP64: nothing to run
    p = pushFlowCheckPlan(C::Float, true, true, B::None);
    CHECK(p.flt && !p.strict && !p.fidelity);
    for (B b : {B::StoppedInsideLastTime, B::FailedThisProcess}) {
        p = pushFlowCheckPlan(C::Auto, true, true, b);
        CHECK(!p.strict && !p.flt && !p.fidelity && p.block == b);
    }
    // A flow is used only after its own self-run completed and found the known motion.
    const auto fid = pushFlowCheckPlan(C::Auto, true, true, B::None), flt = pushFlowCheckPlan(C::Float, true, true, B::None),
               st = pushFlowCheckPlan(C::Strict, true, true, B::None), none = pushFlowCheckPlan(C::Auto, true, true, B::FailedThisProcess);
    CHECK(pushFlowUse(fid, true, true) == U::FidelityFx && pushFlowUse(fid, true, false) == U::None && pushFlowUse(fid, false, true) == U::None);
    CHECK(pushFlowUse(flt, true, true) == U::Float && pushFlowUse(st, true, true) == U::Strict && pushFlowUse(none, true, true) == U::None);
    PushFlowAgreement a; a.strictTrusted = 0.9; a.floatErrorPx = 0.05;
    CHECK(pushFlowFoundMotion(a));
    a.floatErrorPx = 0.6; CHECK(!pushFlowFoundMotion(a));
    a.floatErrorPx = 0.05; a.strictTrusted = 0.4; CHECK(!pushFlowFoundMotion(a));
    CHECK(std::string_view(pushFlowUseName(U::None)) == "none" && std::string_view(pushFlowUseName(U::FidelityFx)) == "fidelityfx");
    CHECK(pushPredictFlowChoiceFrom(std::string_view("fidelityfx")) == C::FidelityFx && std::string_view(pushPredictFlowChoiceName(C::Auto)) == "auto");
    CHECK(pushPredictFlowChoiceFrom(std::wstring_view(L"zero")) == C::Zero && std::string_view(pushPredictFlowChoiceName(C::Zero)) == "zero");
    p = pushFlowCheckPlan(C::Zero, true, true, B::None);
    CHECK(!p.fidelity && !p.strict && !p.flt);   // the test-only zero source runs no self-check (nothing to check)
}

TEST(predict_flow_check_windows) {
    const auto w = pushFlowCheckWindows(3840, 2160, 960, 540);
    CHECK(w[0].vx == 0 && w[0].vy == 0 && w[1].vx == 4 && w[1].vy == 4 && w[2].vx == 16 && w[2].vy == -16);
    CHECK(w[3].vx == -48 && w[3].vy == 16 && w[4].vx == 120 && w[4].vy == 0 && w[5].vx == -16 && w[5].vy == -4);
    CHECK(w[0].x0 == 0 && w[2].x1 == 3840 && w[3].y0 == 1080 && w[5].y1 == 2160);   // 3 x 2, covering the picture
    bool both = false;   // both signs on both axes among the moving windows
    { bool px = false, nx = false, py = false, ny = false; for (const auto& o : w) { px |= o.vx > 0; nx |= o.vx < 0; py |= o.vy > 0; ny |= o.vy < 0; } both = px && nx && py && ny; }
    CHECK(both);
    const auto t = pushFlowCheckWindows(256, 144, 64, 64);   // a small test size: speeds capped at 1/8 of a window
    CHECK(t[4].flowVx == 2 && t[4].vx == 8 && t[3].flowVx == -2 && t[1].flowVx == 1);
    CHECK(pushFlowCheckMargin(w[4], 32.0) == int(std::ceil(120.0 * 4.0 / 3.0 + 32.0)) + 4);
    CHECK(pushFlowWindowPass(w[0], 0.5, 9.0) && !pushFlowWindowPass(w[0], 2.0, 9.0));
    CHECK(pushFlowWindowPass(w[4], 4.0, 9.0) && !pushFlowWindowPass(w[4], 5.0, 9.0) && !pushFlowWindowPass(w[4], 0.0, 0.0));
    CHECK(std::fabs(pushPsnr(1.0) - 48.13) < 0.01 && pushPsnr(0.0) == 99.0);
    CHECK(std::fabs(pushFlowCheckGrey(0, 0) - 128.0 - 25.0 * 0.0) < 1e-9);
}

TEST(predict_flow_check_marker) {
    using B = PushFlowCheckBlock;
    const std::string_view id = "01dc5e7a9b3c0000-0000000002a1b000";
    CHECK(pushFlowCheckMarker("", id) == B::None);
    CHECK(pushFlowCheckMarker("running 01dc5e7a9b3c0000-0000000002a1b000\n", id) == B::StoppedInsideLastTime);
    CHECK(pushFlowCheckMarker("failed 01dc5e7a9b3c0000-0000000002a1b000\r\n", id) == B::StoppedInsideLastTime);
    CHECK(pushFlowCheckMarker("failed 01dc5e7a9b3c0000-0000000002a1b001\n", id) == B::None);   // another program file (an update)
    CHECK(pushFlowCheckMarker("running 01dc5e7a9b3c0000-0000000002a1b001\n", id) == B::None);
    CHECK(pushFlowCheckMarker("running ", id) == B::None && pushFlowCheckMarker("garbage", id) == B::None);
}

// Package 16, item 5: the order in which a FrameIndex-2-only request is tried, and when it stops.
// Review 1 SHOULD 6: the ladder tried again after a real off -> on (at most once in 10 s), and once 2 s after a refusal
// within the first second of a build.
TEST(predict_vfg_ladder_retry_rules) {
    CHECK(pushVfgLadderResetDue(5000, -1) && !pushVfgLadderResetDue(14999, 5000) && pushVfgLadderResetDue(15000, 5000));
    CHECK(pushVfgEarlyRefusal(0, false) && pushVfgEarlyRefusal(999, false) && !pushVfgEarlyRefusal(1000, false));
    CHECK(!pushVfgEarlyRefusal(10, true) && !pushVfgEarlyRefusal(-5, false));
    CHECK(kPushVfgEarlyRetryMs == 2000);
}

TEST(predict_vfg_alone_ladder) {
    PushVfgAloneLadder a;
    CHECK(a.current() == PushVfgAloneMode::Direct);
    CHECK(a.outcome(true) == PushVfgAloneMode::Direct && a.settled());           // accepted: kept
    PushVfgAloneLadder b;
    CHECK(b.outcome(false) == PushVfgAloneMode::Timestep);
    CHECK(b.outcome(false) == PushVfgAloneMode::IndexOneThenTwo);
    CHECK(b.outcome(true) == PushVfgAloneMode::IndexOneThenTwo && b.settled());
    CHECK(b.outcome(false) == PushVfgAloneMode::Refused);                       // a working mode that later fails: stop
    CHECK(b.outcome(true) == PushVfgAloneMode::Refused);
    PushVfgAloneLadder c(false);                                                 // no NvVFX_SetF32: no Timestep step
    CHECK(c.outcome(false) == PushVfgAloneMode::IndexOneThenTwo && c.outcome(false) == PushVfgAloneMode::Refused);
    CHECK(std::fabs(pushVfgTimestep(2, 3) - 2.0f / 3.0f) < 1e-7f && pushVfgTimestep(1, 0) == 0.0f);
    CHECK(std::string_view(pushVfgAloneModeName(PushVfgAloneMode::IndexOneThenTwo)) == "index1-then-2");
}

// Release review REL16 MUST 3: the self-check's budget is enforced by construction: each frame waits at most what is left
// (at least the floor), so a frame that never finishes ends the check by budget + floor.
TEST(predict_flow_check_budget_bounds_every_wait) {
    CHECK(pushFlowCheckWaitMs(0.0, false) == kPushFlowCheckBudgetMs);
    CHECK(pushFlowCheckWaitMs(2900.0, false) == kPushFlowCheckWaitFloorMs);   // 100 ms left: the floor
    CHECK(pushFlowCheckWaitMs(1000.0, false) == 2000.0);
    CHECK(pushFlowCheckWaitMs(5000.0, false) == kPushFlowCheckWaitFloorMs);
    CHECK(pushFlowCheckWaitMs(0.0, true) == 10000.0);                        // the test-only mechanics switch: no budget
    // The worst case: a card that reaches the last frame at any time t <= budget and then never finishes is stopped at
    // t + wait(t) <= budget + floor.
    for (double t = 0.0; t <= kPushFlowCheckBudgetMs; t += 50.0)
        CHECK(t + pushFlowCheckWaitMs(t, false) <= kPushFlowCheckBudgetMs + kPushFlowCheckWaitFloorMs + 1e-9);
    CHECK(pushFlowCheckOverBudget(3000.1) && !pushFlowCheckOverBudget(3000.0));
}

}  // namespace predict_tests
