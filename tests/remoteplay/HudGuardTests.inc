// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// HUD Guard (include/veyra/pipeline/PushHudGuard.h): the per-pixel rule, the masks, the pre-clean and the compose on
// synthetic scenes (HudGuardScene.h) with a SIMULATED block-motion interpolator (not VFG): what a crosshair, text and a
// minimap get at 2X and 3X, that nothing moving or nothing at all in a still picture is protected, no flicker, release.
#include "HudGuardScene.h"
#include <cstdint>
#include <vector>

namespace hud_guard_tests {
using namespace veyra::pipeline;
using namespace hud_guard_scene;

inline PushHudGuardCell stepN(PushHudGuardCell c, unsigned n, float change, bool nearMoving, bool thin, const PushHudGuardParams& p) {
    for (unsigned i = 0; i < n; ++i) c = pushHudGuardStep(c, change, nearMoving, thin, p);
    return c;
}
}  // namespace hud_guard_tests

TEST(hud_guard_step_counts_still_frames_and_releases_at_once) {
    using namespace hud_guard_tests;
    const PushHudGuardParams p;
    PushHudGuardCell c = stepN({}, p.stillFrames - 1, 0.0f, true, true, p);
    CHECK(!pushHudGuardProtected(c, p));                       // not yet: stillFrames real frames are needed
    c = pushHudGuardStep(c, 0.0f, true, true, p);
    CHECK(pushHudGuardProtected(c, p) && pushHudGuardCleaned(c, p));
    // Hysteresis: a protected pixel stays protected through a change up to `hold` (the capture's noise) ...
    c = pushHudGuardStep(c, 0.035f, false, false, p);
    CHECK(pushHudGuardProtected(c, p));
    // ... and is released on the first frame it changes more.
    c = pushHudGuardStep(c, 0.05f, true, true, p);
    CHECK(!pushHudGuardProtected(c, p) && c.run == 0 && c.nearby == 0 && c.thin == 0);
    // A pixel that is not yet protected gets no hysteresis.
    PushHudGuardCell d = stepN({}, 3, 0.0f, true, false, p);
    d = pushHudGuardStep(d, 0.03f, true, false, p);
    CHECK(d.run == 0);
    // Still for long but nothing ever moved around it (a still camera): never protected (and never needs it).
    const PushHudGuardCell e = stepN({}, 40, 0.0f, false, false, p);
    CHECK(e.run == 40 && !pushHudGuardProtected(e, p));
    // One-sided motion (a still wall beside a moving object): protected (composed from the real frames), not cleaned.
    const PushHudGuardCell f = stepN({}, p.stillFrames, 0.0f, true, false, p);
    CHECK(pushHudGuardProtected(f, p) && !pushHudGuardCleaned(f, p));
    // Counters saturate (the GPU keeps them in 16-bit floats).
    const PushHudGuardCell g = stepN({}, 400, 0.0f, true, true, p);
    CHECK(g.run == kPushHudGuardMaxRun && g.nearby == kPushHudGuardMaxCount && g.thin == kPushHudGuardMaxCount && pushHudGuardPackedEvidence(g) == 255u);
}

TEST(hud_guard_change_is_luma_quantised_like_the_gpu) {
    using namespace veyra::pipeline;
    // Equal luma, different colour (4:2:0 chroma of a thin overlay): no change.
    const std::uint32_t a = pushHudGuardPack({0.5f, 0.5f, 0.5f, 1}, false);
    CHECK(pushHudGuardChange(a, a, false) == 0.0f);
    const std::uint32_t b = pushHudGuardPack({0.5f, 0.6f, 0.5f, 1}, false);
    const float c = pushHudGuardChange(a, b, false);
    CHECK(c > 0.06f && c < 0.08f && c * 255.0f == std::floor(c * 255.0f));   // 0.7152 x 0.1, on the 1/255 grid
    // Ten-bit words unpack to the same scale.
    const std::uint32_t t0 = pushHudGuardPack({0.25f, 0.25f, 0.25f, 1}, true), t1 = pushHudGuardPack({0.75f, 0.75f, 0.75f, 1}, true);
    CHECK(std::fabs(pushHudGuardChange(t0, t1, true) - 0.5f) <= 1.0f / 255.0f);
    CHECK(pushHudGuardCentre(1920, 1080, 3840, 2160) && !pushHudGuardCentre(1500, 1080, 3840, 2160) && !pushHudGuardCentre(1920, 800, 3840, 2160));
}

