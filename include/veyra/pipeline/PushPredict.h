// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "predict" (24 Nov 2026): the GPU half of the "one predicted" frame-generation schedule.
// Design: docs/drafts/PREDICT_GPU.md; schedule: docs/drafts/FG_SCHEDULING_NEXT.md sections 2-3 (stream lagfix).
//
// Per content pair (A, B) at 3X the schedule shows, in this order:
//   I  = interpolated between A and B at t = 2/3   (VFG, one Run: FrameMultiplier 3, FrameIndex 2)
//   B  = the real frame
//   P  = predicted: B advanced by 1/3 of the A->B motion (our own pass, shaders/PushPredict.hlsl)
// instead of today's I(1/3), I(2/3), B. B is shown one output slot after the first generated frame instead of two.
//
// Everything here is pure (no D3D12): the switch, the per-pair eligibility, the frame plan, the timestamps, the flow
// size, the per-pixel rule the shader implements (the CPU reference of it), and the whole-frame confidence. Unit tests:
// tests/remoteplay/PredictTests.inc. Nothing here runs unless VEYRA_EXP_RP_FG_SCHEDULE=onepredicted (environment only).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace veyra::pipeline {

enum class PushPairSchedule : std::uint8_t { Interpolate = 0, OnePredicted = 1 };

// The switch's value, exactly "onepredicted" (ASCII, any case). Anything else, or unset: Interpolate.
template <class Char>
inline PushPairSchedule pushFgScheduleFromSwitch(std::basic_string_view<Char> value) {
    constexpr std::string_view want = "onepredicted";
    if (value.size() != want.size()) return PushPairSchedule::Interpolate;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const auto c = static_cast<std::uint32_t>(value[i]);
        const auto lower = c >= 'A' && c <= 'Z' ? c + 32u : c;
        if (lower != static_cast<std::uint32_t>(want[i])) return PushPairSchedule::Interpolate;
    }
    return PushPairSchedule::OnePredicted;
}

// Why a pair runs today's schedule although the switch is on. The scheduler decides BEFORE the pair is submitted,
// so no frame is shown twice or out of order; Ok is the only value that runs the predicted pair.
enum class PushPredictRefusal : std::uint8_t {
    Ok = 0,
    SwitchOff,        // VEYRA_EXP_RP_FG_SCHEDULE unset or another value
    NotVfg,           // only VFG takes a single frame at t = 2/3 here (DLSS MFG has fixed indices; PushFlow, AMF not wired)
    NotThreeX,        // the plan is defined for 3X only
    NoHistory,        // a reset pair: no valid A, nothing to interpolate or to measure motion from
    ShotChange,       // the previous pair crossed a shot change (VfgShotChange) or the CPU scene analysis saw a cut
    HudLockOrExclusion, // both compose into generated frames from held real frames; not designed for a predicted frame
    PresentSinkFg,    // XeSS present-sink generation owns the presentation
    UnsupportedFormat, // VFG's buffers: R8G8B8A8_UNORM (SDR) or R10G10B10A2_UNORM (HDR10); scRGB FP16 has no VFG
    PassUnavailable,  // the predict pass or its flow could not be created
    LowConfidence,    // the scheduler's own decision from the previous pair's counters (pushPredictFrameUsable)
};

struct PushPredictEligibility {
    PushPairSchedule requested = PushPairSchedule::Interpolate;
    bool vfg = false;
    unsigned multiplier = 0;
    bool historyValid = false;
    bool shotChange = false;
    bool hudLock = false;
    bool exclusion = false;
    bool presentSinkFg = false;
    bool formatSupported = false;
    bool passReady = false;
    bool confident = true;   // the scheduler's verdict from the last counters; true when it has none yet
};

