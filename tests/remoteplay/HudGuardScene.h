// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Synthetic scenes for HUD Guard (pipeline/PushHudGuard.h): a world panned by the camera (straight, diagonal or in a
// circle, or a still camera) that is textured, low-contrast texture, or flat sky with thin trunks; optionally a thin bar
// that moves across the picture by itself; a still overlay (crosshair of 1-3 px lines, opaque or translucent, that may
// change its value at a given frame as when aiming; text strokes; a minimap ring with moving content). Two generators:
// SIMULATED, which does what a block-motion generator does to an overlay far smaller than its blocks (every pixel
// moves with the camera: I = (1 - t) A(x + t d) + t B(x - (1 - t) d), bilinear; not NVIDIA VFG, not PushWarp), and
// PERFECT, which returns the truth at time t (any damage the guard's compose does then shows by itself: review MUST 1).
// SDR (8-bit words) or HDR10 (10-bit words: SDR greys at 58 % of the PQ code range, as SDR content sits in PQ); all
// measures are in 8-bit SDR code units. Used by tests/remoteplay/HudGuardTests.inc and tools/hud_guard_eval (CPU).
#include "veyra/pipeline/PushHudGuard.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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

struct World {
    enum class Kind { Textured, LowContrast, SkyTrunks, Sky, Dark } kind = Kind::Textured;
    float contrast = 0.05f;   // LowContrast: the texture's range
    float gain = 1.0f;        // Textured: scales the picture (HDR highlights: above 1 it clips in SDR, goes beyond the SDR span in PQ)
};
inline float worldValue(const World& w, float x, float y) {
    switch (w.kind) {
    case World::Kind::Textured: return w.gain * (0.15f + 0.45f * noise(x, y, 9.0f, 1u) + 0.25f * noise(x, y, 2.5f, 2u));
    // Flat sky: a gentle gradient (0.02 over 80 px: under 0.005 per real frame at 20 px, below every threshold), nothing
    // else. A wall or fog looks the same to the guard. Dark: a night picture, range 0.03 around 0.05.
    case World::Kind::Sky: return 0.72f + 0.02f * noise(x, y, 80.0f, 5u);
    case World::Kind::Dark: return 0.04f + 0.03f * (0.6f * noise(x, y, 9.0f, 1u) + 0.4f * noise(x, y, 2.5f, 2u));
    case World::Kind::LowContrast: return 0.45f + w.contrast * (0.6f * noise(x, y, 9.0f, 1u) + 0.4f * noise(x, y, 2.5f, 2u));
    case World::Kind::SkyTrunks: {
        // 6-px dark trunks (anti-aliased by one pixel) in front of a flat sky, one per 40-px cell at a position that
        // varies from cell to cell (a regular spacing would alias with the pan and hide trunks at every real frame).
        const float cellX = std::floor(x / 40.0f), m = x - 40.0f * cellX;
        const float at = 4.0f + 28.0f * hash01(int(cellX), 0, 11u);
        const float inside = std::clamp(std::min(m - at, at + 6.0f - m) + 0.5f, 0.0f, 1.0f);
        return 0.72f + inside * (0.18f - 0.72f);
    }
    }
    return 0.5f;
}

struct Overlay {
    enum class Kind { None, Crosshair, Text, Minimap } kind = Kind::Crosshair;
    int lineWidth = 2;      // px
    int arm = 14;           // crosshair half length, px
    float alpha = 1.0f;     // 1 = opaque
    float value = 0.96f;    // the overlay's grey
    int changeAt = -1;      // from this real frame on the overlay's grey is changedValue (aiming at a target)
    float changedValue = 0.55f;
    float fringe = 0.0f;    // coverage of a 1-px anti-aliased edge around the overlay (0: hard edge)
    int appearAt = -1;      // shown from this real frame on (-1: from the start): subtitles, a hit marker
    int goneAt = -1;        // not shown from this real frame on (-1: never)
    int bloomAt = -1;       // from this real frame on a crosshair's arms are bloomArm long (spread bloom)
    int bloomArm = 22;
};
struct Camera {
    bool circle = false;
    float speed = 24.0f;    // px of background shift per real frame (0: still camera)
    float radiusFrames = 6.0f;   // circle: the direction turns by 1 / radiusFrames radians per frame
    float dirX = 1.0f, dirY = 0.0f;
};
// A light bar `width` px wide and `height` px tall that crosses the picture by itself at `speed` px per real frame.
struct Mover {
    bool on = false;
    float speed = 12.0f, width = 6.0f, height = 60.0f, value = 0.92f;
    float topFrac = 0.25f;   // top edge as a share of the picture's height
    float startAt = 0.0f;    // real frame the bar enters from the left (absent before; the bar's own clock starts there)
};

