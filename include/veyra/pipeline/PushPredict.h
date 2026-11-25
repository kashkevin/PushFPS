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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

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
    HudLockOrExclusion, // Package 15 only: no longer produced since Package 16 (Live HUD Lock and exclusion boxes compose
                        // into P from B, the real frame P follows: EnhanceGraph's predicted pair); kept for old logs
    PresentSinkFg,    // XeSS present-sink generation owns the presentation
    UnsupportedFormat, // VFG's buffers: R8G8B8A8_UNORM (SDR) or R10G10B10A2_UNORM (HDR10); scRGB FP16 has no VFG
    PassUnavailable,  // the predict pass or its flow could not be created
    LowConfidence,    // the scheduler's own decision from the previous pair's counters (pushPredictFrameUsable)
    VfgRefused,       // review 2, SHOULD 9: VFG refused or failed a FrameIndex-2-only request this session (one attempt; the
                      // row switched off and on allows another). Frame generation goes on as Package 14's.
    SmoothnessMode,   // Package 15 only (review 2, SHOULD 4): the engine refused whenever Smoothness Mode had a target.
                      // No longer produced since Package 16 (PushLowDelay.h pushLowDelayStaticRefusal); kept for old logs
    MotionNotReady,   // 25 Nov 2026: P's FidelityFX motion of this real frame is not usable (FidelityFX warming up after a
                      // reset: 7 frames; or the first frame after a gap, whose vectors span two frame intervals)
    FlowTooSlow,      // review 1 SHOULD 4: the motion source's own start-up self-check took more GPU time than
                      // kPushLowDelayEntryCostShare of an output slot on this card (PushLowDelay.h): the mode is not entered
};

struct PushPredictEligibility {
    PushPairSchedule requested = PushPairSchedule::Interpolate;
    bool vfg = false;
    unsigned multiplier = 0;
    bool historyValid = false;
    bool hudLock = false;      // Package 16: informational only (no longer a refusal)
    bool exclusion = false;    // Package 16: informational only (no longer a refusal)
    bool presentSinkFg = false;
    bool formatSupported = false;
    bool passReady = false;
    bool vfgRefused = false;   // PushLowDelayShared::vfgRefused
    bool confident = true;   // the scheduler's verdict from the last counters; true when it has none yet
    bool motionReady = true; // PushPredictPass::motionReady for this real frame (per pair, like historyValid)
};

inline PushPredictRefusal pushPredictPairRefusal(const PushPredictEligibility& e) {
    if (e.requested != PushPairSchedule::OnePredicted) return PushPredictRefusal::SwitchOff;
    if (e.presentSinkFg) return PushPredictRefusal::PresentSinkFg;
    if (!e.vfg) return PushPredictRefusal::NotVfg;
    if (e.multiplier != 3) return PushPredictRefusal::NotThreeX;
    if (!e.formatSupported) return PushPredictRefusal::UnsupportedFormat;
    if (!e.passReady) return PushPredictRefusal::PassUnavailable;
    if (e.vfgRefused) return PushPredictRefusal::VfgRefused;
    if (!e.historyValid) return PushPredictRefusal::NoHistory;
    if (!e.motionReady) return PushPredictRefusal::MotionNotReady;
    if (!e.confident) return PushPredictRefusal::LowConfidence;
    return PushPredictRefusal::Ok;
}

