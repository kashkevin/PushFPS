// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg" (25 Nov 2026): PushWarp, the project's own interpolated frame I(t) between two real frames A and B, at
// the full working size (3840x2160 for his 4K picture), from ONE motion field per pair at a reduced size: the same B -> A
// field the predicted frame P uses (PushPredictPass::motion / trust: AMD FidelityFX optical flow densified, or a test
// source), computed once per real frame and shared. Design: docs research OWN_FG_DESIGN.md sections 3 and 4.3 (flow
// reversal from one direction, `interp_reversal_one` of tools/ownfg_eval/gen.py on branch exp/s9-ownfg).
//
// Passes (shaders/PushWarp*.hlsl), all at the flow size except the last two:
//   W3 trust   per flow cell: the vector g (B -> A, flow pixels) = stored / trust, its trust, and a block-SAD trust: the
//              mean of the largest colour difference between B and A shifted by g over 4 x 4 work pixels of the cell
//              (OWNFG_RESULTS.md 5.4: the cheap trust signal that measured best for I). Untrusted cells do not splat.
//   R1 splat   B's cells forward to where their content is at time t (p + (1 - t) g), carrying g; collisions resolved by
//              a softmax of the speed (the faster mover is taken to be in front: there is no depth), in the two-pass form
//              integer atomics give exactly: pass 1 InterlockedMax of the quantised importance per target cell, pass 2
//              InterlockedAdd of fixed-point weights x vectors, weights and coverage. No float atomics, no wave operations.
//   R2 fill    push-pull pyramid: cells nothing landed on get their motion from the neighbourhood.
//   R3 compose at the work size: still pixels (A == B within one code) are A exactly; elsewhere one bilinear tap of A at
//              p + t g and one of B at p - (1 - t) g, blended by time (PQ code values for HDR10: OWNFG_RESULTS.md 5.3);
//              where the two samples disagree by more than 24/255 the one with the larger weight alone (no double image).
//   verdict    when more moving pixels are holes (nothing of B landed there: a cut, a failed flow) than not, the whole
//              frame is the real frame nearer in time (B for t >= 1/2), on the GPU (a predicated copy), as P's verdict.
//
// Everything here is pure (no D3D12): the parameters, the fixed-point rules, the pyramid layout and a CPU REFERENCE of
// the whole pass that the WARP tests (tests/integration/PushGpuTests.cpp, warp-*) compare the GPU output with, and the
// start-up self-check compares the user's own GPU with (PushWarpPass). Unit tests: tests/remoteplay/WarpTests.inc.
//
// Measured picture quality (OFFLINE, synthetic 4K scenes, CPU: OWNFG_RESULTS.md): about level with a learned interpolator
// on average, worse at thin objects in front of a moving background; not measured on game footage; not measured on any
// graphics card. This reference implements the same scheme with its own fixed-point splat and pyramid; it is not bit-
// identical to the harness (which used OpenCV's resize for the pyramid and float splats).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace veyra::pipeline {

struct PushWarpParams {
    float t = 2.0f / 3.0f;                 // the frame's time between A (0) and B (1)
    float stillDelta = 1.0f / 1023.0f;     // max |A - B| over RGB of a still pixel (HUD, letterbox): the output is A exactly
    float conflictDelta = 24.0f / 255.0f;  // two warped samples further apart than this: the larger weight alone
    float minConfidence = 0.5f;            // the flow's own trust below this: the cell does not splat
    float maxMotionShare = 0.125f;         // a vector longer than 1/8 of the flow width per interval is not trusted
    float sadLow = 4.0f / 255.0f;          // block-SAD trust: 1 below sadLow, 0 above sadHigh (smoothstep), code values
    float sadHigh = 16.0f / 255.0f;
    float coverageFull = 0.5f;             // coverage (sum of bilinear x trust) that counts as fully visible
    float holeVisibility = 0.05f;          // visibility below this: a hole (nothing of B landed there)
    float fillKnown = 0.25f;               // a cell with less coverage takes its motion from the pyramid
    float beta = 1.0f;                     // importance per flow pixel of speed (softmax of the depth proxy)
    // Step 4 ("fully causal" schedule): t > 1 makes a PREDICTED frame B + (t - 1) of the motion, FORWARD-PROJECTED from B
    // alone (the splat moves B's cells to where their content will be; places nothing lands on hold B; still pixels are B
    // exactly), the scheme OWNFG_RESULTS.md 5.1 found best for P (forward + acceleration: P(1/3) 27.41 dB, P(2/3) 25.51 dB,
    // worst 5 % 23.69 / 22.28, against the gather's 27.07 / 25.32 and 23.17 / 21.23; synthetic scenes, CPU).
    // Acceleration from the previous pair's field (Z -> A, on A's grid) where the motion changed by at least accelMin
    // flow px per interval (a start or a stop: where the study's gain was, turn_start_stop +1.8 / +1.4 dB); below that the
    // acceleration is taken as noise and not used. Displacement d = v s + a (s/2 + s^2/2), the acceleration term clamped to
    // the speed term (the study's rule).
    bool acceleration = false;
    float accelMin = 0.5f;
    float accelShare = 0.0f;   // the acceleration only where it is at least this share of the larger speed (0: always); set below once measured
};
inline bool pushWarpPredicting(const PushWarpParams& p) { return p.t > 1.0f; }

