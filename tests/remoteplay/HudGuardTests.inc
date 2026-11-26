// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// HUD Guard (include/veyra/pipeline/PushHudGuard.h): the per-pixel rule, the masks, the pre-clean and the compose on
// synthetic scenes (HudGuardScene.h) with a SIMULATED block-motion interpolator (not VFG) and a PERFECT one (the truth):
// what a crosshair, text and a minimap get at 2X and 3X; that nothing moving, nothing in a still picture, no flat sky
// between passing trunks and no low-contrast texture is protected; that a fast thin object is never erased (review
// MUST 1); no flicker, release, re-arm after aiming (SHOULD 1), HDR10 thresholds and luma (SHOULD 2, 3).
#include "HudGuardScene.h"
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hud_guard_tests {
using namespace veyra::pipeline;
using namespace hud_guard_scene;

inline PushHudGuardCell stepN(PushHudGuardCell c, unsigned n, float change, float contrast, bool nearMoving, bool thin, bool wasProtected,
                              const PushHudGuardParams& p) {
    for (unsigned i = 0; i < n; ++i) c = pushHudGuardStep(c, change, contrast, nearMoving, thin, wasProtected, p);
    return c;
}
inline const Overlay kCross2{Overlay::Kind::Crosshair, 2, 14, 1.0f};
inline const Overlay kNone{Overlay::Kind::None};
}  // namespace hud_guard_tests

TEST(hud_guard_step_arms_holds_two_frames_releases_and_rearms) {
    using namespace hud_guard_tests;
    const PushHudGuardParams p;
    // Still with motion around and contrast: armed after stillFrames frames (nearby >= nearFrames, edge >= edgeFrames).
    PushHudGuardCell c = stepN({}, p.stillFrames - 1, 0.0f, 0.2f, true, true, false, p);
    CHECK(!pushHudGuardArmed(c, p));
    c = pushHudGuardStep(c, 0.0f, 0.2f, true, true, false, p);
    CHECK(pushHudGuardArmed(c, p) && pushHudGuardStructure(c, p) && c.thin >= 1);
    // Motion around in fewer than nearFrames frames of the run (an object that jumps over once): never armed.
    PushHudGuardCell once = stepN({}, p.stillFrames - 1, 0.0f, 0.2f, false, false, false, p);
    once = pushHudGuardStep(once, 0.0f, 0.2f, true, true, false, p);
    once = stepN(once, 10, 0.0f, 0.2f, false, false, false, p);
    CHECK(once.run > p.stillFrames && once.nearby == 1 && !pushHudGuardArmed(once, p));
    // The hold: a protected pixel survives up to holdFrames changed frames in a row ...
    c = pushHudGuardStep(c, 0.035f, 0.2f, false, false, true, p);
    c = pushHudGuardStep(c, 0.035f, 0.2f, false, false, true, p);
    CHECK(c.held == 2 && pushHudGuardArmed(c, p));
    // ... and is released on the third (review MUST 1 c), keeping its counts as a recent one.
    PushHudGuardCell r = pushHudGuardStep(c, 0.035f, 0.2f, false, false, true, p);
    CHECK(r.run == 0 && r.recent == p.recentFrames && r.nearby == c.nearby && r.thin == c.thin && r.edge == c.edge);
    // A still frame resets the hold count.
    PushHudGuardCell h = pushHudGuardStep(c, 0.0f, 0.2f, false, false, true, p);
    CHECK(h.held == 0);
    // Any change above `hold` releases at once.
    PushHudGuardCell big = pushHudGuardStep(c, 0.05f, 0.2f, true, true, true, p);
    CHECK(big.run == 0 && big.recent > 0);
    // Re-arm (review SHOULD 1): a recently protected pixel is armed again after rearmFrames still frames.
    PushHudGuardCell re = stepN(r, p.rearmFrames - 1, 0.0f, 0.2f, true, true, false, p);
    CHECK(!pushHudGuardArmed(re, p));
    re = pushHudGuardStep(re, 0.0f, 0.2f, true, true, false, p);
    CHECK(pushHudGuardArmed(re, p) && re.recent == p.recentFrames - p.rearmFrames);
    // A pixel never protected gets no hold and no recent counts.
    PushHudGuardCell d = stepN({}, 3, 0.0f, 0.2f, true, false, false, p);
    d = pushHudGuardStep(d, 0.03f, 0.2f, true, false, false, p);
    CHECK(d.run == 0 && d.recent == 0 && d.nearby == 0);
    // The structure counter goes up with contrast and down without (a sky pixel that a trunk touches now and then).
    PushHudGuardCell e = stepN({}, 2, 0.0f, 0.2f, true, false, false, p);
    e = stepN(e, 4, 0.0f, 0.0f, true, false, false, p);
    CHECK(e.edge == 0 && !pushHudGuardStructure(e, p));
    // Still for long, nothing ever moved around it (a still camera): never armed (and never needs it).
    const PushHudGuardCell q = stepN({}, 40, 0.0f, 0.2f, false, false, false, p);
    CHECK(q.run == 40 && !pushHudGuardArmed(q, p));
    // Counters saturate and the state word round-trips.
    const PushHudGuardCell g = stepN({}, 400, 0.0f, 0.2f, true, true, false, p);
    CHECK(g.run == kPushHudGuardMaxRun && g.nearby == kPushHudGuardMaxCount && g.thin == kPushHudGuardMaxCount && g.edge == kPushHudGuardMaxCount);
    PushHudGuardCell all{255, 15, 15, 15, 3, 63};
    const auto back = pushHudGuardUnpackCell(pushHudGuardPackCell(all));
    CHECK(back.run == 255 && back.nearby == 15 && back.thin == 15 && back.edge == 15 && back.held == 3 && back.recent == 63);
    const auto mid = pushHudGuardUnpackCell(pushHudGuardPackCell({6, 3, 1, 2, 1, 60}));
    CHECK(mid.run == 6 && mid.nearby == 3 && mid.thin == 1 && mid.edge == 2 && mid.held == 1 && mid.recent == 60);
}

