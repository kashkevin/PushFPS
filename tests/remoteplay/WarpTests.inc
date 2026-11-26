// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushWarp.h (stream "ownfg"): the CPU reference of PushWarp's interpolated frame I(t) that the
// WARP tests and the start-up self-check compare the GPU pass with. Pictures are small synthetic ones; values are
// examples, the expected pictures are exact where the motion is a whole number of pixels.
#include "veyra/pipeline/PushWarp.h"
#include "../../tools/pushwarp_eval/FlowModel.h"
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
    PushWarpParams product;   // the product's limit (1/4 since 25 Nov 2026): 4 flow px at 16 wide, 3 passes
    CHECK(product.maxMotionShare == 0.25f);
    CHECK(pushWarpReference(a.frame(), b.frame(), false, f.field(), product).cells[size_t(3 * 16 + 6)].trust > 0.99f);
    PushWarpParams tight; tight.maxMotionShare = 0.125f;   // the earlier 1/8: 2 flow px at 16 wide, 3 is too long
    const auto s = pushWarpReference(a.frame(), b.frame(), false, f.field(), tight);
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
    // trust, splat, fill, overlay (the static-overlay mask; 0 on a frame that reuses its pair's), compose, total
    const std::uint64_t t[kPushWarpStamps] = {1000, 1100, 1400, 1500, 1500, 2000};
    const auto ms = pushWarpStagesFromTicks(t, 1000000);
    CHECK(std::abs(ms[0] - 0.1) < 1e-9 && std::abs(ms[1] - 0.3) < 1e-9 && std::abs(ms[2] - 0.1) < 1e-9 && std::abs(ms[3]) < 1e-9 &&
          std::abs(ms[4] - 0.5) < 1e-9 && std::abs(ms[5] - 1.0) < 1e-9);
    const std::uint64_t bad[kPushWarpStamps] = {0, 1, 2, 3, 4, 5};
    CHECK(pushWarpStagesFromTicks(bad, 1000)[0] < 0);
    CHECK(pushWarpStagesFromTicks(t, 0)[5] < 0);
    CHECK(std::string(pushWarpStageName(PushWarpStage::Fill)) == "fill" && std::string(pushWarpStageName(PushWarpStage::Overlay)) == "overlay");
    CHECK(static_cast<unsigned>(PushWarpStage::Total) == 5u && kPushWarpStamps == 6u);
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

TEST(warp_motion_histogram_bins_and_quantiles) {
    CHECK(pushWarpMotionBin(0.0f) == 0 && pushWarpMotionBin(0.99f) == 0 && pushWarpMotionBin(1.0f) == 1);
    CHECK(pushWarpMotionBin(12.0f) == 3);                          // sqrt(12) = 3.46
    CHECK(pushWarpMotionBin(899.0f) == 29 && pushWarpMotionBin(960.0f) == 30 && pushWarpMotionBin(961.0f) == 31 && pushWarpMotionBin(5000.0f) == 31);
    PushWarpMotionHistogram h{};
    CHECK(pushWarpMotionQuantile(h, 0.5) < 0);                     // nothing: n/a
    h[3] = 90; h[31] = 10;                                          // most at about 12.5 px, a tenth at 961 px or more
    CHECK(std::abs(pushWarpMotionQuantile(h, 0.5) - 0.5 * (9.0 + 16.0)) < 1e-9);
    CHECK(pushWarpMotionQuantile(h, 0.95) == 961.0);
}

