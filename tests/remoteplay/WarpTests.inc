// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushWarp.h (stream "ownfg"): the CPU reference of PushWarp's interpolated frame I(t) that the
// WARP tests and the start-up self-check compare the GPU pass with. Pictures are small synthetic ones; values are
// examples, the expected pictures are exact where the motion is a whole number of pixels.
#include "veyra/pipeline/PushWarp.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace warp_tests {
using namespace veyra::pipeline;

// A colour whose channels differ between neighbouring columns and rows (no moving pixel looks still).
inline std::uint32_t texel(int x, int y, bool tenBit) {
    const std::uint32_t r = std::uint32_t((x * 37 + y * 11) & 255), g = std::uint32_t((x * 53 + 7) & 255), b = std::uint32_t((y * 29 + x * 5 + 3) & 255);
    if (tenBit) return (r * 4 + 1) | ((g * 4 + 2) << 10) | ((b * 4 + 3) << 20) | (3u << 30);
    return r | (g << 8) | (b << 16) | (255u << 24);
}
struct Pic {
    std::vector<std::uint32_t> words;
    std::uint32_t w = 0, h = 0;
    PushWarpFrame frame() const { return {words.data(), w, h, w}; }
};
inline Pic picture(std::uint32_t w, std::uint32_t h, const std::function<std::uint32_t(int, int)>& f) {
    Pic p; p.w = w; p.h = h; p.words.resize(std::size_t(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) for (std::uint32_t x = 0; x < w; ++x) p.words[std::size_t(y) * w + x] = f(int(x), int(y));
    return p;
}
struct Flow {
    std::vector<float> sx, sy, tr;
    std::uint32_t w = 0, h = 0;
    PushWarpField field() const { return {sx.data(), sy.data(), tr.data(), w, h}; }
};
// The B -> A vector (flow pixels) and trust of each cell; stored as v x trust (GpuDisValidate / NvofDensify's form).
inline Flow flowOf(std::uint32_t w, std::uint32_t h, const std::function<void(int, int, float&, float&, float&)>& f) {
    Flow o; o.w = w; o.h = h; o.sx.resize(std::size_t(w) * h); o.sy.resize(o.sx.size()); o.tr.resize(o.sx.size());
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            float vx = 0, vy = 0, t = 0;
            f(int(x), int(y), vx, vy, t);
            const std::size_t i = std::size_t(y) * w + x;
            o.sx[i] = vx * t; o.sy[i] = vy * t; o.tr[i] = t;
        }
    return o;
}

// The tests' pictures are small: 3 flow pixels are more than 1/8 of a 16-cell width, so the length limit is widened.
inline PushWarpParams wide() { PushWarpParams p; p.maxMotionShare = 0.5f; return p; }

TEST(warp_pyramid_levels_and_scratch) {
    const auto p = pushWarpPyramid(960, 540);
    // 960x540, 480x270, 240x135, 120x68, 60x34, 30x17, 15x9, 8x5, 4x3, 2x2
    CHECK(p.levels == 10);
    CHECK(p.width[3] == 120 && p.height[3] == 68 && p.width[9] == 2 && p.height[9] == 2);
    CHECK(p.offset[0] == 0 && p.offset[1] == 960u * 540u && p.offset[2] == 960u * 540u + 480u * 270u);
    const auto one = pushWarpPyramid(1, 1);
    CHECK(one.levels == 1 && one.cells == 1);
    CHECK(pushWarpPyramid(0, 4).levels == 0);
    const auto s = pushWarpScratch(960, 540);
    CHECK(s.accum == 960ull * 540 * 16 && s.pyramid == s.accum + 960ull * 540 * 32 && s.bytes == s.pyramid + std::uint64_t(p.cells) * 32);
    // the pass's scratch also holds the self-check's field (64x36), whatever its own flow size
    CHECK(pushWarpScratchBytes(32, 16) == pushWarpScratch(64, 36).bytes);
    CHECK(pushWarpScratchBytes(960, 540) == s.bytes);
}

