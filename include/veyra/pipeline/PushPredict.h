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
// tests/remoteplay/PredictTests.inc. Nothing here runs unless the "Lower delay" row is on (pref pushLowDelay ->
// EnhanceGraphDesc::lowDelay) or VEYRA_EXP_RP_FG_SCHEDULE=onepredicted is in the environment (the test kit).
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
    SwitchOff,        // neither the row nor VEYRA_EXP_RP_FG_SCHEDULE=onepredicted asked for it
    NotVfg,           // only VFG takes a single frame at t = 2/3 here (DLSS MFG has fixed indices; PushFlow, AMF not wired)
    NotThreeX,        // the plan is defined for 3X only
    NoHistory,        // a reset pair: no valid A, nothing to interpolate or to measure motion from. A cut the CPU scene
                      // analysis sees resets history, so it lands here. A cut seen only by VFG's GPU test is handled on
                      // the GPU: P is B, predicated on that test (VfgBackend::copyInput). There is no other cut check
                      // (review 2, SHOULD 7: the earlier "shot change" refusal was never set and is removed).
    HudLockOrExclusion, // both compose into generated frames from held real frames; not designed for a predicted frame
    PresentSinkFg,    // XeSS present-sink generation owns the presentation
    UnsupportedFormat, // VFG's buffers: R8G8B8A8_UNORM (SDR) or R10G10B10A2_UNORM (HDR10); scRGB FP16 has no VFG
    PassUnavailable,  // the predict pass or its flow could not be created
    LowConfidence,    // the scheduler's own decision from the previous pair's counters (pushPredictFrameUsable)
    VfgRefused,       // review 2, SHOULD 9: VFG refused or failed a FrameIndex-2-only request this session (one attempt; the
                      // row switched off and on allows another). Frame generation goes on as Package 14's.
    SmoothnessMode,   // engine only (review 2, SHOULD 4): Smoothness Mode decides the multiplier and assumes the real
                      // frame is the batch's last; the two are exclusive
};

struct PushPredictEligibility {
    PushPairSchedule requested = PushPairSchedule::Interpolate;
    bool vfg = false;
    unsigned multiplier = 0;
    bool historyValid = false;
    bool hudLock = false;
    bool exclusion = false;
    bool presentSinkFg = false;
    bool formatSupported = false;
    bool passReady = false;
    bool vfgRefused = false;   // PushLowDelayShared::vfgRefused
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
    if (e.vfgRefused) return PushPredictRefusal::VfgRefused;
    if (!e.historyValid) return PushPredictRefusal::NoHistory;
    if (!e.confident) return PushPredictRefusal::LowConfidence;
    return PushPredictRefusal::Ok;
}

// The part of the refusal known before a pair is asked (settings and the graph's shape: no pass, not VFG, not 3X, format,
// HUD Lock or exclusion boxes, present-sink FG). The scheduler does not ask while one holds (Ineligible, no back-off);
// only the per-pair reasons (no history, a shot change, low confidence) remain for an asked pair.
inline PushPredictRefusal pushPredictStaticRefusal(PushPredictEligibility e) {
    e.historyValid = true; e.confident = true;
    return pushPredictPairRefusal(e);
}

inline const char* pushPredictRefusalName(PushPredictRefusal r) {
    switch (r) {
    case PushPredictRefusal::Ok: return "ok";
    case PushPredictRefusal::SwitchOff: return "switch-off";
    case PushPredictRefusal::NotVfg: return "not-vfg";
    case PushPredictRefusal::NotThreeX: return "not-3x";
    case PushPredictRefusal::NoHistory: return "no-history";
    case PushPredictRefusal::HudLockOrExclusion: return "hud-lock-or-exclusion";
    case PushPredictRefusal::PresentSinkFg: return "present-sink-fg";
    case PushPredictRefusal::UnsupportedFormat: return "format";
    case PushPredictRefusal::PassUnavailable: return "pass-unavailable";
    case PushPredictRefusal::LowConfidence: return "low-confidence";
    case PushPredictRefusal::SmoothnessMode: return "smoothness-mode";
    case PushPredictRefusal::VfgRefused: return "vfg-refused";
    }
    return "?";
}