inline constexpr float kHdrSpan = 0.58f;   // SDR's 0-203 nits in PQ codes (about 58 % of the range)

class Scene {
public:
    Scene(std::uint32_t w, std::uint32_t h, Overlay o, Camera c, World wd = {}, Mover m = {}, bool hdr = false)
        : w_(w), h_(h), o_(o), c_(c), world_(wd), mover_(m), hdr_(hdr) {}
    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    bool hdr() const { return hdr_; }
    // Camera position (world offset) at (fractional) real frame k.
    void position(float k, float& px, float& py) const {
        if (!c_.circle) { px = c_.speed * c_.dirX * k; py = c_.speed * c_.dirY * k; return; }
        const float r = c_.speed * c_.radiusFrames;   // arc length per frame = speed
        const float a = k / c_.radiusFrames;
        px = r * std::cos(a); py = r * std::sin(a);
    }
    // Overlay coverage (0..1) at a screen pixel, and whether it is the overlay's moving inside (minimap content). With a
    // fringe, a pixel beside the overlay's core has fringe x alpha.
    float coverage(int x, int y, bool& insideMap) const {
        const float c = core(x, y, insideMap);
        if (c > 0 || o_.fringe <= 0) return c;
        bool unused = false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dy) && core(x + dx, y + dy, unused) > 0) return o_.fringe * o_.alpha;
        return 0.0f;
    }
    // A pixel of the soft edge: covered only by the fringe, not by the overlay's own core.
    bool isFringe(int x, int y) const {
        bool inside = false;
        return o_.fringe > 0 && core(x, y, inside) == 0.0f && coverage(x, y, inside) > 0.0f;
    }
    float core(int x, int y, bool& insideMap) const {
        insideMap = false;
        if ((o_.appearAt >= 0 && frameIndex_ < o_.appearAt) || (o_.goneAt >= 0 && frameIndex_ >= o_.goneAt)) return 0.0f;
        const int cx = int(w_) / 2, cy = int(h_) / 2;
        switch (o_.kind) {
        case Overlay::Kind::None: return 0.0f;
        case Overlay::Kind::Crosshair: {
            const int half = o_.lineWidth / 2, lo = -half, hi = lo + o_.lineWidth - 1;
            const int dx = x - cx, dy = y - cy;
            const int arm = o_.bloomAt >= 0 && frameIndex_ >= o_.bloomAt ? o_.bloomArm : o_.arm;
            const bool h = dy >= lo && dy <= hi && std::abs(dx) <= arm && std::abs(dx) >= 3;
            const bool v = dx >= lo && dx <= hi && std::abs(dy) <= arm && std::abs(dy) >= 3;
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
    // Whether the moving bar covers a pixel at (fractional) time k.
    bool moverAt(int x, int y, float k) const {
        if (!mover_.on || k < mover_.startAt) return false;
        const float left = 6.0f + mover_.speed * (k - mover_.startAt), span = float(w_) + 40.0f;
        const float l = left - span * std::floor(left / span) - 20.0f;
        const float top = float(h_) * mover_.topFrac;
        return float(x) >= l && float(x) < l + mover_.width && float(y) >= top && float(y) < top + mover_.height;
    }
    // Screen picture of real frame k (overlay over the world at camera position k); withOverlay false: the world only.
    std::vector<std::uint32_t> frame(int k, bool withOverlay) const { return render(float(k), k, withOverlay); }
    // The truth at time k - 1 + t (for a generated frame of the pair k - 1 -> k), overlay included.
    std::vector<std::uint32_t> truth(int k, float t, bool withOverlay) const { return render(float(k - 1) + t, k, withOverlay); }
    // The camera's shift between real frames k - 1 and k (screen: content moves by -d).
    void shift(int k, float& dx, float& dy) const {
        float ax, ay, bx, by;
        position(float(k - 1), ax, ay); position(float(k), bx, by);
        dx = bx - ax; dy = by - ay;
    }
    // SIMULATED block-motion interpolation of every pixel with the camera's motion.
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
    std::uint32_t grey(float v) const {
        const float c = hdr_ ? v * kHdrSpan : v;
        return veyra::pipeline::pushHudGuardPack({c, c, c, 1.0f}, hdr_);
    }
    // A word's grey in SDR units.
    float value(std::uint32_t w) const {
        return hdr_ ? float(w & 1023u) / 1023.0f / kHdrSpan : float(w & 255u) / 255.0f;
    }

private:
    std::vector<std::uint32_t> render(float time, int k, bool withOverlay) const {
        float px, py;
        frameIndex_ = k;   // coverage() (appear / gone / bloom) follows the frame rendered last
        position(time, px, py);
        const float ov = o_.changeAt >= 0 && k >= o_.changeAt ? o_.changedValue : o_.value;
        std::vector<std::uint32_t> out(std::size_t(w_) * h_);
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                float v = worldValue(world_, float(x) + px, float(y) + py);
                if (moverAt(int(x), int(y), time)) v = mover_.value;
                if (withOverlay) {
                    bool inside = false;
                    const float a = coverage(int(x), int(y), inside);
                    if (inside) v = 0.2f + 0.5f * noise(float(x) + 3.0f * time, float(y), 4.0f, 7u);   // the map scrolls by itself
                    v = a * ov + (1 - a) * v;
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
    mutable int frameIndex_ = 1 << 20;
    std::uint32_t w_, h_;
    Overlay o_;
    Camera c_;
    World world_;
    Mover mover_;
    bool hdr_;
};

// Measures of one generated frame R against the generator's own output on the overlay-free pictures (G0) and the truth.
// Distances: Chebyshev px to the overlay (0 = on it).
struct Measure {
    double ghostMean = 0;            // mean |R - G0| (8-bit codes) farther than 3 px from the overlay: the overlay's copies
    std::uint64_t ghostPixels = 0;   // pixels there with |R - G0| > 8 codes
    double overlayMean = 0;          // mean |R - truth| on the overlay's own pixels (codes)
    std::uint64_t overlayExact = 0, overlayPixels = 0;   // overlay pixels within half a code of the truth
    double ringMean = 0;             // mean |R - truth| 4-12 px around the overlay (background quality near it)
    double near1 = 0, near2 = 0, near3 = 0;   // mean |R - truth| at distance 1, 2, 3 (the grown edge: review SHOULD 4)
    std::uint64_t wrongPixels = 0;   // pixels anywhere (not the map's own content) with |R - truth| > 8 codes
    std::uint64_t wrongFar = 0;      // the same farther than 3 px from the overlay (damage away from any HUD)
    double fringeMean = 0;           // mean |R - truth| (codes) on the anti-aliased fringe pixels (coverage > 0 beside the core): the soft edge
    std::uint64_t fringePixels = 0;
};
inline Measure measure(const Scene& s, const std::vector<std::uint32_t>& r, const std::vector<std::uint32_t>& g0, const std::vector<std::uint32_t>& truth) {
    const int W = int(s.width()), H = int(s.height());
    std::vector<int> dist(std::size_t(W) * H, 1000);
    bool any = false;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            bool inside = false;
            if (s.coverage(x, y, inside) > 0) { dist[std::size_t(y) * W + x] = 0; any = true; }
        }
    std::vector<int> d2(dist);
    if (any)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int dy = -12; dy <= 12; ++dy)
                    for (int dx = -12; dx <= 12; ++dx) {
                        const int qx = x + dx, qy = y + dy;
                        if (qx < 0 || qy < 0 || qx >= W || qy >= H || dist[std::size_t(qy) * W + qx] != 0) continue;
                        d2[std::size_t(y) * W + x] = std::min(d2[std::size_t(y) * W + x], std::max(std::abs(dx), std::abs(dy)));
                    }
    Measure m;
    double ghost = 0, over = 0, ring = 0, fringe = 0, n[4] = {0, 0, 0, 0};
    std::uint64_t ghostN = 0, ringN = 0, nN[4] = {0, 0, 0, 0};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y) * W + x;
            bool inside = false;
            if (s.coverage(x, y, inside) == 0 && inside) continue;   // the map's own moving content: not judged here
            const double rv = 255.0 * s.value(r[i]), gv = 255.0 * s.value(g0[i]), tv = 255.0 * s.value(truth[i]);
            const double e = std::fabs(rv - tv);
            m.wrongPixels += e > 8.0;
            if (d2[i] > 3) m.wrongFar += e > 8.0;
            if (d2[i] == 0 && s.isFringe(x, y)) { ++m.fringePixels; fringe += e; }
            if (d2[i] == 0) { ++m.overlayPixels; over += e; m.overlayExact += e < 0.5 / (s.hdr() ? kHdrSpan * 1023.0 / 255.0 : 1.0); }
            else if (d2[i] > 3) { ghost += std::fabs(rv - gv); ++ghostN; m.ghostPixels += std::fabs(rv - gv) > 8.0; }
            if (d2[i] >= 1 && d2[i] <= 3) { n[d2[i]] += e; ++nN[d2[i]]; }
            if (d2[i] >= 4 && d2[i] <= 12) { ring += e; ++ringN; }
        }
    m.fringeMean = m.fringePixels ? fringe / double(m.fringePixels) : 0;
    m.ghostMean = ghostN ? ghost / double(ghostN) : 0;
    m.overlayMean = m.overlayPixels ? over / double(m.overlayPixels) : 0;
    m.ringMean = ringN ? ring / double(ringN) : 0;
    m.near1 = nN[1] ? n[1] / double(nN[1]) : 0;
    m.near2 = nN[2] ? n[2] / double(nN[2]) : 0;
    m.near3 = nN[3] ? n[3] / double(nN[3]) : 0;
    return m;
}