inline PushPredictRefusal pushPredictPairRefusal(const PushPredictEligibility& e) {
    if (e.requested != PushPairSchedule::OnePredicted) return PushPredictRefusal::SwitchOff;
    if (e.presentSinkFg) return PushPredictRefusal::PresentSinkFg;
    if (!e.vfg) return PushPredictRefusal::NotVfg;
    if (e.multiplier != 3) return PushPredictRefusal::NotThreeX;
    if (!e.formatSupported) return PushPredictRefusal::UnsupportedFormat;
    if (e.hudLock || e.exclusion) return PushPredictRefusal::HudLockOrExclusion;
    if (!e.passReady) return PushPredictRefusal::PassUnavailable;
    if (!e.historyValid) return PushPredictRefusal::NoHistory;
    if (e.shotChange) return PushPredictRefusal::ShotChange;
    if (!e.confident) return PushPredictRefusal::LowConfidence;
    return PushPredictRefusal::Ok;
}

inline const char* pushPredictRefusalName(PushPredictRefusal r) {
    switch (r) {
    case PushPredictRefusal::Ok: return "ok";
    case PushPredictRefusal::SwitchOff: return "switch-off";
    case PushPredictRefusal::NotVfg: return "not-vfg";
    case PushPredictRefusal::NotThreeX: return "not-3x";
    case PushPredictRefusal::NoHistory: return "no-history";
    case PushPredictRefusal::ShotChange: return "shot-change";
    case PushPredictRefusal::HudLockOrExclusion: return "hud-lock-or-exclusion";
    case PushPredictRefusal::PresentSinkFg: return "present-sink-fg";
    case PushPredictRefusal::UnsupportedFormat: return "format";
    case PushPredictRefusal::PassUnavailable: return "pass-unavailable";
    case PushPredictRefusal::LowConfidence: return "low-confidence";
    }
    return "?";
}

// Whether the graph creates the pass at all (EnhanceGraph::initPredict). Without the switch: never, whatever else holds;
// that is the "unset allocates nothing" rule.
inline bool pushPredictCreate(PushPairSchedule asked, bool fgEnabled, bool vfg, bool otherGenerator, bool presentSink,
                              unsigned configuredMultiplier, bool formatOk) {
    return asked == PushPairSchedule::OnePredicted && fgEnabled && vfg && !otherGenerator && !presentSink &&
           configuredMultiplier >= 3 && formatOk;
}

// The frames of one predicted pair, in display order, and where each lives.
// Generated pool slot of subframe `sub` of parity p is p + 2 (sub - 1) (EnhanceGraph, VfgBackend outputs).
struct PushPredictPlan {
    unsigned vfgMultiplier = 3;   // FrameMultiplier given to VFG (unchanged from the graph's 3X: no rebuild)
    unsigned vfgFrameIndex = 2;   // FrameIndex: t = 2/3
    unsigned interpolatedSlot = 0; // genFrame_ slot of I
    unsigned predictedSlot = 0;    // genFrame_ slot of P
    unsigned interpolatedSubframe = 2; // BatchFrame::subframe of I (the VFG index, so VFG's per-slot state matches)
    unsigned predictedSubframe = 1;    // BatchFrame::subframe of P (the slot VFG does not use in this pair)
};
inline PushPredictPlan pushOnePredictedPlan(unsigned parity) {
    PushPredictPlan plan;
    plan.interpolatedSlot = parity + 2u * (plan.vfgFrameIndex - 1u);
    plan.predictedSlot = parity;   // subframe 1's slot, free because VFG's FrameIndex 1 is not run in this pair
    return plan;
}

// Timestamps (100 ns), as FrameBatch::interpolate does for today's frames.
// Same rounding as FrameBatch::interpolate (a + (d / n) j + ((d % n) j + n / 2) / n).
inline std::int64_t pushInterpolatedPts(std::int64_t a, std::int64_t b, unsigned index, unsigned multiplier) {
    const std::int64_t n = std::max<std::int64_t>(1, multiplier), j = index, d = b - a;
    return a + (d / n) * j + ((d % n) * j + n / 2) / n;
}
// P one output slot after B: B + (B - A) / n, rounded the same way.
inline std::int64_t pushPredictedPts(std::int64_t a, std::int64_t b, unsigned multiplier) {
    const std::int64_t n = std::max<std::int64_t>(1, multiplier), d = b - a;
    return b + d / n + (d % n + n / 2) / n;
}