// Stream "ownfg" (directive 33, the owner's crosshair copies): a 2-px white cross over a background moving (+8, 0) work px
// a frame, the field the background's (exact). Without rules the interpolated frame fetches the cross wherever the
// background's vector points at it (copies); with rules 1 + 2 the cross stays put and no copy is made.
TEST(warp_overlay_cross_stays_put_without_copies) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const auto cross = [](int x, int y) { return (x >= 62 && x < 64 && y >= 22 && y < 42) || (y >= 31 && y < 33 && x >= 52 && x < 74); };
    const std::uint32_t white = 0xFFFFFFFFu;
    const auto a = picture(W, H, [&](int x, int y) { return cross(x, y) ? white : texel(x, y, false); });
    const auto b = picture(W, H, [&](int x, int y) { return cross(x, y) ? white : texel(x - 8, y, false); });
    const auto f = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -2; vy = 0; t = 1; });   // B -> A, flow px
    PushWarpParams p = wide(); p.t = 0.5f;
    const auto before = pushWarpReference(a.frame(), b.frame(), false, f.field(), p);
    CHECK(before.overlayMask.empty());   // rules 0 (the default): no mask, the frame as before
    PushWarpParams withRules = p; withRules.overlayRules = 3;
    const auto after = pushWarpReference(a.frame(), b.frame(), false, f.field(), withRules);
    // the mask: the vertical arm found (bit 1); the horizontal arm is longer than the motion along it, so the motion
    // "explains" most of it (A(x + g) is the arm too): not taken, and harmless (its taps fetch the arm where the arm is).
    // Nothing far from the cross.
    unsigned found = 0, cells = 0, outside = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const bool on = (after.overlayMask[std::size_t(y) * W + x] & 1) != 0;
            if (x >= 62 && x < 64 && (y < 31 || y >= 33) && y >= 22 && y < 42) { ++cells; found += on ? 1u : 0u; }
            else if (on && (x < 44 || x > 82)) ++outside;
        }
    CHECK(cells == 36 && found == cells && outside == 0);
    // white copies outside the cross (a cross pixel fetched by a background vector): before some, after none
    const auto copies = [&](const PushWarpResult& r) {
        unsigned n = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
                if (!cross(int(x), int(y)) && x > 4 && x + 4 < W && r.words[std::size_t(y) * W + x] == white) ++n;
        return n;
    };
    CHECK(copies(before) > 10 && copies(after) == 0);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x)
            if (cross(int(x), int(y))) CHECK(after.words[std::size_t(y) * W + x] == white);   // the cross itself, sharp
}

// The gates of the overlay rule (PushWarp.h "Three gates"; the HUD Guard review's damage cases, 25 Nov 2026).
TEST(warp_overlay_gates_agreement_and_surround) {
    constexpr std::uint32_t GW = 16, GH = 8;
    const float move2 = 0.5f * 0.5f;
    std::vector<PushWarpCell> uniform(GW * GH), mixed(GW * GH), still(GW * GH);
    for (std::uint32_t i = 0; i < GW * GH; ++i) {
        uniform[i] = {-2.0f, 0.0f, 1.0f, 0};
        mixed[i] = {(i + i / GW) % 2 ? -2.0f : 3.0f, 0.0f, 1.0f, 0};   // a checkerboard of two vectors: an aliased field
        still[i] = {0.0f, 0.0f, 1.0f, 0};
    }
    float mx = 0, my = 0;
    CHECK(pushWarpOverlayAgrees(uniform, GW, GH, 8, 4, move2, mx, my) && mx == -2.0f && my == 0.0f);
    CHECK(!pushWarpOverlayAgrees(mixed, GW, GH, 8, 4, move2, mx, my));
    CHECK(!pushWarpOverlayAgrees(still, GW, GH, 8, 4, move2, mx, my));   // nothing moves: no surround
    CHECK(pushWarpOverlayMovingShare(uniform, GW, GH, 8, 4, move2) == 1.0f);
    CHECK(pushWarpOverlayMovingShare(still, GW, GH, 8, 4, move2) == 0.0f);
    uniform[4 * GW + 8].trust = 0.2f;   // an untrusted cell does not count as moving
    CHECK(pushWarpOverlayMovingShare(uniform, GW, GH, 8, 4, move2) < 1.0f);
    CHECK(pushWarpOverlayScale(true) == kPushWarpOverlayPqScale && pushWarpOverlayScale(false) == 1.0f);
}

