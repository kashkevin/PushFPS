// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-hud" (25 Nov 2026): HUD Guard, the part of Live HUD Lock that keeps a small still overlay (crosshair, hit
// marker, text, a minimap's ring) out of frame generation. Pure rules and the CPU reference the GPU passes
// (shaders/PushHudGuard*.hlsl, pipeline/PushHudGuardPass.h) are checked against on WARP; no D3D12 here.
//
// Why (docs/drafts/HUD_GUARD.md; research HUD_GHOSTING.md): an interpolator moves every pixel with the motion of its
// block. A crosshair a few pixels wide is far smaller than any block, so it takes the background's motion: in a frame at
// time t between real frames A and B it appears at c + t d (from A, weight 1 - t) and c - (1 - t) d (from B, weight t),
// where d is the background's shift per real frame, and not at its place c. At 3X (t = 1/3, 2/3) that is four faint
// copies around the sharp one of the real frame: the "five copies" seen in a circular pan; at 2X two (three in all).
// Live HUD Lock (PushHudLock) puts the opaque core back at c but cannot remove the copies: they lie on moving pixels.
//
// What the guard does, per real frame B (against the previous real frame A, past frames only, nothing waits):
//   1. change: |luma(B) - luma(A)| per pixel in the encoded (perceptual) values, stored as 8-bit (R8_UNORM): luma, not
//      every channel, because the capture card's 4:2:0 chroma mixes a thin overlay's colour with the moving picture.
//   2. state per pixel: `run` = real frames in a row the pixel stayed still (change <= still), `near` = frames of that run
//      with a moving pixel (change > move) within `reach` px along one of 4 axes, `thin` = frames of that run with moving
//      pixels on BOTH sides along one axis (a still element at most 2 x reach - 1 px thick inside moving picture).
//      A guarded pixel keeps its run while its change stays <= hold (hysteresis: no flicker on the capture's noise);
//      any larger change ends it at once (released on that very frame).
//   3. protect = run >= stillFrames and near >= 1; clean = protect and thin >= 1. Both masks are grown by `grow` px.
//   4. pre-clean (generator input only, never shown): in the copy of B the generator reads, a `clean` pixel takes a
//      background estimate from the nearest unmasked pixels along 8 directions (up to fillReach px, weight 1 / distance),
//      so the generator never sees the overlay and draws no copies of it.
//   5. compose (each generated frame at time t): a `protect` pixel is A + t (B - A) of the real frames, the console's
//      own pixels: the crosshair stays sharp and in place in every frame. A protected pixel changed by at most `hold`
//      between A and B, so this is never far from either real frame.
// An overlay that appears is protected from its stillFrames-th real frame (200 ms at 30 FPS) once something moves
// around it; when nothing moves (a still camera) nothing new is protected, which is harmless: generated == real there.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace veyra::pipeline {

struct PushHudGuardParams {
    float still = 0.015f;          // luma change (encoded 0..1) still counted as the same pixel (about 4 SDR / 15 PQ codes)
    float hold = 0.04f;            // a protected pixel stays protected while its change is at most this
    float move = 0.03f;            // a change above this is motion (evidence for the surround)
    // Real frames in a row a pixel must stay still before it can be protected. 6 (200 ms at 30 FPS): on the synthetic
    // pans of tools/hud_guard_eval (textured world, 24 px per frame) 3 frames protected about 25 background pixels a frame
    // whose luma happened to stay put (each one a wrong pixel in the generated frame), 4 frames 2-3, 5 frames 0-0.2, 6
    // frames 0-0.03 (OFFLINE, CPU, 39 frames). A crosshair is on screen far longer than that before a turn.
    std::uint32_t stillFrames = 6;
    std::uint32_t reach = 8;       // px searched along each axis for moving pixels
    std::uint32_t grow = 1;        // px the masks are grown by (anti-aliased edges)
    std::uint32_t fillReach = 12;  // px searched for unmasked pixels by the pre-clean
};
inline constexpr std::uint32_t kPushHudGuardMaxRun = 255, kPushHudGuardMaxCount = 15;
inline constexpr std::uint32_t kPushHudGuardTile = 16;   // the GPU's early-out tile (>= 2 x reach is required)

// The change as the GPU stores it (R8_UNORM): clamped and rounded to 1/255.
inline float pushHudGuardQuantize(float c) {
    const float v = std::clamp(c, 0.0f, 1.0f);
    return std::floor(v * 255.0f + 0.5f) / 255.0f;
}
inline float pushHudGuardLuma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

