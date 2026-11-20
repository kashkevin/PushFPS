// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace veyra::engine {
// Restoration ("clean-up") of a compressed console stream before any upscaler or neural layer
// (sprint 5, stream "restore"; docs/RESTORATION.md). Portable and header-only: this file is the single
// source of truth for the maths. The GPU stage (shaders/PushClean.hlsl, pipeline/PushCleanPass.*) mirrors
// pushCleanPixel line by line; tests/remoteplay/CleanTests.inc and tests/integration/PushGpuTests.cpp
// (testClean*) hold the two together. Nothing here has run on a GPU or on a PS5 stream.
//
// What it does, per pixel, on display-referred (perceptually uniform) code values in [0, 1]: sRGB for SDR,
// BT.2020 PQ for HDR. One pass, a fixed neighbourhood (5x5, 9 taps along the row and the column, 4 debanding
// taps at up to 16 pixels), no history, no motion:
//   protection  a pixel on a strong edge, a thin line or a text stroke (centre brighter or darker than both
//               sides), or inside a texture (busy small differences) is returned EXACTLY as it came in.
//               PushFPS's own logic: libplacebo has no equivalent (its debanding relies on its threshold).
//   deblock     weak steps along a row or a column with flat pixels on both sides (an encoder's transform
//               block edge, at ANY position: no 8-pixel grid is assumed) become a 4-pixel ramp. Own logic.
//   smooth      otherwise a range-limited mean of the 5x5 (pixels within a small luma distance of the centre):
//               a wider distance near a strong edge (mosquito noise / ringing), a narrow one elsewhere
//               (grain). Own logic (libplacebo's anti-ringing belongs to its scalers, not to a clean-up).
//   deband      libplacebo's debanding (pl_shader_deband, PORTED below): the mean of four samples a random
//               distance (up to 16 pixels) and angle away replaces each colour component that is within a
//               threshold of it, then libplacebo's "grain" (a random value of one code value of the output
//               bit depth, fading to nothing at black) covers what is left. Only for a pixel the debanding
//               changed: flat colour stays bit-exact.
// The result never leaves the range of the samples the pixel read (no overshoot, no new extremes).
// Strength 0 is off: every pixel is returned bit-exact. It cannot restore detail the encoder discarded.

inline constexpr int kPushCleanMaxStrength = 100;
// The parts of the stage, each on its own switch (preference pushCleanParts, a sum of these; default: all
// four) so that a hardware A/B can test the proven part alone (1 = libplacebo's debanding) against the
// stage's own logic. kPushCleanStaticNoise is not a filter part: it keeps the debanding noise the same on
// every frame instead of new on each (libplacebo's debanding draws new noise per frame; that is the default).
inline constexpr unsigned kPushCleanDeband = 1, kPushCleanDeblock = 2, kPushCleanDering = 4, kPushCleanGrain = 8;
inline constexpr unsigned kPushCleanAllParts = 15, kPushCleanStaticNoise = 16, kPushCleanPartsMask = 31;

struct PushCleanSettings {
    int strength = 0;          // 0..100, 0 = off (bit-exact pass-through)
    bool hudProtect = true;    // stricter thin-line / text protection (preference pushCleanHud)
    bool pq = false;           // values are BT.2020 PQ (HDR); else sRGB (SDR)
    int outputBits = 8;        // bit depth the picture is finally quantised to: grain = one code value of it
    std::uint32_t seed = 0;    // third word of the noise generator's state: the frame number (pushCleanFrameSeed)
    unsigned parts = kPushCleanAllParts;
};
// libplacebo seeds its debanding noise with the frame index (sh_prng(sh, true): state = x, y, frame).
constexpr std::uint32_t pushCleanFrameSeed(unsigned parts, std::uint64_t frame) {
    return (parts & kPushCleanStaticNoise) ? 0u : static_cast<std::uint32_t>(frame & 0xFFFFFFFFull);
}