TEST(hud_guard_removes_crosshair_copies_at_2x_and_3x) {
    using namespace hud_guard_tests;
    const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {true, 20});
    for (unsigned m : {2u, 3u}) {
        const auto raw = run(s, m, Mode::Raw, 12, 8), compose = run(s, m, Mode::ComposeOnly, 12, 8), both = run(s, m, Mode::CleanAndCompose, 12, 8);
        // Raw: the crosshair is (almost) never where it belongs in a generated frame, and copies lie around it.
        CHECK(raw.mean.overlayExact * 2 < raw.mean.overlayPixels && raw.mean.ghostPixels >= 40);
        // Compose alone (what Live HUD Lock does at best): the crosshair is back, bit for bit, but the copies stay.
        CHECK(compose.mean.overlayExact == compose.mean.overlayPixels);
        CHECK(compose.mean.ghostPixels * 10 >= raw.mean.ghostPixels * 9);
        // Pre-clean + compose: the crosshair exact and most copies gone (the rest: its 1-px grown edge's background).
        CHECK(both.mean.overlayExact == both.mean.overlayPixels);
        CHECK(both.mean.ghostPixels * 3 <= raw.mean.ghostPixels);
        CHECK(both.mean.ringMean < raw.mean.ringMean);
    }
}

TEST(hud_guard_text_and_minimap_ring_but_not_the_map) {
    using namespace hud_guard_tests;
    const Scene text(160, 112, {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24});
    const auto rawT = run(text, 3, Mode::Raw, 12, 8), bothT = run(text, 3, Mode::CleanAndCompose, 12, 8);
    CHECK(bothT.mean.overlayExact == bothT.mean.overlayPixels && bothT.mean.ghostPixels * 3 <= rawT.mean.ghostPixels);
    // The minimap: its ring is protected; the map inside scrolls by itself and is never protected or cleaned.
    const Scene map(160, 112, {Overlay::Kind::Minimap, 2, 14, 1.0f}, {false, 24});
    PushHudGuardReference g(160, 112);
    auto prev = map.frame(0, true);
    g.update(prev, prev, false, true);
    unsigned ringProtected = 0, ringPixels = 0, mapProtected = 0;
    for (int k = 1; k < 12; ++k) {
        const auto b = map.frame(k, true);
        g.update(prev, b, false, false);
        prev = b;
    }
    for (int y = 0; y < 112; ++y)
        for (int x = 0; x < 160; ++x) {
            bool inside = false;
            const bool ring = map.coverage(x, y, inside) > 0;
            if (ring) { ++ringPixels; ringProtected += pushHudGuardProtected(g.cell(std::uint32_t(x), std::uint32_t(y)), g.params()); }
            // More than one pixel inside the ring (the grown mask may touch the map's first pixel row).
            const float dx = float(x - 80) + 0.5f, dy = float(y - 56) + 0.5f;
            if (inside && dx * dx + dy * dy < 28.0f * 28.0f) mapProtected += pushHudGuardProtected(g.cell(std::uint32_t(x), std::uint32_t(y)), g.params());
        }
    CHECK(ringPixels > 0 && ringProtected == ringPixels);
    CHECK(mapProtected == 0);
}

TEST(hud_guard_protects_nothing_that_moves_and_nothing_new_in_a_still_picture) {
    using namespace hud_guard_tests;
    // The world alone, panned in a circle: every pixel moves; nothing is protected over 30 frames.
    {
        const Scene s(160, 112, {Overlay::Kind::None}, {true, 20});
        PushHudGuardReference g(160, 112);
        auto prev = s.frame(0, false);
        g.update(prev, prev, false, true);
        std::uint64_t protectedFrames = 0;
        for (int k = 1; k < 30; ++k) {
            const auto b = s.frame(k, false);
            protectedFrames += g.update(prev, b, false, false).protect;
            prev = b;
        }
        CHECK(protectedFrames == 0);
    }
    // A still picture (camera at rest) with the crosshair: nothing moves, nothing is protected (generated == real anyway).
    {
        const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 0});
        PushHudGuardReference g(160, 112);
        const auto f = s.frame(0, true);
        g.update(f, f, false, true);
        std::uint64_t protectedFrames = 0;
        for (int k = 1; k < 20; ++k) protectedFrames += g.update(f, f, false, false).protect;
        CHECK(protectedFrames == 0);
    }
}