// Uniform motion: B = A moved right by 12 work pixels (3 flow pixels at a 4x ratio); I(2/3) = A moved by 8.
inline void uniformCase(bool tenBit) {
    constexpr std::uint32_t W = 64, H = 32, GW = 16, GH = 8;
    const auto a = picture(W, H, [&](int x, int y) { return texel(x, y, tenBit); });
    const auto b = picture(W, H, [&](int x, int y) { return texel(x - 12, y, tenBit); });
    const auto f = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -3; vy = 0; t = 1; });
    const auto r = pushWarpReference(a.frame(), b.frame(), tenBit, f.field(), wide());
    CHECK(r.words.size() == std::size_t(W) * H);
    for (const auto& c : r.field) CHECK(std::abs(c.ex + 3.0f) < 1e-5f && std::abs(c.ey) < 1e-5f);   // holes filled with the same motion
    bool exact = true;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 9; x + 5 < W; ++x) exact = exact && r.words[std::size_t(y) * W + x] == texel(int(x) - 8, int(y), tenBit);
    CHECK(exact);
    CHECK(!r.wholeFrameNearest && r.counters.still == 0);
    CHECK(r.counters.hole <= 2 * H * 4);   // only the cells the splat cannot reach (the last flow column) are holes
}
TEST(warp_uniform_motion_sdr) { uniformCase(false); }
TEST(warp_uniform_motion_hdr10) { uniformCase(true); }

TEST(warp_still_picture_is_a_exactly) {
    constexpr std::uint32_t W = 32, H = 16;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto f = flowOf(8, 4, [](int, int, float& vx, float& vy, float& t) { vx = -2; vy = 1; t = 1; });   // a wrong flow changes nothing
    const auto r = pushWarpReference(a.frame(), a.frame(), false, f.field());
    CHECK(r.words == a.words && r.counters.still == W * H);
}

TEST(warp_static_hud_over_moving_background) {
    constexpr std::uint32_t W = 64, H = 32;
    const auto hud = [](int x, int y) { return x >= 24 && x < 40 && y >= 8 && y < 20; };
    const std::uint32_t hudWord = 250u | (12u << 8) | (128u << 16) | (255u << 24);
    const auto a = picture(W, H, [&](int x, int y) { return hud(x, y) ? hudWord : texel(x, y, false); });
    const auto b = picture(W, H, [&](int x, int y) { return hud(x, y) ? hudWord : texel(x - 12, y, false); });
    const auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = -3; vy = 0; t = 1; });   // the flow carries the motion over the HUD
    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), wide());
    bool same = true;
    for (std::uint32_t y = 0; y < H; ++y) for (std::uint32_t x = 0; x < W; ++x) if (hud(int(x), int(y))) same = same && r.words[std::size_t(y) * W + x] == hudWord;
    CHECK(same);
    CHECK(r.counters.still == 16u * 12u);
}

TEST(warp_cut_or_failed_flow_is_the_nearer_real_frame) {
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto b = picture(W, H, [](int x, int y) { return texel(y * 7 + 3, x * 3 + 1, false); });   // unrelated content: a cut
    // (1) the flow claims "still, trusted": the block SAD rejects it, nothing splats, every moving pixel is a hole
    auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = 0; vy = 0; t = 1; });
    auto r = pushWarpReference(a.frame(), b.frame(), false, f.field());
    CHECK(r.wholeFrameNearest && r.words == b.words);
    CHECK(pushWarpHoleShare(r.counters) > 0.5);
    // (2) the flow failed (trust 0): the same
    f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = 0; vy = 0; t = 0; });
    r = pushWarpReference(a.frame(), b.frame(), false, f.field());
    CHECK(r.wholeFrameNearest && r.words == b.words);
    // (3) at t < 1/2 the nearer real frame is A
    PushWarpParams early; early.t = 1.0f / 3.0f;
    r = pushWarpReference(a.frame(), b.frame(), false, f.field(), early);
    CHECK(r.wholeFrameNearest && r.words == a.words);
}