namespace {
// Sum of |error| (8-bit channels) of a result against the truth.
inline std::uint64_t errorAgainst(const PushWarpResult& r, const std::function<std::uint32_t(int, int)>& truth, std::uint32_t W, std::uint32_t H) {
    std::uint64_t e = 0;
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint32_t u = r.words[std::size_t(y) * W + x], v = truth(int(x), int(y));
            for (int c = 0; c < 3; ++c) e += std::uint64_t(std::abs(int((u >> (8 * c)) & 255u) - int((v >> (8 * c)) & 255u)));
        }
    return e;
}
// With and without the rules: no overlay pixel in the mask, and the rules make the frame no worse against the truth.
inline void checkNoOverlayNoWorse(const Pic& a, const Pic& b, const Flow& f, float t, const std::function<std::uint32_t(int, int)>& truth,
                                  std::uint32_t W, std::uint32_t H) {
    PushWarpParams p = wide(); p.t = t;
    const auto plain = pushWarpReference(a.frame(), b.frame(), false, f.field(), p);
    PushWarpParams withRules = p; withRules.overlayRules = 3;
    const auto ruled = pushWarpReference(a.frame(), b.frame(), false, f.field(), withRules);
    unsigned overlay = 0;
    for (auto m : ruled.overlayMask) overlay += (m & 1u) ? 1u : 0u;
    CHECK(overlay == 0);
    CHECK(errorAgainst(ruled, truth, W, H) <= errorAgainst(plain, truth, W, H));
}
}  // namespace

// Still camera, a thin dark object jumping 40 px over a still textured background: the background it crossed between A and
// B is still and "unexplained" by the object's vector, but nothing around it moves: not an overlay (it would be erased).
TEST(warp_overlay_spares_a_fast_thin_object) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const std::uint32_t dark = 0xFF282828u;
    const auto at = [&](double x0) {
        return [&, x0](int x, int y) { return x >= x0 && x < x0 + 4 && y >= 8 && y < 56 ? dark : texel(x, y, false); };
    };
    const auto a = picture(W, H, at(40)), b = picture(W, H, at(80));
    const auto f = flowOf(GW, GH, [](int x, int y, float& vx, float& vy, float& t) {
        vx = (x == 20 && y >= 2 && y < 14) ? -10.0f : 0.0f; vy = 0; t = 1;   // the bar's cells point back 40 px; the rest still
    });
    for (float t : {1.0f / 3.0f, 2.0f / 3.0f}) checkNoOverlayNoWorse(a, b, f, t, at(40.0 + 40.0 * double(t)), W, H);
}

// A pan over a low-contrast sky (changes of at most 2 codes): still only because it has nothing to change.
TEST(warp_overlay_spares_a_low_contrast_sky) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const auto sky = [](int x, int y) {
        const std::uint32_t g = 150u + std::uint32_t(y / 8) + std::uint32_t((((x + 64) / 5) * 7 + y) % 3);
        return g | (g << 8) | ((g + 30u) << 16) | (255u << 24);
    };
    const auto a = picture(W, H, [&](int x, int y) { return sky(x, y); });
    const auto b = picture(W, H, [&](int x, int y) { return sky(x - 8, y); });
    const auto f = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -2; vy = 0; t = 1; });
    for (float t : {1.0f / 3.0f, 2.0f / 3.0f}) {
        const int shift = int(std::lround(8.0 * double(t)));
        checkNoOverlayNoWorse(a, b, f, t, [&](int x, int y) { return sky(x - shift, y); }, W, H);
    }
}

// A fence (bars every 8 px) panning by exactly its period over a moving background: its bars are still in A and B while
// the field moves, but the field's own vector explains them.
TEST(warp_overlay_spares_a_fence_moved_by_its_period) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const std::uint32_t bar = 0xFF5A5A5Au;
    const auto a = picture(W, H, [&](int x, int y) { return (x & 7) < 2 ? bar : texel(x, y, false); });
    const auto b = picture(W, H, [&](int x, int y) { return (x & 7) < 2 ? bar : texel(x - 8, y, false); });
    const auto f = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -2; vy = 0; t = 1; });
    for (float t : {1.0f / 3.0f, 2.0f / 3.0f}) {
        const int shift = int(std::lround(8.0 * double(t)));
        checkNoOverlayNoWorse(a, b, f, t, [&](int x, int y) { return ((x - shift) & 7) < 2 ? bar : texel(x - shift, y, false); }, W, H);
    }
}

