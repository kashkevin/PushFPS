// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Package 16, item 2 (test-only check, off in normal play): are the Live HUD Lock and exclusion-box pixels of a predicted
// frame P exactly the real frame B's? EnhanceGraph reads back, at most once every kPushPredictCheckEvery predicted pairs
// and only with VEYRA_TEST_PREDICT_CHECK=1 (or VEYRA_TEST_FAKE_VFG=1, the end-to-end engine test's fake VFG), the final P,
// the presented B and the HUD Lock state, and logs `[push-predict] hudLockMismatch=<n> ...` from this comparison.
//
// "Locked" / "excluded" here means FULLY: the pixels whose compose weight is 1 (PushHudLockCompose.hlsl: stillness and
// edge both at their upper smoothstep bound; EffectExclusion.hlsl / NrProtection.hlsli: at least `feather` pixels inside a
// box). Feathered pixels mix both frames by design and are not counted. A small margin keeps pixels that sit exactly on a
// bound (where float rounding on the GPU may differ from this CPU copy) out of the count. Pure, portable, unit-tested
// (tests/remoteplay/PredictTests.inc).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace veyra::pipeline {

constexpr unsigned kPushPredictCheckEvery = 32;   // predicted pairs between two checks (one in flight at a time)

inline float pushHalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) ? 1u : 0u, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    float value;
    if (exponent == 0) value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) value = mantissa ? std::nanf("") : INFINITY;
    else value = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
    return sign ? -value : value;
}

// The HUD Lock state of one pixel (R16G16_FLOAT: x stillness, y held contrast) at a row-pitched readback.
struct PushHudLockStateView {
    const std::uint8_t* data = nullptr;
    std::uint32_t rowPitch = 0, width = 0, height = 0;
    unsigned outputMode = 0;   // 0 sRGB, 1 scRGB, 2 PQ (PushHudLock's)
    void at(std::uint32_t x, std::uint32_t y, float& still, float& contrast) const {
        std::uint16_t v[2];
        std::memcpy(v, data + std::size_t(y) * rowPitch + std::size_t(x) * 4, 4);
        still = pushHalfToFloat(v[0]); contrast = pushHalfToFloat(v[1]);
    }
};
// Fully locked (compose weight 1), with a margin: stillness at or above 0.95 (+ margin) and a 3x3 neighbour (itself
// included) that is as still and whose held contrast is at or above 2.5 x the edge threshold (+ margin).
inline bool pushHudLockFull(const PushHudLockStateView& s, std::uint32_t x, std::uint32_t y, float margin = 0.002f) {
    float still = 0, contrast = 0;
    s.at(x, y, still, contrast);
    if (!(still >= 0.95f + margin)) return false;
    const float edge = (s.outputMode == 1 ? 0.03f : 0.04f) * 2.5f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const auto qx = std::uint32_t(std::clamp<int>(int(x) + dx, 0, int(s.width) - 1));
            const auto qy = std::uint32_t(std::clamp<int>(int(y) + dy, 0, int(s.height) - 1));
            float qs = 0, qc = 0;
            s.at(qx, qy, qs, qc);
            if (qs >= 0.95f + margin && qc >= edge * (1.0f + margin)) return true;
        }
    return false;
}

// Exclusion boxes as the shader receives them (engine::protectionConstants: normalised left, top, right, bottom; an
// ellipse has left and right swapped). Fully excluded: the protection weight is 1 (NrProtection.hlsli), with a margin.
struct PushPredictBoxes {
    bool enabled = false;
    float feather = 0;
    std::array<std::array<float, 4>, 4> regions{};
};
inline bool pushExclusionFull(const PushPredictBoxes& b, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, float margin = 0.01f) {
    if (!b.enabled) return false;
    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
    for (const auto& q : b.regions) {
        float l = q[0] * float(width), t = q[1] * float(height), r = q[2] * float(width), bo = q[3] * float(height);
        const bool ellipse = r < l;
        if (ellipse) std::swap(l, r);
        if (r <= l || bo <= t) continue;
        float edge;
        if (ellipse) {
            const float cx = (l + r) * 0.5f, cy = (t + bo) * 0.5f;
            const float ax = std::max((r - l) * 0.5f, 1e-3f), ay = std::max((bo - t) * 0.5f, 1e-3f);
            edge = (1.0f - std::hypot((px - cx) / ax, (py - cy) / ay)) * std::min(ax, ay);
        } else {
            edge = std::min(std::min(px - l, r - px), std::min(py - t, bo - py));
        }
        if (b.feather > 0 ? edge >= b.feather + margin : edge >= margin) return true;
    }
    return false;
}