// What the graph is asked for when it is built (EnhanceGraph::initPredict): the quick-setup row "Lower delay"
// (EnhanceGraphDesc::lowDelay, from EnhancementSettings::lowDelay: a rebuild creates or releases the pass while PushFPS
// runs) or the kit's switch (environment, read once per process). Neither: Interpolate, and nothing is created.
inline PushPairSchedule pushPredictAsked(PushPairSchedule fromSwitch, bool row) {
    return row ? PushPairSchedule::OnePredicted : fromSwitch;
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

// Package 15 review SHOULD 8: P (b + d/3) must stay before the next pair's first frame. When the next pair comes sooner
// than d/3 after b would allow (a badly paced game, e.g. 50 / 16.7 ms intervals), P is dropped before it is shown, so
// shown and recorded times never go backwards. Equal times are kept: leaving the schedule makes the next I(1/3) carry
// exactly P's time (the designed one-frame repeat at an exit). The engine applies it when it queues the next pair.
inline bool pushPredictedBeyondNext(std::int64_t predictedPts, std::int64_t nextFirstPts) { return predictedPts > nextFirstPts; }

// Review 2, SHOULD 8, by construction in presentation time: once the next pair is queued, its first frame's presentation
// deadline (host time) is P's limit. P is dropped when its own deadline is at or after that slot, or when the time has
// passed it, so P can never be presented at or after the next pair's first frame, whatever the timestamps say. Not "drop
// once the next pair is queued": the next pair is queued about T + a few ms after the real frame's arrival, P is due
// about phase + T/3 (earlier than the next I), and such a rule would drop most P on time. No limit known yet: never drop.
inline bool pushPredictedDropBeforeNext(std::int64_t predictedDeadline100ns, std::int64_t now100ns, std::int64_t nextFirstDeadline100ns) {
    return nextFirstDeadline100ns > 0 && (predictedDeadline100ns >= nextFirstDeadline100ns || now100ns >= nextFirstDeadline100ns);
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
//   confidence  = the flow's own trust at y (forward/backward and photometric agreement, 0..1), bilinear
//                 (pushPredictTrustedMotion: the motion itself is the trust-weighted average of the vectors)
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
    // The trust BEFORE the length: where the flow failed (trust 0) GpuDisValidate writes motion 0, and a failed flow
    // counted as "held" would hide a cut or a failure from pushPredictFallbackShare (review of 24 Nov, MUST 2).
    if (!(confidence >= t.minConfidence)) return PushPredictPixel::Fallback;     // untrusted (also NaN): B
    if (!(motionLength >= t.minMotion)) return PushPredictPixel::Held;            // trusted, nothing to move (also NaN)
    if (motionLength > t.maxMotionShare * pictureWidth || !sourceInside || !(photoDelta <= t.maxPhotoDelta))
        return PushPredictPixel::Fallback;                                        // untrusted: B, a held pixel
    return PushPredictPixel::Warped;
}

// The motion and the trust at a point between four flow texels, as shaders/PushPredict.hlsl samples them. GpuDisValidate
// writes motion = v x trust (0 where trust is 0), so the bilinear sample of the motion is the bilinear sum of v_i t_i;
// dividing it by the bilinear trust gives the trust-weighted average of the vectors (review of 24 Nov, MUST 1): a pixel
// whose trust is 0.6 moves the whole third, not 60 % of it, and texels whose flow failed do not pull the vector to 0.
// Inputs per texel: the stored (scaled) vector and its trust, with the bilinear weights tx, ty (0..1). Confidence is
// the bilinear trust. Trust 0 everywhere: motion 0, confidence 0 (a Fallback pixel by the rule above).
struct PushPredictFlowSample { float mx = 0, my = 0, confidence = 0; };
inline PushPredictFlowSample pushPredictTrustedMotion(const float storedX[4], const float storedY[4], const float trust[4], float tx, float ty) {
    // Texel order: (0,0), (1,0), (0,1), (1,1).
    const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    float sx = 0, sy = 0, st = 0;
    for (int i = 0; i < 4; ++i) { sx += w[i] * storedX[i]; sy += w[i] * storedY[i]; st += w[i] * trust[i]; }
    PushPredictFlowSample r;
    r.confidence = st;
    if (st > 1e-6f) { r.mx = sx / st; r.my = sy / st; }
    return r;
}

// Which predicted frame a counter readback belongs to (scheduling review F): the pass numbers every recorded P and
// copies its counters into readback slot (number mod kSlots), so pair n's counters cannot be overwritten by pair n + 2 of
// the same parity before the engine reads them (that took 8 predicted pairs now), and a read of a slot rewritten since
// fails instead of returning another pair's numbers.
struct PushPredictReadbackRing {
    static constexpr unsigned kSlots = 8;
    std::uint64_t next = 0;
    std::uint64_t written[kSlots] = {};
    std::uint64_t record() { const std::uint64_t seq = ++next; written[seq % kSlots] = seq; return seq; }
    bool valid(std::uint64_t seq) const { return seq != 0 && written[seq % kSlots] == seq; }
    static unsigned slotOf(std::uint64_t seq) { return static_cast<unsigned>(seq % kSlots); }
};

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
// Review 2, SHOULD 3: P is replaced by B as a whole when more of its moving pixels fell back than were moved (more than
// half): a mostly held frame shown as a uniform hold rather than a torn mix. The GPU decides it before P is shown
// (shaders/PushPredictVerdict.hlsl + a predicated copy in PushPredictPass::recordPredict); this is the CPU reference.
inline bool pushPredictWholeFrameHeld(const PushPredictCounters& c) { return c.fallback > c.warped; }

// The scheduler's per-pair verdict from the last finished predicted frame: usable while at most maxFallbackShare of the
// moving pixels fell back (a frame that is mostly held is safe but saves nothing visible; one that is half moved and
// half held can show a seam). Default 0.25: an ESTIMATE to be set from his PC (PREDICT_GPU.md section 7).
inline bool pushPredictFrameUsable(const PushPredictCounters& c, double maxFallbackShare = 0.25) {
    return pushPredictFallbackShare(c) <= maxFallbackShare;
}

}  // namespace veyra::pipeline