// Review 3 MUST 1: still camera, a 4-px bar moving 16 px a real frame (faster than its width) over a still background,
// with the object's true vector in the field. The pixels it crosses between A and B are still in A and B: "still -> A"
// erased it from the generated frames; with the rules (pushWarpStillHolds) it is interpolated there and is where it is at t.
TEST(warp_still_rule_keeps_a_fast_thin_object) {
    constexpr std::uint32_t W = 128, H = 64, GW = 32, GH = 16;
    const std::uint32_t dark = 0xFF282828u;
    const auto at = [&](double x0) {
        return [&, x0](int x, int y) { return x >= x0 && x + 1 <= x0 + 4 && y >= 8 && y < 56 ? dark : texel(x, y, false); };
    };
    const auto a = picture(W, H, at(40)), b = picture(W, H, at(56));
    const auto f = flowOf(GW, GH, [](int x, int y, float& vx, float& vy, float& t) {
        vx = (x == 14 && y >= 2 && y < 14) ? -4.0f : 0.0f; vy = 0; t = 1;   // B -> A: the bar's cell points back 16 px
    });
    PushWarpParams p = wide(); p.t = 0.5f;   // the bar at 48..51: every pixel there is background in A and in B
    const auto plain = pushWarpReference(a.frame(), b.frame(), false, f.field(), p);
    PushWarpParams withRules = p; withRules.overlayRules = 3;
    const auto ruled = pushWarpReference(a.frame(), b.frame(), false, f.field(), withRules);
    unsigned keptPlain = 0, keptRuled = 0, n = 0;
    for (std::uint32_t y = 12; y < 52; ++y)
        for (std::uint32_t x = 48; x < 52; ++x) {
            ++n;
            const auto dark8 = [&](std::uint32_t w) {
                bool close = true;
                for (int c = 0; c < 3; ++c) close = close && std::abs(int((w >> (8 * c)) & 255u) - 0x28) <= 8;
                return close;
            };
            keptPlain += dark8(plain.words[std::size_t(y) * W + x]) ? 1u : 0u;
            keptRuled += dark8(ruled.words[std::size_t(y) * W + x]) ? 1u : 0u;
        }
    CHECK(keptPlain == 0);   // the old rule: erased
    // with the rules: part of the bar where it is at t (19 of 160 pixels within 8 codes when written), as far as the field
    // carries it: the filled field is bilinear between cell centres, so a bar narrower than a cell gets its full vector
    // only near its cell's centre, and where the two moved samples do not agree within kPushWarpStillAgree the pixel
    // stays A (the price of never making a textured background worse). The residual is the field's resolution.
    CHECK(keptRuled * 10 >= n);
    CHECK(errorAgainst(ruled, at(48), W, H) < errorAgainst(plain, at(48), W, H));
}
// Review 3 SHOULD 5: a long vector (beyond 1/8 of the width) is trusted only when its neighbours agree.
TEST(warp_long_vector_needs_its_neighbours) {
    constexpr std::uint32_t GW = 32, GH = 16;
    const auto uniform = flowOf(GW, GH, [](int, int, float& vx, float& vy, float& t) { vx = -10; vy = 0; t = 1; });
    const auto lone = flowOf(GW, GH, [](int x, int y, float& vx, float& vy, float& t) { vx = (x == 10 && y == 8) ? -10.0f : 0.0f; vy = 0; t = 1; });
    CHECK(pushWarpLongCorroborated(uniform.field(), 10, 8, -10, 0, 0.5f));
    CHECK(pushWarpLongCorroborated(uniform.field(), 0, 0, -10, 0, 0.5f));   // a corner: three of three neighbours
    CHECK(!pushWarpLongCorroborated(lone.field(), 10, 8, -10, 0, 0.5f));
    // in the cell rule: 40 work px at a 128-px picture is under the 64-px floor (kept); at 512 px (160 work px) it is long
    const auto small = picture(128, 64, [](int x, int y) { return texel(x, y, false); });
    const auto big = picture(512, 256, [](int x, int y) { return texel(x, y, false); });
    PushWarpParams p = wide();
    const auto keptSmall = pushWarpCellOf(small.frame(), small.frame(), false, lone.field(), 10, 8, p);
    const auto cutBig = pushWarpCellOf(big.frame(), big.frame(), false, lone.field(), 10, 8, p);
    const auto keptBig = pushWarpCellOf(big.frame(), big.frame(), false, uniform.field(), 10, 8, p);
    CHECK(keptSmall.gx == -10.0f && cutBig.gx == 0.0f && cutBig.trust == 0.0f && keptBig.gx == -10.0f);
}