struct PushPredictCheckCounts {
    std::uint64_t hudLocked = 0, hudLockMismatch = 0;     // fully locked pixels (not in a box); of them, P != B
    std::uint64_t excluded = 0, exclusionMismatch = 0;     // fully excluded pixels; of them, P != B
};
// P and B: 32-bit pixels (R8G8B8A8_UNORM or R10G10B10A2_UNORM) at row-pitched readbacks; colour bits compared exactly,
// alpha ignored (the composes keep the generated frame's alpha). state.data null: no HUD Lock.
inline PushPredictCheckCounts pushPredictCheck(const std::uint8_t* p, std::uint32_t pPitch, const std::uint8_t* b, std::uint32_t bPitch,
                                               std::uint32_t width, std::uint32_t height, bool tenBit,
                                               const PushHudLockStateView& state, const PushPredictBoxes& boxes) {
    PushPredictCheckCounts c;
    const std::uint32_t colour = tenBit ? 0x3FFFFFFFu : 0x00FFFFFFu;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool excluded = pushExclusionFull(boxes, x, y, width, height);
            const bool locked = !excluded && state.data && pushHudLockFull(state, x, y);
            if (!excluded && !locked) continue;
            std::uint32_t pw = 0, bw = 0;
            std::memcpy(&pw, p + std::size_t(y) * pPitch + std::size_t(x) * 4, 4);
            std::memcpy(&bw, b + std::size_t(y) * bPitch + std::size_t(x) * 4, 4);
            const bool differs = (pw & colour) != (bw & colour);
            if (excluded) { ++c.excluded; c.exclusionMismatch += differs; }
            else { ++c.hudLocked; c.hudLockMismatch += differs; }
        }
    return c;
}

// Whether a pair is checked: the switch, a predicted pair, no readback in flight, every kPushPredictCheckEvery-th.
inline bool pushPredictCheckDue(bool switchOn, bool predicted, bool inFlight, std::uint64_t predictedPairs) {
    return switchOn && predicted && !inFlight && predictedPairs % kPushPredictCheckEvery == 1;
}

