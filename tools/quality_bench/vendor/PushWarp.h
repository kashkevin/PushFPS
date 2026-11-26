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
    // A vector longer than 1/4 of the flow width per real-frame interval (240 flow px, 960 px at 3840 wide) is not trusted.
    // It was 1/8 (480 px at 4K) until 25 Nov 2026: above that a fast turn held the whole frame (a visible repeat). OFFLINE
    // (tools/pushwarp_eval/fast_pan.py, synthetic 4K turns of 500-900 px per interval on the harness's FidelityFX-like
    // field, CPU): 1/4 is +1.1 to +5.4 dB over 1/8 there and no frame is held whole; 1/2 adds -0.1 to +0.2 dB;
    // the study's scenes up to about 330 px (P95) are identical at 1/8, 1/4 and 1/2. Not measured on a graphics card.
    float maxMotionShare = 0.25f;
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
    // Static overlays (crosshair, HUD text, minimap ring; see "Static overlays" below): bit 1 static override, bit 2 source
    // exclusion. 0: off (the frame as before). Interpolated frames only (t <= 1); a predicted frame ignores it.
    unsigned overlayRules = 0;
    float accelShare = 0.0f;   // the acceleration only where it is at least this share of the larger speed (0: always); set below once measured
};
inline bool pushWarpPredicting(const PushWarpParams& p) { return p.t > 1.0f; }

// Fixed-point scales of the splat (identical in shaders/PushWarpSplat.hlsl). Bound: a vector is at most maxMotionShare x
// the flow width (240 flow px at 960 wide with 1/4) = 7680 in 1/32 units; x a weight of at most 256 = 1966080 per tap;
// int32 holds about 1090 such taps landing on one cell (half the headroom of the earlier 1/8; a wrap on the GPU is a wrong
// vector for that cell, never a fault).
constexpr int kPushWarpWeightScale = 256;
constexpr int kPushWarpVectorScale = 32;
constexpr float kPushWarpImportanceScale = 64.0f;
constexpr std::uint32_t kPushWarpImportanceCap = 1u << 20;
constexpr unsigned kPushWarpMaxLevels = 16;

// Per-pixel kinds the compose counts (four uint32 per slot, this order).
enum class PushWarpPixel : std::uint8_t { Still = 0, Blend = 1, Conflict = 2, Hole = 3 };
struct PushWarpCounters { std::uint32_t still = 0, blend = 0, conflict = 0, hole = 0; };
// The fast-motion evidence (shaders/PushWarpTrust.hlsl): trusted vectors per flow cell by length in full-size pixels per
// real frame, 32 bins on a square-root scale: bin b covers [b^2, (b+1)^2) px, the last one 961 px and more (since the
// cut-off became 1/4, 25 Nov 2026: fast turns of up to 960 px are trusted, so the evidence must reach that far; it was
// [(b/2)^2, ((b+1)/2)^2) with 240 px as the last bin).
constexpr unsigned kPushWarpMotionBins = 32;
using PushWarpMotionHistogram = std::array<std::uint32_t, kPushWarpMotionBins>;
inline unsigned pushWarpMotionBin(float lengthPx) {
    if (!(lengthPx > 0)) return 0;
    const float b = std::sqrt(lengthPx);
    return b >= float(kPushWarpMotionBins - 1) ? kPushWarpMotionBins - 1 : unsigned(b);
}
// Nearest-rank quantile q of the histogram as the bin's middle in px (the last bin reads 961: "961 px or more");
// -1 when empty.
template <class Count>
inline double pushWarpMotionQuantile(const std::array<Count, kPushWarpMotionBins>& h, double q) {
    double total = 0;
    for (auto c : h) total += double(c);
    if (!(total > 0)) return -1.0;
    const double rank = std::ceil(std::clamp(q, 0.0, 1.0) * total);
    double seen = 0;
    for (unsigned b = 0; b < kPushWarpMotionBins; ++b) {
        seen += double(h[b]);
        if (seen >= rank && h[b] > 0) {
            if (b == kPushWarpMotionBins - 1) return 961.0;
            const double lo = double(b), hi = double(b + 1);
            return 0.5 * (lo * lo + hi * hi);
        }
    }
    return 961.0;
}
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