TEST(hud_guard_change_luma_and_hdr10_thresholds) {
    using namespace veyra::pipeline;
    // Equal luma, different colour (4:2:0 chroma of a thin overlay): no change.
    const std::uint32_t a = pushHudGuardPack({0.5f, 0.5f, 0.5f, 1}, false);
    CHECK(pushHudGuardChange(a, a, false) == 0.0f);
    const std::uint32_t b = pushHudGuardPack({0.5f, 0.6f, 0.5f, 1}, false);
    const float c = pushHudGuardChange(a, b, false);
    CHECK(c > 0.06f && c < 0.08f && c * 255.0f == std::floor(c * 255.0f));   // 0.7152 x 0.1, on the 1/255 grid
    // HDR10: BT.2020 weights (review SHOULD 3): a green step of 0.1 is 0.0678.
    const std::uint32_t t0 = pushHudGuardPack({0.5f, 0.5f, 0.5f, 1}, true), t1 = pushHudGuardPack({0.5f, 0.6f, 0.5f, 1}, true);
    CHECK(std::fabs(pushHudGuardChange(t0, t1, true) - 0.0678f) <= 1.5f / 255.0f);
    // HDR10 thresholds are 0.6 x SDR's (review SHOULD 2); SDR and scRGB unchanged.
    const PushHudGuardParams p;
    const auto pq = pushHudGuardModeParams(p, 2), sdr = pushHudGuardModeParams(p, 0), sc = pushHudGuardModeParams(p, 1);
    CHECK(std::fabs(pq.still - 0.009f) < 1e-6f && std::fabs(pq.move - 0.018f) < 1e-6f && std::fabs(pq.hold - 0.024f) < 1e-6f && std::fabs(pq.edge - 0.03f) < 1e-6f);
    CHECK(sdr.still == p.still && sc.move == p.move);
    // The middle tenth and the 128 x 128 aim window.
    CHECK(pushHudGuardCentre(1920, 1080, 3840, 2160) && !pushHudGuardCentre(1500, 1080, 3840, 2160) && !pushHudGuardCentre(1920, 800, 3840, 2160));
    CHECK(pushHudGuardAim(1920, 1080, 3840, 2160) && pushHudGuardAim(1856, 1016, 3840, 2160) && !pushHudGuardAim(1855, 1080, 3840, 2160) &&
          !pushHudGuardAim(1920, 1144, 3840, 2160));
}

TEST(hud_guard_removes_crosshair_copies_at_2x_and_3x_sdr_and_hdr10) {
    using namespace hud_guard_tests;
    for (bool hdr : {false, true}) {
        const Scene s(160, 112, kCross2, {true, 20}, {}, {}, hdr);
        for (unsigned m : {2u, 3u}) {
            const auto raw = run(s, m, Mode::Raw, 14, 8), compose = run(s, m, Mode::ComposeOnly, 14, 8), both = run(s, m, Mode::CleanAndCompose, 14, 8);
            // Raw: the crosshair is (almost) never where it belongs in a generated frame, and copies lie around it.
            CHECK(raw.mean.overlayExact * 2 < raw.mean.overlayPixels && raw.mean.ghostPixels >= 40);
            // Compose alone: the crosshair is back, but the copies stay (why no compose-only mode is offered).
            CHECK(compose.mean.overlayExact == compose.mean.overlayPixels && compose.mean.ghostPixels * 10 >= raw.mean.ghostPixels * 9);
            // Pre-clean + compose: the crosshair exact, most copies gone, no worse anywhere, its edge better than raw.
            CHECK(both.mean.overlayExact == both.mean.overlayPixels);
            CHECK(both.mean.ghostPixels * 3 <= raw.mean.ghostPixels);
            CHECK(both.mean.wrongPixels <= raw.mean.wrongPixels && both.mean.ringMean < raw.mean.ringMean);
            CHECK(both.mean.near1 < raw.mean.near1 && both.mean.near2 < raw.mean.near2 && both.mean.near3 < raw.mean.near3);   // review SHOULD 4
        }
    }
}