// libplacebo's defaults (PL_DEBAND_DEFAULTS, src/include/libplacebo/shaders/sampling.h): iterations 1,
// threshold 3.0 (thousandths of full scale), radius 16.0 pixels, grain 4.0 (thousandths: one 8-bit code
// value is 3.92). Strength 50 gives exactly that threshold; the radius is the same at every strength; the
// grain is one code value of the output depth (8 bit: 3.92; 10 bit: 0.98, close to what libplacebo's
// renderer does for HDR, which divides the grain by peak / 203 cd/m2).
inline constexpr float kPushCleanDebandThreshold = 3.0f / 1000.0f;
inline constexpr float kPushCleanDebandRadius = 16.0f;
inline constexpr int kPushCleanDebandReferenceStrength = 50;

// Thresholds in code values of [0, 1]. One 8-bit code value = 1/255 on both SDR (sRGB) and HDR (PQ)
// signals: both encodings are close to perceptually uniform, which is why the filter works on them.
struct PushCleanParams {
    bool active = false;
    float flat = 0, minStep = 0, maxStep = 0;   // deblock: side flatness, smallest / largest block step
    float grainTau = 0, ringTau = 0;            // smooth: luma distance admitted, away from / near an edge
    float edge = 0;                             // a neighbour difference this large is a real edge
    float texture = 0, ringTexture = 0;         // mean small-difference activity that marks a texture
    float line = 0;                             // a centre this far above/below both sides is a line
    float deband = 0;                           // deband: a component this close to the tap mean takes it
    float grain = 0;                            // deband: noise amplitude = one code value of the output depth
    std::uint32_t radius16 = 0;                 // deband: largest tap distance, in 1/16 pixel
    std::uint32_t seed = 0;                     // deband: third word of the generator's state
    std::uint32_t parts = 0;                    // kPushCleanDeband | kPushCleanDeblock | ...
    float wr = 0, wg = 0, wb = 0;               // luma weights of the encoded signal
};

constexpr float kPushCleanCode = 1.0f / 255.0f;

inline PushCleanParams pushCleanParams(const PushCleanSettings& s) {
    PushCleanParams p;
    const int strength = std::clamp(s.strength, 0, kPushCleanMaxStrength);
    p.parts = s.parts & kPushCleanAllParts;
    p.active = strength > 0 && p.parts != 0;
    const float k = float(strength) / 100.0f, c = kPushCleanCode;
    p.flat = (0.75f + 1.25f * k) * c;
    p.minStep = 0.75f * c;
    p.maxStep = (2.0f + 14.0f * k) * c;
    p.grainTau = (1.0f + 4.0f * k) * c;
    p.ringTau = (2.0f + 8.0f * k) * c;
    p.edge = (14.0f + 14.0f * k) * c;
    p.texture = (1.0f + 3.0f * k) * c;
    p.ringTexture = (2.0f + 6.0f * k) * c;
    p.line = (s.hudProtect ? 4.0f : 12.0f) * c;
    p.deband = kPushCleanDebandThreshold * float(strength) / float(kPushCleanDebandReferenceStrength);
    const int bits = std::clamp(s.outputBits, 6, 16);
    p.grain = 1.0f / float((1 << bits) - 1);
    p.radius16 = static_cast<std::uint32_t>(kPushCleanDebandRadius * 16.0f);
    p.seed = s.seed;
    if (s.pq) { p.wr = 0.2627f; p.wg = 0.6780f; p.wb = 0.0593f; }   // BT.2020
    else { p.wr = 0.2126f; p.wg = 0.7152f; p.wb = 0.0722f; }        // BT.709
    return p;
}

// The GPU constant block (shaders/PushClean.hlsl, cbuffer CleanConstants): 20 words, same order. The pass
// fills width, height and stage.
struct PushCleanConstants {
    float flat, minStep, maxStep, grainTau;
    float ringTau, edge, texture, ringTexture;
    float line, deband, grain, wr;
    float wg, wb;
    std::uint32_t radius16, seed;
    std::uint32_t parts, width, height, stage;
};
static_assert(sizeof(PushCleanConstants) == 80);
inline PushCleanConstants pushCleanConstants(const PushCleanParams& p) {
    return {p.flat, p.minStep, p.maxStep, p.grainTau, p.ringTau, p.edge, p.texture, p.ringTexture,
            p.line, p.deband, p.grain, p.wr, p.wg, p.wb, p.radius16, p.seed, p.active ? p.parts : 0u, 0u, 0u, 0u};
}