TEST(warp_trust_cells) {
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto b = picture(W, H, [](int x, int y) { return texel(x - 12, y, false); });
    // right vector, low flow trust, wrong vector, too long
    const auto f = flowOf(16, 8, [](int x, int, float& vx, float& vy, float& t) {
        vy = 0; vx = -3; t = 1;
        if (x == 4) t = 0.4f;
        if (x == 8) vx = 2;
    });
    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), wide());
    const auto& good = r.cells[size_t(3 * 16 + 6)];   // (cells 0-2 see A outside the picture: the SAD rejects them)
    CHECK(std::abs(good.gx + 3.0f) < 1e-6f && good.trust > 0.99f && good.importance == pushWarpImportance(3.0f, 1.0f));
    CHECK(r.cells[size_t(3 * 16 + 4)].trust == 0.0f);   // below minConfidence
    CHECK(r.cells[size_t(3 * 16 + 8)].trust < 0.01f);    // a vector that does not match the pictures: the block SAD rejects it
    const auto s = pushWarpReference(a.frame(), b.frame(), false, f.field());   // the product's limit: 2 flow px at 16 wide, 3 is too long
    CHECK(s.cells[size_t(3 * 16 + 6)].trust == 0.0f);
}

// Two layers: a background moving 6 work px per interval and a square in front of it moving 24, both to the right (whole
// pixels at t = 2/3, so the truth is exact). The
// field is the ideal B -> A field on B's grid. At t = 2/3 the square must be whole where it is at that time, the
// background must be right away from the square, and the square must win where it covers the background (the faster
// mover is in front).
TEST(warp_two_layers_occlusion) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const auto bg = [](double x, double y) { return texel(int(std::floor(x)), int(std::floor(y)), false); };
    const auto fgColour = [](double x, double y) { return (40u + std::uint32_t(int(std::floor(x)) & 7) * 20u) | (200u << 8) | ((60u + std::uint32_t(int(std::floor(y)) & 3) * 30u) << 16) | (255u << 24); };
    const auto at = [&](double s, int x, int y) {
        const double sq = 24.0 + 24.0 * s;   // the square's left edge at time s; 32 px wide, rows 16..47
        if (x >= sq && x < sq + 32 && y >= 16 && y < 48) return fgColour(x - sq, y - 16);
        return bg(x - 6.0 * s, y);
    };
    const auto a = picture(W, H, [&](int x, int y) { return at(0.0, x, y); });
    const auto b = picture(W, H, [&](int x, int y) { return at(1.0, x, y); });
    const auto truth = picture(W, H, [&](int x, int y) { return at(2.0 / 3.0, x, y); });
    const auto f = flowOf(GW, GH, [](int x, int y, float& vx, float& vy, float& t) {
        const int wx = x * 4 + 2, wy = y * 4 + 2;   // the cell's centre in work pixels, in B
        vx = (wx >= 48 && wx < 80 && wy >= 16 && wy < 48) ? -6.0f : -1.5f; vy = 0; t = 1;
    });
    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), wide());
    std::uint32_t squareOk = 0, squareN = 0, bgOk = 0, bgN = 0;
    const double sq = 24.0 + 24.0 * 2.0 / 3.0;
    for (std::uint32_t y = 20; y < 44; ++y)
        for (std::uint32_t x = std::uint32_t(sq) + 4; x < std::uint32_t(sq) + 28; ++x) {
            ++squareN; squareOk += pushWarpMaxDelta(pushWarpUnpack(r.words[y * W + x], false), pushWarpUnpack(truth.words[y * W + x], false)) <= 2.0f / 255.0f;
        }
    for (std::uint32_t y = 2; y < H - 2; ++y)
        for (std::uint32_t x = 4; x < W - 4; ++x) {
            if (y >= 10 && y < 54 && x >= 20 && x < 88) continue;   // around the square's path
            ++bgN; bgOk += pushWarpMaxDelta(pushWarpUnpack(r.words[y * W + x], false), pushWarpUnpack(truth.words[y * W + x], false)) <= 2.0f / 255.0f;
        }
    CHECK(squareN > 0 && double(squareOk) >= 0.95 * double(squareN));
    CHECK(bgN > 0 && double(bgOk) >= 0.98 * double(bgN));
}