TEST(hud_guard_text_and_minimap_ring_but_not_the_map) {
    using namespace hud_guard_tests;
    const Scene text(160, 112, {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24});
    const auto rawT = run(text, 3, Mode::Raw, 14, 8), bothT = run(text, 3, Mode::CleanAndCompose, 14, 8);
    CHECK(bothT.mean.overlayExact == bothT.mean.overlayPixels && bothT.mean.ghostPixels * 3 <= rawT.mean.ghostPixels);
    CHECK(bothT.mean.wrongPixels <= rawT.mean.wrongPixels);
    // The minimap: its ring is protected; the map inside scrolls by itself and is never protected or cleaned.
    const Scene map(160, 112, {Overlay::Kind::Minimap, 2, 14, 1.0f}, {false, 24});
    PushHudGuardReference g(160, 112);
    auto prev = map.frame(0, true);
    g.update(prev, prev, false, true);
    for (int k = 1; k < 12; ++k) {
        const auto b = map.frame(k, true);
        g.update(prev, b, false, false);
        prev = b;
    }
    unsigned ringProtected = 0, ringPixels = 0, mapProtected = 0;
    for (int y = 0; y < 112; ++y)
        for (int x = 0; x < 160; ++x) {
            bool inside = false;
            const bool ring = map.coverage(x, y, inside) > 0;
            if (ring) { ++ringPixels; ringProtected += g.isProtected(std::uint32_t(x), std::uint32_t(y)); }
            const float dx = float(x - 80) + 0.5f, dy = float(y - 56) + 0.5f;
            if (inside && dx * dx + dy * dy < 28.0f * 28.0f) mapProtected += g.isProtected(std::uint32_t(x), std::uint32_t(y));
        }
    CHECK(ringPixels > 0 && ringProtected == ringPixels);
    CHECK(mapProtected == 0);
}

TEST(hud_guard_never_erases_a_fast_thin_object_or_protects_sky_and_low_contrast) {
    using namespace hud_guard_tests;
    // Review MUST 1, PERFECT generator (raw = the truth): a thin bar crossing a still picture faster than its own width
    // per frame, with and without a crosshair, on texture and on low-contrast texture: nothing wrong at all.
    for (float speed : {12.0f, 20.0f})
        for (const auto& world : {World{}, World{World::Kind::LowContrast, 0.05f}})
            for (bool cross : {false, true}) {
                const Scene s(160, 112, cross ? kCross2 : kNone, {false, 0}, world, {true, speed});
                const auto both = run(s, 3, Mode::CleanAndCompose, 16, 8, Generator::Perfect);
                CHECK(both.mean.wrongPixels == 0 && both.protectedPixels == 0);   // one object in a still picture: no camera turn
            }
    // Sky with thin trunks in a pan, no HUD: nothing protected, nothing wrong.
    {
        const Scene s(160, 112, kNone, {false, 24}, {World::Kind::SkyTrunks});
        const auto both = run(s, 3, Mode::CleanAndCompose, 16, 8, Generator::Perfect);
        CHECK(both.protectedPixels == 0 && both.mean.wrongPixels == 0);
    }
    // The same with a crosshair: no damage farther than 3 px from it; next to it the flat sky that touches the crosshair is
    // protected too, so a trunk crossing exactly there between two real frames is missing in that 1-3 px band (a known,
    // measured limit: about 45-60 px per generated frame at 256 x 160, tools/hud_guard_eval).
    {
        const Scene s(160, 112, kCross2, {false, 24}, {World::Kind::SkyTrunks});
        for (unsigned m : {2u, 3u}) {
            const auto both = run(s, m, Mode::CleanAndCompose, 16, 8, Generator::Perfect);
            CHECK(both.mean.wrongFar == 0 && both.mean.wrongPixels <= 80 && both.mean.overlayExact == both.mean.overlayPixels);
        }
    }
    // Low-contrast texture panned, SDR and HDR10 (review MUST 1 c, SHOULD 2): nothing protected.
    for (bool hdr : {false, true}) {
        const Scene s(160, 112, kNone, {false, 24}, {World::Kind::LowContrast, 0.05f}, {}, hdr);
        const auto both = run(s, 3, Mode::CleanAndCompose, 16, 8, Generator::Simulated);
        const auto raw = run(s, 3, Mode::Raw, 16, 8, Generator::Simulated);
        CHECK(both.protectedPixels == 0 && both.mean.wrongPixels == raw.mean.wrongPixels);
    }
}