// ---- Resumed 26 Nov 2026: duplicates in generated frames (problem B) and thin moving objects (problem A) -----------------------
// SIMULATED: synthetic pictures, the CPU reference, and the FidelityFX-like block field model of tools/pushwarp_eval/FlowModel.h
// (not AMD's code, not game footage, no graphics card). The pictures are 256 x 128 (flow 64 x 32: 4 work px per flow pixel and
// 8 x 8 flow-pixel blocks = 32 work px, as at 4K). The numbers are printed so that the log of the run keeps them.
namespace {
using Scene = std::function<std::uint32_t(int, int, double)>;   // x, y, time in real-frame intervals

// A smooth, detailed picture (a few sinusoids, about 10 grey levels per pixel of gradient): shifted by any fraction of a pixel
// exactly, so the truth of a generated frame is the scene at that time.
inline double softAt(double x, double y) {
    return 128.0 + 46.0 * std::sin(0.21 * x + 0.13 * y) + 38.0 * std::sin(0.07 * x - 0.31 * y + 1.0) + 26.0 * std::sin(0.43 * x + 0.05 * y + 2.0);
}
inline std::uint32_t grey8(double v) { return std::uint32_t(std::clamp(std::lround(v), 0L, 255L)); }
inline std::uint32_t softWord(double x, double y, double gain = 1.0) {
    return grey8(softAt(x, y) * gain) | (grey8((softAt(x + 17.0, y - 9.0) * 0.9 + 10.0) * gain) << 8) | (grey8(softAt(x - 23.0, y + 31.0) * gain) << 16) | (255u << 24);
}

// The model field of a pair (B -> A, flow pixels, stored as vector x trust).
inline Flow ffxField(const Pic& a, const Pic& b) {
    const auto rgb = [](const Pic& p) {
        std::vector<std::uint8_t> v(std::size_t(p.w) * p.h * 3);
        for (std::size_t i = 0; i < std::size_t(p.w) * p.h; ++i)
            for (std::size_t c = 0; c < 3; ++c) v[i * 3 + c] = std::uint8_t((p.words[i] >> (8 * c)) & 255u);
        return v;
    };
    const auto la = pushwarp_eval::modelLumaQuarter(rgb(a), a.w, a.h), lb = pushwarp_eval::modelLumaQuarter(rgb(b), b.w, b.h);
    const auto df = pushwarp_eval::modelDensify(pushwarp_eval::modelFlow(lb, la), lb, la);
    Flow f;
    f.w = df.w; f.h = df.h; f.sx = df.x; f.sy = df.y; f.tr = df.trust;
    return f;
}

inline double meanError(const std::vector<std::uint32_t>& words, const Pic& truth) {
    double e = 0;
    for (std::size_t i = 0; i < words.size() && i < truth.words.size(); ++i)
        for (int c = 0; c < 3; ++c) e += std::abs(double((words[i] >> (8 * c)) & 255u) - double((truth.words[i] >> (8 * c)) & 255u));
    return e / (3.0 * double(words.size()));
}

// What the 3X output of `pairs` real-frame pairs shows: per pair I(1/3), I(2/3), B in this order, each compared with the
// frame shown before it ("distinct": more than 0.5 % of the pixels differ by more than one code), and the mean error of
// the generated frames against the scene at their time.
struct SceneRun { unsigned frames = 0, distinct = 0, whole = 0; double error = 0; };
inline SceneRun runScene(const Scene& scene, unsigned pairs, unsigned rules, std::uint32_t W = 256, std::uint32_t H = 128) {
    SceneRun out;
    std::vector<Pic> real;
    for (unsigned k = 0; k <= pairs; ++k) real.push_back(picture(W, H, [&](int x, int y) { return scene(x, y, double(k)); }));
    const std::uint64_t threshold = std::uint64_t(W) * H / 200;
    unsigned generated = 0;
    for (unsigned k = 0; k < pairs; ++k) {
        const Flow f = ffxField(real[k], real[k + 1]);
        std::vector<std::uint32_t> shown = real[k].words;
        for (int s = 1; s <= 2; ++s) {
            PushWarpParams p = wide();
            p.t = float(s) / 3.0f;
            p.refineRules = rules;
            const auto r = pushWarpReference(real[k].frame(), real[k + 1].frame(), false, f.field(), p);
            ++out.frames;
            if (pushWarpDiffering(r.words, shown, false, 1) > threshold) ++out.distinct;
            shown = r.words;
            out.whole += r.wholeFrameNearest ? 1u : 0u;
            const double when = double(k) + double(p.t);
            out.error += meanError(r.words, picture(W, H, [&](int x, int y) { return scene(x, y, when); }));
            ++generated;
        }
        ++out.frames;
        if (pushWarpDiffering(real[k + 1].words, shown, false, 1) > threshold) ++out.distinct;
    }
    out.error /= double(generated);
    return out;
}

inline Scene panScene(double vx, double vy) {
    return [=](int x, int y, double time) { return softWord(double(x) - vx * time, double(y) - vy * time); };
}
// A dark bar `width` px wide, 96 px tall, moving `speed` px per real frame over a background panning `bg` px per frame.
constexpr std::uint32_t kBarColour = 0xFF202020u;
inline Scene barScene(int width, double speed, double bg) {
    return [=](int x, int y, double time) {
        const double x0 = 20.0 + speed * time;
        return (y >= 16 && y < 112 && double(x) >= x0 && double(x) < x0 + double(width)) ? kBarColour : softWord(double(x) - bg * time, double(y));
    };
}
// Share of the bar's own pixels (the truth at `when`, rows 24..103) that a frame shows within 8 codes of the bar's colour.
inline double barKept(const std::vector<std::uint32_t>& words, double x0, int width, std::uint32_t W) {
    unsigned kept = 0, n = 0;
    for (int y = 24; y < 104; ++y)
        for (int x = int(std::ceil(x0)); double(x) < x0 + double(width); ++x) {
            if (x < 0 || x >= int(W)) continue;
            ++n;
            bool close = true;
            for (int c = 0; c < 3; ++c) close = close && std::abs(int((words[std::size_t(y) * W + std::size_t(x)] >> (8 * c)) & 255u) - 0x20) <= 8;
            kept += close ? 1u : 0u;
        }
    return n ? double(kept) / double(n) : 0.0;
}
}  // namespace