// Fixed-point scales of the splat (identical in shaders/PushWarpSplat.hlsl). Bound: a vector is at most maxMotionShare x
// the flow width (120 flow px at 960 wide) = 3840 in 1/32 units; x a weight of at most 256 = 983040 per tap; int32 holds
// about 2180 such taps landing on one cell (a 46 x 46 cell area collapsing onto one cell inside one interval).
constexpr int kPushWarpWeightScale = 256;
constexpr int kPushWarpVectorScale = 32;
constexpr float kPushWarpImportanceScale = 64.0f;
constexpr std::uint32_t kPushWarpImportanceCap = 1u << 20;
constexpr unsigned kPushWarpMaxLevels = 16;

// Per-pixel kinds the compose counts (four uint32 per slot, this order).
enum class PushWarpPixel : std::uint8_t { Still = 0, Blend = 1, Conflict = 2, Hole = 3 };
struct PushWarpCounters { std::uint32_t still = 0, blend = 0, conflict = 0, hole = 0; };
// The whole-frame verdict (shaders/PushWarpVerdict.hlsl): more holes than other moving pixels.
inline bool pushWarpWholeFrameNearest(const PushWarpCounters& c) { return c.hole > c.blend + c.conflict; }
// Share of the moving pixels that are holes; 0 when nothing moved.
inline double pushWarpHoleShare(const PushWarpCounters& c) {
    const double moving = double(c.blend) + double(c.conflict) + double(c.hole);
    return moving > 0 ? double(c.hole) / moving : 0.0;
}

// The pyramid of the fill (R2): level 0 is the flow size; each next level halves (rounding up) while both axes stay
// above 2; at most kPushWarpMaxLevels. Cells of all levels are stored one after another (offsets in cells).
struct PushWarpPyramid {
    unsigned levels = 0;
    std::array<std::uint32_t, kPushWarpMaxLevels> width{}, height{}, offset{};
    std::uint32_t cells = 0;   // all levels
};
inline PushWarpPyramid pushWarpPyramid(std::uint32_t w, std::uint32_t h) {
    PushWarpPyramid p;
    if (w == 0 || h == 0) return p;
    std::uint32_t cw = w, ch = h, at = 0;
    for (;;) {
        p.width[p.levels] = cw; p.height[p.levels] = ch; p.offset[p.levels] = at;
        at += cw * ch;
        ++p.levels;
        if (std::min(cw, ch) <= 2 || p.levels == kPushWarpMaxLevels) break;
        cw = (cw + 1) / 2; ch = (ch + 1) / 2;
    }
    p.cells = at;
    return p;
}

// Byte layout of the pass's scratch buffer (one RWByteAddressBuffer; identical in every PushWarp shader through the
// constants the pass sets): cells (16 bytes: gx, gy, trust, importance), accumulators (32 bytes: top, wsum, cov, accx,
// accy, 3 unused), pyramid cells of all levels (32 bytes: premultiplied vx, vy, known, visibility, estimate x, y, 2 unused).
struct PushWarpScratch {
    std::uint64_t cells = 0, accum = 0, pyramid = 0, bytes = 0;
};
inline PushWarpScratch pushWarpScratch(std::uint32_t flowW, std::uint32_t flowH) {
    PushWarpScratch s;
    const std::uint64_t n = std::uint64_t(flowW) * flowH;
    s.cells = 0;
    s.accum = n * 16;
    s.pyramid = s.accum + n * 32;
    s.bytes = s.pyramid + std::uint64_t(pushWarpPyramid(flowW, flowH).cells) * 32;
    return s;
}

