// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Synthetic scenes for HUD Guard (pipeline/PushHudGuard.h): a textured background panned by the camera (straight or in a
// circle), a still overlay (crosshair of 1-3 px lines, opaque or translucent; text strokes; a minimap ring with moving
// content), and a SIMULATED interpolator that does what a block-motion generator does to an overlay far smaller than its
// blocks: every pixel moves with the background (I = (1 - t) A(x + t d) + t B(x - (1 - t) d), bilinear). It is a model
// of the mechanism, not NVIDIA VFG (a black box) and not PushWarp. Used by tests/remoteplay/HudGuardTests.inc and
// tools/hud_guard_eval (OFFLINE, CPU). 8-bit sRGB words (R8G8B8A8), grey.
#include "veyra/pipeline/PushHudGuard.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace hud_guard_scene {

inline float hash01(int x, int y, unsigned seed) {
    std::uint32_t h = std::uint32_t(x) * 374761393u + std::uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xffffu) / 65535.0f;
}
// Value noise at a cell size, bilinear between lattice points (continuous, so sub-pixel shifts are meaningful).
inline float noise(float x, float y, float cell, unsigned seed) {
    const float fx = x / cell, fy = y / cell, x0 = std::floor(fx), y0 = std::floor(fy), tx = fx - x0, ty = fy - y0;
    const int ix = int(x0), iy = int(y0);
    const float a = hash01(ix, iy, seed), b = hash01(ix + 1, iy, seed), c = hash01(ix, iy + 1, seed), d = hash01(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}
// The game world: grey 0.15..0.85 with coarse and fine detail (a textured surface the motion estimate can follow).
inline float world(float x, float y) { return 0.15f + 0.45f * noise(x, y, 9.0f, 1u) + 0.25f * noise(x, y, 2.5f, 2u); }

struct Overlay {
    enum class Kind { None, Crosshair, Text, Minimap } kind = Kind::Crosshair;
    int lineWidth = 2;      // px
    int arm = 14;           // crosshair half length, px
    float alpha = 1.0f;     // 1 = opaque
    float value = 0.96f;    // the overlay's grey
};
struct Camera {
    bool circle = false;
    float speed = 24.0f;    // px of background shift per real frame
    float radiusFrames = 6.0f;   // circle: the direction turns by 1 / radiusFrames radians per frame
    float dirX = 1.0f, dirY = 0.0f;
};

class Scene {
public:
    Scene(std::uint32_t w, std::uint32_t h, Overlay o, Camera c) : w_(w), h_(h), o_(o), c_(c) {}
    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    // Camera position (world offset) at real frame k.
    void position(int k, float& px, float& py) const {
        if (!c_.circle) { px = c_.speed * c_.dirX * float(k); py = c_.speed * c_.dirY * float(k); return; }
        const float r = c_.speed * c_.radiusFrames;   // arc length per frame = speed
        const float a = float(k) / c_.radiusFrames;
        px = r * std::cos(a); py = r * std::sin(a);
    }
    // Overlay coverage (0..1) at a screen pixel, and whether it is the overlay's moving inside (minimap content).
    float coverage(int x, int y, bool& insideMap) const {
        insideMap = false;
        const int cx = int(w_) / 2, cy = int(h_) / 2;
        switch (o_.kind) {
        case Overlay::Kind::None: return 0.0f;
        case Overlay::Kind::Crosshair: {
            const int half = o_.lineWidth / 2, lo = -half, hi = lo + o_.lineWidth - 1;
            const int dx = x - cx, dy = y - cy;
            const bool h = dy >= lo && dy <= hi && std::abs(dx) <= o_.arm && std::abs(dx) >= 3;
            const bool v = dx >= lo && dx <= hi && std::abs(dy) <= o_.arm && std::abs(dy) >= 3;
            const bool dot = dx >= lo && dx <= hi && dy >= lo && dy <= hi;
            return h || v || dot ? o_.alpha : 0.0f;
        }
        case Overlay::Kind::Text: {
            // Five "glyphs" of 2-px strokes in a 12 x 16 box each, left of centre (an ammo counter drawn over the game).
            const int bx = cx - 40, by = cy - 8;
            if (y < by || y >= by + 16 || x < bx || x >= bx + 80) return 0.0f;
            const int gx = (x - bx) % 16, gy = y - by;
            if (gx >= 12) return 0.0f;
            const bool stroke = gx < 2 || gx >= 10 || gy < 2 || gy >= 14 || (gy >= 7 && gy < 9);
            return stroke ? o_.alpha : 0.0f;
        }
        case Overlay::Kind::Minimap: {
            const float dx = float(x - cx) + 0.5f, dy = float(y - cy) + 0.5f, r = std::sqrt(dx * dx + dy * dy);
            if (r >= 30.0f && r < 33.0f) return o_.alpha;   // the ring: 3 px
            if (r < 30.0f) insideMap = true;
            return 0.0f;
        }
        }
        return 0.0f;
    }
    // Screen picture of real frame k (overlay over the world at camera position k); withOverlay false: the world only.
    std::vector<std::uint32_t> frame(int k, bool withOverlay) const {
        float px, py;
        position(k, px, py);
        return render(px, py, k, withOverlay);
    }
    // The world as it really is at time k - 1 + t (for the truth of a generated frame), overlay included.
    std::vector<std::uint32_t> truth(int k, float t, bool withOverlay) const {
        float ax, ay, bx, by;
        position(k - 1, ax, ay); position(k, bx, by);
        return render(ax + t * (bx - ax), ay + t * (by - ay), k, withOverlay);
    }
    // The shift of the background between real frames k - 1 and k (screen px: content moves by -d).
    void shift(int k, float& dx, float& dy) const {
        float ax, ay, bx, by;
        position(k - 1, ax, ay); position(k, bx, by);
        dx = bx - ax; dy = by - ay;
    }
    // SIMULATED block-motion interpolation of every pixel with the background's motion.
    std::vector<std::uint32_t> interpolate(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, int k, float t) const {
        float dx, dy;
        shift(k, dx, dy);
        std::vector<std::uint32_t> out(a.size());
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                const float va = sample(a, float(x) + t * dx, float(y) + t * dy), vb = sample(b, float(x) - (1 - t) * dx, float(y) - (1 - t) * dy);
                out[std::size_t(y) * w_ + x] = grey((1 - t) * va + t * vb);
            }
        return out;
    }
    static std::uint32_t grey(float v) {
        return veyra::pipeline::pushHudGuardPack({v, v, v, 1.0f}, false);
    }
    static float value(std::uint32_t w) { return float(w & 255u) / 255.0f; }

private:
    std::vector<std::uint32_t> render(float px, float py, int k, bool withOverlay) const {
        std::vector<std::uint32_t> out(std::size_t(w_) * h_);
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                float v = world(float(x) + px, float(y) + py);
                if (withOverlay) {
                    bool inside = false;
                    const float a = coverage(int(x), int(y), inside);
                    if (inside) v = 0.2f + 0.5f * noise(float(x) + 3.0f * float(k), float(y), 4.0f, 7u);   // the map scrolls by itself
                    v = a * o_.value + (1 - a) * v;
                }
                out[std::size_t(y) * w_ + x] = grey(v);
            }
        return out;
    }
    float sample(const std::vector<std::uint32_t>& f, float qx, float qy) const {
        qx = std::clamp(qx, 0.0f, float(w_ - 1)); qy = std::clamp(qy, 0.0f, float(h_ - 1));
        const float x0 = std::floor(qx), y0 = std::floor(qy), tx = qx - x0, ty = qy - y0;
        const std::uint32_t ix = std::uint32_t(x0), iy = std::uint32_t(y0), ix1 = std::min(ix + 1, w_ - 1), iy1 = std::min(iy + 1, h_ - 1);
        const float a = value(f[std::size_t(iy) * w_ + ix]), b = value(f[std::size_t(iy) * w_ + ix1]);
        const float c = value(f[std::size_t(iy1) * w_ + ix]), d = value(f[std::size_t(iy1) * w_ + ix1]);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    }
    std::uint32_t w_, h_;
    Overlay o_;
    Camera c_;
};

