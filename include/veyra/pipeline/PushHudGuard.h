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
//   1. change = |luma(B) - luma(A)| and contrast = largest |luma(B) - luma(4-neighbour of B)|, encoded (perceptual)
//      values, both rounded to 1/255 (R8G8_UNORM). Luma (BT.709 weights for SDR, BT.2020 for HDR10 PQ, review SHOULD 3),
//      not every channel: the capture card's 4:2:0 chroma mixes a thin overlay's colour with the moving picture.
//   2. state per pixel (R32_UINT, pushHudGuardPackCell): `run` = real frames in a row still (change <= still);
//      `nearby` = frames of that run with a moving pixel (change > move) within `reach` px along one of 4 axes while at least
//      surroundShare of the 80 x 80 px around changed by more than `still` (a camera turn, not one object passing a still
//      scene; counted above `still`, not `move`, review 2 SHOULD 2: protection then starts over texture of range 0.10
//      too); `thin` =
//      frames with moving pixels on BOTH sides along one axis (an element at most 2 x reach - 1 px thick); `edge` =
//      frames of the run with contrast >= edge (a still STRUCTURE, not a flat patch); `held` = frames in a row a protected
//      pixel changed by more than `still` but at most `hold` (at most holdFrames, then released); `recent` = frames since
//      the pixel was last protected (review SHOULD 1: a crosshair that changes when he aims or fires re-arms after
//      rearmFrames still frames instead of stillFrames, its counts kept).
//   3. armed = run >= K and nearby >= nearFrames, K = rearmFrames while `recent`, else stillFrames;
//      protected = armed and, within `gate` px, a pixel with run >= its K and edge >= edgeFrames (review MUST 1: a flat
//      background pixel crossed by a fast thin object, or sky between moving trunks, has no still structure and motion
//      on one frame only: never protected); cleaned = protected and thin >= 1. Kept as the mask (bit 0 / bit 1).
//   4. pre-clean (generator input only, never shown): in the copy of B the generator reads, a pixel of the cleaned mask
//      grown by `grow` px takes the weighted mean (1 / distance) of the nearest pixel outside it along 8 directions
//      (fillReach px), so the generator never sees the overlay and draws no copies of it.
//   5. compose (each generated frame at time t): a protected pixel, and a pixel within `grow` px of one whose own change
//      is at most `move` (review SHOULD 4: no growth into moving picture, which made a double-image ring), is
//      A + t (B - A) of the real frames: the console's own pixels, in place in every frame; except a pixel of the cleaned
//      area whose real value matches the cleaned copy within backgroundMatch (review 2 SHOULD 1: background next to the
//      overlay, left to the generator).
// An overlay that appears is protected from its stillFrames-th real frame (200 ms at 30 FPS) once something has moved
// around it in nearFrames of them; when nothing moves (a still camera) nothing new is protected, which is harmless.
// Not protected: overlays that change with the picture (translucent below about 0.9 opacity), elements shown for less
// than 200 ms (hit markers), the first 200 ms of new text.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace veyra::pipeline {