// ---- Static overlays (crosshair, HUD text, minimap ring) over a moving picture: stream "ownfg", 25 Nov 2026 -------------
// The owner sees about five faded copies of the crosshair in a circular camera move at 3X. A generated pixel takes its
// colour from A at p + t g and from B at p - (1 - t) g; wherever the background's vector points at the crosshair, the
// crosshair is fetched there: a copy. PushWarp's own rule, from A, B and the pair's field (its cells: one mask per pair,
// no history, nothing waits;
// the classic MEMC "static pixel inside a moving region"): a pixel is an OVERLAY CORE pixel when it is still (A and B
// within kPushWarpOverlayStill), the field there moves (|g| >= kPushWarpOverlayMinMove work px) AND the motion does not
// explain it (B(x) differs from A(x + g), where its content came from, by more than kPushWarpOverlayContrast): a smooth
// moving area is still too, but A(x + g) matches it; both directions are checked (also A(x) against B(x - g)), so a
// background pixel next to the overlay, whose A(x + g) is the overlay, is not taken). The anti-aliased rim (partly covered, so it changes a little with
// the background) is added where |A - B| <= kPushWarpOverlayRim next to a core pixel and the motion does not explain
// it either. Mask bits (pushWarpOverlayMask):
//   1  overlay (core or rim): with rule 1 its pixel is A + t (B - A) of the real frames (at t > 1: B): it stays put
//   2  an overlay pixel within 1 px (the sources): with rule 2 a tap that lands there is not used, the other side's tap
//      alone is ("only use pixels from the other direction"); both sides there: the one on an overlay pixel (bit 1)
//      loses; both on it: unchanged
// Translucent overlays that change strongly with the background are not still: not found, not protected.
// Three gates keep the rule off pictures it would damage (tools/pushwarp_eval/hud_eval.py --damage; the HUD Guard review's
// cases, 25 Nov 2026). A core pixel also needs:
//   structure    an edge of its own: within 2 px (4 directions) a pixel differs by kPushWarpOverlayStructure or more, in A
//                and in B
//                (a low-contrast sky or haze that is "still" only because it has nothing to change is not an overlay)
//   surround     the picture around it moves: at least kPushWarpOverlayShare of the flow cells within
//                kPushWarpOverlayReach cells are trusted and move kPushWarpOverlayMinMove or more (a still background
//                that a fast thin object crossed between A and B is not: the object would be erased)
//   no vector    no trusted moving vector of the 3 x 3 cells around it explains it (a fence or grille moved by exactly its
//                period is still, but its own motion explains it), and the moving cells within kPushWarpOverlaySpread agree
//                (pushWarpOverlayAgrees: where the flow aliased a repeating texture its vectors jump) and their mean does
//                not explain it either
// HDR10: the thresholds are in PQ code values, where the SDR range covers about 58 % of the codes: still, rim and structure
// (and the compose's own still test, PushWarpParams::stillDelta) are scaled by kPushWarpOverlayPqScale so that they mean
// about the same picture change as in SDR; the contrast ("unexplained") is not (review 3 SHOULD 7).
constexpr float kPushWarpOverlayStill = 3.0f / 255.0f;
constexpr float kPushWarpOverlayRim = 24.0f / 255.0f;
constexpr float kPushWarpOverlayMinMove = 1.5f;
constexpr float kPushWarpOverlayContrast = 16.0f / 255.0f;
constexpr int kPushWarpOverlayRun = 16;   // work px: how far an overlay segment along the motion is followed from a core pixel
constexpr float kPushWarpOverlayStructure = 13.0f / 255.0f;   // about 0.05
constexpr float kPushWarpOverlayShare = 0.5f;
constexpr int kPushWarpOverlayReach = 4;   // flow cells (16 work px at the play ratio 4:1)
constexpr float kPushWarpOverlayPqScale = 0.6f;
inline float pushWarpOverlayScale(bool tenBit) { return tenBit ? kPushWarpOverlayPqScale : 1.0f; }
// With the overlay rules, "a still pixel is A exactly" holds only where the filled field moves less than
// kPushWarpStillField work px or the mask calls the pixel an overlay (review 3 MUST 1b): elsewhere it is interpolated
// like a moving pixel, so an object that crossed it between A and B is not erased; but only where the two moved samples
// agree (otherwise A): the block field gives a thin object's vector to the background around it too, and that vector
// fetches the object into the background (a copy) where the two samples disagree.
constexpr float kPushWarpStillField = 0.5f;
// Only where the picture around stands still (moving share of the cells within kPushWarpOverlayReach below
// kPushWarpStillSurround): the case of an object crossing a still background. In a pan or turn a still pixel is a flat
// area that happens to match, and moving it with a field mixed from its neighbours made more pixels worse than better
// (trunks over a sky, hud_eval.py --damage): there it stays A.
constexpr float kPushWarpStillSurround = 0.5f;
// ... and only where the two moved samples agree within this (tighter than conflictDelta: on texture two unrelated samples
// agree within 24/255 often enough to make pixels worse).
constexpr float kPushWarpStillAgree = 12.0f / 255.0f;
inline bool pushWarpStillHolds(float gx, float gy, bool overlayPixel, float surroundShare) {
    return overlayPixel || gx * gx + gy * gy < kPushWarpStillField * kPushWarpStillField || surroundShare >= kPushWarpStillSurround;
}
inline bool pushWarpOverlayCore(float delta, float gx, float gy, float unexplained, float scale = 1.0f) {
    return delta <= kPushWarpOverlayStill * scale && gx * gx + gy * gy >= kPushWarpOverlayMinMove * kPushWarpOverlayMinMove &&
           unexplained > kPushWarpOverlayContrast;
}
// A mask given from outside (W x H bytes, pushWarpOverlayMask's bits) in place of PushWarp's own: the ideal mask of a
// synthetic scene, or later a history-based mask (HUD Guard) combined with this one. Null: PushWarp's own mask.
struct PushWarpOverlay {
    const std::uint8_t* given = nullptr;
};
struct PushWarpResult {
    std::vector<std::uint8_t> overlayMask;   // with overlay rules: the mask used (bits 1 overlay, 2 sources)
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

// Review 3 SHOULD 5: a vector longer than kPushWarpLongShare of the field's width (480 px at 4K, the old cut-off) and than
// kPushWarpLongMinWork work px is trusted only when at least three quarters of its neighbour cells (6 of 8 inside the
// field) carry a trusted vector within kPushWarpLongTolerance flow px of it: the block SAD alone accepts a long wrong
// vector on repeating content (a tear of up to 960 px).
constexpr float kPushWarpLongShare = 0.125f;
constexpr float kPushWarpLongMinWork = 64.0f;
constexpr float kPushWarpLongTolerance = 2.0f;
inline bool pushWarpLongCorroborated(const PushWarpField& f, std::uint32_t cx, std::uint32_t cy, float gx, float gy, float minConfidence) {
    int agree = 0, around = 0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            if (i == 0 && j == 0) continue;
            const int x = int(cx) + i, y = int(cy) + j;
            if (x < 0 || y < 0 || x >= int(f.width) || y >= int(f.height)) continue;
            ++around;
            float vx = 0, vy = 0;
            if (!pushWarpVectorAt(f, x, y, minConfidence, vx, vy)) continue;
            const float dx = vx - gx, dy = vy - gy;
            agree += dx * dx + dy * dy <= kPushWarpLongTolerance * kPushWarpLongTolerance ? 1 : 0;
        }
    return agree * 4 >= around * 3;
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
    if (!(conf >= p.minConfidence) || !(length <= p.maxMotionShare * float(field.width)) ||
        (length > kPushWarpLongShare * float(field.width) && length * float(b.width) / float(field.width) > kPushWarpLongMinWork &&
         !pushWarpLongCorroborated(field, cx, cy, c.gx, c.gy, p.minConfidence))) { c.gx = c.gy = 0; return c; }
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

// The filled field at work pixel (x, y): g (work px, B -> A) and the visibility, bilinear between flow-cell centres (R3's).
inline void pushWarpFieldAt(const std::vector<PushWarpLevelCell>& field, std::uint32_t gw, std::uint32_t gh, std::uint32_t W, std::uint32_t H,
                            std::uint32_t x, std::uint32_t y, float& gx, float& gy, float& vis) {
    const float sx = float(gw) / float(W), sy = float(gh) / float(H);
    const float scaleX = float(W) / float(gw), scaleY = float(H) / float(gh);
    const float qx = std::max((float(x) + 0.5f) * sx - 0.5f, 0.0f), qy = std::max((float(y) + 0.5f) * sy - 0.5f, 0.0f);
    const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
    const std::uint32_t x0 = std::min(std::uint32_t(fx), gw - 1), y0 = std::min(std::uint32_t(fy), gh - 1);
    const std::uint32_t x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
    const auto& f00 = field[y0 * gw + x0];
    const auto& f10 = field[y0 * gw + x1];
    const auto& f01 = field[y1 * gw + x0];
    const auto& f11 = field[y1 * gw + x1];
    const auto bl = [&](float PushWarpLevelCell::*m) {
        const float top = f00.*m + (f10.*m - f00.*m) * tx, bottom = f01.*m + (f11.*m - f01.*m) * tx;
        return top + (bottom - top) * ty;
    };
    gx = bl(&PushWarpLevelCell::ex) * scaleX; gy = bl(&PushWarpLevelCell::ey) * scaleY;
    vis = bl(&PushWarpLevelCell::visibility);
}
// The pair's field at work pixel (x, y) from its cells (W3: the B -> A vector on B's grid, flow px; zero where the flow
// was not trusted), bilinear between flow-cell centres and scaled to work px. Independent of t: one mask per pair.
inline void pushWarpCellsAt(const std::vector<PushWarpCell>& cells, std::uint32_t gw, std::uint32_t gh, std::uint32_t W, std::uint32_t H,
                            std::uint32_t x, std::uint32_t y, float& gx, float& gy) {
    const float sx = float(gw) / float(W), sy = float(gh) / float(H);
    const float qx = std::max((float(x) + 0.5f) * sx - 0.5f, 0.0f), qy = std::max((float(y) + 0.5f) * sy - 0.5f, 0.0f);
    const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
    const std::uint32_t x0 = std::min(std::uint32_t(fx), gw - 1), y0 = std::min(std::uint32_t(fy), gh - 1);
    const std::uint32_t x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
    const auto& c00 = cells[y0 * gw + x0];
    const auto& c10 = cells[y0 * gw + x1];
    const auto& c01 = cells[y1 * gw + x0];
    const auto& c11 = cells[y1 * gw + x1];
    const float topX = c00.gx + (c10.gx - c00.gx) * tx, bottomX = c01.gx + (c11.gx - c01.gx) * tx;
    const float topY = c00.gy + (c10.gy - c00.gy) * tx, bottomY = c01.gy + (c11.gy - c01.gy) * tx;
    gx = (topX + (bottomX - topX) * ty) * (float(W) / float(gw));
    gy = (topY + (bottomY - topY) * ty) * (float(H) / float(gh));
}
// The overlay's view of the pair's motion per flow cell: the trust-weighted mean of the trusted cell vectors within
// kPushWarpOverlaySpread cells (an overlay pulls its own cells' trust down: their neighbours say how the picture moves
// there). Independent of t.
constexpr int kPushWarpOverlaySpread = 2;
// Whether a pair cell is trusted and moves at least kPushWarpOverlayMinMove work px (moveFlow2: that length squared in flow px).
inline bool pushWarpOverlayMovingCell(const PushWarpCell& c, float moveFlow2) {
    return c.trust >= 0.5f && c.gx * c.gx + c.gy * c.gy >= moveFlow2;
}
// The share of the cells within kPushWarpOverlayReach that move (the "surround" gate).
inline float pushWarpOverlayMovingShare(const std::vector<PushWarpCell>& cells, std::uint32_t gw, std::uint32_t gh, int x, int y, float moveFlow2) {
    float moving = 0, all = 0;
    for (int j = -kPushWarpOverlayReach; j <= kPushWarpOverlayReach; ++j)
        for (int i = -kPushWarpOverlayReach; i <= kPushWarpOverlayReach; ++i) {
            const int xx = x + i, yy = y + j;
            if (xx < 0 || yy < 0 || xx >= int(gw) || yy >= int(gh)) continue;
            all += 1.0f;
            moving += pushWarpOverlayMovingCell(cells[std::size_t(yy) * gw + std::size_t(xx)], moveFlow2) ? 1.0f : 0.0f;
        }
    return all > 0 ? moving / all : 0.0f;
}
// Whether the moving trusted cells within kPushWarpOverlaySpread agree (RMS distance to their mean at most
// kPushWarpOverlayAgree of its length, and at least 0.75 flow px): a field that jumps between vectors there (a repeating
// texture the flow aliased, parallax, a crossing) gives no reliable "the picture moves, this pixel does not".
constexpr float kPushWarpOverlayAgree = 0.25f;
inline bool pushWarpOverlayAgrees(const std::vector<PushWarpCell>& cells, std::uint32_t gw, std::uint32_t gh, int x, int y, float moveFlow2,
                                  float& meanX, float& meanY) {
    float n = 0, mx = 0, my = 0, qx = 0, qy = 0;
    meanX = meanY = 0;
    for (int j = -kPushWarpOverlaySpread; j <= kPushWarpOverlaySpread; ++j)
        for (int i = -kPushWarpOverlaySpread; i <= kPushWarpOverlaySpread; ++i) {
            const int xx = x + i, yy = y + j;
            if (xx < 0 || yy < 0 || xx >= int(gw) || yy >= int(gh)) continue;
            const auto& c = cells[std::size_t(yy) * gw + std::size_t(xx)];
            if (!pushWarpOverlayMovingCell(c, moveFlow2)) continue;
            n += 1.0f; mx += c.gx; my += c.gy; qx += c.gx * c.gx; qy += c.gy * c.gy;
        }
    if (n < 1.0f) return false;
    mx /= n; my /= n;
    meanX = mx; meanY = my;
    const float spread2 = std::max(0.0f, qx / n - mx * mx) + std::max(0.0f, qy / n - my * my);
    const float limit = std::max(0.75f, kPushWarpOverlayAgree * std::sqrt(mx * mx + my * my));
    return spread2 <= limit * limit;
}
// Per flow cell (flow px): spread = the trust-weighted mean of every cell within kPushWarpOverlaySpread (where the pixel's
// content came from); moving = the mean of the moving trusted cells there (gx, gy) and the surround's moving share (trust;
// 0 where those cells disagree).
struct PushWarpOverlayCells { std::vector<PushWarpCell> spread, moving; std::vector<float> share; };   // share: the moving share, whether or not the cells agree
inline PushWarpOverlayCells pushWarpOverlayCells(const std::vector<PushWarpCell>& cells, std::uint32_t gw, std::uint32_t gh, std::uint32_t W) {
    const float moveFlow = kPushWarpOverlayMinMove * float(gw) / float(W), moveFlow2 = moveFlow * moveFlow;
    PushWarpOverlayCells result{std::vector<PushWarpCell>(cells.size()), std::vector<PushWarpCell>(cells.size()), std::vector<float>(cells.size())};
    auto& out = result.spread;
    for (int y = 0; y < int(gh); ++y)
        for (int x = 0; x < int(gw); ++x) {
            float sx = 0, sy = 0, sw = 0;
            for (int j = -kPushWarpOverlaySpread; j <= kPushWarpOverlaySpread; ++j)
                for (int i = -kPushWarpOverlaySpread; i <= kPushWarpOverlaySpread; ++i) {
                    const int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= int(gw) || yy >= int(gh)) continue;
                    const auto& c = cells[std::size_t(yy) * gw + std::size_t(xx)];
                    sx += c.gx * c.trust; sy += c.gy * c.trust; sw += c.trust;
                }
            auto& o = out[std::size_t(y) * gw + std::size_t(x)];
            if (sw > 1e-6f) { o.gx = sx / sw; o.gy = sy / sw; o.trust = 1.0f; }
            auto& m = result.moving[std::size_t(y) * gw + std::size_t(x)];
            const float share = pushWarpOverlayMovingShare(cells, gw, gh, x, y, moveFlow2);
            result.share[std::size_t(y) * gw + std::size_t(x)] = share;
            m.trust = pushWarpOverlayAgrees(cells, gw, gh, x, y, moveFlow2, m.gx, m.gy) ? share : 0.0f;
        }
    return result;
}
// The overlay mask (bits as described above) from A, B and the pair's cells (shaders/PushWarpOverlay.hlsl, 6 passes).
inline std::vector<std::uint8_t> pushWarpOverlayMask(const PushWarpFrame& a, const PushWarpFrame& b, bool tenBit, const std::vector<PushWarpCell>& pairCells,
                                                     std::uint32_t gw, std::uint32_t gh) {
    const std::uint32_t W = b.width, H = b.height;
    const auto overlayCells = pushWarpOverlayCells(pairCells, gw, gh, W);
    const auto& cells = overlayCells.spread;
    const float scale = pushWarpOverlayScale(tenBit), moveFlow = kPushWarpOverlayMinMove * float(gw) / float(W);
    // the gates of a core candidate (see "Three gates" above)
    const auto gated = [&](std::uint32_t x, std::uint32_t y, const PushWarpColor& ca, const PushWarpColor& cb) {
        float structure = 0;
        const int offsets[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
        float structureB = 0;
        for (const auto& o : offsets) {
            structure = std::max(structure, pushWarpMaxDelta(ca, pushWarpUnpack(a.at(int(x) + o[0], int(y) + o[1]), tenBit)));
            structureB = std::max(structureB, pushWarpMaxDelta(cb, pushWarpUnpack(b.at(int(x) + o[0], int(y) + o[1]), tenBit)));
        }
        if (std::min(structure, structureB) < kPushWarpOverlayStructure * scale) return false;
        const std::uint32_t cx = std::min(gw - 1, std::uint32_t(float(x) * float(gw) / float(W))), cy = std::min(gh - 1, std::uint32_t(float(y) * float(gh) / float(H)));
        const auto& moving = overlayCells.moving[std::size_t(cy) * gw + cx];
        if (moving.trust < kPushWarpOverlayShare) return false;
        const auto explains = [&](float vx, float vy) {
            const float back = pushWarpMaxDelta(cb, pushWarpBilinear(a, tenBit, float(x) + vx, float(y) + vy));
            const float fwd = pushWarpMaxDelta(ca, pushWarpBilinear(b, tenBit, float(x) - vx, float(y) - vy));
            return std::min(back, fwd) <= kPushWarpOverlayContrast;
        };
        if (explains(moving.gx * float(W) / float(gw), moving.gy * float(H) / float(gh))) return false;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const int xx = int(cx) + i, yy = int(cy) + j;
                if (xx < 0 || yy < 0 || xx >= int(gw) || yy >= int(gh)) continue;
                const auto& c = pairCells[std::size_t(yy) * gw + std::size_t(xx)];
                if (!pushWarpOverlayMovingCell(c, moveFlow * moveFlow)) continue;
                if (explains(c.gx * float(W) / float(gw), c.gy * float(H) / float(gh))) return false;
            }
        return true;
    };
    std::vector<std::uint8_t> core(std::size_t(W) * H), mask(core.size()), unexplained(core.size()), extend(core.size());
    std::vector<float> delta(core.size());
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y) * W + x;
            const auto ca = pushWarpUnpack(a.at(int(x), int(y)), tenBit), cb = pushWarpUnpack(b.at(int(x), int(y)), tenBit);
            delta[i] = pushWarpMaxDelta(ca, cb);
            float gx = 0, gy = 0;
            pushWarpCellsAt(cells, gw, gh, W, H, x, y, gx, gy);
            // unexplained both ways: B(x) is not A where it came from, and A(x) is not B where it went (a background pixel
            // next to the overlay fails only one of them: the overlay is in A at x + g, not in B at x - g)
            const float back = pushWarpMaxDelta(cb, pushWarpBilinear(a, tenBit, float(x) + gx, float(y) + gy));
            const float fwd = pushWarpMaxDelta(ca, pushWarpBilinear(b, tenBit, float(x) - gx, float(y) - gy));
            const float u = std::min(back, fwd);
            unexplained[i] = u > kPushWarpOverlayContrast ? 1 : 0;
            core[i] = pushWarpOverlayCore(delta[i], gx, gy, u, scale) && gated(x, y, ca, cb) ? 1 : 0;
            // an overlay segment that lies along the motion and is longer than it is "explained" by the motion (A(x + g)
            // is the segment too): still where the field moves, it may extend a core run (below)
            extend[i] = delta[i] <= kPushWarpOverlayStill * scale && gx * gx + gy * gy >= kPushWarpOverlayMinMove * kPushWarpOverlayMinMove ? 1 : 0;
        }
    const auto coreNear = [&](int x, int y, int r) {
        for (int j = -r; j <= r; ++j)
            for (int i = -r; i <= r; ++i) {
                const int xx = x + i, yy = y + j;
                if (xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H) && core[std::size_t(yy) * W + std::size_t(xx)]) return true;
            }
        return false;
    };
    // a still pixel joined to a core pixel by an unbroken horizontal or vertical run of extend pixels (at most
    // kPushWarpOverlayRun long) belongs to the same overlay; two rounds (a segment along the motion that meets the core
    // only through a crossing, as a crosshair's arm through its centre)
    std::vector<std::uint8_t> seed = core;
    const auto joined = [&](int x, int y) {
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs)
            for (int k = 1; k <= kPushWarpOverlayRun; ++k) {
                const int xx = x + k * d[0], yy = y + k * d[1];
                if (xx < 0 || yy < 0 || xx >= int(W) || yy >= int(H)) break;
                const std::size_t j = std::size_t(yy) * W + std::size_t(xx);
                if (seed[j]) return true;
                if (!extend[j]) break;
            }
        return false;
    };
    for (int round = 0; round < 2; ++round) {
        std::vector<std::uint8_t> next = seed;
        for (int y = 0; y < int(H); ++y)
            for (int x = 0; x < int(W); ++x) {
                const std::size_t i = std::size_t(y) * W + std::size_t(x);
                if (!next[i] && extend[i] && joined(x, y)) next[i] = 1;
            }
        seed.swap(next);
    }
    for (int y = 0; y < int(H); ++y)
        for (int x = 0; x < int(W); ++x) {
            const std::size_t i = std::size_t(y) * W + std::size_t(x);
            if (seed[i] || (delta[i] <= kPushWarpOverlayRim * scale && unexplained[i] && coreNear(x, y, 1))) mask[i] = 1;
        }
    for (int y = 0; y < int(H); ++y)
        for (int x = 0; x < int(W); ++x) {
            bool beside = false;
            for (int j = -1; j <= 1 && !beside; ++j)
                for (int i = -1; i <= 1 && !beside; ++i) {
                    const int xx = x + i, yy = y + j;
                    beside = xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H) && (mask[std::size_t(yy) * W + std::size_t(xx)] & 1) != 0;
                }
            if (beside) mask[std::size_t(y) * W + std::size_t(x)] |= 2;
        }
    return mask;
}