// ---- the arithmetic both sides share --------------------------------------------------------------------------------

struct PushWarpColor { float r = 0, g = 0, b = 0, a = 0; };
inline PushWarpColor pushWarpUnpack(std::uint32_t word, bool tenBit) {
    if (tenBit)
        return {float(word & 1023u) / 1023.0f, float((word >> 10) & 1023u) / 1023.0f, float((word >> 20) & 1023u) / 1023.0f, float(word >> 30) / 3.0f};
    return {float(word & 255u) / 255.0f, float((word >> 8) & 255u) / 255.0f, float((word >> 16) & 255u) / 255.0f, float(word >> 24) / 255.0f};
}
// float -> UNORM as D3D stores it into a UNORM UAV: clamp, scale, round to nearest (the GPU may differ by one code at
// an exact half: the tests compare within one code).
inline std::uint32_t pushWarpPack(const PushWarpColor& c, bool tenBit) {
    const auto q = [](float v, float m) { return std::uint32_t(std::lround(double(std::clamp(v, 0.0f, 1.0f)) * double(m))); };
    if (tenBit) return q(c.r, 1023.0f) | (q(c.g, 1023.0f) << 10) | (q(c.b, 1023.0f) << 20) | (q(c.a, 3.0f) << 30);
    return q(c.r, 255.0f) | (q(c.g, 255.0f) << 8) | (q(c.b, 255.0f) << 16) | (q(c.a, 255.0f) << 24);
}
inline float pushWarpMaxDelta(const PushWarpColor& x, const PushWarpColor& y) {
    return std::max(std::abs(x.r - y.r), std::max(std::abs(x.g - y.g), std::abs(x.b - y.b)));
}
inline float pushWarpSmoothstep(float lo, float hi, float x) {
    const float s = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return s * s * (3.0f - 2.0f * s);
}
// floor(x + 0.5) as an int (HLSL: int(floor(x + 0.5))).
inline int pushWarpRound(float x) { return int(std::floor(x + 0.5f)); }
// The importance of a vector of length `length` flow pixels, quantised (HLSL: min(uint(length * beta * 64 + 0.5), cap)).
inline std::uint32_t pushWarpImportance(float length, float beta) {
    const float v = length * beta * kPushWarpImportanceScale + 0.5f;
    return v >= float(kPushWarpImportanceCap) ? kPushWarpImportanceCap : std::uint32_t(std::max(v, 0.0f));
}

// The 4 x 4 work pixels of flow cell (cx, cy) the block SAD samples: x_i = x0 + i (x1 - x0) / 4 with
// x0 = cx W / gw, x1 = max(x0 + 1, (cx + 1) W / gw) (the downsample's footprint).
inline std::uint32_t pushWarpFootprint(std::uint32_t cell, std::uint32_t i, std::uint32_t work, std::uint32_t grid) {
    const std::uint32_t x0 = cell * work / grid, x1 = std::max(x0 + 1, (cell + 1) * work / grid);
    return std::min(x0 + i * (x1 - x0) / 4, work - 1);
}

// ---- the CPU reference ----------------------------------------------------------------------------------------------