// What the interface asked for, read by the graph on every frame (as NIS sharpness: no graph rebuild for
// a strength above 0; EnhancementSettings::clean decides whether the stage exists).
struct PushCleanLive { int strength = 0; bool hudProtect = true; unsigned parts = kPushCleanAllParts; };
inline std::atomic<std::uint32_t>& pushCleanLiveStore() {
    static std::atomic<std::uint32_t> value{(1u << 8) | (kPushCleanAllParts << 16)};
    return value;
}
inline void setPushCleanLive(int strength, bool hudProtect, unsigned parts) {
    pushCleanLiveStore().store(static_cast<std::uint32_t>(std::clamp(strength, 0, kPushCleanMaxStrength)) |
                               (hudProtect ? 1u << 8 : 0u) | ((parts & kPushCleanPartsMask) << 16));
}
inline PushCleanLive pushCleanLive() {
    const std::uint32_t v = pushCleanLiveStore().load();
    return {static_cast<int>(v & 0xFFu), (v & (1u << 8)) != 0, (v >> 16) & kPushCleanPartsMask};
}

// ---------------------------------------------------------------------------------------------------------
// PORTED from libplacebo (https://code.videolan.org/videolan/libplacebo, commit 89b04663135b, LGPL-2.1 or
// later): the generator of src/shaders.c `sh_prng` (pcg3d, http://jcgt.org/published/0009/03/02/) and the
// debanding of src/shaders/sampling.c `pl_shader_deband`. The original notice of those files:
/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */
// Changed for PushFPS (2026-11-20): GLSL to C++ (and HLSL in shaders/PushClean.hlsl); one iteration only;
// the tap distance and angle come from the generator's integer state through a 16-step quarter-circle table
// (the four taps are mirror images, so a quarter turn covers every angle) so that the CPU reference and
// every GPU pick the same texels, where libplacebo multiplies by cos / sin; the state's third word is a
// seed (the frame number, as in libplacebo, or 0 for a pattern that never changes); the grain is added only
// to a pixel the debanding changed and the result is clamped to the samples read; black is the grain's
// neutral.
struct PushCleanRandom { std::uint32_t x = 0, y = 0, z = 0; };
inline void pushCleanRandomStep(PushCleanRandom& s) {
    s.x = 1664525u * s.x + 1013904223u; s.y = 1664525u * s.y + 1013904223u; s.z = 1664525u * s.z + 1013904223u;
    s.x += s.y * s.z; s.y += s.z * s.x; s.z += s.x * s.y;
    s.x ^= s.x >> 16; s.y ^= s.y >> 16; s.z ^= s.z >> 16;
    s.x += s.y * s.z; s.y += s.z * s.x; s.z += s.x * s.y;
}
// The 24 high bits as a float in [0, 1): exact in a float, on a CPU and on a GPU.
inline float pushCleanRandomFloat(std::uint32_t word) { return float(word >> 8) * (1.0f / 16777216.0f); }
// 256 * cos((k + 0.5) * pi / 32), k = 0..15; the sine is the same table read backwards.
inline constexpr std::array<int, 16> kPushCleanQuarterCos = {256, 253, 248, 241, 231, 220, 206, 190,
                                                             172, 152, 132, 109, 86, 62, 38, 13};