inline PushWarpResult pushWarpReference(const PushWarpFrame& a, const PushWarpFrame& b, bool tenBit, const PushWarpField& field,
                                        const PushWarpParams& p = {}, const PushWarpField* previous = nullptr,
                                        const PushWarpOverlay* overlay = nullptr) {
    const unsigned rules = p.t <= 1.0f ? p.overlayRules : 0u;
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
    const bool overlayOn = rules != 0;
    if (overlayOn) r.overlayMask = overlay && overlay->given ? std::vector<std::uint8_t>(overlay->given, overlay->given + std::size_t(W) * H)
                                                             : pushWarpOverlayMask(a, b, tenBit, r.cells, gw, gh);
    // the surround's moving share at the pixel's flow cell (pushWarpStillHolds)
    const auto overlayCells = overlayOn ? pushWarpOverlayCells(r.cells, gw, gh, W) : PushWarpOverlayCells{};
    const auto surroundAt = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t cx = std::min(gw - 1, std::uint32_t(float(x) * float(gw) / float(W))), cy = std::min(gh - 1, std::uint32_t(float(y) * float(gh) / float(H)));
        return overlayCells.share[std::size_t(cy) * gw + cx];
    };
    const auto maskAt = [&](int x, int y, std::uint8_t bit) {
        const int cx = std::clamp(x, 0, int(W) - 1), cy = std::clamp(y, 0, int(H) - 1);
        return (r.overlayMask[std::size_t(cy) * W + std::size_t(cx)] & bit) != 0;
    };
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint32_t aw = a.at(int(x), int(y)), bw = b.at(int(x), int(y));
            const auto ca = pushWarpUnpack(aw, tenBit), cb = pushWarpUnpack(bw, tenBit);
            float gx = 0, gy = 0, vis = 0;
            pushWarpFieldAt(r.field, gw, gh, W, H, x, y, gx, gy, vis);
            // A still pixel is A exactly, except (with the overlay rules) where the field moves and the mask does not call it
            // an overlay: a still background that a fast thin object crossed between A and B (pushWarpStillHolds).
            const bool still = pushWarpMaxDelta(ca, cb) <= p.stillDelta * pushWarpOverlayScale(tenBit);   // HDR10: in PQ codes (review 3 SHOULD 7)
            if (still && (!overlayOn || pushWarpStillHolds(gx, gy, maskAt(int(x), int(y), 1), surroundAt(x, y)))) { r.words[std::size_t(y) * W + x] = predicting ? bw : aw; ++r.counters.still; continue; }
            if (overlayOn && (rules & 1u) && maskAt(int(x), int(y), 1)) {   // static override
                const PushWarpColor o = predicting ? cb : PushWarpColor{ca.r + p.t * (cb.r - ca.r), ca.g + p.t * (cb.g - ca.g), ca.b + p.t * (cb.b - ca.b), ca.a + p.t * (cb.a - ca.a)};
                r.words[std::size_t(y) * W + x] = pushWarpPack(o, tenBit);
                ++r.counters.still;
                continue;
            }
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
            float w0 = (1.0f - p.t) * vis, w1 = p.t * vis;
            PushWarpColor out;
            bool conflict = pushWarpMaxDelta(s0, s1) > p.conflictDelta;
            // a still pixel under a moving field (pushWarpStillHolds): the moved samples are used only when they agree (the
            // object passes here at t); when they disagree the field's vector is not this pixel's (a block vector of a thin
            // object beside it): the still pixel stays A
            if (still && pushWarpMaxDelta(s0, s1) > kPushWarpStillAgree * pushWarpOverlayScale(tenBit)) { r.words[std::size_t(y) * W + x] = aw; ++r.counters.still; continue; }
            if (overlayOn && (rules & 2u)) {   // source exclusion: a tap on the overlay is not used
                const int x0 = pushWarpRound(float(x) + p.t * gx), y0 = pushWarpRound(float(y) + p.t * gy);
                const int x1 = pushWarpRound(float(x) - (1.0f - p.t) * gx), y1 = pushWarpRound(float(y) - (1.0f - p.t) * gy);
                bool m0 = maskAt(x0, y0, 2), m1 = maskAt(x1, y1, 2);
                if (m0 && m1) { m0 = maskAt(x0, y0, 1); m1 = maskAt(x1, y1, 1); }   // both near it: the one ON the overlay loses
                if (m0 != m1) { if (m0) w0 = 0.0f; else w1 = 0.0f; conflict = false; }
            }
            if (conflict) out = w0 >= w1 ? s0 : s1;
            else {
                const float d = w0 + w1;
                out = {(s0.r * w0 + s1.r * w1) / d, (s0.g * w0 + s1.g * w1) / d, (s0.b * w0 + s1.b * w1) / d, (s0.a * w0 + s1.a * w1) / d};
            }
            r.words[std::size_t(y) * W + x] = pushWarpPack(out, tenBit);
            // a still pixel moved by pushWarpStillHolds stays "still" in the counters, so the whole-frame verdict (more holes
            // than moving pixels) is decided by the moving pixels alone, as without the rules
            if (still) ++r.counters.still; else if (hole) ++r.counters.hole; else if (conflict) ++r.counters.conflict; else ++r.counters.blend;
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