// The fraction of the A->B motion P is advanced by: one output slot of the pair.
inline float pushPredictStep(unsigned multiplier) { return 1.0f / float(std::max(1u, multiplier)); }

// Flow extent for the work extent: a quarter in each axis (3840x2160 -> 960x540), at least 32 and at most 4096 per axis
// (GpuDisOpticalFlow's limits). Motion is measured on the enhanced real frames, after SR and NR, at this size.
struct PushExtent { std::uint32_t width = 0, height = 0; };
inline PushExtent pushPredictFlowExtent(std::uint32_t workWidth, std::uint32_t workHeight) {
    auto axis = [](std::uint32_t v) { return std::clamp<std::uint32_t>((v + 3u) / 4u, 32u, 4096u); };
    return {axis(workWidth), axis(workHeight)};
}

// The per-pixel rule (shaders/PushPredict.hlsl implements exactly this; the CPU form is for the tests and the docs).
// Inputs are per output pixel y, all in the encoded working values (0..1 per channel; PQ for HDR10):
//   stillDelta  = max over RGB of |B(y) - A(y)|
//   motion      = the A->B flow at y (B->A vectors negated), in WORK pixels per content interval
//   confidence  = the flow's own trust at y (forward/backward and photometric agreement, 0..1)
//   sourceInside = y - step * motion lies inside the picture
//   photoDelta  = max over RGB of |B(y) - A(y + motion)|: does B's pixel match where the flow says it came from in A
struct PushPredictThresholds {
    float stillDelta = 1.0f / 1023.0f;   // a still pixel (HUD, letterbox, a still picture part): output = B exactly
    float minMotion = 0.25f;             // work pixels per interval below which a pixel is not moved
    float maxMotionShare = 0.125f;       // more than 1/8 of the picture width per interval is not trusted
    float minConfidence = 0.5f;
    float maxPhotoDelta = 24.0f / 255.0f;
};
enum class PushPredictPixel : std::uint8_t { Still = 0, Held = 1, Fallback = 2, Warped = 3 };
inline PushPredictPixel pushPredictPixelRule(float stillDelta, float motionLength, float confidence, bool sourceInside,
                                             float photoDelta, float pictureWidth, const PushPredictThresholds& t = {}) {
    if (stillDelta <= t.stillDelta) return PushPredictPixel::Still;              // bit-identical to B
    if (!(motionLength >= t.minMotion)) return PushPredictPixel::Held;            // nothing to move (also NaN)
    if (motionLength > t.maxMotionShare * pictureWidth || !(confidence >= t.minConfidence) || !sourceInside ||
        !(photoDelta <= t.maxPhotoDelta))
        return PushPredictPixel::Fallback;                                        // untrusted: B, a held pixel
    return PushPredictPixel::Warped;
}

// Whole-frame counters the pass writes (four uint32: still, held, fallback, warped pixels).
struct PushPredictCounters { std::uint32_t still = 0, held = 0, fallback = 0, warped = 0; };
// Share of the MOVING pixels (fallback + warped) that fell back to B. 0 when nothing moved.
inline double pushPredictFallbackShare(const PushPredictCounters& c) {
    const double moving = double(c.fallback) + double(c.warped);
    return moving > 0 ? double(c.fallback) / moving : 0.0;
}
// Share of the whole picture that is predicted (moved).
inline double pushPredictWarpedShare(const PushPredictCounters& c) {
    const double all = double(c.still) + double(c.held) + double(c.fallback) + double(c.warped);
    return all > 0 ? double(c.warped) / all : 0.0;
}
// The scheduler's per-pair verdict from the last finished predicted frame: usable while at most maxFallbackShare of the
// moving pixels fell back (a frame that is mostly held is safe but saves nothing visible; one that is half moved and
// half held can show a seam). Default 0.25: an ESTIMATE to be set from his PC (PREDICT_GPU.md section 7).
inline bool pushPredictFrameUsable(const PushPredictCounters& c, double maxFallbackShare = 0.25) {
    return pushPredictFallbackShare(c) <= maxFallbackShare;
}

}  // namespace veyra::pipeline