// The debanding tap offset (dx, dy >= 0) of one pixel; the taps are (+-dx, +-dy).
inline void pushCleanDebandOffset(const PushCleanRandom& s, std::uint32_t radius16, int& dx, int& dy) {
    const int distance = static_cast<int>(((s.x >> 16) * radius16) >> 16);   // 1/16 pixel, [0, radius)
    const std::size_t k = static_cast<std::size_t>(s.y >> 28);
    dx = (distance * kPushCleanQuarterCos[k] + 2048) >> 12;
    dy = (distance * kPushCleanQuarterCos[15 - k] + 2048) >> 12;
}
// One colour component: `value` (the pixel so far) and the four taps.
// Returns the debanded value; `changed` is set when the tap mean replaced a different value.
inline float pushCleanDebandComponent(float value, float t0, float t1, float t2, float t3, float threshold, bool& changed) {
    const float avg = (t0 + t1 + t2 + t3) * 0.25f;
    if (std::fabs(value - avg) > threshold) return value;
    if (avg != value) changed = true;
    return avg;
}
inline float pushCleanGrain(float value, float grain, float random) {
    const float strength = std::min(std::fabs(value), grain);   // no grain near true black
    return value + strength * (random - 0.5f);
}
// The final 8- or 10-bit write of a frame the stage changed (shaders/ScaleBlit.hlsl OrderedDitherRound):
// libplacebo's `sh_dither` (src/shaders/dithering.c, same commit and licence), method PL_DITHER_ORDERED_FIXED:
// a 16 x 16 ordered pattern made with bit operations, the same bias for R, G and B, not temporal, and true
// black is never lifted. Changed: GLSL to C++ / HLSL; the low-depth gamma branch (4 bits and fewer) left out.
inline float pushCleanOrderedBias(int x, int y) {
    std::uint32_t px = static_cast<std::uint32_t>(x) % 16u, py = static_cast<std::uint32_t>(y) % 16u;
    px ^= py;                                    // bitwise merge (morton number)
    px = (px | (px << 2)) & 0x33333333u; py = (py | (py << 2)) & 0x33333333u;
    px = (px | (px << 1)) & 0x55555555u; py = (py | (py << 1)) & 0x55555555u;
    std::uint32_t b = px + (py << 1);            // bitwise inversion
    b = ((b * 0x0802u) & 0x22110u) | ((b * 0x8020u) & 0x88440u);
    b = 0x10101u * b;
    b = (b >> 16) & 0xFFu;
    return static_cast<float>(b) * (1.0f / 256.0f);
}
// steps = 255 for an 8-bit target, 1023 for 10 bits.
inline float pushCleanDitherRound(float value, int x, int y, float steps) {
    if (std::fabs(value) < 1e-5f) value = 0.0f;  // don't let the dither pattern lift true black off zero
    return std::floor(steps * value + pushCleanOrderedBias(x, y)) * (1.0f / steps);
}
// ---- end of the part ported from libplacebo -------------------------------------------------------------

// A planar float picture (R, G, B) of encoded code values.
struct PushCleanImage {
    int width = 0, height = 0;
    std::array<std::vector<float>, 3> plane;
    PushCleanImage() = default;
    PushCleanImage(int w, int h) : width(w), height(h) {
        for (auto& p : plane) p.assign(std::size_t(w) * std::size_t(h), 0.0f);
    }
    std::size_t index(int x, int y) const {
        x = std::clamp(x, 0, width - 1); y = std::clamp(y, 0, height - 1);
        return std::size_t(y) * std::size_t(width) + std::size_t(x);
    }
    float at(int c, int x, int y) const { return plane[std::size_t(c)][index(x, y)]; }
    void set(int c, int x, int y, float v) { plane[std::size_t(c)][index(x, y)] = v; }
};

enum class PushCleanClass : std::uint8_t { Off, Edge, Line, Texture, Flat, Deband, Deblock, Smooth };
inline constexpr std::size_t kPushCleanClassCount = 8;
inline const char* pushCleanClassName(PushCleanClass c) {
    switch (c) {
    case PushCleanClass::Off: return "off";
    case PushCleanClass::Edge: return "edge";
    case PushCleanClass::Line: return "line";
    case PushCleanClass::Texture: return "texture";
    case PushCleanClass::Flat: return "flat";
    case PushCleanClass::Deband: return "deband";
    case PushCleanClass::Deblock: return "deblock";
    case PushCleanClass::Smooth: return "smooth";
    }
    return "?";
}
// True for the classes that leave the pixel exactly as it was.
constexpr bool pushCleanUntouched(PushCleanClass c) {
    return c == PushCleanClass::Off || c == PushCleanClass::Edge || c == PushCleanClass::Line ||
           c == PushCleanClass::Texture || c == PushCleanClass::Flat;
}

struct PushCleanStats {
    std::array<std::uint64_t, kPushCleanClassCount> count{};
    std::uint64_t changed = 0;
};