// Measures of one generated frame R against the generator's own output on the overlay-free pictures (G0) and the truth.
struct Measure {
    double ghostMean = 0;        // mean |R - G0| (8-bit codes) outside the overlay grown by 3 px: the overlay's copies
    std::uint64_t ghostPixels = 0;   // pixels there with |R - G0| > 8 codes
    double overlayMean = 0;      // mean |R - truth| on the overlay's own pixels (codes)
    std::uint64_t overlayExact = 0, overlayPixels = 0;   // overlay pixels with R == truth exactly
    double ringMean = 0;         // mean |R - truth| in the ring 4..12 px around the overlay (background quality near it)
};
inline Measure measure(const Scene& s, const std::vector<std::uint32_t>& r, const std::vector<std::uint32_t>& g0, const std::vector<std::uint32_t>& truth) {
    const int W = int(s.width()), H = int(s.height());
    std::vector<int> dist(std::size_t(W) * H, 1000);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            bool inside = false;
            if (s.coverage(x, y, inside) > 0) dist[std::size_t(y) * W + x] = 0;
        }
    // Chebyshev distance to the overlay (small scenes: brute force within 12 px).
    std::vector<int> d2(dist);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int dy = -12; dy <= 12; ++dy)
                for (int dx = -12; dx <= 12; ++dx) {
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || qx >= W || qy >= H || dist[std::size_t(qy) * W + qx] != 0) continue;
                    d2[std::size_t(y) * W + x] = std::min(d2[std::size_t(y) * W + x], std::max(std::abs(dx), std::abs(dy)));
                }
    Measure m;
    double ghost = 0, over = 0, ring = 0;
    std::uint64_t ghostN = 0, ringN = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y) * W + x;
            bool inside = false;
            if (s.coverage(x, y, inside) == 0 && inside) continue;   // the map's own moving content: not judged here
            const int rv = int(r[i] & 255u), gv = int(g0[i] & 255u), tv = int(truth[i] & 255u);
            if (d2[i] == 0) { ++m.overlayPixels; over += std::abs(rv - tv); m.overlayExact += rv == tv; }
            else if (d2[i] > 3) { ghost += std::abs(rv - gv); ++ghostN; m.ghostPixels += std::abs(rv - gv) > 8; }
            if (d2[i] >= 4 && d2[i] <= 12) { ring += std::abs(rv - tv); ++ringN; }
        }
    m.ghostMean = ghostN ? ghost / double(ghostN) : 0;
    m.overlayMean = m.overlayPixels ? over / double(m.overlayPixels) : 0;
    m.ringMean = ringN ? ring / double(ringN) : 0;
    return m;
}