// A real frame as the pass reads it: 32-bit words (R8G8B8A8_UNORM or R10G10B10A2_UNORM), `pitch` words per row.
struct PushWarpFrame {
    const std::uint32_t* words = nullptr;
    std::uint32_t width = 0, height = 0, pitch = 0;   // pitch in words
    std::uint32_t at(int x, int y) const {
        const int cx = std::clamp(x, 0, int(width) - 1), cy = std::clamp(y, 0, int(height) - 1);
        return words[std::size_t(cy) * pitch + std::size_t(cx)];
    }
};
// The B -> A field as the pass reads it: stored vector (v x trust, flow pixels) and trust, per flow cell, row-major.
struct PushWarpField {
    const float* storedX = nullptr;
    const float* storedY = nullptr;
    const float* trust = nullptr;
    std::uint32_t width = 0, height = 0;
};
struct PushWarpCell { float gx = 0, gy = 0, trust = 0; std::uint32_t importance = 0; };
// The B -> A vector of field f at flow cell (x, y) (stored / trust), and whether it is trusted (minConfidence).
inline bool pushWarpVectorAt(const PushWarpField& f, int x, int y, float minConfidence, float& vx, float& vy) {
    vx = vy = 0;
    if (!f.trust || x < 0 || y < 0 || x >= int(f.width) || y >= int(f.height)) return false;
    const std::size_t i = std::size_t(y) * f.width + std::size_t(x);
    const float t = f.trust[i];
    if (!(t >= minConfidence)) return false;
    vx = f.storedX[i] / t; vy = f.storedY[i] / t;
    return true;
}
// Prediction with acceleration (PushWarpParams::acceleration): the effective B -> A vector g' whose forward projection by
// s = t - 1 is the accelerated displacement: d = -g s + a k, k = s/2 + s^2/2, a = gz - g (gz: the previous pair's vector
// where B's content was in A, nearest cell), |a k| <= |g| s, a = 0 when |a| < accelMin or gz untrusted; g' = -d / s.
inline void pushWarpAccelerated(float s, float gx, float gy, float gzx, float gzy, bool gzOk, float accelMin, float& ex, float& ey, float accelShare = 0.0f) {
    ex = gx; ey = gy;
    if (!gzOk || !(s > 0)) return;
    float ax = gzx - gx, ay = gzy - gy;
    const float alen = std::sqrt(ax * ax + ay * ay);
    if (!(alen >= accelMin)) return;
    // Only a large change of speed (a start or a stop: the offline study's only gain); a small one on steady motion is the
    // block field's noise and costs picture (tools/pushwarp_eval).
    if (accelShare > 0) {
        const float g = std::sqrt(gx * gx + gy * gy), z = std::sqrt(gzx * gzx + gzy * gzy);
        if (!(alen >= accelShare * (g > z ? g : z))) return;
    }
    const float k = s * 0.5f + s * s * 0.5f;
    const float limit = std::sqrt(gx * gx + gy * gy) * s, termLen = alen * k;
    const float scale = termLen > limit ? (termLen > 0 ? limit / termLen : 0.0f) : 1.0f;
    ax *= scale; ay *= scale;
    const float dx = -gx * s + ax * k, dy = -gy * s + ay * k;
    ex = -dx / s; ey = -dy / s;
}
struct PushWarpAccum { std::uint32_t top = 0, wsum = 0, cov = 0; std::int32_t accx = 0, accy = 0; };
struct PushWarpLevelCell { float vx = 0, vy = 0, known = 0, visibility = 0, ex = 0, ey = 0; };

struct PushWarpResult {
    std::vector<std::uint32_t> words;   // width x height, tight
    PushWarpCounters counters;
    bool wholeFrameNearest = false;
    std::vector<PushWarpCell> cells;          // W3, for the tests
    std::vector<PushWarpLevelCell> field;     // level 0 after the fill
};

inline PushWarpColor pushWarpBilinear(const PushWarpFrame& f, bool tenBit, float qx, float qy) {
    const float fx = std::floor(qx), fy = std::floor(qy);
    const float tx = qx - fx, ty = qy - fy;
    const int ax = int(fx), ay = int(fy);
    const auto c00 = pushWarpUnpack(f.at(ax, ay), tenBit), c10 = pushWarpUnpack(f.at(ax + 1, ay), tenBit);
    const auto c01 = pushWarpUnpack(f.at(ax, ay + 1), tenBit), c11 = pushWarpUnpack(f.at(ax + 1, ay + 1), tenBit);
    const auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
    PushWarpColor r;
    r.r = lerp(lerp(c00.r, c10.r, tx), lerp(c01.r, c11.r, tx), ty);
    r.g = lerp(lerp(c00.g, c10.g, tx), lerp(c01.g, c11.g, tx), ty);
    r.b = lerp(lerp(c00.b, c10.b, tx), lerp(c01.b, c11.b, tx), ty);
    r.a = lerp(lerp(c00.a, c10.a, tx), lerp(c01.a, c11.a, tx), ty);
    return r;
}