namespace clean_detail {
inline float luma(const PushCleanImage& img, const PushCleanParams& p, int x, int y) {
    return p.wr * img.at(0, x, y) + p.wg * img.at(1, x, y) + p.wb * img.at(2, x, y);
}
// Deblocking contribution of the edge between a = x + e and a + 1 along (dx, dy), for the pixel at x.
// e in {-2, -1, 0, 1}; the pixel is q1, q0, p0 or p1 of that edge. Returns the share (+-1/8, +-3/8) or 0.
inline float edgeShare(const PushCleanImage& img, const PushCleanParams& p, int x, int y, int dx, int dy, int e,
                       float& stepOut, int& channelBaseX, int& channelBaseY) {
    const int ax = x + e * dx, ay = y + e * dy;   // p0
    const float p2 = luma(img, p, ax - 2 * dx, ay - 2 * dy), p1 = luma(img, p, ax - dx, ay - dy);
    const float p0 = luma(img, p, ax, ay), q0 = luma(img, p, ax + dx, ay + dy);
    const float q1 = luma(img, p, ax + 2 * dx, ay + 2 * dy), q2 = luma(img, p, ax + 3 * dx, ay + 3 * dy);
    const float step = q0 - p0, mag = std::fabs(step);
    const float side = std::max(std::max(std::fabs(p0 - p1), std::fabs(p1 - p2)),
                                std::max(std::fabs(q1 - q0), std::fabs(q2 - q1)));
    if (mag < p.minStep || mag > p.maxStep || side > p.flat || mag < 2.0f * side) return 0.0f;
    stepOut = step; channelBaseX = ax; channelBaseY = ay;
    return e == 0 ? 0.375f : e == -1 ? -0.375f : e == 1 ? 0.125f : -0.125f;
}
}