TEST(hud_guard_mask_does_not_flicker_and_resets) {
    using namespace hud_guard_tests;
    const Scene s(160, 112, {Overlay::Kind::Crosshair, 1, 14, 1.0f}, {true, 20});
    PushHudGuardReference g(160, 112);
    auto prev = s.frame(0, true);
    g.update(prev, prev, false, true);
    std::vector<std::uint8_t> seen(160 * 112, 0);
    unsigned frames = 0, changedAfterOnset = 0;
    for (int k = 1; k < 30; ++k) {
        const auto b = s.frame(k, true);
        const auto counts = g.update(prev, b, false, false);
        prev = b;
        if (k < int(g.params().stillFrames)) { CHECK(counts.protect == 0); continue; }   // past frames only: not before stillFrames
        CHECK(counts.centreProtect > 0 && counts.centreClean > 0);
        ++frames;
        for (std::uint32_t y = 0; y < 112; ++y)
            for (std::uint32_t x = 0; x < 160; ++x) {
                bool inside = false;
                if (s.coverage(int(x), int(y), inside) <= 0) continue;
                const std::uint8_t now = pushHudGuardProtected(g.cell(x, y), g.params()) ? 1 : 2;
                if (seen[y * 160 + x] && seen[y * 160 + x] != now) ++changedAfterOnset;
                seen[y * 160 + x] = now;
            }
    }
    CHECK(frames > 20 && changedAfterOnset == 0);   // every crosshair pixel protected on every frame after the onset
    // The crosshair disappears (a menu opens, the picture cuts): released on that frame.
    const auto plain = s.frame(30, false);
    CHECK(g.update(prev, plain, false, false).centreProtect == 0);
    // A reset starts over.
    const auto with = s.frame(31, true);
    CHECK(g.update(with, with, false, true).protect == 0 && !g.protectMask(80, 56));
}

TEST(hud_guard_translucent_overlay_limit_is_as_documented) {
    using namespace hud_guard_tests;
    // A crosshair at 60 % opacity over a moving picture changes by 0.4 x the background's change: not still, so not
    // protected (HUD_GHOSTING.md: the known limit; the raw generator's picture stays as it is there, nothing gets worse).
    const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 0.6f}, {true, 20});
    const auto raw = run(s, 3, Mode::Raw, 12, 8), both = run(s, 3, Mode::CleanAndCompose, 12, 8);
    CHECK(both.mean.ghostPixels == raw.mean.ghostPixels && both.mean.overlayExact == raw.mean.overlayExact);
}

TEST(hud_guard_clean_leaves_unmasked_pixels_and_fills_from_neighbours) {
    using namespace veyra::pipeline;
    // 24 x 9: a 1-px still line in the middle row over a picture that moves on both sides.
    constexpr std::uint32_t W = 24, H = 9;
    PushHudGuardReference g(W, H);
    auto pic = [&](int k) {
        std::vector<std::uint32_t> f(W * H);
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const float v = y == 4 ? 1.0f : ((x + y + std::uint32_t(k)) % 2 ? 0.2f : 0.6f);
                f[y * W + x] = pushHudGuardPack({v, v, v, 1}, false);
            }
        return f;
    };
    auto prev = pic(0);
    g.update(prev, prev, false, true);
    for (int k = 1; k <= 8; ++k) { const auto b = pic(k); g.update(prev, b, false, false); prev = b; }
    CHECK(g.cleanMask(10, 4) && g.cleanMask(10, 3) && !g.cleanMask(10, 1));   // the line, grown by one px
    const auto cleaned = g.clean(prev, false);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            if (!g.cleanMask(int(x), int(y))) CHECK(cleaned[y * W + x] == prev[y * W + x]);
            else if (y == 4) { const float v = hud_guard_scene::Scene::value(cleaned[y * W + x]); CHECK(v >= 0.19f && v <= 0.61f); }   // the moving picture around it, no white left
        }
    // Compose at t: protected pixels from A + t (B - A); the line (A == B) exact.
    std::vector<std::uint32_t> gen(W * H, pushHudGuardPack({0, 0, 0, 1}, false));
    g.compose(gen, prev, prev, 2.0f / 3.0f, false);
    CHECK(gen[4 * W + 10] == prev[4 * W + 10] && gen[0] == pushHudGuardPack({0, 0, 0, 1}, false));
}
