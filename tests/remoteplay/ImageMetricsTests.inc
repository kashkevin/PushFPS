// SPDX-License-Identifier: GPL-3.0-or-later
// No-reference picture metrics of the pipeline probe (veyra/engine/PushImageMetrics.h), on synthetic patches.
// Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushImageMetrics.h"
namespace {
namespace pim = veyra::engine;
template <class F> pim::PushLumaImage pimMake(int w, int h, F value) {
    pim::PushLumaImage img;
    img.resize(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.set(x, y, float(value(x, y)));
    return img;
}
// A small deterministic generator (the tests must give the same numbers on every compiler).
struct PimRandom {
    std::uint32_t state = 12345u;
    float next() {  // 0..1
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) * (1.0f / 16777216.0f);
    }
};
bool pimNear(double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; }
}  // namespace

TEST(image_metrics_encodings_and_luma_conversions){
    CHECK(pim::pushHalfToFloat(0x3C00) == 1.0f && pim::pushHalfToFloat(0x0000) == 0.0f && pim::pushHalfToFloat(0xC000) == -2.0f);
    CHECK(pim::pushHalfToFloat(0x3800) == 0.5f && pim::pushHalfToFloat(0x0001) > 5.9e-8f && pim::pushHalfToFloat(0x0001) < 6.0e-8f);
    CHECK(std::isinf(pim::pushHalfToFloat(0x7C00)) && std::isnan(pim::pushHalfToFloat(0x7E00)));
    CHECK(pimNear(pim::pushPqEncode(10000.0f), 1.0, 1e-4) && pimNear(pim::pushPqEncode(100.0f), 0.5081, 1e-3) && pimNear(pim::pushPqEncode(203.0f), 0.5807, 1e-3));
    CHECK(pim::pushPqEncode(0.0f) < 1e-6f && pim::pushPqEncode(-5.0f) < 1e-6f);
    CHECK(pimNear(pim::pushSrgbEncode(1.0f), 1.0, 1e-5) && pimNear(pim::pushSrgbEncode(0.18f), 0.4614, 1e-3) && pim::pushSrgbEncode(-1.0f) == 0.0f);
    // 8-bit: BT.709 weights on the encoded values; BGRA swaps red and blue.
    const std::uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 128, 128, 128, 255};
    pim::PushLumaImage img;
    CHECK(pim::pushLumaFromRgba8(rgba, 8, 2, 2, false, img) && img.width == 2 && img.height == 2);
    CHECK(pimNear(img.at(0, 0), 54.21, 0.02) && pimNear(img.at(1, 0), 182.38, 0.02) && pimNear(img.at(0, 1), 18.41, 0.02) && pimNear(img.at(1, 1), 128.0, 0.02));
    CHECK(pim::pushLumaFromRgba8(rgba, 8, 2, 2, true, img) && pimNear(img.at(0, 0), 18.41, 0.02) && pimNear(img.at(0, 1), 54.21, 0.02));
    CHECK(pimNear(pim::pushMeanLuma(img), (18.41 + 182.38 + 54.21 + 128.0) / 4.0, 0.03));
    CHECK(!pim::pushLumaFromRgba8(rgba, 7, 2, 2, false, img) && !pim::pushLumaFromRgba8(nullptr, 8, 2, 2, false, img));
    // HDR10: 10-bit PQ codes, a quarter of a code is kept.
    const std::uint32_t packed[2] = {1023u | (1023u << 10) | (1023u << 20), 513u | (513u << 10) | (513u << 20)};
    CHECK(pim::pushLumaFromRgb10a2(reinterpret_cast<const std::uint8_t*>(packed), 8, 2, 1, img));
    CHECK(pimNear(img.at(0, 0), 255.0, 0.02) && pimNear(img.at(1, 0), 513.0 * 255.0 / 1023.0, 0.02));
    // FP16 scRGB: SDR white is 255; in HDR 1.0 is 80 nits, and 203/80 gain gives the 203-nit white.
    const std::uint16_t white[4] = {0x3C00, 0x3C00, 0x3C00, 0x3C00};
    CHECK(pim::pushLumaFromRgba16f(reinterpret_cast<const std::uint8_t*>(white), 8, 1, 1, false, 1.0f, img) && pimNear(img.at(0, 0), 255.0, 0.02));
    CHECK(pim::pushLumaFromRgba16f(reinterpret_cast<const std::uint8_t*>(white), 8, 1, 1, true, 1.0f, img));
    CHECK(pimNear(img.at(0, 0), 255.0 * double(pim::pushPqEncode(80.0f)), 0.1));
    CHECK(pim::pushLumaFromRgba16f(reinterpret_cast<const std::uint8_t*>(white), 8, 1, 1, true, 203.0f / 80.0f, img));
    CHECK(pimNear(img.at(0, 0), 255.0 * 0.5807, 0.3));
    // Out-of-range and NaN samples are stored as valid codes.
    img.resize(2, 1); img.set(0, 0, 999.0f); img.set(1, 0, std::nanf(""));
    CHECK(img.at(0, 0) == 255.0f && img.at(1, 0) == 0.0f);
    CHECK(pim::pushMeanLuma(pim::PushLumaImage{}) == -1.0);
}
TEST(image_metrics_blockiness_sees_a_block_grid_at_any_phase){
    // A smooth picture and plain noise have no grid.
    const auto smooth = pimMake(256, 128, [](int x, int y) { return 40.0 + 0.5 * x + 0.3 * y + 10.0 * std::sin(x * 0.05) * std::cos(y * 0.07); });
    CHECK(pim::pushBlockiness(smooth) < 0.1);
    PimRandom random;
    const auto noise = pimMake(256, 128, [&](int, int) { return 100.0 + 40.0 * random.next(); });
    CHECK(pim::pushBlockiness(noise) < 0.15);
    // 8x8 blocks, each with its own level, over the same smooth picture.
    const auto level = [](int bx, int by) { return double((bx * 37 + by * 91) % 7) - 3.0; };
    const auto blocks = pimMake(256, 128, [&](int x, int y) { return 100.0 + 0.2 * x + 2.0 * level(x / 8, y / 8); });
    const double blocky = pim::pushBlockiness(blocks);
    CHECK(blocky > 3.0);
    // The same grid moved by 3 pixels is found as well (no fixed origin is assumed).
    const auto shifted = pimMake(256, 128, [&](int x, int y) { return 100.0 + 0.2 * x + 2.0 * level((x + 3) / 8, (y + 5) / 8); });
    CHECK(pim::pushBlockiness(shifted) > 3.0);
    // Weaker blocks score lower: the number orders pictures.
    const auto weak = pimMake(256, 128, [&](int x, int y) { return 100.0 + 0.2 * x + 0.5 * level(x / 8, y / 8); });
    CHECK(pim::pushBlockiness(weak) > 0.3 && pim::pushBlockiness(weak) < blocky);
    // After a 1.5x enlargement the grid has a period of 12 pixels.
    const auto enlarged = pimMake(288, 144, [&](int x, int y) { return 100.0 + 2.0 * level(x / 12, y / 12); });
    CHECK(pim::pushBlockiness(enlarged, 12.0) > 3.0);
    // A flat picture has no steps at all: 0, not a division by nothing. Too small: no answer.
    CHECK(pim::pushBlockiness(pimMake(64, 64, [](int, int) { return 50.0; })) == 0.0);
    CHECK(pim::pushBlockiness(pimMake(16, 16, [](int, int) { return 50.0; })) == -1.0);
}
TEST(image_metrics_banding_counts_small_steps_between_flat_runs){
    // A gradient quantised to steps of 2 codes every 16 pixels: one band edge per 16 row positions.
    const auto banded = pimMake(256, 64, [](int x, int) { return 60.0 + 2.0 * (x / 16); });
    const double b = pim::pushBanding(banded);
    CHECK(b > 20.0 && b < 40.0);
    // The same gradient with fractional values (what a 10-bit picture or a debanded one has): nothing.
    const auto smooth = pimMake(256, 64, [](int x, int) { return 60.0 + 0.125 * x; });
    CHECK(pim::pushBanding(smooth) == 0.0);
    // Dither over the bands hides them from this metric (and from the eye: that is its purpose).
    PimRandom random;
    const auto dithered = pimMake(256, 64, [&](int x, int) { return 60.0 + 2.0 * (x / 16) + (random.next() - 0.5); });
    CHECK(pim::pushBanding(dithered) < 2.0);
    // Steps of 8 codes are edges, not bands; a flat picture has none.
    CHECK(pim::pushBanding(pimMake(256, 64, [](int x, int) { return 60.0 + 8.0 * (x / 16); })) == 0.0);
    CHECK(pim::pushBanding(pimMake(64, 64, [](int, int) { return 77.0; })) == 0.0);
    // Vertical bands are counted too.
    CHECK(pim::pushBanding(pimMake(64, 256, [](int, int y) { return 60.0 + 2.0 * (y / 16); })) > 20.0);
    CHECK(pim::pushBanding(pimMake(4, 4, [](int, int) { return 0.0; })) == -1.0);
}
TEST(image_metrics_ringing_is_overshoot_beside_an_edge_with_a_flat_surround){
    // A clean step edge between x = 39 and x = 40: nothing.
    const auto clean = pimMake(96, 48, [](int x, int) { return x < 40 ? 50.0 : 200.0; });
    CHECK(pim::pushRinging(clean) == 0.0);
    // An overshoot of 15 codes two pixels into the bright side: 15 on that side, 0 on the dark one.
    const auto over = pimMake(96, 48, [](int x, int) { return x < 40 ? 50.0 : x == 41 ? 215.0 : 200.0; });
    CHECK(pimNear(pim::pushRinging(over), 7.5, 0.05));
    // Plus an undershoot of 10 on the dark side.
    const auto both = pimMake(96, 48, [](int x, int) { return x == 37 ? 40.0 : x < 40 ? 50.0 : x == 41 ? 215.0 : 200.0; });
    CHECK(pimNear(pim::pushRinging(both), 12.5, 0.05));
    // A soft edge approaches its level from the other side: its tail is not ringing.
    const auto soft = pimMake(96, 48, [](int x, int) {
        const double t[] = {50, 52, 60, 85, 125, 165, 190, 198, 200};
        return x < 36 ? 50.0 : x > 44 ? 200.0 : t[x - 36];
    });
    CHECK(pim::pushRinging(soft) < 0.01);
    // An edge inside texture has no flat surround: not measured (the metric is blind there).
    const auto textured = pimMake(96, 48, [](int x, int) { return (x < 40 ? 50.0 : 200.0) + (x % 2 ? 8.0 : -8.0); });
    CHECK(pim::pushRinging(textured) == 0.0);
    // The same overshoot in a picture enlarged twice, measured with scale 2; and on a horizontal edge.
    const auto twice = pimMake(192, 96, [](int x, int) { return x < 80 ? 50.0 : (x == 82 || x == 83) ? 215.0 : 200.0; });
    CHECK(pimNear(pim::pushRinging(twice, 2.0), 7.5, 0.05));
    const auto rows = pimMake(48, 96, [](int, int y) { return y < 40 ? 50.0 : y == 41 ? 215.0 : 200.0; });
    CHECK(pimNear(pim::pushRinging(rows), 7.5, 0.05));
    CHECK(pim::pushRinging(pimMake(8, 8, [](int, int) { return 0.0; })) == -1.0);
}
TEST(image_metrics_detail_is_the_mean_gradient){
    CHECK(pim::pushDetail(pimMake(32, 32, [](int, int) { return 90.0; })) == 0.0);
    const auto checker = pimMake(32, 32, [](int x, int y) { return (x + y) % 2 ? 20.0 : 0.0; });
    CHECK(pimNear(pim::pushDetail(checker), 40.0, 1e-6) && pimNear(pim::pushDetail(checker, 2.0), 80.0, 1e-6));
    const auto stripes = pimMake(32, 32, [](int x, int) { return x % 2 ? 20.0 : 0.0; });
    CHECK(pimNear(pim::pushDetail(stripes), 20.0, 1e-6));
    // An enlarged picture has half the gradient per pixel: the scale puts the number back.
    const auto ramp = pimMake(32, 32, [](int x, int) { return 2.0 * x; }), wide = pimMake(64, 64, [](int x, int) { return 1.0 * x; });
    CHECK(pimNear(pim::pushDetail(ramp), pim::pushDetail(wide, 2.0), 1e-6));
    CHECK(pim::pushDetail(pim::PushLumaImage{}) == -1.0);
}
TEST(image_metrics_flicker_is_change_inside_still_blocks){
    const auto a = pimMake(64, 64, [](int x, int y) { return 100.0 + (x % 16) + (y % 16); });
    CHECK(pim::pushFlicker(a, a) == 0.0);
    // Every pixel moves by 2 codes, up and down in turn: the block means stand still, the change is 2.
    auto b = pimMake(64, 64, [](int x, int y) { return 100.0 + (x % 16) + (y % 16) + ((x + y) % 2 ? 2.0 : -2.0); });
    CHECK(pimNear(pim::pushFlicker(a, b), 2.0, 1e-3));
    // A block whose content changed (its mean moved) is not "still": it is left out, the answer stays 2.
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) b.set(x, y, b.at(x, y) + 50.0f);
    CHECK(pimNear(pim::pushFlicker(a, b), 2.0, 1e-3));
    // Everything moved: no still block, no answer. Sizes that differ: no answer.
    const auto moved = pimMake(64, 64, [](int x, int y) { return 150.0 + (x % 16) + (y % 16); });
    CHECK(pim::pushFlicker(a, moved) == -1.0);
    CHECK(pim::pushFlicker(a, pimMake(32, 64, [](int, int) { return 0.0; })) == -1.0);
}
TEST(image_metrics_hud_delta_compares_output_with_the_enlarged_raw_picture){
    const auto raw = pimMake(64, 36, [](int x, int y) { return 30.0 + 2.0 * x + 1.5 * y; });
    // The same picture enlarged twice (the continuous ramp, as a bilinear enlargement gives it away from the border).
    const auto same = pimMake(128, 72, [](int x, int y) { return 30.0 + 2.0 * ((x + 0.5) / 2.0 - 0.5) + 1.5 * ((y + 0.5) / 2.0 - 0.5); });
    const pim::PushRectN middle{0.25f, 0.25f, 0.75f, 0.75f};
    CHECK(pim::pushHudDelta(raw, same, &middle, 1) < 0.05);
    // The lower right quarter raised by 10 codes: 10 inside that rectangle, 0 in the upper left one.
    const auto changed = pimMake(128, 72, [](int x, int y) {
        return 30.0 + 2.0 * ((x + 0.5) / 2.0 - 0.5) + 1.5 * ((y + 0.5) / 2.0 - 0.5) + (x >= 64 && y >= 36 ? 10.0 : 0.0);
    });
    const pim::PushRectN lowerRight{0.5f, 0.5f, 0.95f, 0.95f}, upperLeft{0.05f, 0.05f, 0.5f, 0.5f};
    CHECK(pimNear(pim::pushHudDelta(raw, changed, &lowerRight, 1), 10.0, 0.05));
    CHECK(pim::pushHudDelta(raw, changed, &upperLeft, 1) < 0.05);
    const pim::PushRectN two[] = {lowerRight, upperLeft};
    CHECK(pimNear(pim::pushHudDelta(raw, changed, two, 2), 5.0, 0.1));
    // Rectangles outside 0..1 are cut to the picture; none, or an empty one, gives no answer.
    const pim::PushRectN wild{-3.0f, -3.0f, 9.0f, 9.0f}, empty{0.4f, 0.4f, 0.4f, 0.4f};
    CHECK(pim::pushHudDelta(raw, changed, &wild, 1) > 2.0 && pim::pushHudDelta(raw, changed, &wild, 1) < 3.5);
    CHECK(pim::pushHudDelta(raw, changed, &empty, 1) == -1.0 && pim::pushHudDelta(raw, changed, nullptr, 0) == -1.0);
}
TEST(image_metrics_quality_line_has_the_contract_keys){
    pim::PushQualityStats s;
    s.frames = 6; s.width = 2560; s.height = 1440; s.hdr = true;
    for (double v : {0.5, 1.5}) s.blockiness.add(v);
    s.banding.add(12.0); s.ringing.add(0.25); s.detail.add(7.125); s.flicker.add(-1.0); s.flicker.add(0.75); s.meanLuma.add(101.5);
    CHECK(pim::pushQualityLine(s, "output") ==
          "frames=6 size=2560x1440 blockiness=1.0000 banding=12.0000 ringing=0.2500 detail=7.1250 hudDelta=-1.0000 flicker=0.7500 "
          "meanLuma=101.500 hdr=1 stage=output");
    CHECK(pim::pushQualityLine(pim::PushQualityStats{}, "raw") ==
          "frames=0 size=0x0 blockiness=-1.0000 banding=-1.0000 ringing=-1.0000 detail=-1.0000 hudDelta=-1.0000 flicker=-1.0000 "
          "meanLuma=-1.000 hdr=0 stage=raw");
    // A value that is not a number of the metric (not computed, NaN) never enters a mean.
    pim::PushMetricMean m;
    m.add(-1.0); m.add(std::nan("")); m.add(4.0);
    CHECK(m.count == 1 && m.value() == 4.0);
}