// One output pixel. `out` gets R, G, B; the return value says what was done.
inline PushCleanClass pushCleanPixel(const PushCleanImage& img, const PushCleanParams& p, int x, int y,
                                     std::array<float, 3>& out) {
    using clean_detail::luma;
    for (int c = 0; c < 3; ++c) out[std::size_t(c)] = img.at(c, x, y);
    if (!p.active) return PushCleanClass::Off;
    // 5x5 luma window, row-major, centre at 12.
    std::array<float, 25> w{};
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) w[std::size_t((j + 2) * 5 + (i + 2))] = luma(img, p, x + i, y + j);
    const float c0 = w[12];
    // Protection 1: a strong edge through the pixel.
    const float gx = std::fabs(w[13] - w[11]), gy = std::fabs(w[17] - w[7]);
    if (std::max(gx, gy) > p.edge) return PushCleanClass::Edge;
    // Protection 2: a thin line, a text stroke or an isolated highlight (centre beyond both sides).
    const auto extreme = [&](float a, float b) { return (c0 - a) * (c0 - b) > 0.0f && std::min(std::fabs(c0 - a), std::fabs(c0 - b)) > p.line; };
    if (extreme(w[11], w[13]) || extreme(w[7], w[17]) || extreme(w[6], w[18]) || extreme(w[8], w[16]) ||
        extreme(w[11], w[14]) || extreme(w[10], w[13]) || extreme(w[7], w[22]) || extreme(w[2], w[17]))
        return PushCleanClass::Line;
    // Neighbourhood activity: adjacent differences inside the 5x5, the strong ones (edges) set apart.
    float activity = 0.0f;
    int weak = 0;
    bool nearEdge = false;
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i) {
            const float v = w[std::size_t(j * 5 + i)];
            for (int dir = 0; dir < 2; ++dir) {
                const int ni = i + (dir == 0 ? 1 : 0), nj = j + (dir == 1 ? 1 : 0);
                if (ni > 4 || nj > 4) continue;
                const float d = std::fabs(v - w[std::size_t(nj * 5 + ni)]);
                if (d > p.edge) nearEdge = true;
                else { activity += d; ++weak; }
            }
        }
    activity = weak > 0 ? activity / float(weak) : 0.0f;
    // Protection 3: texture (busy small differences); a wider allowance beside a strong edge (ringing).
    if (activity > (nearEdge ? p.ringTexture : p.texture)) return PushCleanClass::Texture;
    // 3x3 range of each channel: the clamp of the deblock and smooth results.
    std::array<float, 3> mn{}, mx{};
    for (int c = 0; c < 3; ++c) {
        mn[std::size_t(c)] = mx[std::size_t(c)] = img.at(c, x, y);
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const float v = img.at(c, x + i, y + j);
                mn[std::size_t(c)] = std::min(mn[std::size_t(c)], v); mx[std::size_t(c)] = std::max(mx[std::size_t(c)], v);
            }
    }
    // Deblock: the four edges along the row and the four along the column that this pixel is part of.
    std::array<float, 3> delta{};
    bool deblocked = false;
    for (int axis = 0; axis < 2 && (p.parts & kPushCleanDeblock) != 0; ++axis) {
        const int dx = axis == 0 ? 1 : 0, dy = axis == 0 ? 0 : 1;
        for (int e = -2; e <= 1; ++e) {
            float step = 0.0f; int ax = 0, ay = 0;
            const float share = clean_detail::edgeShare(img, p, x, y, dx, dy, e, step, ax, ay);
            if (share == 0.0f) continue;
            deblocked = true;
            for (int c = 0; c < 3; ++c)
                delta[std::size_t(c)] += share * (img.at(c, ax + dx, ay + dy) - img.at(c, ax, ay));
        }
    }
    bool local = false;   // the deblock or the smooth changed the pixel
    if (deblocked) {
        for (int c = 0; c < 3; ++c)
            out[std::size_t(c)] = std::clamp(img.at(c, x, y) + delta[std::size_t(c)], mn[std::size_t(c)], mx[std::size_t(c)]);
    } else if ((p.parts & (nearEdge ? kPushCleanDering : kPushCleanGrain)) != 0) {
        // Smooth: range-limited mean of the 5x5, guided by luma (dering beside a strong edge, grain elsewhere).
        const float tau = nearEdge ? p.ringTau : p.grainTau;
        std::array<float, 3> sum{};
        float n = 0.0f;
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                if (std::fabs(w[std::size_t((j + 2) * 5 + (i + 2))] - c0) > tau) continue;
                for (int c = 0; c < 3; ++c) sum[std::size_t(c)] += img.at(c, x + i, y + j);
                n += 1.0f;
            }
        for (int c = 0; c < 3; ++c)
            out[std::size_t(c)] = std::clamp(sum[std::size_t(c)] / n, mn[std::size_t(c)], mx[std::size_t(c)]);
    }
    for (int c = 0; c < 3; ++c) local = local || out[std::size_t(c)] != img.at(c, x, y);
    // Deband (libplacebo): four taps at one random distance and angle; then its grain.
    if ((p.parts & kPushCleanDeband) == 0) return deblocked ? PushCleanClass::Deblock : local ? PushCleanClass::Smooth : PushCleanClass::Flat;
    PushCleanRandom rng{static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), p.seed};
    pushCleanRandomStep(rng);
    int dx = 0, dy = 0;
    pushCleanDebandOffset(rng, p.radius16, dx, dy);
    bool debanded = false;
    std::array<float, 3> lo{}, hi{};
    for (int c = 0; c < 3; ++c) {
        const float t0 = img.at(c, x + dx, y + dy), t1 = img.at(c, x - dx, y + dy);
        const float t2 = img.at(c, x - dx, y - dy), t3 = img.at(c, x + dx, y - dy);
        const float before = out[std::size_t(c)];
        lo[std::size_t(c)] = std::min(std::min(before, t0), std::min(std::min(t1, t2), t3));
        hi[std::size_t(c)] = std::max(std::max(before, t0), std::max(std::max(t1, t2), t3));
        out[std::size_t(c)] = pushCleanDebandComponent(before, t0, t1, t2, t3, p.deband, debanded);
    }
    if (debanded) {
        pushCleanRandomStep(rng);
        const std::array<float, 3> noise{pushCleanRandomFloat(rng.x), pushCleanRandomFloat(rng.y),
                                         pushCleanRandomFloat(rng.z)};
        for (int c = 0; c < 3; ++c)
            out[std::size_t(c)] = std::clamp(pushCleanGrain(out[std::size_t(c)], p.grain, noise[std::size_t(c)]),
                                             lo[std::size_t(c)], hi[std::size_t(c)]);
    }
    if (deblocked) return PushCleanClass::Deblock;
    if (debanded) return PushCleanClass::Deband;
    return local ? PushCleanClass::Smooth : PushCleanClass::Flat;
}