// Stage GPU times of one I (PushWarpPass timestamps): trust, splat, fill, overlay (the static-overlay mask: 6 passes on
// the pair's first frame, 0 on the frames that reuse it or with the rules off), compose (with its verdict), total. -1 unknown.
enum class PushWarpStage : std::uint8_t { Trust = 0, Splat, Fill, Overlay, Compose, Total, Count };
inline const char* pushWarpStageName(PushWarpStage s) {
    switch (s) {
    case PushWarpStage::Trust: return "trust";
    case PushWarpStage::Splat: return "splat";
    case PushWarpStage::Fill: return "fill";
    case PushWarpStage::Overlay: return "overlay";
    case PushWarpStage::Compose: return "compose";
    case PushWarpStage::Total: return "total";
    default: return "?";
    }
}
constexpr unsigned kPushWarpStamps = 6;
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
// The static-overlay mask's buffers (PushWarpOverlay.hlsl): three masks of one byte per pixel in rows of ceil(W / 4)
// words, and the overlay cells (24 bytes per flow cell: spread vector, moving mean, share); sized for the play size and the self-check's.
inline std::uint64_t pushWarpOverlayMaskBytes(std::uint32_t w, std::uint32_t h) {
    const auto bytes = [](std::uint32_t x, std::uint32_t y) { return std::uint64_t((x + 3u) / 4u) * 4u * y; };
    return std::max(bytes(w, h), bytes(kPushWarpCheckWidth, kPushWarpCheckHeight));
}
inline std::uint64_t pushWarpOverlayCellBytes(std::uint32_t flowW, std::uint32_t flowH, std::uint32_t w, std::uint32_t h) {
    // the cells (24 bytes each), then one word per 16 x 16 tile (whether it holds a core pixel: the run passes' gate)
    const auto tiles = [](std::uint32_t x, std::uint32_t y) { return std::uint64_t((x + 15u) / 16u) * ((y + 15u) / 16u); };
    return std::max(std::uint64_t(flowW) * flowH, std::uint64_t(kPushWarpCheckFlowWidth) * kPushWarpCheckFlowHeight) * 24u +
           std::max(tiles(w, h), tiles(kPushWarpCheckWidth, kPushWarpCheckHeight)) * 4u;
}
// Whether pixel (x, y) is in the middle eighth of the width and height (the crosshair's place; PushWarpOverlay.hlsl).
inline bool pushWarpOverlayCentre(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const int dx = int(x) - int(w / 2u), dy = int(y) - int(h / 2u);
    return std::abs(dx) * 16 < int(w) && std::abs(dy) * 16 < int(h);
}
inline bool pushWarpCheckPass(std::uint64_t differing, std::uint64_t pixels, const PushWarpCounters& gpu, const PushWarpCounters& cpu) {
    if (pixels == 0) return false;
    const auto close = [&](std::uint32_t x, std::uint32_t y) { return double(x > y ? x - y : y - x) <= kPushWarpCheckCounterSlack * double(pixels) + 1.0; };
    return double(differing) <= kPushWarpCheckMaxDiffering * double(pixels) && close(gpu.still, cpu.still) && close(gpu.blend, cpu.blend) &&
           close(gpu.conflict, cpu.conflict) && close(gpu.hole, cpu.hole);
}

}  // namespace veyra::pipeline