struct PushHudGuardParams {
    // Thresholds in SDR code values (0..1); HDR10 (PQ) uses them x pqScale (pushHudGuardModeParams): SDR's 0-203 nits
    // take about 58 % of the PQ code range (review SHOULD 2).
    float still = 0.015f;          // luma change still counted as the same pixel (about 4 SDR codes)
    float hold = 0.04f;            // a protected pixel may change up to this for holdFrames frames in a row
    float move = 0.03f;            // a change above this is motion (evidence for the surround)
    float edge = 0.05f;            // contrast to a 4-neighbour of a still structure
    // Compose (review 2 SHOULD 1): a pixel of the cleaned area whose real value is within this of the cleaned copy is
    // background beside the overlay (the cleaned copy is the background estimate): it keeps the generator's output, so a
    // thin object passing behind the crosshair is not painted over and an anti-aliased edge is left to the generator.
    float backgroundMatch = 0.03f;
    float pqScale = 0.6f;
    // Real frames in a row a pixel must stay still before it can be protected. 6 (200 ms at 30 FPS): on the synthetic
    // pans of tools/hud_guard_eval, 3 frames protected about 25 background pixels a frame whose luma happened to stay
    // put; 6 frames 0-0.03 (OFFLINE, CPU). A crosshair is on screen far longer than that before a turn.
    std::uint32_t stillFrames = 6;
    std::uint32_t rearmFrames = 2;     // the same for a pixel protected within the last recentFrames
    std::uint32_t nearFrames = 3;      // frames of the run with motion around (review MUST 1 a)
    std::uint32_t edgeFrames = 3;      // frames of the run with contrast >= edge (review MUST 1 b)
    std::uint32_t gate = 1;            // px around a protected pixel searched for such a structure (the inside of 3-px lines)
    // `nearby` counts a frame only when the picture around is mostly moving: at least surroundShare of the pixels of the
    // 5 x 5 tiles (80 x 80 px) around the pixel's own moved. A camera turn moves nearly everything around a crosshair; a
    // still camera with one object moving through (a thin bar 6 x 60 px at 20 px per frame moves about 11 % of those
    // pixels) does not, and without this the still scenery next to its path was protected and the object erased where it
    // jumped over it between two real frames (OFFLINE, tools/hud_guard_eval "DAMAGE thin bar").
    float surroundShare = 0.3f;
    // Aim anchor (stream s12-hud, 26 Nov 2026): over a flat picture (sky, a wall, fog) nothing moves around the crosshair, so
    // `nearby` never counts and the overlay was never protected. In the 128 x 128 aim window at the exact centre a still
    // structure is also protected after anchorFrames still frames (1 s at 30 FPS) without any motion evidence, unless a
    // lone object moves within `reach` px that frame (`iso`: moving pixels near, but the picture around does not move:
    // something passing through, which must never be painted over). 0 switches the anchor off (Package 16 + review rules).
    std::uint32_t anchorFrames = 30;
    // Scope (second revision, 26 Nov 2026, after the first run showed the anchor protected nearly a whole TEXTURED still
    // picture): the anchor applies only where the surround is flat, i.e. at most anchorFlatMax of the 17 x 17 px around the
    // pixel are a still structure (a crosshair's own arms are about 120; texture or text is far more), and never in a frame in
    // which anything in the 128 x 128 aim window plus anchorMargin px on each side changed (a lone mover, however far it
    // jumps within that area, drops the anchor for that pair; `iso`, state bit 28).
    std::uint32_t anchorFlatMax = 144;
    std::uint32_t anchorMargin = 64;
    std::uint32_t holdFrames = 2;      // review MUST 1 c: the hold ends on the 3rd changed frame in a row
    std::uint32_t recentFrames = 60;   // about 2 s at 30 FPS (6 bits in the state)
    std::uint32_t reach = 8;           // px searched along each axis for moving pixels
    std::uint32_t grow = 1;            // px the masks are grown by (anti-aliased edges)
    std::uint32_t fillReach = 12;      // px searched for unmasked pixels by the pre-clean
};
inline constexpr std::uint32_t kPushHudGuardMaxRun = 255, kPushHudGuardMaxCount = 15, kPushHudGuardMaxRecent = 63;
inline constexpr std::uint32_t kPushHudGuardTile = 16;   // tiles of moving-pixel counts (>= reach is required)
inline constexpr int kPushHudGuardSurroundTiles = 2;     // tiles each side for the surround share (5 x 5)
inline constexpr std::uint32_t kPushHudGuardAim = 128;   // the aim window: 128 x 128 px at the exact centre (review SHOULD 6)

// The thresholds the passes use for an output mode (0 sRGB, 1 scRGB, 2 HDR10 PQ).
inline PushHudGuardParams pushHudGuardModeParams(PushHudGuardParams p, unsigned outputMode) {
    if (outputMode == 2) { p.still *= p.pqScale; p.hold *= p.pqScale; p.move *= p.pqScale; p.edge *= p.pqScale; p.backgroundMatch *= p.pqScale; }
    return p;
}