enum class Mode { Raw, ComposeOnly, CleanAndCompose };
enum class Generator { Simulated, Perfect };
// Runs `frames` real frames through the guard; for every pair from `from` on, generates the multiplier's frames in the
// given mode and averages the measures over the generated frames. protectedFrames: sum of protected pixels per pair.
struct Result { Measure mean; unsigned generated = 0; std::uint64_t protectedPixels = 0; };
// Every frame of a measured pair, in display order (real frame k - 1 is shown before the generated ones; `sub` 0 = the
// real frame B of pair k, shown after them): kind "real", "generated", "truth" (the truth at the generated frame's time)
// or "plain" (the generator's output on the overlay-free pictures). For tools/hud_guard_eval --write and its downscale.
using FrameSink = std::function<void(int k, unsigned sub, float t, const char* kind, const std::vector<std::uint32_t>& words)>;
inline Result run(const Scene& s, unsigned multiplier, Mode mode, int frames = 16, int from = 8, Generator gen = Generator::Simulated,
                  const FrameSink& sink = {}, veyra::pipeline::PushHudGuardParams rules = {}) {
    veyra::pipeline::PushHudGuardReference guard(s.width(), s.height(), rules);
    const bool tenBit = s.hdr();
    Result res;
    std::vector<std::uint32_t> prev = s.frame(0, true), prevClean = prev, prevPlain = s.frame(0, false);
    guard.update(prev, prev, tenBit, true);
    double sums[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // [6] fringe
    for (int k = 1; k < frames; ++k) {
        const auto b = s.frame(k, true), bPlain = s.frame(k, false);
        const auto counts = guard.update(prev, b, tenBit, false);
        const auto bClean = mode == Mode::CleanAndCompose ? guard.clean(b, tenBit) : b;
        if (k >= from) {
            res.protectedPixels += counts.protect;
            for (unsigned sub = 1; sub < multiplier; ++sub) {
                const float t = float(sub) / float(multiplier);
                auto r = gen == Generator::Perfect ? s.truth(k, t, true) : s.interpolate(mode == Mode::CleanAndCompose ? prevClean : prev, bClean, k, t);
                if (mode != Mode::Raw) guard.compose(r, prev, b, t, tenBit, mode == Mode::CleanAndCompose ? &bClean : nullptr);
                const auto g0 = gen == Generator::Perfect ? s.truth(k, t, false) : s.interpolate(prevPlain, bPlain, k, t);
                const auto truthNow = s.truth(k, t, true);
                const auto m = measure(s, r, g0, truthNow);
                if (sink) { sink(k, sub, t, "generated", r); sink(k, sub, t, "truth", truthNow); sink(k, sub, t, "plain", g0); }
                sums[6] += m.fringeMean; sums[0] += m.ghostMean; sums[1] += m.overlayMean; sums[2] += m.ringMean; sums[3] += m.near1; sums[4] += m.near2; sums[5] += m.near3;
                res.mean.ghostPixels += m.ghostPixels; res.mean.overlayExact += m.overlayExact; res.mean.overlayPixels += m.overlayPixels;
                res.mean.wrongPixels += m.wrongPixels; res.mean.wrongFar += m.wrongFar; res.mean.fringePixels += m.fringePixels;
                ++res.generated;
            }
        }
        if (sink && k >= from) sink(k, 0, 1.0f, "real", b);
        prev = b; prevClean = bClean; prevPlain = bPlain;
    }
    if (res.generated) {
        const double n = double(res.generated);
        res.mean.ghostMean = sums[0] / n; res.mean.overlayMean = sums[1] / n; res.mean.ringMean = sums[2] / n; res.mean.fringeMean = sums[6] / n;
        res.mean.near1 = sums[3] / n; res.mean.near2 = sums[4] / n; res.mean.near3 = sums[5] / n;
        res.mean.ghostPixels /= res.generated; res.mean.wrongPixels /= res.generated; res.mean.wrongFar /= res.generated;
    }
    return res;
}

}  // namespace hud_guard_scene