struct PushHudGuardColor { float r = 0, g = 0, b = 0, a = 0; };
// R8G8B8A8_UNORM or R10G10B10A2_UNORM words (encoded values: sRGB / PQ codes).
inline PushHudGuardColor pushHudGuardUnpack(std::uint32_t w, bool tenBit) {
    if (tenBit) return {float(w & 1023u) / 1023.0f, float((w >> 10) & 1023u) / 1023.0f, float((w >> 20) & 1023u) / 1023.0f, float(w >> 30) / 3.0f};
    return {float(w & 255u) / 255.0f, float((w >> 8) & 255u) / 255.0f, float((w >> 16) & 255u) / 255.0f, float(w >> 24) / 255.0f};
}
inline std::uint32_t pushHudGuardPack(const PushHudGuardColor& c, bool tenBit) {
    const auto q = [](float v, float scale) { return std::uint32_t(std::floor(std::clamp(v, 0.0f, 1.0f) * scale + 0.5f)); };
    if (tenBit) return q(c.r, 1023.0f) | (q(c.g, 1023.0f) << 10) | (q(c.b, 1023.0f) << 20) | (q(c.a, 3.0f) << 30);
    return q(c.r, 255.0f) | (q(c.g, 255.0f) << 8) | (q(c.b, 255.0f) << 16) | (q(c.a, 255.0f) << 24);
}
inline float pushHudGuardChange(std::uint32_t a, std::uint32_t b, bool tenBit) {
    const auto ca = pushHudGuardUnpack(a, tenBit), cb = pushHudGuardUnpack(b, tenBit);
    return pushHudGuardQuantize(std::fabs(pushHudGuardLuma(cb.r, cb.g, cb.b) - pushHudGuardLuma(ca.r, ca.g, ca.b)));
}

struct PushHudGuardCell { std::uint32_t run = 0, nearby = 0, thin = 0; };
inline bool pushHudGuardProtected(const PushHudGuardCell& c, const PushHudGuardParams& p) { return c.run >= p.stillFrames && c.nearby >= 1; }
inline bool pushHudGuardCleaned(const PushHudGuardCell& c, const PushHudGuardParams& p) { return pushHudGuardProtected(c, p) && c.thin >= 1; }
// The GPU's state texture (R16G16_FLOAT, exact for these integers): x = run, y = near + 16 x thin.
inline std::uint32_t pushHudGuardPackedEvidence(const PushHudGuardCell& c) { return c.nearby + 16u * c.thin; }

// One step of a pixel's state. `wasProtected` from the cell before this frame; scan results from this frame's changes.
inline PushHudGuardCell pushHudGuardStep(PushHudGuardCell c, float change, bool nearMoving, bool thinMoving, const PushHudGuardParams& p) {
    const bool was = pushHudGuardProtected(c, p);
    if (change <= p.still) c.run = std::min(c.run + 1u, kPushHudGuardMaxRun);
    else if (!(was && change <= p.hold)) return {};
    if (nearMoving) c.nearby = std::min(c.nearby + 1u, kPushHudGuardMaxCount);
    if (thinMoving) c.thin = std::min(c.thin + 1u, kPushHudGuardMaxCount);
    return c;
}

// The middle tenth of the picture in each direction (where a crosshair is): counted apart in the evidence line.
inline bool pushHudGuardCentre(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const std::int64_t dx = std::llabs(2 * std::int64_t(x) + 1 - std::int64_t(w)), dy = std::llabs(2 * std::int64_t(y) + 1 - std::int64_t(h));
    return dx * 10 <= std::int64_t(w) && dy * 10 <= std::int64_t(h);
}

struct PushHudGuardCounts {
    std::uint64_t protect = 0, clean = 0, centreProtect = 0, centreClean = 0;   // pixels, before growing
};