// W3: one cell.
inline PushWarpCell pushWarpCellOf(const PushWarpFrame& a, const PushWarpFrame& b, bool tenBit, const PushWarpField& field,
                                   std::uint32_t cx, std::uint32_t cy, const PushWarpParams& p, const PushWarpField* previous = nullptr) {
    PushWarpCell c;
    const std::size_t i = std::size_t(cy) * field.width + cx;
    const float conf = field.trust[i];
    if (!(conf > 1e-6f)) return c;
    c.gx = field.storedX[i] / conf; c.gy = field.storedY[i] / conf;
    const float length = std::sqrt(c.gx * c.gx + c.gy * c.gy);
    if (!(conf >= p.minConfidence) || !(length <= p.maxMotionShare * float(field.width))) { c.gx = c.gy = 0; return c; }
    const float sx = float(b.width) / float(field.width), sy = float(b.height) / float(field.height);
    const int ox = pushWarpRound(c.gx * sx), oy = pushWarpRound(c.gy * sy);
    float sum = 0;
    for (std::uint32_t j = 0; j < 4; ++j)
        for (std::uint32_t k = 0; k < 4; ++k) {
            const int x = int(pushWarpFootprint(cx, k, b.width, field.width)), y = int(pushWarpFootprint(cy, j, b.height, field.height));
            sum += pushWarpMaxDelta(pushWarpUnpack(b.at(x, y), tenBit), pushWarpUnpack(a.at(x + ox, y + oy), tenBit));
        }
    const float mad = sum / 16.0f;
    c.trust = conf * (1.0f - pushWarpSmoothstep(p.sadLow, p.sadHigh, mad));
    c.importance = pushWarpImportance(length, p.beta);
    if (pushWarpPredicting(p) && p.acceleration && previous) {
        float zx = 0, zy = 0;
        const bool ok = pushWarpVectorAt(*previous, pushWarpRound(float(cx) + c.gx), pushWarpRound(float(cy) + c.gy), p.minConfidence, zx, zy);
        pushWarpAccelerated(p.t - 1.0f, c.gx, c.gy, zx, zy, ok, p.accelMin, c.gx, c.gy, p.accelShare);
    }
    return c;
}

// R1, one tap of one cell: target cell index and bilinear weight, false when outside or zero.
struct PushWarpTap { std::uint32_t index = 0; float weight = 0; bool ok = false; };
inline std::array<PushWarpTap, 4> pushWarpTaps(std::uint32_t cx, std::uint32_t cy, const PushWarpCell& c, float t, std::uint32_t gw, std::uint32_t gh) {
    std::array<PushWarpTap, 4> taps{};
    const float qx = float(cx) + (1.0f - t) * c.gx, qy = float(cy) + (1.0f - t) * c.gy;
    const float fx = std::floor(qx), fy = std::floor(qy);
    const float tx = qx - fx, ty = qy - fy;
    const int x0 = int(fx), y0 = int(fy);
    const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    for (int k = 0; k < 4; ++k) {
        const int x = x0 + (k & 1), y = y0 + (k >> 1);
        auto& tap = taps[std::size_t(k)];
        tap.ok = c.trust > 0 && w[k] > 0 && x >= 0 && y >= 0 && x < int(gw) && y < int(gh);
        if (tap.ok) { tap.index = std::uint32_t(y) * gw + std::uint32_t(x); tap.weight = w[k]; }
    }
    return taps;
}