enum class Mode { Raw, ComposeOnly, CleanAndCompose };
// Runs `frames` real frames through the guard; for every pair from `from` on, generates the multiplier's frames in the
// given mode and accumulates the measures (averaged over generated frames).
struct Result { Measure mean; unsigned generated = 0; };
inline Result run(const Scene& s, unsigned multiplier, Mode mode, int frames = 16, int from = 8) {
    veyra::pipeline::PushHudGuardReference guard(s.width(), s.height());
    Result res;
    std::vector<std::uint32_t> prev = s.frame(0, true), prevClean = prev, prevPlain = s.frame(0, false);
    guard.update(prev, prev, false, true);
    for (int k = 1; k < frames; ++k) {
        const auto b = s.frame(k, true), bPlain = s.frame(k, false);
        guard.update(prev, b, false, false);
        const auto bClean = mode == Mode::CleanAndCompose ? guard.clean(b, false) : b;
        if (k >= from)
            for (unsigned sub = 1; sub < multiplier; ++sub) {
                const float t = float(sub) / float(multiplier);
                auto r = s.interpolate(mode == Mode::CleanAndCompose ? prevClean : prev, bClean, k, t);
                if (mode != Mode::Raw) guard.compose(r, prev, b, t, false);
                const auto g0 = s.interpolate(prevPlain, bPlain, k, t);
                const auto m = measure(s, r, g0, s.truth(k, t, true));
                res.mean.ghostMean += m.ghostMean; res.mean.ghostPixels += m.ghostPixels; res.mean.overlayMean += m.overlayMean;
                res.mean.overlayExact += m.overlayExact; res.mean.overlayPixels += m.overlayPixels; res.mean.ringMean += m.ringMean;
                ++res.generated;
            }
        prev = b; prevClean = bClean; prevPlain = bPlain;
    }
    if (res.generated) {
        res.mean.ghostMean /= res.generated; res.mean.overlayMean /= res.generated; res.mean.ringMean /= res.generated;
        res.mean.ghostPixels /= res.generated;
    }
    return res;
}

}  // namespace hud_guard_scene