// Package 16, item 4 (d): the start-up self-check of the float-only flow (GPU DIS compiled with `double` as `float`) against
// the strict one (FP64 shader arithmetic) on one synthetic pair whose true motion is known. Both flows are stored the way
// GpuDisValidate writes them: R16G16_FLOAT vectors multiplied by their trust, R8_UNORM trust. The float flow is used by
// default only when it agrees: the share of pixels whose trust class (>= 0.5) differs is at most maxTrustDisagree, the
// mean vector difference over pixels both trust is at most maxMeanDiffPx, and its mean error against the true motion is
// at most the strict flow's plus maxExtraErrorPx (and the strict flow itself found the motion: at least minTrusted of the
// pixels trusted). Thresholds are choices (stated, not tuned on any GPU).
struct PushFlowAgreementLimits {
    double maxTrustDisagree = 0.05, maxMeanDiffPx = 0.25, maxExtraErrorPx = 0.25, minTrusted = 0.5;
};
struct PushFlowAgreement {
    double trustDisagree = 1.0, meanDiffPx = 1e9, strictErrorPx = 1e9, floatErrorPx = 1e9, strictTrusted = 0.0;
    std::uint64_t bothTrusted = 0;
    bool pass = false;
};
struct PushFlowView {
    const std::uint8_t* motion = nullptr; std::uint32_t motionPitch = 0;   // R16G16_FLOAT rows
    const std::uint8_t* trust = nullptr; std::uint32_t trustPitch = 0;     // R8_UNORM rows
};
inline PushFlowAgreement pushFlowAgreement(const PushFlowView& strict, const PushFlowView& fl, std::uint32_t width, std::uint32_t height,
                                           float truthX, float truthY, const PushFlowAgreementLimits& limits = {}) {
    PushFlowAgreement r;
    if (!strict.motion || !strict.trust || !fl.motion || !fl.trust || width == 0 || height == 0) return r;
    const auto vector = [](const PushFlowView& v, std::uint32_t x, std::uint32_t y, float& vx, float& vy, float& t) {
        std::uint16_t h[2];
        std::memcpy(h, v.motion + std::size_t(y) * v.motionPitch + std::size_t(x) * 4, 4);
        t = float(v.trust[std::size_t(y) * v.trustPitch + x]) / 255.0f;
        vx = t > 0 ? pushHalfToFloat(h[0]) / t : 0.0f; vy = t > 0 ? pushHalfToFloat(h[1]) / t : 0.0f;
    };
    std::uint64_t disagree = 0, strictTrusted = 0, floatTrusted = 0;
    double diff = 0, strictError = 0, floatError = 0;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            float sx, sy, st, fx, fy, ft;
            vector(strict, x, y, sx, sy, st);
            vector(fl, x, y, fx, fy, ft);
            const bool a = st >= 0.5f, b = ft >= 0.5f;
            disagree += a != b;
            if (a) { ++strictTrusted; strictError += std::hypot(double(sx - truthX), double(sy - truthY)); }
            if (b) { ++floatTrusted; floatError += std::hypot(double(fx - truthX), double(fy - truthY)); }
            if (a && b) { ++r.bothTrusted; diff += std::hypot(double(sx - fx), double(sy - fy)); }
        }
    const double all = double(width) * double(height);
    r.trustDisagree = double(disagree) / all;
    r.strictTrusted = double(strictTrusted) / all;
    if (r.bothTrusted) r.meanDiffPx = diff / double(r.bothTrusted);
    if (strictTrusted) r.strictErrorPx = strictError / double(strictTrusted);
    if (floatTrusted) r.floatErrorPx = floatError / double(floatTrusted);
    r.pass = r.strictTrusted >= limits.minTrusted && r.trustDisagree <= limits.maxTrustDisagree && r.meanDiffPx <= limits.maxMeanDiffPx &&
             r.floatErrorPx <= r.strictErrorPx + limits.maxExtraErrorPx;
    return r;
}