// The four scan axes (both directions of each are searched).
inline constexpr int kPushHudGuardAxes[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
// Eight fill directions of the pre-clean.
inline constexpr int kPushHudGuardFill[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};

// CPU reference of the whole guard (what the GPU passes must give).
class PushHudGuardReference {
public:
    PushHudGuardReference(std::uint32_t w, std::uint32_t h, PushHudGuardParams rules = {})
        : w_(w), h_(h), p_(rules), cells_(std::size_t(w) * h), change_(std::size_t(w) * h, 0.0f) {}

    const PushHudGuardParams& params() const { return p_; }
    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    const PushHudGuardCell& cell(std::uint32_t x, std::uint32_t y) const { return cells_[std::size_t(y) * w_ + x]; }
    float change(std::uint32_t x, std::uint32_t y) const { return change_[std::size_t(y) * w_ + x]; }

    // B (this real frame) against A (the previous one). reset: no valid A (start, seek, settings change): start over.
    PushHudGuardCounts update(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, bool tenBit, bool reset) {
        PushHudGuardCounts counts;
        if (reset) {
            std::fill(cells_.begin(), cells_.end(), PushHudGuardCell{});
            std::fill(change_.begin(), change_.end(), 0.0f);
            return counts;
        }
        for (std::size_t i = 0; i < change_.size(); ++i) change_[i] = pushHudGuardChange(a[i], b[i], tenBit);
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                auto& c = cells_[std::size_t(y) * w_ + x];
                bool nearMoving = false, thinMoving = false;
                scan(x, y, nearMoving, thinMoving);
                c = pushHudGuardStep(c, change_[std::size_t(y) * w_ + x], nearMoving, thinMoving, p_);
                if (pushHudGuardProtected(c, p_)) {
                    ++counts.protect;
                    const bool centre = pushHudGuardCentre(x, y, w_, h_);
                    if (centre) ++counts.centreProtect;
                    if (pushHudGuardCleaned(c, p_)) { ++counts.clean; if (centre) ++counts.centreClean; }
                }
            }
        return counts;
    }
    // The masks the passes use: grown by `grow` px (any protected / cleaned pixel in the (2 grow + 1)^2 square).
    bool protectMask(int x, int y) const { return grown(x, y, false); }
    bool cleanMask(int x, int y) const { return grown(x, y, true); }

    // Pre-clean of B (the generator's input copy): cleanMask pixels from the nearest unmasked pixels along 8 directions.
    std::vector<std::uint32_t> clean(const std::vector<std::uint32_t>& b, bool tenBit) const {
        std::vector<std::uint32_t> out = b;
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                if (!cleanMask(int(x), int(y))) continue;
                float r = 0, g = 0, bl = 0, sum = 0;
                for (const auto& d : kPushHudGuardFill)
                    for (std::uint32_t k = 1; k <= p_.fillReach; ++k) {
                        const int qx = int(x) + d[0] * int(k), qy = int(y) + d[1] * int(k);
                        if (qx < 0 || qy < 0 || qx >= int(w_) || qy >= int(h_)) break;
                        if (cleanMask(qx, qy)) continue;
                        const auto c = pushHudGuardUnpack(b[std::size_t(qy) * w_ + std::size_t(qx)], tenBit);
                        const float wgt = 1.0f / float(k);
                        r += wgt * c.r; g += wgt * c.g; bl += wgt * c.b; sum += wgt;
                        break;
                    }
                if (sum <= 0) continue;   // nothing unmasked within reach: left as it is
                auto c = pushHudGuardUnpack(b[std::size_t(y) * w_ + x], tenBit);
                c.r = r / sum; c.g = g / sum; c.b = bl / sum;
                out[std::size_t(y) * w_ + x] = pushHudGuardPack(c, tenBit);
            }
        return out;
    }
    // Compose of a generated frame at time t (0 = A, 1 = B): protectMask pixels from the real frames.
    void compose(std::vector<std::uint32_t>& generated, const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, float t, bool tenBit) const {
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                if (!protectMask(int(x), int(y))) continue;
                const std::size_t i = std::size_t(y) * w_ + x;
                const auto ca = pushHudGuardUnpack(a[i], tenBit), cb = pushHudGuardUnpack(b[i], tenBit);
                generated[i] = pushHudGuardPack({ca.r + t * (cb.r - ca.r), ca.g + t * (cb.g - ca.g), ca.b + t * (cb.b - ca.b), ca.a + t * (cb.a - ca.a)}, tenBit);
            }
    }

private:
    bool moving(int x, int y) const {
        if (x < 0 || y < 0 || x >= int(w_) || y >= int(h_)) return false;
        return change_[std::size_t(y) * w_ + std::size_t(x)] > p_.move;
    }
    void scan(std::uint32_t x, std::uint32_t y, bool& nearMoving, bool& thinMoving) const {
        for (const auto& axis : kPushHudGuardAxes) {
            bool plus = false, minus = false;
            for (std::uint32_t k = 1; k <= p_.reach && !plus; ++k) plus = moving(int(x) + axis[0] * int(k), int(y) + axis[1] * int(k));
            for (std::uint32_t k = 1; k <= p_.reach && !minus; ++k) minus = moving(int(x) - axis[0] * int(k), int(y) - axis[1] * int(k));
            nearMoving = nearMoving || plus || minus;
            thinMoving = thinMoving || (plus && minus);
        }
    }
    bool grown(int x, int y, bool cleanOnly) const {
        const int g = int(p_.grow);
        for (int dy = -g; dy <= g; ++dy)
            for (int dx = -g; dx <= g; ++dx) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= int(w_) || qy >= int(h_)) continue;
                const auto& c = cells_[std::size_t(qy) * w_ + std::size_t(qx)];
                if (cleanOnly ? pushHudGuardCleaned(c, p_) : pushHudGuardProtected(c, p_)) return true;
            }
        return false;
    }

    std::uint32_t w_, h_;
    PushHudGuardParams p_;
    std::vector<PushHudGuardCell> cells_;
    std::vector<float> change_;
};

// The evidence line (one a second; pipeline/PushHudGuardPass.cpp): shares in per mille of the picture.
inline double pushHudGuardPerMille(std::uint64_t pixels, std::uint64_t total) { return total ? 1000.0 * double(pixels) / double(total) : 0.0; }

}  // namespace veyra::pipeline