TEST(warp_whole_frame_rule_and_check_rule) {
    PushWarpCounters c; c.blend = 10; c.conflict = 5; c.hole = 15;
    CHECK(!pushWarpWholeFrameNearest(c));
    c.hole = 16;
    CHECK(pushWarpWholeFrameNearest(c));
    CHECK(std::abs(pushWarpHoleShare(c) - 16.0 / 31.0) < 1e-12);
    CHECK(pushWarpHoleShare(PushWarpCounters{}) == 0.0);
    PushWarpCounters g = c, e = c;
    CHECK(pushWarpCheckPass(0, 10000, g, e));
    CHECK(pushWarpCheckPass(20, 10000, g, e));
    CHECK(!pushWarpCheckPass(21, 10000, g, e));
    g.blend += 25;
    CHECK(!pushWarpCheckPass(0, 10000, g, e));
    CHECK(!pushWarpCheckPass(0, 0, e, e));
}

TEST(warp_stage_times) {
    const std::uint64_t t[kPushWarpStamps] = {1000, 1100, 1400, 1500, 2000};
    const auto ms = pushWarpStagesFromTicks(t, 1000000);
    CHECK(std::abs(ms[0] - 0.1) < 1e-9 && std::abs(ms[1] - 0.3) < 1e-9 && std::abs(ms[3] - 0.5) < 1e-9 && std::abs(ms[4] - 1.0) < 1e-9);
    const std::uint64_t bad[kPushWarpStamps] = {0, 1, 2, 3, 4};
    CHECK(pushWarpStagesFromTicks(bad, 1000)[0] < 0);
    CHECK(pushWarpStagesFromTicks(t, 0)[4] < 0);
    CHECK(std::string(pushWarpStageName(PushWarpStage::Fill)) == "fill");
}

TEST(warp_pack_rounds_and_clamps) {
    CHECK(pushWarpPack({1.5f, -0.2f, 0.5f, 1.0f}, false) == (255u | (0u << 8) | (128u << 16) | (255u << 24)));
    CHECK(pushWarpPack(pushWarpUnpack(0xC0123456u, true), true) == 0xC0123456u);
    CHECK(pushWarpPack(pushWarpUnpack(0x80FF10EEu, false), false) == 0x80FF10EEu);
}

// Step 4: the predicted frame P(s) = B + s, forward-projected from B alone (PushWarpParams t = 1 + s).
inline void predictCase(bool tenBit, float s, int shift) {
    constexpr std::uint32_t W = 64, H = 32, GW = 16, GH = 8;
    const auto a = picture(W, H, [&](int x, int y) { return texel(x, y, tenBit); });
    const auto b = picture(W, H, [&](int x, int y) { return texel(x - 12, y, tenBit); });
    const auto f = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -3; vy = 0; t = 1; });
    auto p = wide(); p.t = 1.0f + s;
    const auto r = pushWarpReference(a.frame(), b.frame(), tenBit, f.field(), p);
    bool exact = true;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = std::uint32_t(16 + shift); x + 2 < W; ++x) exact = exact && r.words[std::size_t(y) * W + x] == texel(int(x) - 12 - shift, int(y), tenBit);
    CHECK(exact);
    // the revealed columns at the left (content moved right, nothing landed there) hold B
    bool held = true;
    for (std::uint32_t y = 0; y < H; ++y) for (std::uint32_t x = 0; x < 4; ++x) held = held && r.words[std::size_t(y) * W + x] == b.words[std::size_t(y) * W + x];
    CHECK(held);
    CHECK(r.counters.hole > 0 && r.counters.blend > r.counters.hole && !r.wholeFrameNearest);
}
TEST(warp_predict_forward_one_third) { predictCase(false, 1.0f / 3.0f, 4); }
TEST(warp_predict_forward_two_thirds_hdr10) { predictCase(true, 2.0f / 3.0f, 8); }