// Which flow the predicted frame uses. Default (Auto): AMD FidelityFX optical flow, the estimator upstream runs in normal
// operation (decision of 25 Nov 2026: P must not depend on a motion estimator that has never completed a run; the
// strict FP64 GPU DIS removes the device on WARP at its first FP64 shader, dis_perfect_area, gpu-quick run
// 45536213921, and has completed nowhere). VEYRA_TEST_PREDICT_FLOW=strict|float selects GPU DIS for comparison only;
// =fidelityfx names the default; anything else: Auto.
// =zero (tests only, the end-to-end engine test on WARP): no motion source at all, P's flow is zero with full trust, so P
// is B where anything moves (held) and the pair's plumbing, schedule and timing run at no flow cost; logged TEST-ONLY.
enum class PushPredictFlowChoice : std::uint8_t { Auto = 0, Strict, Float, FidelityFx, Zero };
template <class Char>
inline PushPredictFlowChoice pushPredictFlowChoiceFrom(std::basic_string_view<Char> v) {
    const auto is = [&](const char* w) {
        std::size_t n = 0;
        while (w[n]) ++n;
        if (v.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i) if (static_cast<std::uint32_t>(v[i]) != static_cast<std::uint32_t>(w[i])) return false;
        return true;
    };
    return is("strict") ? PushPredictFlowChoice::Strict : is("float") ? PushPredictFlowChoice::Float
         : is("fidelityfx") ? PushPredictFlowChoice::FidelityFx : is("zero") ? PushPredictFlowChoice::Zero : PushPredictFlowChoice::Auto;
}
inline const char* pushPredictFlowChoiceName(PushPredictFlowChoice c) {
    return c == PushPredictFlowChoice::Strict ? "strict" : c == PushPredictFlowChoice::Float ? "float"
         : c == PushPredictFlowChoice::FidelityFx ? "fidelityfx" : c == PushPredictFlowChoice::Zero ? "zero" : "auto";
}
// Crash containment of the flow self-check (2026-11-25; the WARP run of gate 45526571434 stopped with an access violation
// inside it, the device then removed). Which flow the self-check may run, and whether it is then used:
//  - nothing at all when the self-check did not finish in an earlier run of this same program file (a marker file is
//    written before it and removed after it: still there at the next start = PushFPS stopped inside it), or it failed
//    in this process (device removed, a fault caught, a submission not finished): the predicted frame is then unavailable;
//  - exactly one flow is checked: FidelityFX by default, the chosen GPU DIS build otherwise; the strict build only on a
//    device that reports FP64 shader operations (D3D12_OPTIONS.DoublePrecisionFloatShaderOps);
//  - a flow is used only after its own self-run completed, was read back and found the known motion: an untested flow
//    is never used.
enum class PushFlowCheckBlock : std::uint8_t { None = 0, StoppedInsideLastTime, FailedThisProcess };
struct PushFlowCheckPlan {
    bool strict = false, flt = false, fidelity = false;
    PushFlowCheckBlock block = PushFlowCheckBlock::None;
};
inline PushFlowCheckPlan pushFlowCheckPlan(PushPredictFlowChoice choice, bool deviceDoubles, bool fidelityBuilt, PushFlowCheckBlock block) {
    PushFlowCheckPlan p;
    p.block = block;
    if (block != PushFlowCheckBlock::None) return p;
    p.strict = choice == PushPredictFlowChoice::Strict && deviceDoubles;
    p.flt = choice == PushPredictFlowChoice::Float;
    p.fidelity = (choice == PushPredictFlowChoice::Auto || choice == PushPredictFlowChoice::FidelityFx) && fidelityBuilt;
    return p;
}
enum class PushFlowUse : std::uint8_t { None = 0, Strict, Float, FidelityFx };
// ok: the planned flow's self-run completed and was read back; found: it found the known motion (pushFlowFoundMotion).
inline PushFlowUse pushFlowUse(const PushFlowCheckPlan& plan, bool ok, bool found) {
    if (!ok || !found) return PushFlowUse::None;
    return plan.fidelity ? PushFlowUse::FidelityFx : plan.strict ? PushFlowUse::Strict : plan.flt ? PushFlowUse::Float : PushFlowUse::None;
}
// The known-motion test of one flow on its own (the strict-against-float comparison is gone with the strict default):
// at least half of the pixels trusted, and their mean error against the true motion at most maxErrorPx.
inline bool pushFlowFoundMotion(const PushFlowAgreement& selfAgreement, double maxErrorPx = 0.5) {
    return selfAgreement.strictTrusted >= 0.5 && selfAgreement.floatErrorPx <= maxErrorPx;
}
inline const char* pushFlowUseName(PushFlowUse u) {
    return u == PushFlowUse::Strict ? "strict-fp64" : u == PushFlowUse::Float ? "float" : u == PushFlowUse::FidelityFx ? "fidelityfx" : "none";
}
// The FidelityFX start-up self-check, END TO END (orchestrator, 25 Nov 2026: its motion cannot be verified on any machine
// we have, WARP's 4-lane waves cannot run it): synthetic real frames at the WORK size go through exactly what play runs
// (PushPredictPass::recordFlowFrame: downsample, FidelityFX, densify; then recordMotion / recordPredict: the warp), and P
// is compared with the analytically shifted picture at +1/3. A sign or scale mismatch anywhere (FidelityFX's vectors,
// the densify, the flow -> work scale, the warp's direction) makes P worse than B, which a vector comparison in the
// wrong convention could miss. The picture is six windows (3 x 2) through each of which a texture scrolls at its own
// speed: static, and 1, 4, 12, 30 flow pixels per frame in mixed directions (both signs on both axes), so it also
// checks that FidelityFX keeps neighbouring motions apart. Speeds are capped at 1/8 of a window (small test sizes).
struct PushFlowWindow { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; int vx = 0, vy = 0; int flowVx = 0, flowVy = 0; };
constexpr unsigned kPushFlowCheckFrames = 10;   // FidelityFX: 7 frames of warm-up (AmdOpticalFlow::warmedUp) and its 4-frame
                                               // scene-change history must have passed frame index 5; the 10th is scored