// The part of the refusal known before a pair is asked (settings and the graph's shape: no pass, not VFG, not 3X, format,
// present-sink FG; Package 15 also refused Live HUD Lock and exclusion boxes here). The scheduler does not ask while one holds (Ineligible, no back-off);
// only the per-pair reasons (no history, a shot change, low confidence) remain for an asked pair.
inline PushPredictRefusal pushPredictStaticRefusal(PushPredictEligibility e) {
    e.historyValid = true; e.confident = true; e.motionReady = true;
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
    case PushPredictRefusal::MotionNotReady: return "motion-not-ready";
    case PushPredictRefusal::FlowTooSlow: return "flow-too-slow";
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

// Why pushPredictCreate declined (the e2e scenario m2, 25 Nov 2026: the row on at a CONFIGURED 2X read "unavailable" /
// switch-off, because no pass exists and the graph then reported SwitchOff). The graph reports this reason as its static
// refusal while no pass exists, so the row reads "not here" with the real cause. Ok when it was created (or not asked).
inline PushPredictRefusal pushPredictCreateRefusal(PushPairSchedule asked, bool fgEnabled, bool vfg, bool otherGenerator, bool presentSink,
                                                   unsigned configuredMultiplier, bool formatOk) {
    if (asked != PushPairSchedule::OnePredicted) return PushPredictRefusal::SwitchOff;
    if (presentSink) return PushPredictRefusal::PresentSinkFg;
    if (!fgEnabled || !vfg || otherGenerator) return PushPredictRefusal::NotVfg;
    if (configuredMultiplier < 3) return PushPredictRefusal::NotThreeX;
    if (!formatOk) return PushPredictRefusal::UnsupportedFormat;
    return PushPredictRefusal::Ok;
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

// Flow extent for the work extent: a quarter in each axis (3840x2160 -> 960x540), at most 4096 per axis, and at least
// kPushPredictMinFlowAxis where the work extent has that many pixels (AMD FidelityFX optical flow builds a 7-level input
// pyramid, the last level at 1/64: under 64 pixels an axis has no level 6 and context creation fails; found on WARP,
// gpu-quick run 45538401850, at 64x36). A work axis under 64 pixels keeps its own size (FidelityFX refuses it: logged).
// Motion is measured on the enhanced real frames, after SR and NR, at this size.
struct PushExtent { std::uint32_t width = 0, height = 0; };
constexpr std::uint32_t kPushPredictMinFlowAxis = 64;
inline PushExtent pushPredictFlowExtent(std::uint32_t workWidth, std::uint32_t workHeight) {
    auto axis = [](std::uint32_t v) { return std::clamp<std::uint32_t>((v + 3u) / 4u, std::min(kPushPredictMinFlowAxis, v), 4096u); };
    return {axis(workWidth), axis(workHeight)};
}
inline bool pushPredictFidelityExtentOk(PushExtent e) { return e.width >= kPushPredictMinFlowAxis && e.height >= kPushPredictMinFlowAxis; }
// FidelityFX optical flow's block search reduces its SADs and minimums with wave operations over a 64-thread group and
// combines two waves only for a lane count of 32 (ffx_opticalflow_compute_optical_flow_v5.h BlockSad64 /
// SadMapMinReduction256): with fewer lanes per wave the reductions cover part of the group and the search returns zero
// vectors. WARP reports 4 lanes: gpu-quick run 45542585360 found all-zero vectors at every size (64x64 .. 960x540). So
// the source is used only where D3D12_OPTIONS1.WaveLaneCountMin is at least 32.
constexpr unsigned kPushPredictFidelityMinWaveLanes = 32;
inline bool pushPredictFidelityWaveOk(unsigned waveLaneCountMin) { return waveLaneCountMin >= kPushPredictFidelityMinWaveLanes; }

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

// Package 16: Live HUD Lock and exclusion boxes in a predicted pair (EnhanceGraph). The rule is the one every generated
// frame follows, with the right real frame: a generated frame's locked / excluded pixels come from the real frame it is
// generated from. I(2/3) lies between A and B and is composed as today's interpolated frames are (held parity A, raw A,
// with the lock's state already updated with B in the base list); P is B advanced, so its pixels come from B (a copy of
// B taken from VFG's input buffer: PushHudLock's held slot kPredictedHeld; raw B for the boxes). The state is updated
// once per real frame only (in B's base list), never with a predicted frame.
struct PushPredictComposeSources { unsigned hudHeld = 0, exclusionRaw = 0; };
constexpr unsigned kPushPredictHeldCopy = 2;   // PushHudLock::compose's held index for the copy of B
inline PushPredictComposeSources pushPredictComposeSources(bool predicted, unsigned parity) {
    return predicted ? PushPredictComposeSources{kPushPredictHeldCopy, parity} : PushPredictComposeSources{1u - parity, 1u - parity};
}

// Package 16, item 4 (forensics F2): the predicted frame's GPU time by stage, from five timestamps the pass writes in its
// own list (PushPredictPass): before the downsample, after it, after the flow (GPU DIS + validate), after the warp, after
// the whole-frame verdict and its predicated copy. MEASURED on whatever GPU runs it; -1 where unknown. The graph's
// composes after recordPredict (HUD Lock, boxes) are in the Fg2 stage of the graph's own timer, not here.
enum class PushPredictStage : std::uint8_t { Downsample = 0, Flow, Warp, Verdict, Total, Count };
inline const char* pushPredictStageName(PushPredictStage s) {
    switch (s) {
    case PushPredictStage::Downsample: return "downsample";
    case PushPredictStage::Flow: return "flow";
    case PushPredictStage::Warp: return "warp";
    case PushPredictStage::Verdict: return "verdict";
    case PushPredictStage::Total: return "total";
    default: return "?";
    }
}
constexpr unsigned kPushPredictStamps = 5;
using PushPredictStageMs = std::array<double, static_cast<unsigned>(PushPredictStage::Count)>;
// Ticks in submission order; a tick counter that went backwards or a zero frequency gives -1 for that stage.
inline PushPredictStageMs pushPredictStagesFromTicks(const std::uint64_t (&t)[kPushPredictStamps], std::uint64_t frequency) {
    PushPredictStageMs ms;
    ms.fill(-1.0);
    if (frequency == 0) return ms;
    const auto span = [&](unsigned a, unsigned b) { return t[b] >= t[a] && t[a] != 0 ? double(t[b] - t[a]) * 1000.0 / double(frequency) : -1.0; };
    for (unsigned i = 0; i + 1 < kPushPredictStamps; ++i) ms[i] = span(i, i + 1);
    ms[static_cast<unsigned>(PushPredictStage::Total)] = span(0, kPushPredictStamps - 1);
    return ms;
}
// The last kWindow100ns of samples per stage (at most kMaxSamples), with nearest-rank percentiles.
class PushPredictCostWindow {
public:
    static constexpr std::int64_t kWindow100ns = 100000000;   // 10 s
    static constexpr std::size_t kMaxSamples = 512;
    void clear() { samples_.clear(); }
    void add(std::int64_t now100ns, const PushPredictStageMs& ms) {
        samples_.push_back({now100ns, ms});
        while (samples_.size() > kMaxSamples || (!samples_.empty() && samples_.front().first < now100ns - kWindow100ns)) samples_.pop_front();
    }
    std::size_t size() const { return samples_.size(); }
    // Percentile q (0..1) of a stage over the window; -1 with no measured sample.
    double percentile(PushPredictStage stage, double q) const {
        std::vector<double> v;
        v.reserve(samples_.size());
        for (const auto& s : samples_) { const double x = s.second[static_cast<unsigned>(stage)]; if (x >= 0) v.push_back(x); }
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        const auto rank = static_cast<std::size_t>(std::ceil(std::clamp(q, 0.0, 1.0) * double(v.size())));
        return v[rank == 0 ? 0 : rank - 1];
    }

private:
    std::deque<std::pair<std::int64_t, PushPredictStageMs>> samples_;
};
// For [capture-timing]: "samples:<n>,downsample:P50/P95/P99,flow:...,warp:...,verdict:...,total:..." in ms (-1 unknown).
inline std::string pushPredictCostText(const PushPredictCostWindow& w) {
    std::string t = "samples:" + std::to_string(w.size());
    for (unsigned i = 0; i < static_cast<unsigned>(PushPredictStage::Count); ++i) {
        const auto stage = static_cast<PushPredictStage>(i);
        char part[96];
        const int n = std::snprintf(part, sizeof part, ",%s:%.2f/%.2f/%.2f", pushPredictStageName(stage), w.percentile(stage, 0.5), w.percentile(stage, 0.95), w.percentile(stage, 0.99));
        if (n > 0) t.append(part, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof part) - 1)));
    }
    return t;
}