TEST(hud_guard_protects_nothing_that_moves_and_nothing_new_in_a_still_picture) {
    using namespace hud_guard_tests;
    {
        const Scene s(160, 112, kNone, {true, 20});
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
    {
        const Scene s(160, 112, kCross2, {false, 0});
        PushHudGuardReference g(160, 112);
        const auto f = s.frame(0, true);
        g.update(f, f, false, true);
        std::uint64_t protectedFrames = 0;
        for (int k = 1; k < 20; ++k) protectedFrames += g.update(f, f, false, false).protect;
        CHECK(protectedFrames == 0);
    }
}

TEST(hud_guard_mask_does_not_flicker_releases_rearms_and_resets) {
    using namespace hud_guard_tests;
    // The crosshair changes its value at frame 20 (he aims at a target): released at once, protected again two frames
    // later (review SHOULD 1), not after six.
    const Scene s(160, 112, {Overlay::Kind::Crosshair, 1, 14, 1.0f, 0.96f, 20, 0.55f}, {true, 20});
    PushHudGuardReference g(160, 112);
    auto prev = s.frame(0, true);
    g.update(prev, prev, false, true);
    std::vector<std::uint8_t> seen(160 * 112, 0);
    unsigned frames = 0, changedAfterOnset = 0;
    std::uint64_t aimAt[24]{};
    for (int k = 1; k < 24; ++k) {
        const auto b = s.frame(k, true);
        const auto counts = g.update(prev, b, false, false);
        prev = b;
        aimAt[k] = counts.aimProtect;
        if (k < int(g.params().stillFrames)) { CHECK(counts.protect == 0); continue; }   // past frames only: not before stillFrames
        if (k >= 20) continue;
        CHECK(counts.centreProtect > 0 && counts.centreClean > 0 && counts.aimProtect > 0 && counts.aimRunMax >= g.params().stillFrames);
        ++frames;
        for (std::uint32_t y = 0; y < 112; ++y)
            for (std::uint32_t x = 0; x < 160; ++x) {
                bool inside = false;
                if (s.coverage(int(x), int(y), inside) <= 0) continue;
                const std::uint8_t now = g.isProtected(x, y) ? 1 : 2;
                if (seen[y * 160 + x] && seen[y * 160 + x] != now) ++changedAfterOnset;
                seen[y * 160 + x] = now;
            }
    }
    CHECK(frames >= 12 && changedAfterOnset == 0);   // every crosshair pixel protected on every frame after the onset
    CHECK(aimAt[20] == 0 && aimAt[21] == 0 && aimAt[22] > 0 && aimAt[23] > 0);
    // The crosshair disappears (a menu opens, the picture cuts): its pixels are released (a background pixel there that
    // happens to keep its value may stay or re-arm: it is still, so that is harmless).
    // The hold keeps a pixel whose new value is within `hold` of the old one for at most holdFrames frames.
    for (int k = 24; k < 24 + int(g.params().holdFrames) + 1; ++k) {
        const auto plain = s.frame(k, false);
        g.update(prev, plain, false, false);
        prev = plain;
    }
    unsigned crossPixels = 0, stillProtected = 0;
    for (std::uint32_t y = 0; y < 112; ++y)
        for (std::uint32_t x = 0; x < 160; ++x) {
            bool inside = false;
            if (s.coverage(int(x), int(y), inside) <= 0) continue;
            ++crossPixels;
            stillProtected += g.isProtected(x, y);
        }
    CHECK(crossPixels > 40 && stillProtected * 20 <= crossPixels);
    // A reset starts over.
    const auto with = s.frame(30, true);
    CHECK(g.update(with, with, false, true).protect == 0 && !g.protectMask(80, 56));
}

TEST(hud_guard_translucent_overlay_limit_is_as_documented) {
    using namespace hud_guard_tests;
    // A crosshair at 60 % opacity over a moving picture changes by 0.4 x the background's change: not still, so not
    // protected (HUD_GHOSTING.md: the known limit; the raw generator's picture stays as it is there, nothing gets worse).
    const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 0.6f}, {true, 20});
    const auto raw = run(s, 3, Mode::Raw, 14, 8), both = run(s, 3, Mode::CleanAndCompose, 14, 8);
    CHECK(both.mean.ghostPixels == raw.mean.ghostPixels && both.mean.overlayExact == raw.mean.overlayExact && both.mean.wrongPixels == raw.mean.wrongPixels);
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
    CHECK(g.isProtected(10, 4) && g.isCleaned(10, 4) && g.cleanMask(10, 4) && g.cleanMask(10, 3) && !g.cleanMask(10, 1));   // the line, grown by one px
    // The compose mask does not grow into the moving rows beside the line (review SHOULD 4).
    CHECK(g.protectMask(10, 4) && !g.protectMask(10, 3) && !g.protectMask(10, 5));
    const auto cleaned = g.clean(prev, false);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            if (!g.cleanMask(int(x), int(y))) CHECK(cleaned[y * W + x] == prev[y * W + x]);
            else if (y == 4) { const float v = float(cleaned[y * W + x] & 255u) / 255.0f; CHECK(v >= 0.19f && v <= 0.61f); }   // no white left
        }
    // Compose at t: protected pixels from A + t (B - A); the line (A == B) exact.
    std::vector<std::uint32_t> gen(W * H, pushHudGuardPack({0, 0, 0, 1}, false));
    g.compose(gen, prev, prev, 2.0f / 3.0f, false);
    CHECK(gen[4 * W + 10] == prev[4 * W + 10] && gen[0] == pushHudGuardPack({0, 0, 0, 1}, false));
    // Review 2 SHOULD 1: a protected pixel whose value in B is within backgroundMatch of the cleaned copy shows the background
    // there (the overlay is not drawn on it): it keeps the generator's output; one that differs is put back.
    auto bg = prev;
    bg[4 * W + 10] = cleaned[4 * W + 10];   // as if the overlay left this pixel uncovered
    std::vector<std::uint32_t> gen2(W * H, pushHudGuardPack({0.3f, 0.3f, 0.3f, 1}, false));
    g.compose(gen2, bg, bg, 0.5f, false, &cleaned);
    CHECK(gen2[4 * W + 10] == pushHudGuardPack({0.3f, 0.3f, 0.3f, 1}, false));
    CHECK(gen2[4 * W + 11] == prev[4 * W + 11]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Stream "s12-hud", 26 Nov 2026: the aim anchor (protection over flat pictures), moving objects that must never be
// erased, subtitles, a crosshair that changes, HDR10 highlights, dark pictures, a fast camera and soft edges. Every number
// printed here is SIMULATED (CPU reference, synthetic pictures); the generator is named per line ("simulated" = the
// block-motion model of HudGuardScene.h, "perfect" = the truth, so every wrong pixel is the guard's own).
// ---------------------------------------------------------------------------------------------------------------------
namespace hud_guard_tests {
inline void printLine(const char* label, const char* gen, unsigned multiplier, const hud_guard_scene::Result& raw, const hud_guard_scene::Result& guard) {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("[hud-guard-sim] SIMULATED %-50s %s %uX | raw: exact %llu/%llu ghost %llu wrong %llu wrongFar %llu fringe %.1f ring %.2f | guard: exact %llu/%llu ghost %llu "
                "wrong %llu wrongFar %llu fringe %.1f ring %.2f protectedPx %llu\n",
                label, gen, multiplier, u(raw.mean.overlayExact), u(raw.mean.overlayPixels), u(raw.mean.ghostPixels), u(raw.mean.wrongPixels), u(raw.mean.wrongFar),
                raw.mean.fringeMean, raw.mean.ringMean, u(guard.mean.overlayExact), u(guard.mean.overlayPixels), u(guard.mean.ghostPixels), u(guard.mean.wrongPixels),
                u(guard.mean.wrongFar), guard.mean.fringeMean, guard.mean.ringMean, u(guard.protectedPixels));
}
// Protected pixels per real frame k (counts.protect), the frames fed through the guard like run() does.
inline std::vector<std::uint64_t> protectedPerFrame(const Scene& s, int frames, PushHudGuardParams rules = {}) {
    PushHudGuardReference g(s.width(), s.height(), rules);
    std::vector<std::uint64_t> out(std::size_t(frames), 0);
    auto prev = s.frame(0, true);
    g.update(prev, prev, s.hdr(), true);
    for (int k = 1; k < frames; ++k) {
        const auto b = s.frame(k, true);
        out[std::size_t(k)] = g.update(prev, b, s.hdr(), false).protect;
        prev = b;
    }
    return out;
}
}  // namespace hud_guard_tests

TEST(hud_guard_aim_anchor_protects_the_crosshair_over_a_flat_picture) {
    using namespace hud_guard_tests;
    for (bool hdr : {false, true})
        for (unsigned m : {2u, 3u}) {
            const Scene s(160, 112, kCross2, {false, 20}, {World::Kind::Sky}, {}, hdr);
            PushHudGuardParams off;
            off.anchorFrames = 0;   // Package 16 + review rules: protection only while the picture around moves
            const auto raw = run(s, m, Mode::Raw, 48, 36), before = run(s, m, Mode::CleanAndCompose, 48, 36, Generator::Simulated, {}, off);
            const auto after = run(s, m, Mode::CleanAndCompose, 48, 36);
            printLine(hdr ? "flat sky + crosshair, pan 20, HDR10" : "flat sky + crosshair, pan 20, SDR", "simulated", m, before, after);
            // Before: nothing protected (nothing moves around it), the crosshair is where the generator carries it.
            CHECK(before.protectedPixels == 0 && before.mean.overlayExact == raw.mean.overlayExact && before.mean.overlayExact < before.mean.overlayPixels);
            // After: protected from the 30th still frame on, the crosshair exact in every generated frame, nothing worse.
            CHECK(after.protectedPixels > 0 && after.mean.overlayExact == after.mean.overlayPixels);
            CHECK(after.mean.wrongPixels <= raw.mean.wrongPixels && after.mean.wrongFar <= raw.mean.wrongFar);
        }
    // Not before it has been still for anchorFrames real frames (past frames only).
    const Scene s(160, 112, kCross2, {false, 20}, {World::Kind::Sky});
    const auto per = protectedPerFrame(s, 40);
    for (int k = 1; k < 30; ++k) CHECK(per[std::size_t(k)] == 0);
    CHECK(per[30] > 0 && per[39] > 0);
    // Outside the aim window nothing is anchored (the window is the 128 x 128 px at the exact centre). A 320 x 200
    // flat picture with a crosshair: protected inside the window only. (Text over flat sky is NOT anchored: 17 x 17 px
    // around its strokes are mostly structure; see the printed number below.)
    {
        const Scene big(320, 200, kCross2, {false, 20}, {World::Kind::Sky});
        PushHudGuardReference g(320, 200);
        auto prev = big.frame(0, true);
        g.update(prev, prev, false, true);
        for (int k = 1; k < 36; ++k) {
            const auto b = big.frame(k, true);
            g.update(prev, b, false, false);
            prev = b;
        }
        std::uint64_t inside = 0, outside = 0;
        for (std::uint32_t y = 0; y < 200; ++y)
            for (std::uint32_t x = 0; x < 320; ++x)
                if (g.isProtected(x, y)) (pushHudGuardAim(x, y, 320, 200) ? inside : outside) += 1;
        CHECK(inside > 0 && outside == 0);
    }
    {
        const Scene txt(160, 112, {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 20}, {World::Kind::Sky});
        std::printf("[hud-guard-sim] SIMULATED text over flat sky, pan 20, anchor: protected px at real frame 39 = %llu (informational)\n",
                    static_cast<unsigned long long>(protectedPerFrame(txt, 40)[39]));
    }
}

TEST(hud_guard_aim_anchor_never_erases_a_moving_object_over_a_still_picture) {
    using namespace hud_guard_tests;
    // PERFECT generator (raw = the truth): every wrong pixel is the guard's own. A still camera, a crosshair, a textured
    // picture that has been still long enough for the anchor, and an object that crosses the picture by itself.
    struct Case { const char* name; Mover mover; unsigned multiplier; };
    const Case cases[] = {
        {"bar 6x60 at 12 px/frame", {true, 12.0f, 6.0f, 60.0f, 0.92f, 0.25f}, 3},
        {"bar 6x60 at 20 px/frame", {true, 20.0f, 6.0f, 60.0f, 0.92f, 0.25f}, 3},
        {"pole 2x112 (wire, pole) at 12 px/frame", {true, 12.0f, 2.0f, 112.0f, 0.92f, 0.0f}, 3},
        {"tracer 24x2 through the centre rows, 30 px/frame", {true, 30.0f, 24.0f, 2.0f, 0.92f, 55.0f / 112.0f}, 3},
        {"small square 8x8 through the centre, 16 px/frame", {true, 16.0f, 8.0f, 8.0f, 0.92f, 52.0f / 112.0f}, 3},
    };
    for (const auto& c : cases) {
        const Scene s(160, 112, kCross2, {false, 0}, {}, c.mover);
        PushHudGuardParams off;
        off.anchorFrames = 0;
        const auto before = run(s, c.multiplier, Mode::CleanAndCompose, 60, 36, Generator::Perfect, {}, off);
        const auto after = run(s, c.multiplier, Mode::CleanAndCompose, 60, 36, Generator::Perfect);
        printLine(c.name, "perfect", c.multiplier, before, after);
        CHECK(before.mean.wrongPixels == 0);   // as shipped before: one object in a still picture: nothing protected, nothing wrong
        CHECK(after.mean.wrongFar == 0 && after.mean.wrongPixels == 0 && after.mean.overlayExact == after.mean.overlayPixels);
    }
    // The same over a FLAT picture (sky), where the anchor really is reached: the crosshair has been still for 30+ frames when
    // the object enters at frame 40 (startAt), also by a jump of 40-60 px per frame (no changed pixel within `reach` of the
    // still pixels it jumps over): anything that changed around the aim window drops the anchor for that pair.
    const Case flat[] = {
        {"flat sky: square 8x8 through the centre, 40 px/frame", {true, 40.0f, 8.0f, 8.0f, 0.92f, 52.0f / 112.0f, 40.0f}, 3},
        {"flat sky: tracer 24x2 through the centre rows, 60 px/frame", {true, 60.0f, 24.0f, 2.0f, 0.92f, 55.0f / 112.0f, 40.0f}, 3},
        {"flat sky: bar 6x60 at 20 px/frame", {true, 20.0f, 6.0f, 60.0f, 0.92f, 0.25f, 40.0f}, 3},
        {"flat sky: square 8x8 at 40 px/frame, 2X", {true, 40.0f, 8.0f, 8.0f, 0.92f, 52.0f / 112.0f, 40.0f}, 2},
    };
    for (const auto& c : flat) {
        const Scene s(160, 112, kCross2, {false, 0}, {World::Kind::Sky}, c.mover);
        const auto raw = run(s, c.multiplier, Mode::Raw, 64, 36, Generator::Perfect), after = run(s, c.multiplier, Mode::CleanAndCompose, 64, 36, Generator::Perfect);
        printLine(c.name, "perfect", c.multiplier, raw, after);
        CHECK(raw.mean.wrongPixels == 0);
        CHECK(after.mean.wrongFar == 0 && after.mean.wrongPixels == 0 && after.mean.overlayExact == after.mean.overlayPixels);
    }
}

TEST(hud_guard_subtitles_appear_and_disappear_without_being_frozen) {
    using namespace hud_guard_tests;
    // A subtitle (text strokes) shown from real frame 16 to 35 over a panned picture: past frames only, so not protected
    // before 6 frames after it appeared; released at once when it goes (a frozen subtitle would be the worst outcome).
    Overlay text{Overlay::Kind::Text, 2, 14, 1.0f};
    text.appearAt = 16; text.goneAt = 36;
    const Scene s(160, 112, text, {true, 20});
    const auto per = protectedPerFrame(s, 48);
    for (int k = 1; k < 16 + int(PushHudGuardParams{}.stillFrames); ++k) CHECK(per[std::size_t(k)] == 0);
    CHECK(per[24] > 0 && per[35] > 0);
    // Gone from frame 36: its pixels change by far more than `hold` and are released on that frame.
    CHECK(per[36] * 10 <= per[35] && per[37] * 10 <= per[35] && per[40] * 10 <= per[35]);
    const auto both = run(s, 3, Mode::CleanAndCompose, 48, 8, Generator::Perfect), raw = run(s, 3, Mode::Raw, 48, 8, Generator::Perfect);
    printLine("subtitle 16..35 over a circular pan 20", "perfect", 3, raw, both);
    CHECK(both.mean.wrongFar == 0);
    // Over a still camera and a flat picture (the anchor; text and texture are too dense a surround for it, a crosshair is
    // not): shown long enough to be anchored, then gone: protection ends on the frame it goes.
    Overlay text2{Overlay::Kind::Crosshair, 2, 14, 1.0f};
    text2.appearAt = 5; text2.goneAt = 45;
    const Scene still(160, 112, text2, {false, 0}, {World::Kind::Sky});
    const auto per2 = protectedPerFrame(still, 52);
    CHECK(per2[40] > 0 && per2[44] > 0);
    CHECK(per2[45] * 10 <= per2[44] && per2[46] * 10 <= per2[44]);
}

TEST(hud_guard_crosshair_that_changes_hit_marker_and_spread_bloom) {
    using namespace hud_guard_tests;
    // Spread bloom: the arms grow from 14 to 22 px at real frame 24. The old pixels stay protected without a gap; the new
    // ones are protected only after stillFrames frames (a new shape is not known as an overlay before that).
    Overlay bloom{Overlay::Kind::Crosshair, 2, 14, 1.0f};
    bloom.bloomAt = 24; bloom.bloomArm = 22;
    const Scene s(160, 112, bloom, {true, 20});
    PushHudGuardReference g(160, 112);
    auto prev = s.frame(0, true);
    g.update(prev, prev, false, true);
    const std::uint32_t cx = 80, cy = 56;
    const int still = int(g.params().stillFrames);
    for (int k = 1; k < 40; ++k) {
        const auto b = s.frame(k, true);
        g.update(prev, b, false, false);
        prev = b;
        if (k >= 10) CHECK(g.isProtected(cx + 10, cy) || g.isProtected(cx + 10, cy - 1));   // the old arm: no gap
        if (k >= 24 && k < 24 + still - 1) CHECK(!g.isProtected(cx + 20, cy) && !g.isProtected(cx + 20, cy - 1));
        if (k >= 24 + still + 3) CHECK(g.isProtected(cx + 20, cy) || g.isProtected(cx + 20, cy - 1));
    }
    const auto rawB = run(s, 3, Mode::Raw, 40, 30, Generator::Simulated), bothB = run(s, 3, Mode::CleanAndCompose, 40, 30, Generator::Simulated);
    printLine("spread bloom 14 -> 22 px at frame 24, pan 20", "simulated", 3, rawB, bothB);
    CHECK(bothB.mean.wrongPixels <= rawB.mean.wrongPixels);
    // A hit marker shown for 4 real frames (133 ms): never protected, and nothing it does is made worse.
    Overlay marker{Overlay::Kind::Crosshair, 2, 14, 1.0f};
    marker.appearAt = 20; marker.goneAt = 24;
    const Scene hit(160, 112, marker, {true, 20});
    for (const auto n : protectedPerFrame(hit, 40)) CHECK(n == 0);
    const auto hitRaw = run(hit, 3, Mode::Raw, 28, 21, Generator::Simulated), hitBoth = run(hit, 3, Mode::CleanAndCompose, 28, 21, Generator::Simulated);
    printLine("hit marker 4 frames (frames 20-23), pan 20", "simulated", 3, hitRaw, hitBoth);
    CHECK(hitBoth.protectedPixels == 0 && hitBoth.mean.wrongPixels == hitRaw.mean.wrongPixels);
    // Aiming down sights: the crosshair's value changes at frame 20: exact again two frames later (re-arm), at 2X and 3X.
    for (unsigned m : {2u, 3u}) {
        const Scene ads(160, 112, {Overlay::Kind::Crosshair, 1, 14, 1.0f, 0.96f, 20, 0.55f}, {true, 20});
        const auto r = run(ads, m, Mode::Raw, 34, 23, Generator::Simulated), b = run(ads, m, Mode::CleanAndCompose, 34, 23, Generator::Simulated);
        printLine("aim down sights (value change at 20), pan 20", "simulated", m, r, b);
        CHECK(b.mean.overlayExact > r.mean.overlayExact * 2);
    }
}

TEST(hud_guard_test_matrix_hdr10_highlights_dark_pictures_and_a_fast_camera) {
    using namespace hud_guard_tests;
    // HDR10 (PQ, thresholds x 0.6) over a picture with highlights above the SDR span (gain 1.6: PQ codes up to about 0.99).
    {
        World w;
        w.gain = 1.6f;
        const Scene s(160, 112, kCross2, {true, 24}, w, {}, true);
        for (unsigned m : {2u, 3u}) {
            const auto raw = run(s, m, Mode::Raw, 14, 8), both = run(s, m, Mode::CleanAndCompose, 14, 8);
            printLine("HDR10 PQ highlights (gain 1.6), circle 24", "simulated", m, raw, both);
            CHECK(both.mean.overlayExact == both.mean.overlayPixels && both.mean.wrongPixels <= raw.mean.wrongPixels);
        }
    }
    // A dark picture (night: range 0.03 around 0.05) and a dim HUD (grey 0.30): is the crosshair still a structure?
    for (bool hdr : {false, true}) {
        const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 1.0f, 0.30f}, {true, 24}, {World::Kind::Dark}, {}, hdr);
        const auto raw = run(s, 3, Mode::Raw, 16, 8), both = run(s, 3, Mode::CleanAndCompose, 16, 8);
        printLine(hdr ? "dark picture + dim crosshair 0.30, HDR10" : "dark picture + dim crosshair 0.30, SDR", "simulated", 3, raw, both);
        CHECK(both.mean.wrongPixels <= raw.mean.wrongPixels);
        CHECK(both.mean.overlayExact == both.mean.overlayPixels);
    }
    // A fast camera: 60 px of shift per real frame (a flick), straight and in a circle.
    for (bool circle : {false, true}) {
        const Scene s(160, 112, kCross2, {circle, 60.0f}, {});
        const auto raw = run(s, 3, Mode::Raw, 16, 8), both = run(s, 3, Mode::CleanAndCompose, 16, 8);
        printLine(circle ? "fast camera 60 px/frame, circle" : "fast camera 60 px/frame, straight", "simulated", 3, raw, both);
        CHECK(both.mean.wrongPixels <= raw.mean.wrongPixels);
    }
}

TEST(hud_guard_soft_edged_crosshair_edge_metric) {
    using namespace hud_guard_tests;
    // An anti-aliased crosshair: a 1-px fringe at 0.5 / 0.3 of the core's opacity over a moving picture. The metric is
    // the mean error (8-bit codes) on the fringe pixels against the truth, with the generator's own picture as raw.
    for (float fringe : {0.5f, 0.3f}) {
        const Scene s(160, 112, {Overlay::Kind::Crosshair, 2, 14, 1.0f, 0.96f, -1, 0.55f, fringe}, {true, 24});
        for (unsigned m : {2u, 3u}) {
            const auto raw = run(s, m, Mode::Raw, 14, 8), both = run(s, m, Mode::CleanAndCompose, 14, 8);
            printLine(fringe > 0.4f ? "soft-edged crosshair (fringe 0.5), circle 24" : "soft-edged crosshair (fringe 0.3), circle 24", "simulated", m, raw, both);
            CHECK(raw.mean.fringePixels > 0 && both.mean.fringePixels > 0);
            CHECK(both.mean.fringeMean <= raw.mean.fringeMean);   // the soft edge is not made worse than the raw generator's
        }
    }
}