inline std::array<PushFlowWindow, 6> pushFlowCheckWindows(std::uint32_t workW, std::uint32_t workH, std::uint32_t flowW, std::uint32_t flowH) {
    const int spec[6][2] = {{0, 0}, {1, 1}, {4, -4}, {-12, 4}, {30, 0}, {-4, -1}};   // flow pixels per frame
    std::array<PushFlowWindow, 6> w{};
    const double sx = flowW ? double(workW) / double(flowW) : 1.0, sy = flowH ? double(workH) / double(flowH) : 1.0;
    for (int i = 0; i < 6; ++i) {
        auto& o = w[std::size_t(i)];
        o.x0 = int(workW) * (i % 3) / 3; o.x1 = int(workW) * (i % 3 + 1) / 3;
        o.y0 = int(workH) * (i / 3) / 2; o.y1 = int(workH) * (i / 3 + 1) / 2;
        const int capX = std::max(1, int(double(o.x1 - o.x0) / sx / 8.0)), capY = std::max(1, int(double(o.y1 - o.y0) / sy / 8.0));
        o.flowVx = std::clamp(spec[i][0], -capX, capX); o.flowVy = std::clamp(spec[i][1], -capY, capY);
        if (spec[i][0] == 0) o.flowVx = 0;
        if (spec[i][1] == 0) o.flowVy = 0;
        o.vx = int(std::lround(o.flowVx * sx)); o.vy = int(std::lround(o.flowVy * sy));
    }
    return w;
}
// The texture (grey levels 0..255, smooth, features a few flow pixels wide at a 4x work/flow ratio): separable parts
// so play-size frames are filled quickly; continuous, so the truth at a fractional shift is exact.
inline double pushFlowCheckGrey(double x, double y) {
    return 128.0 + 45.0 * std::sin(0.21 * x) + 35.0 * std::sin(0.17 * y) + 25.0 * std::sin(0.53 * x) * std::cos(0.29 * y);
}
// The FidelityFX check's texture (stream "ownfg", 25 Nov 2026, after his Package 16 log: "did not find the known
// motions", every moving window wrong, the static one right). pushFlowCheckGrey above is periodic with all its energy at
// periods of 3-9 FLOW pixels: FidelityFX's coarse-to-fine 8x8 block search (+-8 px on 7 pyramid levels, SDK 1.1.4) sees
// nothing at its coarse levels and many equal minima at the fine ones, so it locks onto aliases. A FidelityFX-like model
// of that search (tools/pushwarp_eval/ffx_check_sim.py, SIMULATED, CPU) gives 0-22 % right vectors in the moving windows
// on that texture and 91-100 % on this one. This texture: value noise at 512, 128, 32 and 8 work pixels (non-periodic,
// energy at every pyramid level), as separable parts and products of 1-D noises so play-size frames fill from tables.
inline double pushFlowCheckNoise(double t, double cell, std::uint32_t seed) {
    const double fi = std::floor(t / cell), f = t / cell - fi;
    const auto hash = [seed](double i) {
        std::uint32_t n = std::uint32_t(std::int64_t(i)) * 374761393u + seed * 668265263u;
        n = (n ^ (n >> 13)) * 1274126177u;
        return double((n ^ (n >> 16)) & 0xFFFFu) / 65535.0 * 2.0 - 1.0;
    };
    const double s = f * f * (3.0 - 2.0 * f);
    return hash(fi) * (1.0 - s) + hash(fi + 1.0) * s;
}
struct PushFlowCheckOctave { double cell, amplitude; };
constexpr std::array<PushFlowCheckOctave, 4> kPushFlowCheckOctaves{{{512.0, 24.0}, {128.0, 18.0}, {32.0, 12.0}, {8.0, 8.0}}};
// The parts of octave k at x (or y): the separable half-terms and the product factors (pushFlowCheckGreyBroadband).
inline double pushFlowCheckSeparable(double t, unsigned k, bool vertical) {
    return 0.5 * kPushFlowCheckOctaves[k].amplitude * pushFlowCheckNoise(t, kPushFlowCheckOctaves[k].cell, 2u * k + (vertical ? 2u : 1u));
}
inline double pushFlowCheckFactor(double t, unsigned k, bool vertical) {
    return (vertical ? 1.0 : kPushFlowCheckOctaves[k].amplitude) * pushFlowCheckNoise(t, kPushFlowCheckOctaves[k].cell * 0.7, 2u * k + (vertical ? 12u : 11u));
}
inline double pushFlowCheckGreyBroadband(double x, double y) {
    double g = 128.0;
    for (unsigned k = 0; k < kPushFlowCheckOctaves.size(); ++k)
        g += pushFlowCheckSeparable(x, k, false) + pushFlowCheckSeparable(y, k, true) + pushFlowCheckFactor(x, k, false) * pushFlowCheckFactor(y, k, true);
    return g;
}
// The raw FidelityFX vectors of one window (diagnostics of the check, his next log): blocks two blocks inside the
// window; truth = the window's current -> previous motion in flow pixels (-flowVx, -flowVy); "right" = exactly the
// truth (the windows move whole flow pixels; a tolerance of 1 would count the zero vector right in the 1-px window).
struct PushFlowRawStats {
    std::uint32_t blocks = 0, right = 0, nonzero = 0;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    double meanX = 0.0, meanY = 0.0;
};
// raw: rawW x rawH pairs of int16 (R16G16_SINT, row pitch in pairs), block = 8 flow pixels; window in WORK pixels with
// workPerFlowX / workPerFlowY the work/flow ratio.
inline PushFlowRawStats pushFlowRawWindowStats(const std::int16_t* raw, std::uint32_t rowPitchPairs, std::uint32_t rawW, std::uint32_t rawH,
                                               const PushFlowWindow& w, double workPerFlowX, double workPerFlowY) {
    PushFlowRawStats s;
    if (!raw || workPerFlowX <= 0.0 || workPerFlowY <= 0.0) return s;
    const int bx0 = int(std::ceil(double(w.x0) / workPerFlowX / 8.0)) + 2, bx1 = int(std::floor(double(w.x1) / workPerFlowX / 8.0)) - 2;
    const int by0 = int(std::ceil(double(w.y0) / workPerFlowY / 8.0)) + 2, by1 = int(std::floor(double(w.y1) / workPerFlowY / 8.0)) - 2;
    double sx = 0.0, sy = 0.0;
    for (int by = std::max(by0, 0); by < std::min(by1, int(rawH)); ++by)
        for (int bx = std::max(bx0, 0); bx < std::min(bx1, int(rawW)); ++bx) {
            const std::int16_t* v = raw + (std::size_t(by) * rowPitchPairs + std::size_t(bx)) * 2;
            const int vx = v[0], vy = v[1];
            if (s.blocks == 0) { s.minX = s.maxX = vx; s.minY = s.maxY = vy; }
            s.minX = std::min(s.minX, vx); s.maxX = std::max(s.maxX, vx); s.minY = std::min(s.minY, vy); s.maxY = std::max(s.maxY, vy);
            ++s.blocks;
            if (vx != 0 || vy != 0) ++s.nonzero;
            if (vx == -w.flowVx && vy == -w.flowVy) ++s.right;
            sx += vx; sy += vy;
        }
    if (s.blocks) { s.meanX = sx / s.blocks; s.meanY = sy / s.blocks; }
    return s;
}
// Review 3 SHOULD 4: what FidelityFX was fed. The flow-size picture (B8G8R8A8, the grey in R) of one window's inside
// (2 flow px in from its edges): mean and standard deviation of the grey, and the largest difference from `expected` (the
// CPU's box downsample of the same synthetic frame, one value per flow pixel, -1 where unknown). Whether a wrong vector
// comes from FidelityFX or from what it read (downsample, state, a stale input).
struct PushFlowPictureStats { std::uint32_t pixels = 0; double mean = 0.0, stdev = 0.0; int maxDiff = -1; };
inline PushFlowPictureStats pushFlowPictureWindowStats(const std::uint8_t* bgra, std::size_t rowPitch, std::uint32_t fw, std::uint32_t fh,
                                                       const std::vector<int>& expected, const PushFlowWindow& w, double workPerFlowX, double workPerFlowY) {
    PushFlowPictureStats s;
    if (!bgra || workPerFlowX <= 0.0 || workPerFlowY <= 0.0) return s;
    const int x0 = int(std::ceil(double(w.x0) / workPerFlowX)) + 2, x1 = int(std::floor(double(w.x1) / workPerFlowX)) - 2;
    const int y0 = int(std::ceil(double(w.y0) / workPerFlowY)) + 2, y1 = int(std::floor(double(w.y1) / workPerFlowY)) - 2;
    double sum = 0.0, sq = 0.0;
    for (int y = std::max(y0, 0); y < std::min(y1, int(fh)); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, int(fw)); ++x) {
            const int g = bgra[std::size_t(y) * rowPitch + std::size_t(x) * 4 + 2];
            ++s.pixels; sum += g; sq += double(g) * g;
            const std::size_t i = std::size_t(y) * fw + std::size_t(x);
            if (i < expected.size() && expected[i] >= 0) s.maxDiff = std::max(s.maxDiff, std::abs(g - expected[i]));
        }
    if (s.pixels) { s.mean = sum / s.pixels; s.stdev = std::sqrt(std::max(0.0, sq / s.pixels - s.mean * s.mean)); }
    return s;
}