// A value as the GPU stores it (R8_UNORM): clamped and rounded to 1/255.
inline float pushHudGuardQuantize(float c) {
    const float v = std::clamp(c, 0.0f, 1.0f);
    return std::floor(v * 255.0f + 0.5f) / 255.0f;
}
inline float pushHudGuardLuma(float r, float g, float b, bool pq) {
    return pq ? 0.2627f * r + 0.6780f * g + 0.0593f * b : 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

struct PushHudGuardColor { float r = 0, g = 0, b = 0, a = 0; };
// R8G8B8A8_UNORM or R10G10B10A2_UNORM words (encoded values: sRGB / PQ codes). In the reference, ten-bit means HDR10 PQ.
inline PushHudGuardColor pushHudGuardUnpack(std::uint32_t w, bool tenBit) {
    if (tenBit) return {float(w & 1023u) / 1023.0f, float((w >> 10) & 1023u) / 1023.0f, float((w >> 20) & 1023u) / 1023.0f, float(w >> 30) / 3.0f};
    return {float(w & 255u) / 255.0f, float((w >> 8) & 255u) / 255.0f, float((w >> 16) & 255u) / 255.0f, float(w >> 24) / 255.0f};
}
inline std::uint32_t pushHudGuardPack(const PushHudGuardColor& c, bool tenBit) {
    const auto q = [](float v, float scale) { return std::uint32_t(std::floor(std::clamp(v, 0.0f, 1.0f) * scale + 0.5f)); };
    if (tenBit) return q(c.r, 1023.0f) | (q(c.g, 1023.0f) << 10) | (q(c.b, 1023.0f) << 20) | (q(c.a, 3.0f) << 30);
    return q(c.r, 255.0f) | (q(c.g, 255.0f) << 8) | (q(c.b, 255.0f) << 16) | (q(c.a, 255.0f) << 24);
}
inline float pushHudGuardWordLuma(std::uint32_t w, bool tenBit) {
    const auto c = pushHudGuardUnpack(w, tenBit);
    return pushHudGuardLuma(c.r, c.g, c.b, tenBit);
}
inline float pushHudGuardChange(std::uint32_t a, std::uint32_t b, bool tenBit) {
    return pushHudGuardQuantize(std::fabs(pushHudGuardWordLuma(b, tenBit) - pushHudGuardWordLuma(a, tenBit)));
}

struct PushHudGuardCell { std::uint32_t run = 0, nearby = 0, thin = 0, edge = 0, held = 0, recent = 0, iso = 0; };
// The GPU's state word (R32_UINT): run 8 bits | nearby 4 | thin 4 | edge 4 | held 2 | recent 6 | iso 1 (bit 28).
inline std::uint32_t pushHudGuardPackCell(const PushHudGuardCell& c) {
    return c.run | (c.nearby << 8) | (c.thin << 12) | (c.edge << 16) | (c.held << 20) | (c.recent << 22) | ((c.iso & 1u) << 28);
}
inline PushHudGuardCell pushHudGuardUnpackCell(std::uint32_t w) {
    return {w & 255u, (w >> 8) & 15u, (w >> 12) & 15u, (w >> 16) & 15u, (w >> 20) & 3u, (w >> 22) & 63u, (w >> 28) & 1u};
}
inline std::uint32_t pushHudGuardArmFrames(const PushHudGuardCell& c, const PushHudGuardParams& p) { return c.recent > 0 ? p.rearmFrames : p.stillFrames; }
inline bool pushHudGuardArmed(const PushHudGuardCell& c, const PushHudGuardParams& p) { return c.run >= pushHudGuardArmFrames(c, p) && c.nearby >= p.nearFrames; }
// The aim anchor: still long enough in the aim window with no lone mover near this frame.
inline bool pushHudGuardAnchored(const PushHudGuardCell& c, const PushHudGuardParams& p, bool inAim) {
    return inAim && p.anchorFrames > 0 && c.run >= p.anchorFrames && c.iso == 0;
}
inline bool pushHudGuardStructure(const PushHudGuardCell& c, const PushHudGuardParams& p) { return c.run >= pushHudGuardArmFrames(c, p) && c.edge >= p.edgeFrames; }

// The surround rule in integers (the GPU compares the same products): moved >= surroundShare x pixels.
inline bool pushHudGuardSurroundMoving(std::uint64_t moved, std::uint64_t pixels, const PushHudGuardParams& p) {
    return pixels > 0 && double(moved) * 1000.0 >= std::floor(double(p.surroundShare) * 1000.0 + 0.5) * double(pixels);
}

// One step of a pixel's state. wasProtected: the pixel's mask bit of the previous frame; scan results and contrast
// from this frame.
inline PushHudGuardCell pushHudGuardStep(PushHudGuardCell c, float change, float contrast, bool nearMoving, bool thinMoving, bool wasProtected,
                                         const PushHudGuardParams& p, bool isolated = false) {
    if (!wasProtected && c.recent > 0) --c.recent;
    if (change <= p.still) {
        c.run = std::min(c.run + 1u, kPushHudGuardMaxRun);
        c.held = 0;
    } else if (wasProtected && change <= p.hold && c.held < p.holdFrames) {
        ++c.held;   // the run goes on
    } else {
        // Released on this frame. A pixel protected until now re-arms quickly (its counts kept, `recent`).
        PushHudGuardCell r;
        r.recent = wasProtected ? std::min(p.recentFrames, kPushHudGuardMaxRecent) : c.recent;
        if (r.recent > 0) { r.nearby = c.nearby; r.thin = c.thin; r.edge = c.edge; }
        return r;
    }
    c.iso = isolated ? 1u : 0u;
    if (nearMoving) c.nearby = std::min(c.nearby + 1u, kPushHudGuardMaxCount);
    if (thinMoving) c.thin = std::min(c.thin + 1u, kPushHudGuardMaxCount);
    // Up and down: a structure has contrast in most frames (a crosshair against the picture behind it); a sky pixel that
    // a passing trunk touches now and then does not (review MUST 1 b, made stricter after the irregular-trunk scene).
    if (contrast >= p.edge) c.edge = std::min(c.edge + 1u, kPushHudGuardMaxCount);
    else if (c.edge > 0) --c.edge;
    return c;
}

// The middle tenth of the picture in each direction, and the 128 x 128 aim window at the exact centre.
inline bool pushHudGuardCentre(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const std::int64_t dx = std::llabs(2 * std::int64_t(x) + 1 - std::int64_t(w)), dy = std::llabs(2 * std::int64_t(y) + 1 - std::int64_t(h));
    return dx * 10 <= std::int64_t(w) && dy * 10 <= std::int64_t(h);
}
inline bool pushHudGuardAim(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const std::int64_t half = kPushHudGuardAim / 2, cx = std::int64_t(w / 2), cy = std::int64_t(h / 2);
    return std::int64_t(x) >= cx - half && std::int64_t(x) < cx + half && std::int64_t(y) >= cy - half && std::int64_t(y) < cy + half;
}

struct PushHudGuardCounts {
    std::uint64_t protect = 0, clean = 0, centreProtect = 0, centreClean = 0, aimProtect = 0, aimClean = 0;   // pixels, before growing
    std::uint32_t aimRunMax = 0;   // the longest still run in the aim window (0: nothing still there)
};

// The four scan axes (both directions of each are searched).
inline constexpr int kPushHudGuardAxes[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
// Eight fill directions of the pre-clean.
inline constexpr int kPushHudGuardFill[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};

// CPU reference of the whole guard (what the GPU passes must give). tenBit = HDR10 PQ words (thresholds x pqScale,
// BT.2020 luma), else SDR RGBA8 words.
class PushHudGuardReference {
public:
    PushHudGuardReference(std::uint32_t w, std::uint32_t h, PushHudGuardParams rules = {})
        : w_(w), h_(h), base_(rules), p_(rules), cells_(std::size_t(w) * h), change_(std::size_t(w) * h, 0.0f), contrast_(std::size_t(w) * h, 0.0f),
          mask_(std::size_t(w) * h, 0) {}

    const PushHudGuardParams& params() const { return p_; }
    std::uint32_t width() const { return w_; }
    std::uint32_t height() const { return h_; }
    const PushHudGuardCell& cell(std::uint32_t x, std::uint32_t y) const { return cells_[std::size_t(y) * w_ + x]; }
    float change(std::uint32_t x, std::uint32_t y) const { return change_[std::size_t(y) * w_ + x]; }
    float contrast(std::uint32_t x, std::uint32_t y) const { return contrast_[std::size_t(y) * w_ + x]; }
    // Mask bits of the last update: 1 protected, 2 cleaned.
    std::uint8_t mask(std::uint32_t x, std::uint32_t y) const { return mask_[std::size_t(y) * w_ + x]; }
    bool isProtected(std::uint32_t x, std::uint32_t y) const { return (mask(x, y) & 1u) != 0; }
    bool isCleaned(std::uint32_t x, std::uint32_t y) const { return (mask(x, y) & 2u) != 0; }

    // B (this real frame) against A (the previous one). reset: no valid A (start, seek, settings change): start over.
    PushHudGuardCounts update(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, bool tenBit, bool reset) {
        p_ = pushHudGuardModeParams(base_, tenBit ? 2u : 0u);
        PushHudGuardCounts counts;
        if (reset) {
            std::fill(cells_.begin(), cells_.end(), PushHudGuardCell{});
            std::fill(change_.begin(), change_.end(), 0.0f);
            std::fill(contrast_.begin(), contrast_.end(), 0.0f);
            std::fill(mask_.begin(), mask_.end(), std::uint8_t(0));
            return counts;
        }
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                const std::size_t i = std::size_t(y) * w_ + x;
                change_[i] = pushHudGuardChange(a[i], b[i], tenBit);
                const float l = pushHudGuardWordLuma(b[i], tenBit);
                float c = 0;
                const int nx[4] = {int(x) + 1, int(x) - 1, int(x), int(x)}, ny[4] = {int(y), int(y), int(y) + 1, int(y) - 1};
                for (int k = 0; k < 4; ++k) {
                    const int qx = std::clamp(nx[k], 0, int(w_) - 1), qy = std::clamp(ny[k], 0, int(h_) - 1);
                    c = std::max(c, std::fabs(l - pushHudGuardWordLuma(b[std::size_t(qy) * w_ + std::size_t(qx)], tenBit)));
                }
                contrast_[i] = pushHudGuardQuantize(c);
            }
        // Changed pixels (> still) per 16 x 16 tile (the GPU's change pass counts the same per thread group); the axis scan
        // and the early-out need moving pixels (> move), which are a subset, so a tile without changed pixels has none.
        const std::uint32_t tx = (w_ + kPushHudGuardTile - 1) / kPushHudGuardTile, ty = (h_ + kPushHudGuardTile - 1) / kPushHudGuardTile;
        std::vector<std::uint32_t> tiles(std::size_t(tx) * ty, 0);
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x)
                if (change_[std::size_t(y) * w_ + x] > p_.still) ++tiles[std::size_t(y / kPushHudGuardTile) * tx + x / kPushHudGuardTile];
        // The aim anchor's busy test: a changed pixel in the tiles that overlap the aim window grown by anchorMargin px.
        bool aimBusy = false;
        {
            const std::int64_t cx = std::int64_t(w_ / 2), cy = std::int64_t(h_ / 2), half = std::int64_t(kPushHudGuardAim / 2) + std::int64_t(p_.anchorMargin);
            const std::int64_t x0 = std::max<std::int64_t>(0, cx - half) / kPushHudGuardTile, x1 = std::min<std::int64_t>(std::int64_t(w_) - 1, cx + half - 1) / kPushHudGuardTile;
            const std::int64_t y0 = std::max<std::int64_t>(0, cy - half) / kPushHudGuardTile, y1 = std::min<std::int64_t>(std::int64_t(h_) - 1, cy + half - 1) / kPushHudGuardTile;
            for (std::int64_t qy = y0; qy <= y1; ++qy)
                for (std::int64_t qx = x0; qx <= x1; ++qx)
                    if (tiles[std::size_t(qy) * tx + std::size_t(qx)] > 0) aimBusy = true;
        }
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                const std::size_t i = std::size_t(y) * w_ + x;
                bool nearMoving = false, thinMoving = false;
                scan(x, y, nearMoving, thinMoving);
                if (nearMoving && !surroundMoving(tiles, tx, ty, x / kPushHudGuardTile, y / kPushHudGuardTile)) nearMoving = thinMoving = false;   // a lone mover
                cells_[i] = pushHudGuardStep(cells_[i], change_[i], contrast_[i], nearMoving, thinMoving, (mask_[i] & 1u) != 0, p_,
                                             aimBusy && pushHudGuardAim(x, y, w_, h_));
            }
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                const auto& c = cells_[std::size_t(y) * w_ + x];
                const bool aim = pushHudGuardAim(x, y, w_, h_);
                if (aim) counts.aimRunMax = std::max(counts.aimRunMax, c.run);
                std::uint8_t m = 0;
                if ((pushHudGuardArmed(c, p_) || (pushHudGuardAnchored(c, p_, aim) && flatSurround(int(x), int(y)))) && structureNear(int(x), int(y))) {
                    m = c.thin >= 1 ? 3 : 1;
                    ++counts.protect;
                    const bool centre = pushHudGuardCentre(x, y, w_, h_);
                    if (centre) ++counts.centreProtect;
                    if (aim) ++counts.aimProtect;
                    if (m & 2u) { ++counts.clean; if (centre) ++counts.centreClean; if (aim) ++counts.aimClean; }
                }
                mask_[std::size_t(y) * w_ + x] = m;
            }
        return counts;
    }
    // The pre-clean mask: any cleaned pixel within `grow` px.
    bool cleanMask(int x, int y) const { return grownAny(x, y, 2u); }
    // The compose mask: protected, or within `grow` px of a protected pixel and not itself moving.
    bool protectMask(int x, int y) const {
        if (x < 0 || y < 0 || x >= int(w_) || y >= int(h_)) return false;
        if (mask_[std::size_t(y) * w_ + std::size_t(x)] & 1u) return true;
        return change_[std::size_t(y) * w_ + std::size_t(x)] <= p_.move && grownAny(x, y, 1u);
    }

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
    // Compose of a generated frame at time t (0 = A, 1 = B): protectMask pixels from the real frames. bClean: this pair's
    // cleaned copy of B (null when it was not cleaned): background beside the overlay keeps the generator's output.
    void compose(std::vector<std::uint32_t>& generated, const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, float t, bool tenBit,
                 const std::vector<std::uint32_t>* bClean = nullptr) const {
        for (std::uint32_t y = 0; y < h_; ++y)
            for (std::uint32_t x = 0; x < w_; ++x) {
                if (!protectMask(int(x), int(y))) continue;
                const std::size_t i = std::size_t(y) * w_ + x;
                if (bClean && cleanMask(int(x), int(y)) &&
                    std::fabs(pushHudGuardWordLuma(b[i], tenBit) - pushHudGuardWordLuma((*bClean)[i], tenBit)) <= p_.backgroundMatch) continue;
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
    // At least surroundShare of the pixels of the 5 x 5 tiles around tile (cx, cy) moved (tiles outside the picture are
    // not counted; edge tiles count their real pixels).
    bool surroundMoving(const std::vector<std::uint32_t>& tiles, std::uint32_t tx, std::uint32_t ty, std::uint32_t cx, std::uint32_t cy) const {
        std::uint64_t moved = 0, pixels = 0;
        for (int dy = -kPushHudGuardSurroundTiles; dy <= kPushHudGuardSurroundTiles; ++dy)
            for (int dx = -kPushHudGuardSurroundTiles; dx <= kPushHudGuardSurroundTiles; ++dx) {
                const int qx = int(cx) + dx, qy = int(cy) + dy;
                if (qx < 0 || qy < 0 || qx >= int(tx) || qy >= int(ty)) continue;
                moved += tiles[std::size_t(qy) * tx + std::size_t(qx)];
                const std::uint32_t pw = std::min(kPushHudGuardTile, w_ - std::uint32_t(qx) * kPushHudGuardTile);
                const std::uint32_t ph = std::min(kPushHudGuardTile, h_ - std::uint32_t(qy) * kPushHudGuardTile);
                pixels += std::uint64_t(pw) * ph;
            }
        return pushHudGuardSurroundMoving(moved, pixels, p_);
    }
    // The aim anchor's flat surround: at most anchorFlatMax pixels of the 17 x 17 around (x, y) are a still structure.
    bool flatSurround(int x, int y) const {
        std::uint32_t n = 0;
        for (int dy = -8; dy <= 8; ++dy)
            for (int dx = -8; dx <= 8; ++dx) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= int(w_) || qy >= int(h_)) continue;
                if (pushHudGuardStructure(cells_[std::size_t(qy) * w_ + std::size_t(qx)], p_) && ++n > p_.anchorFlatMax) return false;
            }
        return true;
    }
    bool structureNear(int x, int y) const {
        const int g = int(p_.gate);
        for (int dy = -g; dy <= g; ++dy)
            for (int dx = -g; dx <= g; ++dx) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= int(w_) || qy >= int(h_)) continue;
                if (pushHudGuardStructure(cells_[std::size_t(qy) * w_ + std::size_t(qx)], p_)) return true;
            }
        return false;
    }
    bool grownAny(int x, int y, std::uint8_t bit) const {
        const int g = int(p_.grow);
        for (int dy = -g; dy <= g; ++dy)
            for (int dx = -g; dx <= g; ++dx) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= int(w_) || qy >= int(h_)) continue;
                if (mask_[std::size_t(qy) * w_ + std::size_t(qx)] & bit) return true;
            }
        return false;
    }

    std::uint32_t w_, h_;
    PushHudGuardParams base_, p_;
    std::vector<PushHudGuardCell> cells_;
    std::vector<float> change_, contrast_;
    std::vector<std::uint8_t> mask_;
};

// The evidence line (one a second; pipeline/PushHudGuardPass.cpp): shares in per mille of the picture.
inline double pushHudGuardPerMille(std::uint64_t pixels, std::uint64_t total) { return total ? 1000.0 * double(pixels) / double(total) : 0.0; }

}  // namespace veyra::pipeline
