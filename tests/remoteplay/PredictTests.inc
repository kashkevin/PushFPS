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
    e = eligible(); e.shotChange = true;    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::ShotChange);
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