// The densified field of one window (what P warps with): R16G16_FLOAT v x trust and R8_UNORM trust at the flow size,
// pixels 16 flow pixels inside the window; trusted = trust >= 0.5; right = |v - truth| <= 0.5 flow pixel.
struct PushFlowFieldStats {
    std::uint32_t pixels = 0, trusted = 0, right = 0;
    double meanX = 0.0, meanY = 0.0;   // over the trusted pixels
};
inline PushFlowFieldStats pushFlowFieldWindowStats(const std::uint16_t* motion, std::uint32_t motionPitchPairs, const std::uint8_t* trust, std::uint32_t trustPitch,
                                                   std::uint32_t flowW, std::uint32_t flowH, const PushFlowWindow& w, double workPerFlowX, double workPerFlowY) {
    PushFlowFieldStats s;
    if (!motion || !trust || workPerFlowX <= 0.0 || workPerFlowY <= 0.0) return s;
    const int x0 = int(std::ceil(double(w.x0) / workPerFlowX)) + 16, x1 = int(std::floor(double(w.x1) / workPerFlowX)) - 16;
    const int y0 = int(std::ceil(double(w.y0) / workPerFlowY)) + 16, y1 = int(std::floor(double(w.y1) / workPerFlowY)) - 16;
    double sx = 0.0, sy = 0.0;
    for (int y = std::max(y0, 0); y < std::min(y1, int(flowH)); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, int(flowW)); ++x) {
            ++s.pixels;
            const double t = double(trust[std::size_t(y) * trustPitch + std::size_t(x)]) / 255.0;
            if (t < 0.5) continue;
            const std::uint16_t* m = motion + (std::size_t(y) * motionPitchPairs + std::size_t(x)) * 2;
            const double vx = double(pushHalfToFloat(m[0])) / t, vy = double(pushHalfToFloat(m[1])) / t;
            ++s.trusted; sx += vx; sy += vy;
            if (std::abs(vx + w.flowVx) <= 0.5 && std::abs(vy + w.flowVy) <= 0.5) ++s.right;
        }
    if (s.trusted) { s.meanX = sx / s.trusted; s.meanY = sy / s.trusted; }
    return s;
}
// Pixels scored in a window: inside it by the motion over the pair and P's extra third, plus one FidelityFX block (its
// 8 x 8 flow-pixel cells straddle the window edges) and 4 pixels. A window with nothing left to score fails.
inline int pushFlowCheckMargin(const PushFlowWindow& w, double blockWork) {
    return int(std::ceil(std::max(std::abs(w.vx), std::abs(w.vy)) * (1.0 + 1.0 / 3.0) + blockWork)) + 4;
}
// A window passes: static -> P equals the picture (mean error at most 1 grey level); moving -> P's mean error against the
// truth at +1/3 is at most half of B's (B = what showing the real frame again would give). Choices, stated.
inline bool pushFlowWindowPass(const PushFlowWindow& w, double errP, double errB) {
    if (w.vx == 0 && w.vy == 0) return errP <= 1.0;
    return errB > 0.0 && errP <= 0.5 * errB;
}
// The self-check's hard time budget (orchestrator, 25 Nov 2026: a start-up freeze of unknown length is not acceptable).
// Release review REL16 MUST 3: enforced by construction: each frame's GPU wait is the budget still left (at least
// kPushFlowCheckWaitFloorMs), and the budget is checked after every frame, the last included. Past it the check stops and
// the source is unavailable for this run; a frame still in flight then keeps its objects (the pass's own included) for
// the rest of the process and counts as a failure (REL16 delta MUST 2: remembered by the marker until an update). So
// the check itself ends within kPushFlowCheckBudgetMs + kPushFlowCheckWaitFloorMs of its start (plus the CPU work of one
// frame); the graph rebuild around it is not part of the check.
constexpr double kPushFlowCheckBudgetMs = 3000.0;
constexpr double kPushFlowCheckWaitFloorMs = 250.0;
inline bool pushFlowCheckOverBudget(double elapsedMs) { return elapsedMs > kPushFlowCheckBudgetMs; }
// The GPU wait for the next frame (ms): what is left of the budget, at least the floor; 10 s with the budget off (the
// test-only mechanics switch on WARP).
// REL16 delta MUST 2: what a self-check's end means. A failure writes the marker "failed <identity>", caches nothing
// and blocks every later check of the process; a frame that may still be on the GPU (hung, device removed, or still in
// flight when the budget ran out) also keeps every object it uses, the pass's own included.
inline bool pushFlowCheckFailed(bool hung, bool removed, bool faulted, bool inFlight) { return hung || removed || faulted || inFlight; }
inline bool pushFlowCheckGpuMayStillUse(bool hung, bool removed, bool inFlight) { return hung || removed || inFlight; }
inline double pushFlowCheckWaitMs(double elapsedMs, bool budgetOff) {
    if (budgetOff) return 10000.0;
    const double left = kPushFlowCheckBudgetMs - elapsedMs;
    return left > kPushFlowCheckWaitFloorMs ? left : kPushFlowCheckWaitFloorMs;
}
inline double pushPsnr(double meanSquared) { return meanSquared > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / meanSquared) : 99.0; }

// The marker file's text: "running <identity>" while a self-check runs, "failed <identity>" once one is known not to have
// finished (written by the next start, or by this process after a failure).
// identity names the program file (its write time and size), so an update runs the self-check again.
inline PushFlowCheckBlock pushFlowCheckMarker(std::string_view marker, std::string_view identity) {
    const auto rest = [&](std::string_view head) { return marker.size() > head.size() && marker.substr(0, head.size()) == head ? marker.substr(head.size()) : std::string_view(); };
    const auto trim = [](std::string_view v) { while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.remove_suffix(1); return v; };
    for (const char* head : {"running ", "failed "})
        if (!rest(head).empty() && trim(rest(head)) == trim(identity)) return PushFlowCheckBlock::StoppedInsideLastTime;
    return PushFlowCheckBlock::None;   // no marker, or one left by another program file (an update runs the check again)
}

}  // namespace veyra::pipeline