inline PushWarpResult pushWarpReference(const PushWarpFrame& a, const PushWarpFrame& b, bool tenBit, const PushWarpField& field,
                                        const PushWarpParams& p = {}, const PushWarpField* previous = nullptr) {
    const bool predicting = pushWarpPredicting(p);
    PushWarpResult r;
    const std::uint32_t W = b.width, H = b.height, gw = field.width, gh = field.height;
    if (W == 0 || H == 0 || gw == 0 || gh == 0) return r;
    const std::size_t n = std::size_t(gw) * gh;
    // W3
    r.cells.resize(n);
    for (std::uint32_t y = 0; y < gh; ++y)
        for (std::uint32_t x = 0; x < gw; ++x) r.cells[std::size_t(y) * gw + x] = pushWarpCellOf(a, b, tenBit, field, x, y, p, previous);
    // R1 pass 1 and 2
    std::vector<PushWarpAccum> acc(n);
    for (std::uint32_t y = 0; y < gh; ++y)
        for (std::uint32_t x = 0; x < gw; ++x) {
            const auto& c = r.cells[std::size_t(y) * gw + x];
            for (const auto& tap : pushWarpTaps(x, y, c, p.t, gw, gh))
                if (tap.ok) acc[tap.index].top = std::max(acc[tap.index].top, c.importance);
        }
    for (std::uint32_t y = 0; y < gh; ++y)
        for (std::uint32_t x = 0; x < gw; ++x) {
            const auto& c = r.cells[std::size_t(y) * gw + x];
            const std::int32_t vx = pushWarpRound(c.gx * float(kPushWarpVectorScale)), vy = pushWarpRound(c.gy * float(kPushWarpVectorScale));
            for (const auto& tap : pushWarpTaps(x, y, c, p.t, gw, gh)) {
                if (!tap.ok) continue;
                auto& s = acc[tap.index];
                const float soft = std::exp((float(c.importance) - float(s.top)) / kPushWarpImportanceScale);
                const auto wq = std::uint32_t(tap.weight * c.trust * soft * float(kPushWarpWeightScale) + 0.5f);
                const auto cq = std::uint32_t(tap.weight * c.trust * float(kPushWarpWeightScale) + 0.5f);
                s.wsum += wq; s.cov += cq;
                s.accx += std::int32_t(wq) * vx; s.accy += std::int32_t(wq) * vy;
            }
        }
    // R2: level 0 from the accumulators, push (down), pull (up).
    const auto pyr = pushWarpPyramid(gw, gh);
    std::vector<PushWarpLevelCell> cells(pyr.cells);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& s = acc[i];
        auto& o = cells[i];
        const float cov = float(s.cov) / float(kPushWarpWeightScale);
        o.visibility = std::clamp(cov / p.coverageFull, 0.0f, 1.0f);
        const bool known = s.wsum > 0 && cov >= p.fillKnown;
        o.known = known ? 1.0f : 0.0f;
        if (known) {
            o.vx = float(s.accx) / float(s.wsum) / float(kPushWarpVectorScale);
            o.vy = float(s.accy) / float(s.wsum) / float(kPushWarpVectorScale);
        }
    }
    for (unsigned l = 1; l < pyr.levels; ++l)
        for (std::uint32_t y = 0; y < pyr.height[l]; ++y)
            for (std::uint32_t x = 0; x < pyr.width[l]; ++x) {
                float vx = 0, vy = 0, k = 0, count = 0;
                for (std::uint32_t j = 0; j < 4; ++j) {
                    const std::uint32_t chx = 2 * x + (j & 1), chy = 2 * y + (j >> 1);
                    if (chx >= pyr.width[l - 1] || chy >= pyr.height[l - 1]) continue;
                    const auto& ch = cells[pyr.offset[l - 1] + chy * pyr.width[l - 1] + chx];
                    vx += ch.vx; vy += ch.vy; k += ch.known; count += 1.0f;
                }
                auto& o = cells[pyr.offset[l] + y * pyr.width[l] + x];
                o.vx = vx / count; o.vy = vy / count; o.known = k / count;
            }
    for (unsigned li = pyr.levels; li-- > 0;)
        for (std::uint32_t y = 0; y < pyr.height[li]; ++y)
            for (std::uint32_t x = 0; x < pyr.width[li]; ++x) {
                auto& o = cells[pyr.offset[li] + y * pyr.width[li] + x];
                const float ox = o.known > 1e-6f ? o.vx / o.known : 0.0f, oy = o.known > 1e-6f ? o.vy / o.known : 0.0f;
                if (li + 1 == pyr.levels) { o.ex = ox; o.ey = oy; continue; }
                const unsigned l = li + 1;
                const float qx = std::clamp((float(x) + 0.5f) * 0.5f - 0.5f, 0.0f, float(pyr.width[l] - 1));
                const float qy = std::clamp((float(y) + 0.5f) * 0.5f - 0.5f, 0.0f, float(pyr.height[l] - 1));
                const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
                const std::uint32_t x0 = std::uint32_t(fx), y0 = std::uint32_t(fy);
                const std::uint32_t x1 = std::min(x0 + 1, pyr.width[l] - 1), y1 = std::min(y0 + 1, pyr.height[l] - 1);
                const auto& c00 = cells[pyr.offset[l] + y0 * pyr.width[l] + x0];
                const auto& c10 = cells[pyr.offset[l] + y0 * pyr.width[l] + x1];
                const auto& c01 = cells[pyr.offset[l] + y1 * pyr.width[l] + x0];
                const auto& c11 = cells[pyr.offset[l] + y1 * pyr.width[l] + x1];
                const float ux = (c00.ex + (c10.ex - c00.ex) * tx) + ((c01.ex + (c11.ex - c01.ex) * tx) - (c00.ex + (c10.ex - c00.ex) * tx)) * ty;
                const float uy = (c00.ey + (c10.ey - c00.ey) * tx) + ((c01.ey + (c11.ey - c01.ey) * tx) - (c00.ey + (c10.ey - c00.ey) * tx)) * ty;
                const float s = std::clamp(o.known, 0.0f, 1.0f);
                o.ex = s * ox + (1.0f - s) * ux;
                o.ey = s * oy + (1.0f - s) * uy;
            }
    r.field.assign(cells.begin(), cells.begin() + std::ptrdiff_t(n));
    // R3
    r.words.resize(std::size_t(W) * H);
    const float sx = float(gw) / float(W), sy = float(gh) / float(H);
    const float scaleX = float(W) / float(gw), scaleY = float(H) / float(gh);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint32_t aw = a.at(int(x), int(y)), bw = b.at(int(x), int(y));
            const auto ca = pushWarpUnpack(aw, tenBit), cb = pushWarpUnpack(bw, tenBit);
            if (pushWarpMaxDelta(ca, cb) <= p.stillDelta) { r.words[std::size_t(y) * W + x] = predicting ? bw : aw; ++r.counters.still; continue; }
            const float qx = std::max((float(x) + 0.5f) * sx - 0.5f, 0.0f), qy = std::max((float(y) + 0.5f) * sy - 0.5f, 0.0f);
            const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
            const std::uint32_t x0 = std::min(std::uint32_t(fx), gw - 1), y0 = std::min(std::uint32_t(fy), gh - 1);
            const std::uint32_t x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
            const auto& f00 = r.field[y0 * gw + x0];
            const auto& f10 = r.field[y0 * gw + x1];
            const auto& f01 = r.field[y1 * gw + x0];
            const auto& f11 = r.field[y1 * gw + x1];
            const auto bl = [&](float PushWarpLevelCell::*m) {
                const float top = f00.*m + (f10.*m - f00.*m) * tx, bottom = f01.*m + (f11.*m - f01.*m) * tx;
                return top + (bottom - top) * ty;
            };
            const float gx = bl(&PushWarpLevelCell::ex) * scaleX, gy = bl(&PushWarpLevelCell::ey) * scaleY;
            float vis = bl(&PushWarpLevelCell::visibility);
            const bool hole = vis < p.holeVisibility;
            if (predicting) {   // from B alone: nothing landed here (revealed, untrusted) -> B held; else B moved
                if (hole) { r.words[std::size_t(y) * W + x] = bw; ++r.counters.hole; continue; }
                r.words[std::size_t(y) * W + x] = pushWarpPack(pushWarpBilinear(b, tenBit, float(x) - (1.0f - p.t) * gx, float(y) - (1.0f - p.t) * gy), tenBit);
                ++r.counters.blend;
                continue;
            }
            if (hole) vis = 1.0f;
            const auto s0 = pushWarpBilinear(a, tenBit, float(x) + p.t * gx, float(y) + p.t * gy);
            const auto s1 = pushWarpBilinear(b, tenBit, float(x) - (1.0f - p.t) * gx, float(y) - (1.0f - p.t) * gy);
            const float w0 = (1.0f - p.t) * vis, w1 = p.t * vis;
            PushWarpColor out;
            const bool conflict = pushWarpMaxDelta(s0, s1) > p.conflictDelta;
            if (conflict) out = w0 >= w1 ? s0 : s1;
            else {
                const float d = w0 + w1;
                out = {(s0.r * w0 + s1.r * w1) / d, (s0.g * w0 + s1.g * w1) / d, (s0.b * w0 + s1.b * w1) / d, (s0.a * w0 + s1.a * w1) / d};
            }
            r.words[std::size_t(y) * W + x] = pushWarpPack(out, tenBit);
            if (hole) ++r.counters.hole; else if (conflict) ++r.counters.conflict; else ++r.counters.blend;
        }
    r.wholeFrameNearest = pushWarpWholeFrameNearest(r.counters);
    if (r.wholeFrameNearest) {
        const PushWarpFrame& nearest = p.t >= 0.5f ? b : a;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) r.words[std::size_t(y) * W + x] = nearest.at(int(x), int(y));
    }
    return r;
}