TEST(warp_predict_still_is_b_and_cut_is_b) {
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    auto p = wide(); p.t = 5.0f / 3.0f;
    const auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = -2; vy = 0; t = 1; });
    auto r = pushWarpReference(a.frame(), a.frame(), false, f.field(), p);
    CHECK(r.words == a.words && r.counters.still == W * H);
    const auto b = picture(W, H, [](int x, int y) { return texel(y * 7 + 3, x * 3 + 1, false); });
    const auto z = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = 0; vy = 0; t = 1; });
    r = pushWarpReference(a.frame(), b.frame(), false, z.field(), p);
    CHECK(r.wholeFrameNearest && r.words == b.words);
}

TEST(warp_predict_acceleration_rule) {
    float ex = 0, ey = 0;
    // moving right 3 flow px now, 2 before (an acceleration of 1): d = 3 s + 1 (s/2 + s^2/2)
    pushWarpAccelerated(1.0f / 3.0f, -3.0f, 0.0f, -2.0f, 0.0f, true, 0.5f, ex, ey);
    const float s = 1.0f / 3.0f, d = 3.0f * s + (s / 2 + s * s / 2);
    CHECK(std::abs(ex + d / s) < 1e-5f && std::abs(ey) < 1e-6f);
    // below the dead zone: no acceleration (noise); untrusted previous vector: none
    pushWarpAccelerated(s, -3.0f, 0.0f, -2.75f, 0.0f, true, 0.5f, ex, ey);
    CHECK(ex == -3.0f && ey == 0.0f);
    pushWarpAccelerated(s, -3.0f, 0.0f, 10.0f, 0.0f, false, 0.5f, ex, ey);
    CHECK(ex == -3.0f);
    // a stop (moved 6 before, 1 now): the acceleration term is clamped to the speed term, never reverses the motion
    pushWarpAccelerated(2.0f / 3.0f, -1.0f, 0.0f, -6.0f, 0.0f, true, 0.5f, ex, ey);
    CHECK(std::abs(ex) < 1e-5f);   // d = -(-1)(2/3) + clamp(-5 k) = 2/3 - 2/3 = 0: P holds still
    // through the reference: an accelerating pan with the previous field lands where the accelerated motion says
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto b = picture(W, H, [](int x, int y) { return texel(x - 12, y, false); });
    const auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = -3; vy = 0; t = 1; });
    const auto prev = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = -1.5f; vy = 0; t = 1; });   // 6 px before, 12 now
    auto p = wide(); p.t = 1.0f + 1.0f / 3.0f; p.acceleration = true;
    const auto pf = prev.field();
    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), p, &pf);
    // a = 1.5 flow px, k = 2/9: d = 1 + 1/3 flow px = 16/3 work px: P(x) = B(x - 16/3) in the middle of the picture
    const auto& c = r.cells[std::size_t(3 * 16 + 8)];
    CHECK(std::abs(c.gx + 4.0f) < 1e-4f);   // g' = -d / s = -(4/3) * 3
    bool moved = true;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 24; x + 2 < W; ++x)
            moved = moved && pushWarpMaxDelta(pushWarpUnpack(r.words[std::size_t(y) * W + x], false),
                                             pushWarpBilinear(b.frame(), false, float(x) - 16.0f / 3.0f, float(y))) <= 1.0f / 255.0f;
    CHECK(moved);
}

}  // namespace warp_tests