// Package 16, item 5 (forensics F3): how a predicted pair asks NVIDIA VFG for I(2/3) when it is not the pair's first
// request. NVIDIA's page for the filter documents FrameMultiplier M with FrameIndex 1..M-1 ("t = i / M"; its example runs
// 1, 2, 3 in sequence, no ordering rule stated) and an explicit timestep mode (FrameMultiplier 0, Timestep in (0, 1) set
// before each run; setting FrameMultiplier resets FrameIndex and Timestep). Tried in this order, each at most once per
// backend, the first that works kept for the rest of the session:
//   Direct          FrameIndex 2 of 3 alone: one Run (the cost of one generated frame).
//   Timestep        FrameMultiplier 0, Timestep 2/3: one Run; the multiplier is set back to 3 before the next normal pair.
//   IndexOneThenTwo FrameIndex 1 (into the slot this pair leaves free, discarded), then 2: two Runs. Arithmetic with his
//                   measured 2.0-2.3 ms per VFG frame at 4K: I is ready about one frame's run (about 2 ms) later than with
//                   Direct, against one output slot (11.1 ms at 29.97 FPS) the schedule moves B by. Not a measurement.
// All refused: the mode is unavailable for the session (as Package 15); frame generation itself goes on in every case.
enum class PushVfgAloneMode : std::uint8_t { Direct = 0, Timestep, IndexOneThenTwo, Refused };
inline const char* pushVfgAloneModeName(PushVfgAloneMode m) {
    switch (m) {
    case PushVfgAloneMode::Direct: return "direct";
    case PushVfgAloneMode::Timestep: return "timestep";
    case PushVfgAloneMode::IndexOneThenTwo: return "index1-then-2";
    case PushVfgAloneMode::Refused: return "refused";
    }
    return "?";
}
// Review 1 SHOULD 6 (25 Nov 2026): VFG's FrameIndex-2-only ladder can be tried again. (a) A real off -> on of the row
// (PushLowDelayShared::retryEpoch) resets it, at most once every kPushVfgLadderResetGapMs; (b) a refusal within
// kPushVfgEarlyRefusalMs of the graph's build is not taken as the session's answer: one reset kPushVfgEarlyRetryMs later.
constexpr std::int64_t kPushVfgLadderResetGapMs = 10000, kPushVfgEarlyRefusalMs = 1000, kPushVfgEarlyRetryMs = 2000;
inline bool pushVfgLadderResetDue(std::int64_t nowMs, std::int64_t lastResetMs) { return lastResetMs < 0 || nowMs - lastResetMs >= kPushVfgLadderResetGapMs; }
inline bool pushVfgEarlyRefusal(std::int64_t msSinceBuild, bool retryUsed) { return !retryUsed && msSinceBuild >= 0 && msSinceBuild < kPushVfgEarlyRefusalMs; }