// Per-pixel comparison of two outputs: the number of pixels whose colour channels differ by more than `codes` codes.
inline std::uint64_t pushWarpDiffering(const std::vector<std::uint32_t>& x, const std::vector<std::uint32_t>& y, bool tenBit, int codes) {
    const int bits = tenBit ? 10 : 8, mask = (1 << bits) - 1;
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i)
        for (int c = 0; c < 3; ++c) {
            const int u = int((x[i] >> (c * bits)) & std::uint32_t(mask)), v = int((y[i] >> (c * bits)) & std::uint32_t(mask));
            if (std::abs(u - v) > codes) { ++n; break; }
        }
    return n + (x.size() > y.size() ? x.size() - y.size() : y.size() - x.size());
}

// Stage GPU times of one I (PushWarpPass timestamps): trust, splat, fill, compose (with its verdict), total. -1 unknown.
enum class PushWarpStage : std::uint8_t { Trust = 0, Splat, Fill, Compose, Total, Count };
inline const char* pushWarpStageName(PushWarpStage s) {
    switch (s) {
    case PushWarpStage::Trust: return "trust";
    case PushWarpStage::Splat: return "splat";
    case PushWarpStage::Fill: return "fill";
    case PushWarpStage::Compose: return "compose";
    case PushWarpStage::Total: return "total";
    default: return "?";
    }
}
constexpr unsigned kPushWarpStamps = 5;
using PushWarpStageMs = std::array<double, static_cast<unsigned>(PushWarpStage::Count)>;
inline PushWarpStageMs pushWarpStagesFromTicks(const std::uint64_t (&t)[kPushWarpStamps], std::uint64_t frequency) {
    PushWarpStageMs ms;
    ms.fill(-1.0);
    if (frequency == 0) return ms;
    const auto span = [&](unsigned x, unsigned y) { return t[y] >= t[x] && t[x] != 0 ? double(t[y] - t[x]) * 1000.0 / double(frequency) : -1.0; };
    for (unsigned i = 0; i + 1 < kPushWarpStamps; ++i) ms[i] = span(i, i + 1);
    ms[static_cast<unsigned>(PushWarpStage::Total)] = span(0, kPushWarpStamps - 1);
    return ms;
}