// The whole picture. `out` may not alias `in`.
inline void pushCleanApply(const PushCleanImage& in, PushCleanImage& out, const PushCleanSettings& settings,
                           PushCleanStats* stats = nullptr) {
    out = PushCleanImage(in.width, in.height);
    const auto p = pushCleanParams(settings);
    std::array<float, 3> px{};
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            const auto cls = pushCleanPixel(in, p, x, y, px);
            bool changed = false;
            for (int c = 0; c < 3; ++c) {
                const std::size_t i = in.index(x, y);
                out.plane[std::size_t(c)][i] = px[std::size_t(c)];
                changed = changed || px[std::size_t(c)] != in.plane[std::size_t(c)][i];
            }
            if (stats) { ++stats->count[std::size_t(cls)]; if (changed) ++stats->changed; }
        }
}

// --- colour boundary (mirrors NisColor.hlsl / HdrColor.hlsli; the GPU stage encodes and decodes) ---------
inline float pushCleanSrgbEncode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
inline float pushCleanSrgbDecode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
// PQ of one BT.2020 component in scRGB units (1 = 80 cd/m2), as HdrEncodePq after its 709 -> 2020 matrix.
inline float pushCleanPqEncode(float scRgb2020) {
    const float y = std::clamp(scRgb2020 * 0.008f, 0.0f, 1.0f);
    const float p = std::pow(y, 2610.0f / 16384.0f);
    return std::pow((3424.0f / 4096.0f + (2413.0f / 128.0f) * p) / (1.0f + (2392.0f / 128.0f) * p), 2523.0f / 32.0f);
}
inline float pushCleanPqDecode(float pq) {
    const float p = std::pow(std::clamp(pq, 0.0f, 1.0f), 32.0f / 2523.0f);
    return 125.0f * std::pow(std::max(p - 3424.0f / 4096.0f, 0.0f) / std::max(2413.0f / 128.0f - (2392.0f / 128.0f) * p, 1e-6f),
                             16384.0f / 2610.0f);
}
// PQ code of HDR reference white (203 cd/m2).
inline float pushCleanPqReferenceWhite() { return pushCleanPqEncode(203.0f / 80.0f); }
// IEEE half precision, the format of the graph's FP16 textures (round to nearest even), for the GPU tests'
// reference: what a value becomes when the shader writes it and what a stored word reads back as.
inline std::uint16_t pushCleanToHalf(float value) {
    std::uint32_t x = 0;
    std::memcpy(&x, &value, sizeof x);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = static_cast<int>((x >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);   // too large (or not a number): infinity
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u), tie = 1u << (shift - 1u);
        if (rest > tie || (rest == tie && (half & 1u) != 0)) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) ++half;   // a carry into the exponent is right
    return static_cast<std::uint16_t>(sign | half);
}
inline float pushCleanFromHalf(std::uint16_t word) {
    const int exponent = (word >> 10) & 0x1F;
    const float mantissa = static_cast<float>(word & 0x3FFu);
    const float magnitude = exponent == 0 ? std::ldexp(mantissa, -24)
                          : exponent == 31 ? std::numeric_limits<float>::infinity()
                          : std::ldexp(mantissa + 1024.0f, exponent - 25);
    return (word & 0x8000u) != 0 ? -magnitude : magnitude;
}