// Problem B: the generated frames of one scene class, with the old rules and with the refinement. The old rules duplicate
// frames in the slow-pan classes (the flow's zero vector fails the block SAD everywhere and the whole-frame verdict shows
// the nearer real frame); the refinement does not. Classes that cannot be fixed by a better vector (repeating patterns,
// a global flash) are reported, not asserted.
TEST(warp_refine_distinct_frames_per_scene_class) {
    struct Case { const char* name; Scene scene; bool mustBeDistinct; };
    const std::vector<Case> cases = {
        {"pan_1px", panScene(1.0, 0.0), true},
        {"pan_2px", panScene(2.0, 0.0), true},
        {"pan_3px_diagonal", panScene(2.0, 2.0), true},
        {"pan_8px", panScene(8.0, 0.0), true},
        {"grille_period12_speed5",
         [](int x, int, double time) {
             const double u = std::fmod(double(x) - 5.0 * time + 1200.0, 12.0);
             const std::uint32_t g = u < 6.0 ? 210u : 50u;
             return g | (g << 8) | ((g / 2u + 20u) << 16) | (255u << 24);
         }, false},
        {"flash_pan_4px", [](int x, int y, double time) { return softWord(double(x) - 4.0 * time, double(y), 1.0 + 0.4 * time); }, false},
        {"thin_bar_4px_30px", barScene(4, 30.0, 8.0), false},
    };
    for (const auto& c : cases) {
        const SceneRun old = runScene(c.scene, 3, 0), now = runScene(c.scene, 3, kPushWarpRefineAll);
        std::cerr << "[warp-refine] " << c.name << ": distinct of " << old.frames << " shown: old " << old.distinct << " new " << now.distinct
                  << "; whole-frame holds old " << old.whole << " new " << now.whole << "; mean error old " << old.error << " new " << now.error << std::endl;
        CHECK(now.distinct >= old.distinct);
        if (c.mustBeDistinct) CHECK(now.distinct == now.frames);
    }
}