// The start-up self-check (PushWarpPass::selfCheck): a small synthetic pair through the GPU pass, compared with this
// reference. Passes when at most kPushWarpCheckMaxDiffering of the pixels differ by more than one code and the counters
// agree within kPushWarpCheckCounterSlack of the pixels (float rounding of exp / bilinear weights can move a few pixels
// across a threshold). Choices, stated.
constexpr double kPushWarpCheckMaxDiffering = 0.002, kPushWarpCheckCounterSlack = 0.002;
// The self-check's sizes (work and flow). The pass's scratch holds at least the self-check's flow as well as its own
// (found on WARP, gpu-quick run 45565487744: a pass made for a 32x16 flow refused the self-check's 64x36 field).
constexpr std::uint32_t kPushWarpCheckWidth = 256, kPushWarpCheckHeight = 144, kPushWarpCheckFlowWidth = 64, kPushWarpCheckFlowHeight = 36;
inline std::uint64_t pushWarpScratchBytes(std::uint32_t flowW, std::uint32_t flowH) {
    return std::max(pushWarpScratch(flowW, flowH).bytes, pushWarpScratch(kPushWarpCheckFlowWidth, kPushWarpCheckFlowHeight).bytes);
}
inline bool pushWarpCheckPass(std::uint64_t differing, std::uint64_t pixels, const PushWarpCounters& gpu, const PushWarpCounters& cpu) {
    if (pixels == 0) return false;
    const auto close = [&](std::uint32_t x, std::uint32_t y) { return double(x > y ? x - y : y - x) <= kPushWarpCheckCounterSlack * double(pixels) + 1.0; };
    return double(differing) <= kPushWarpCheckMaxDiffering * double(pixels) && close(gpu.still, cpu.still) && close(gpu.blend, cpu.blend) &&
           close(gpu.conflict, cpu.conflict) && close(gpu.hole, cpu.hole);
}

}  // namespace veyra::pipeline