// --- no-reference measures for tests and the offline bench (8-bit-equivalent luma) -------------------------
// Blockiness on an 8-pixel grid: mean |step| across grid lines over the mean |step| elsewhere, each plus a
// tenth of an 8-bit code so that a flat picture reads 1 (1 = no grid structure; above 1 = visible block
// edges). x264 codes 4x4/8x8 transforms inside 16x16 macroblocks, so an 8-grid sees its block edges; HEVC's
// variable transform sizes make this an underestimate there.
inline double pushCleanBlockiness(const std::vector<float>& y, int width, int height, int grid = 8) {
    double onGrid = 0, offGrid = 0; std::uint64_t nOn = 0, nOff = 0;
    for (int r = 0; r < height; ++r)
        for (int x = 1; x < width; ++x) {
            const double d = std::fabs(double(y[std::size_t(r) * std::size_t(width) + std::size_t(x)]) -
                                       double(y[std::size_t(r) * std::size_t(width) + std::size_t(x - 1)]));
            if (x % grid == 0) { onGrid += d; ++nOn; } else { offGrid += d; ++nOff; }
        }
    for (int r = 1; r < height; ++r)
        for (int x = 0; x < width; ++x) {
            const double d = std::fabs(double(y[std::size_t(r) * std::size_t(width) + std::size_t(x)]) -
                                       double(y[std::size_t(r - 1) * std::size_t(width) + std::size_t(x)]));
            if (r % grid == 0) { onGrid += d; ++nOn; } else { offGrid += d; ++nOff; }
        }
    if (!nOn || !nOff) return 1.0;
    const double floor = 0.1 / 255.0;
    return (onGrid / double(nOn) + floor) / (offGrid / double(nOff) + floor);
}
// Banding: mean length (pixels) of the runs of one 8-bit code along rows, counted only inside `mask`
// (non-zero = a smooth gradient). Long runs with a one-code step between them are visible bands; a
// dithered or finer gradient has short runs.
inline double pushCleanBandRun(const std::vector<float>& y, const std::vector<std::uint8_t>& mask, int width, int height) {
    std::uint64_t runs = 0, pixels = 0;
    for (int r = 0; r < height; ++r) {
        int run = 0, last = -1;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = std::size_t(r) * std::size_t(width) + std::size_t(x);
            if (!mask[i]) { if (run) { ++runs; pixels += std::uint64_t(run); } run = 0; last = -1; continue; }
            const int code = int(std::lround(std::clamp(y[i], 0.0f, 1.0f) * 255.0f));
            if (code == last) ++run;
            else { if (run) { ++runs; pixels += std::uint64_t(run); } run = 1; last = code; }
        }
        if (run) { ++runs; pixels += std::uint64_t(run); }
    }
    return runs ? double(pixels) / double(runs) : 0.0;
}

// False contours: positions inside `mask` where the 8-bit code changes between two neighbours along a row
// or a column while it is constant for `flat` pixels on both sides (the edge of a band; dither, grain and
// texture have no such flat runs). Returns the summed code steps per 1000 mask pixels: 0 = no contour.
inline double pushCleanContourIndex(const std::vector<float>& y, const std::vector<std::uint8_t>& mask, int width, int height, int flat = 6) {
    std::vector<int> code(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) code[i] = int(std::lround(std::clamp(y[i], 0.0f, 1.0f) * 255.0f));
    double steps = 0; std::uint64_t pixels = 0;
    const auto at = [&](int x, int r) { return std::size_t(r) * std::size_t(width) + std::size_t(x); };
    for (int r = 0; r < height; ++r)
        for (int x = 0; x < width; ++x) {
            if (!mask[at(x, r)]) continue;
            ++pixels;
            for (int axis = 0; axis < 2; ++axis) {
                const int dx = axis == 0 ? 1 : 0, dy = 1 - dx;
                const int lo = (axis == 0 ? x : r) - (flat - 1), hi = (axis == 0 ? x : r) + flat;
                if (lo < 0 || hi >= (axis == 0 ? width : height)) continue;
                const int a = code[at(x, r)], b = code[at(x + dx, r + dy)];
                if (a == b || !mask[at(x + dx, r + dy)]) continue;
                bool level = true;
                for (int k = 1; k < flat && level; ++k)
                    level = code[at(x - k * dx, r - k * dy)] == a && code[at(x + dx + k * dx, r + dy + k * dy)] == b &&
                            mask[at(x - k * dx, r - k * dy)] && mask[at(x + dx + k * dx, r + dy + k * dy)];
                if (level) steps += std::abs(a - b);
            }
        }
    return pixels ? 1000.0 * steps / double(pixels) : 0.0;
}

// The text of `[slipstream-clean]` (S5_CONTRACT.md item 6; the log category supplies the tag): one line per
// statistics window.
inline std::string pushCleanStatsLine(std::uint64_t frames, int strength, unsigned width, unsigned height,
                                      double gpuMsP50, double gpuMsP95, bool hudProtect, bool hdr) {
    char buffer[256];
    std::snprintf(buffer, sizeof buffer,
                  "frames=%llu strength=%d size=%ux%u gpuMsP50=%.3f gpuMsP95=%.3f hudProtect=%d hdr=%d",
                  static_cast<unsigned long long>(frames), strength, width, height, gpuMsP50, gpuMsP95,
                  hudProtect ? 1 : 0, hdr ? 1 : 0);
    return buffer;
}
}