// Problem A: thin bars (2, 4, 8 px) moving 20, 40 and 60 px per frame over a background moving 8 px per frame. The share of the
// bar's pixels a generated frame shows (barKept) with the old rules and with the refinement, at t = 1/3 and 2/3.
TEST(warp_refine_thin_moving_objects) {
    double oldSum = 0, newSum = 0;
    unsigned rows = 0;
    for (int width : {2, 4, 8})
        for (double speed : {20.0, 40.0, 60.0}) {
            const Scene scene = barScene(width, speed, 8.0);
            const std::uint32_t W = 256, H = 128;
            const Pic a = picture(W, H, [&](int x, int y) { return scene(x, y, 1.0); }), b = picture(W, H, [&](int x, int y) { return scene(x, y, 2.0); });
            const Flow f = ffxField(a, b);
            for (int s = 1; s <= 2; ++s) {
                double kept[2] = {0, 0};
                for (int mode = 0; mode < 2; ++mode) {
                    PushWarpParams p = wide();
                    p.t = float(s) / 3.0f;
                    p.refineRules = mode ? kPushWarpRefineAll : 0u;
                    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), p);
                    kept[mode] = barKept(r.words, 20.0 + speed * (1.0 + double(p.t)), width, W);
                }
                std::cerr << "[warp-refine] bar " << width << " px, " << speed << " px/frame, t=" << s << "/3: bar pixels kept old " << kept[0] << " new " << kept[1] << std::endl;
                oldSum += kept[0]; newSum += kept[1]; ++rows;
            }
        }
    CHECK(rows == 18);
    CHECK(newSum >= oldSum);   // never a net loss over the table
}

// The refinement bits are independent and off by default: refineRules 0 gives the frame of before.
TEST(warp_refine_off_by_default_and_bits) {
    const PushWarpParams p;
    CHECK(p.refineRules == 0);
    CHECK(kPushWarpRefineAll == (kPushWarpRefineAgreeingHoles | kPushWarpRefineCells | kPushWarpRefinePixels));
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto b = picture(W, H, [](int x, int y) { return texel(x - 12, y, false); });
    const auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = -3; vy = 0; t = 1; });
    const auto base = pushWarpReference(a.frame(), b.frame(), false, f.field(), wide());
    PushWarpParams q = wide();
    q.refineRules = 0;
    CHECK(pushWarpReference(a.frame(), b.frame(), false, f.field(), q).words == base.words);
    // an exact field is not changed by the refinement (every cell is already under sadLow)
    q.refineRules = kPushWarpRefineAll;
    CHECK(pushWarpReference(a.frame(), b.frame(), false, f.field(), q).words == base.words);
}

// Cut: unrelated pictures still hold the nearer real frame with every refinement bit on.
TEST(warp_refine_keeps_the_cut_verdict) {
    constexpr std::uint32_t W = 64, H = 32;
    const auto a = picture(W, H, [](int x, int y) { return texel(x, y, false); });
    const auto b = picture(W, H, [](int x, int y) { return texel(y * 7 + 3, x * 3 + 1, false); });
    const auto f = flowOf(16, 8, [](int, int, float& vx, float& vy, float& t) { vx = 0; vy = 0; t = 1; });
    PushWarpParams p = wide();
    p.refineRules = kPushWarpRefineAll;
    const auto r = pushWarpReference(a.frame(), b.frame(), false, f.field(), p);
    CHECK(r.wholeFrameNearest && r.words == b.words);
}

}  // namespace warp_tests