class PushVfgAloneLadder {
public:
    // timestepAvailable: the SDK exports NvVFX_SetF32 (needed for Timestep); otherwise that step is skipped.
    explicit PushVfgAloneLadder(bool timestepAvailable = true) : timestep_(timestepAvailable) {}
    PushVfgAloneMode current() const { return mode_; }
    bool settled() const { return settled_; }
    // The outcome of a request made in current(); returns the mode to try next for the SAME pair (current() again on
    // success; Refused when nothing is left).
    PushVfgAloneMode outcome(bool ok) {
        if (mode_ == PushVfgAloneMode::Refused) return mode_;
        if (ok) { settled_ = true; return mode_; }
        if (settled_) { mode_ = PushVfgAloneMode::Refused; return mode_; }   // a mode that worked failed later: stop asking
        if (mode_ == PushVfgAloneMode::Direct) mode_ = timestep_ ? PushVfgAloneMode::Timestep : PushVfgAloneMode::IndexOneThenTwo;
        else if (mode_ == PushVfgAloneMode::Timestep) mode_ = PushVfgAloneMode::IndexOneThenTwo;
        else mode_ = PushVfgAloneMode::Refused;
        return mode_;
    }

private:
    PushVfgAloneMode mode_ = PushVfgAloneMode::Direct;
    bool timestep_ = true, settled_ = false;
};
// The Timestep value of FrameIndex `index` of multiplier `m` (2/3 for I(2/3)).
inline float pushVfgTimestep(unsigned index, unsigned multiplier) { return multiplier ? float(index) / float(multiplier) : 0.0f; }

}  // namespace veyra::pipeline
