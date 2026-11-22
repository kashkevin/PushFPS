// SPDX-License-Identifier: GPL-3.0-or-later
// Restoration stage (include/veyra/engine/PushClean.h): the CPU reference that the GPU stage mirrors.
// Synthetic patches, code values in [0, 1]; one 8-bit code = 1/255.
#include "veyra/engine/PushClean.h"

namespace clean_test {
using veyra::engine::PushCleanImage;
using veyra::engine::PushCleanSettings;
constexpr float kCode = 1.0f / 255.0f;
inline PushCleanImage grey(int w, int h, float v) {
    PushCleanImage img(w, h);
    for (auto& p : img.plane) std::fill(p.begin(), p.end(), v);
    return img;
}
inline void setGrey(PushCleanImage& img, int x, int y, float v) { for (int c = 0; c < 3; ++c) img.set(c, x, y, v); }
inline float quant8(float v) { return float(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) / 255.0f; }
inline PushCleanImage noise(int w, int h, float base, float amplitude, unsigned seed) {
    PushCleanImage img(w, h);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-amplitude, amplitude);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = base + u(rng);
            img.set(0, x, y, quant8(v + u(rng) * 0.2f)); img.set(1, x, y, quant8(v)); img.set(2, x, y, quant8(v - u(rng) * 0.2f));
        }
    return img;
}
inline PushCleanImage run(const PushCleanImage& in, int strength, bool pq = false, bool hud = true,
                          veyra::engine::PushCleanStats* stats = nullptr, int bits = 0, std::uint32_t seed = 0,
                          unsigned parts = veyra::engine::kPushCleanAllParts) {
    PushCleanSettings s; s.strength = strength; s.pq = pq; s.hudProtect = hud; s.outputBits = bits ? bits : pq ? 10 : 8;
    s.seed = seed; s.parts = parts;
    PushCleanImage out; veyra::engine::pushCleanApply(in, out, s, stats); return out;
}
inline double meanAbsDiff(const PushCleanImage& a, const PushCleanImage& b) {
    double sum = 0; std::size_t n = 0;
    for (std::size_t c = 0; c < 3; ++c) for (std::size_t i = 0; i < a.plane[c].size(); ++i) { sum += std::fabs(double(a.plane[c][i]) - double(b.plane[c][i])); ++n; }
    return n ? sum / double(n) : 0.0;
}
inline std::vector<float> lumaOf(const PushCleanImage& img) {
    std::vector<float> y(img.plane[1].size());
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = 0.2126f * img.plane[0][i] + 0.7152f * img.plane[1][i] + 0.0722f * img.plane[2][i];
    return y;
}
// A quantised horizontal gradient (one code value per `band` pixels) with the exact gradient beside it.
inline PushCleanImage bandedGradient(int w, int h, int band, PushCleanImage* exact) {
    PushCleanImage img(w, h);
    if (exact) *exact = PushCleanImage(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = 0.30f + float(x) / float(band) * kCode;
            setGrey(img, x, y, float(std::floor(v * 255.0f)) / 255.0f);
            if (exact) setGrey(*exact, x, y, v - 0.5f * kCode);
        }
    return img;
}
}

TEST(clean_strength_zero_is_bit_exact) {
    using namespace clean_test;
    const auto in = noise(64, 48, 0.5f, 0.3f, 11);
    veyra::engine::PushCleanStats stats;
    const auto out = run(in, 0, false, true, &stats);
    for (std::size_t c = 0; c < 3; ++c) CHECK(out.plane[c] == in.plane[c]);
    CHECK(stats.changed == 0);
    CHECK(stats.count[std::size_t(veyra::engine::PushCleanClass::Off)] == 64u * 48u);
    CHECK(!veyra::engine::pushCleanParams(PushCleanSettings{}).active);
    PushCleanSettings negative; negative.strength = -5;
    CHECK(!veyra::engine::pushCleanParams(negative).active);
}

TEST(clean_flat_patch_unchanged) {
    using namespace clean_test;
    const auto in = grey(48, 48, 0.42f);
    for (int strength : {1, 50, 100}) {
        const auto out = run(in, strength);
        for (std::size_t c = 0; c < 3; ++c) CHECK(out.plane[c] == in.plane[c]);
    }
}

TEST(clean_texture_left_alone) {
    using namespace clean_test;
    // Foliage-like noise of +-14 code values: busy small differences everywhere.
    const auto in = noise(96, 64, 0.45f, 14.0f * kCode, 7);
    for (int strength : {25, 50, 100}) {
        veyra::engine::PushCleanStats stats;
        const auto out = run(in, strength, false, true, &stats);
        CHECK(double(stats.changed) < 0.10 * 96.0 * 64.0);
        CHECK(meanAbsDiff(in, out) < 0.25 * kCode);
    }
}

TEST(clean_text_and_thin_lines_untouched) {
    using namespace clean_test;
    // Flat panel with 1-px and 2-px strokes (HUD text, outlines, wires) of 40 and 12 code values.
    auto in = grey(64, 64, 0.20f);
    for (int y = 8; y < 56; ++y) { setGrey(in, 10, y, 0.20f + 40 * kCode); setGrey(in, 20, y, 0.20f + 12 * kCode);
                                   setGrey(in, 30, y, 0.20f + 40 * kCode); setGrey(in, 31, y, 0.20f + 40 * kCode); }
    for (int x = 36; x < 60; ++x) { setGrey(in, x, 12, 0.20f - 12 * kCode); setGrey(in, x, 40, 0.90f); }
    for (int i = 0; i < 16; ++i) setGrey(in, 40 + i, 20 + i, 0.20f + 30 * kCode);   // diagonal stroke
    for (int strength : {50, 100}) {
        const auto out = run(in, strength);
        for (int y = 8; y < 56; ++y) for (int x : {10, 20, 30, 31})
            for (int c = 0; c < 3; ++c) CHECK(out.at(c, x, y) == in.at(c, x, y));
        for (int x = 36; x < 60; ++x) for (int c = 0; c < 3; ++c) { CHECK(out.at(c, x, 12) == in.at(c, x, 12)); CHECK(out.at(c, x, 40) == in.at(c, x, 40)); }
        for (int i = 1; i < 15; ++i) for (int c = 0; c < 3; ++c) CHECK(out.at(c, 40 + i, 20 + i) == in.at(c, 40 + i, 20 + i));
        // The background beside the strokes keeps its value (nothing bleeds out of a line).
        for (int c = 0; c < 3; ++c) { CHECK(std::fabs(out.at(c, 9, 30) - 0.20f) < 0.5f * kCode); CHECK(std::fabs(out.at(c, 11, 30) - 0.20f) < 0.5f * kCode); }
    }
}

TEST(clean_gradient_bands_reduced) {
    using namespace clean_test;
    PushCleanImage exact;
    const auto in = bandedGradient(160, 40, 12, &exact);
    std::vector<std::uint8_t> mask(in.plane[0].size(), 0);
    for (int y = 0; y < 40; ++y) for (int x = 20; x < 140; ++x) mask[std::size_t(y) * 160 + std::size_t(x)] = 1;
    const double runBefore = veyra::engine::pushCleanBandRun(lumaOf(in), mask, 160, 40);
    const auto out = run(in, 50);
    const double runAfter = veyra::engine::pushCleanBandRun(lumaOf(out), mask, 160, 40);
    CHECK(runBefore > 10.0);
    CHECK(runAfter < 0.5 * runBefore);
    // Closer to the exact gradient than the banded input (inside the mask). Judged without the grain
    // (16-bit output: 1/257 of an 8-bit code), which hides the steps left but is noise of its own size.
    const auto fine = run(in, 50, false, true, nullptr, 16);
    double before = 0, after = 0;
    for (std::size_t i = 0; i < mask.size(); ++i) if (mask[i]) {
        before += std::fabs(double(in.plane[1][i]) - double(exact.plane[1][i]));
        after += std::fabs(double(fine.plane[1][i]) - double(exact.plane[1][i]));
    }
    CHECK(after < 0.8 * before);
    // The grain is at most half an output code value around the debanded value, and the same on every frame
    // unless a seed is given.
    double worst = 0;
    for (std::size_t i = 0; i < mask.size(); ++i) worst = std::max(worst, std::fabs(double(out.plane[1][i]) - double(fine.plane[1][i])));
    CHECK(worst <= 0.51 * kCode);
    const auto again = run(in, 50), other = run(in, 50, false, true, nullptr, 0, 7);
    CHECK(again.plane[1] == out.plane[1]);
    CHECK(other.plane[1] != out.plane[1]);
}

TEST(clean_deband_follows_libplacebo) {
    using namespace clean_test;
    namespace e = veyra::engine;
    // Defaults: PL_DEBAND_DEFAULTS (threshold 3.0 / 1000, radius 16, grain 4.0 / 1000 ~ one 8-bit code).
    PushCleanSettings s; s.strength = e::kPushCleanDebandReferenceStrength;
    const auto p = e::pushCleanParams(s);
    CHECK(std::fabs(p.deband - 0.003f) < 1e-7f);
    // The threshold grows with the square of the strength: a quarter at 25, four times libplacebo's at 100.
    PushCleanSettings low = s, high = s; low.strength = 25; high.strength = 100;
    CHECK(std::fabs(e::pushCleanParams(low).deband - 0.00075f) < 1e-7f && std::fabs(e::pushCleanParams(high).deband - 0.012f) < 1e-7f);
    CHECK(p.radius16 == 256u);
    CHECK(std::fabs(p.grain - 1.0f / 255.0f) < 1e-7f && std::fabs(p.grain - 0.004f) < 1e-4f);
    // The generator (pcg3d as in sh_prng): values from an independent implementation (Python integers).
    e::PushCleanRandom r{3u, 5u, 0u};
    e::pushCleanRandomStep(r);
    CHECK(r.x == 475257306u && r.y == 1062618912u && r.z == 1914884155u);
    e::pushCleanRandomStep(r);
    CHECK(r.x == 1988445481u && r.y == 2571549408u && r.z == 4097656531u);
    CHECK(e::pushCleanRandomFloat(0xFFFFFFFFu) < 1.0f && e::pushCleanRandomFloat(0u) == 0.0f);
    // Tap offsets: never beyond the radius, every distance and direction of the quarter circle used.
    int farthest = 0; bool axisX = false, axisY = false, zero = false;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        e::PushCleanRandom q{std::uint32_t(x), std::uint32_t(y), 0u};
        e::pushCleanRandomStep(q);
        int dx = -1, dy = -1;
        e::pushCleanDebandOffset(q, p.radius16, dx, dy);
        CHECK(dx >= 0 && dy >= 0 && dx * dx + dy * dy <= 16 * 16 + 16);
        farthest = std::max(farthest, dx * dx + dy * dy);
        axisX = axisX || (dx >= 12 && dy <= 1); axisY = axisY || (dy >= 12 && dx <= 1); zero = zero || (dx == 0 && dy == 0);
    }
    CHECK(farthest >= 14 * 14 && axisX && axisY && zero);
    // One component: within the threshold the tap mean wins, beyond it the pixel stays (libplacebo's mix()).
    bool changed = false;
    CHECK(e::pushCleanDebandComponent(0.500f, 0.502f, 0.502f, 0.502f, 0.502f, 0.003f, changed) == 0.502f && changed);
    changed = false;
    CHECK(e::pushCleanDebandComponent(0.500f, 0.504f, 0.504f, 0.504f, 0.504f, 0.003f, changed) == 0.500f && !changed);
    CHECK(e::pushCleanDebandComponent(0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.003f, changed) == 0.25f && !changed);
    // Grain: half a code value either way at most, nothing at true black, less than its value near black.
    CHECK(e::pushCleanGrain(0.0f, p.grain, 0.99f) == 0.0f);
    CHECK(std::fabs(e::pushCleanGrain(0.5f, p.grain, 0.0f) - (0.5f - 0.5f * p.grain)) < 1e-7f);
    CHECK(e::pushCleanGrain(0.001f, p.grain, 0.0f) > 0.0f);
    // A black picture with a faint step stays free of noise where it is black.
    auto dark = grey(64, 32, 0.0f);
    for (int y = 0; y < 32; ++y) for (int x = 32; x < 64; ++x) setGrey(dark, x, y, kCode);
    const auto out = run(dark, 50);
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) for (int c = 0; c < 3; ++c) CHECK(out.at(c, x, y) >= 0.0f && out.at(c, x, y) <= kCode);
    for (int y = 0; y < 32; ++y) for (int c = 0; c < 3; ++c) CHECK(out.at(c, 2, y) == 0.0f);
}

TEST(clean_block_steps_deblocked) {
    using namespace clean_test;
    // Smooth picture coded as 8x8 blocks of constant value: steps of up to +-4 codes at the block edges,
    // shifted by 3 pixels so the filter cannot rely on a grid at multiples of 8.
    PushCleanImage in(96, 64), exact(96, 64);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 96; ++x) {
        const float v = 0.35f + 0.05f * std::sin(float(x) * 0.05f) + 0.04f * std::cos(float(y) * 0.07f);
        setGrey(exact, x, y, v);
        const int bx = ((x + 3) / 8) * 8 - 3 + 4, by = ((y + 3) / 8) * 8 - 3 + 4;
        setGrey(in, x, y, quant8(0.35f + 0.05f * std::sin(float(bx) * 0.05f) + 0.04f * std::cos(float(by) * 0.07f)));
    }
    const auto out = run(in, 60);
    // Blockiness measured on the shifted grid (crop 5 pixels so the block edges fall on multiples of 8).
    const auto crop = [](const std::vector<float>& y) { std::vector<float> c; for (int r = 5; r < 61; ++r) for (int x = 5; x < 93; ++x) c.push_back(y[std::size_t(r) * 96 + std::size_t(x)]); return c; };
    const double before = veyra::engine::pushCleanBlockiness(crop(lumaOf(in)), 88, 56);
    const double after = veyra::engine::pushCleanBlockiness(crop(lumaOf(out)), 88, 56);
    CHECK(before > 3.0);
    CHECK(after < 0.6 * before);
    CHECK(meanAbsDiff(out, exact) < meanAbsDiff(in, exact));
}

TEST(clean_ringing_beside_edge_reduced) {
    using namespace clean_test;
    // A strong vertical edge (dark | bright) with decaying ripples of +-3 codes on both flat sides.
    PushCleanImage in(64, 32), exact(64, 32);
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) {
        const float base = x < 32 ? 0.15f : 0.75f;
        setGrey(exact, x, y, base);
        const int d = x < 32 ? 31 - x : x - 32;
        const float ripple = d < 7 ? 3.0f * kCode * (d % 2 ? 1.0f : -1.0f) * float(7 - d) / 7.0f * (y % 3 == 0 ? 1.0f : 0.7f) : 0.0f;
        setGrey(in, x, y, quant8(base + ripple));
    }
    const auto out = run(in, 60);
    double before = 0, after = 0;
    for (int y = 0; y < 32; ++y) for (int x : {26, 27, 28, 29, 34, 35, 36, 37}) {
        before += std::fabs(double(in.at(1, x, y)) - double(exact.at(1, x, y)));
        after += std::fabs(double(out.at(1, x, y)) - double(exact.at(1, x, y)));
    }
    CHECK(after < 0.7 * before);
    for (int y = 0; y < 32; ++y) for (int x : {31, 32}) CHECK(out.at(1, x, y) == in.at(1, x, y));   // the edge itself
}

TEST(clean_never_leaves_neighbourhood_range) {
    using namespace clean_test;
    // Mixed content: blocks, gradient, noise, strokes.
    PushCleanImage in(80, 60);
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> code(-3, 3);
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x) {
        float v = 0.2f + 0.004f * float(x) + float(((x / 8) + (y / 8)) % 3) * 2.0f * kCode + float(code(rng)) * kCode * (y > 30 ? 1.0f : 0.0f);
        if (x == 40) v = 0.9f;
        for (int c = 0; c < 3; ++c) in.set(c, x, y, quant8(v + float(c) * 0.01f));
    }
    // Every part alone and the combinations a user can pick, SDR and PQ: no halo, no overshoot. Without the
    // debanding (whose taps reach 16 pixels) the range is that of the 9x9 window the other parts read.
    namespace e = veyra::engine;
    const unsigned partSets[] = {e::kPushCleanDeband, e::kPushCleanDeblock, e::kPushCleanDering, e::kPushCleanGrain,
                                 e::kPushCleanDefaultParts, e::kPushCleanAllParts};
    // The range of each pixel's window, for the two reaches, computed once.
    PushCleanImage lo4(80, 60), hi4(80, 60), lo16(80, 60), hi16(80, 60);
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x) for (int c = 0; c < 3; ++c) {
        float a4 = in.at(c, x, y), b4 = a4, a16 = a4, b16 = a4;
        for (int j = -16; j <= 16; ++j) for (int i = -16; i <= 16; ++i) {
            const float v = in.at(c, x + i, y + j);
            a16 = std::min(a16, v); b16 = std::max(b16, v);
            if (i >= -4 && i <= 4 && j >= -4 && j <= 4) { a4 = std::min(a4, v); b4 = std::max(b4, v); }
        }
        lo4.set(c, x, y, a4); hi4.set(c, x, y, b4); lo16.set(c, x, y, a16); hi16.set(c, x, y, b16);
    }
    for (unsigned parts : partSets)
        for (bool pq : {false, true})
            for (int strength : {10, 50, 100}) {
                const auto out = run(in, strength, pq, true, nullptr, 0, 7u, parts);
                const bool far = (parts & e::kPushCleanDeband) != 0;
                const PushCleanImage& lo = far ? lo16 : lo4;
                const PushCleanImage& hi = far ? hi16 : hi4;
                for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x) for (int c = 0; c < 3; ++c) {
                    const float v = out.at(c, x, y);
                    CHECK(v >= lo.at(c, x, y) && v <= hi.at(c, x, y));
                }
            }
}

TEST(clean_nearly_idempotent) {
    using namespace clean_test;
    auto in = bandedGradient(96, 48, 9, nullptr);
    const auto tex = noise(96, 48, 0.0f, 2.0f * kCode, 3);
    for (std::size_t c = 0; c < 3; ++c) for (std::size_t i = 0; i < in.plane[c].size(); ++i)
        in.plane[c][i] = quant8(in.plane[c][i] + (i % 96 > 48 ? tex.plane[c][i] : 0.0f));
    const auto once = run(in, 50), twice = run(once, 50);
    const double first = meanAbsDiff(in, once), second = meanAbsDiff(once, twice);
    CHECK(first > 0.0);
    // The debanding draws its taps and its grain again on every application, so a second pass is not an
    // identity: the tolerance is that it moves the picture less than the first and by less than half an
    // 8-bit code value on average.
    CHECK(second < 0.8 * first);
    CHECK(second < 0.5 * kCode);
}

TEST(clean_hdr_highlights_preserved) {
    using namespace clean_test;
    const float white = veyra::engine::pushCleanPqReferenceWhite();
    CHECK(white > 0.57f && white < 0.59f);   // 203 cd/m2 in PQ
    // A PQ picture: mid-grey wall with specular highlights (1000 cd/m2) and a flat bright window (600 cd/m2).
    const float wall = veyra::engine::pushCleanPqEncode(50.0f / 80.0f);
    const float spec = veyra::engine::pushCleanPqEncode(1000.0f / 80.0f);
    const float window = veyra::engine::pushCleanPqEncode(600.0f / 80.0f);
    CHECK(spec > white && window > white);
    auto in = noise(64, 48, wall, 1.5f * kCode, 9);
    for (int y = 30; y < 44; ++y) for (int x = 40; x < 60; ++x) setGrey(in, x, y, window);
    const std::array<std::array<int, 2>, 4> spots{{{{8, 8}}, {{20, 12}}, {{12, 30}}, {{30, 20}}}};
    for (const auto& s : spots) setGrey(in, s[0], s[1], spec);
    for (int strength : {50, 100}) {
        const auto out = run(in, strength, true);
        for (const auto& s : spots) for (int c = 0; c < 3; ++c) CHECK(out.at(c, s[0], s[1]) == spec);
        for (int y = 32; y < 42; ++y) for (int x = 42; x < 58; ++x) for (int c = 0; c < 3; ++c) CHECK(out.at(c, x, y) == window);
        float peak = 0.0f; for (const auto& p : out.plane) for (float v : p) peak = std::max(peak, v);
        CHECK(peak == spec);
    }
    // Round trip of the colour boundary helpers (used by the GPU tests).
    for (float nits : {0.1f, 100.0f, 203.0f, 1000.0f, 4000.0f}) {
        const float lin = nits / 80.0f;
        CHECK(std::fabs(veyra::engine::pushCleanPqDecode(veyra::engine::pushCleanPqEncode(lin)) - lin) < 1e-3f * std::max(1.0f, lin));
    }
    CHECK(std::fabs(veyra::engine::pushCleanSrgbDecode(veyra::engine::pushCleanSrgbEncode(0.18f)) - 0.18f) < 1e-5f);
    // Half precision (the graph's FP16 textures): known words, round to nearest even, every word round trips.
    namespace e = veyra::engine;
    CHECK(e::pushCleanToHalf(0.0f) == 0x0000u && e::pushCleanToHalf(1.0f) == 0x3C00u && e::pushCleanToHalf(-2.0f) == 0xC000u);
    CHECK(e::pushCleanToHalf(0.5f) == 0x3800u && e::pushCleanToHalf(65504.0f) == 0x7BFFu && e::pushCleanToHalf(1.0e6f) == 0x7C00u);
    CHECK(e::pushCleanToHalf(5.9604645e-8f) == 0x0001u && e::pushCleanToHalf(1.0e-9f) == 0x0000u);
    CHECK(e::pushCleanToHalf(1.0f + 1.0f / 2048.0f) == 0x3C00u && e::pushCleanToHalf(1.0f + 3.0f / 2048.0f) == 0x3C02u);   // ties to even
    CHECK(e::pushCleanToHalf(1.0f + 1.1f / 2048.0f) == 0x3C01u);
    CHECK(e::pushCleanFromHalf(0x3C00u) == 1.0f && e::pushCleanFromHalf(0x3555u) == 0.333251953125f && e::pushCleanFromHalf(0x0001u) == 5.9604645e-8f);
    bool roundTrip = true;
    for (std::uint32_t w = 0; w < 0x7C00u; ++w)
        roundTrip = roundTrip && e::pushCleanToHalf(e::pushCleanFromHalf(static_cast<std::uint16_t>(w))) == w &&
                    e::pushCleanToHalf(-e::pushCleanFromHalf(static_cast<std::uint16_t>(w))) == (w | 0x8000u);
    CHECK(roundTrip);
}

// HDR (PQ): colour is kept. A saturated textured area and saturated edges are not written at all; in a smooth
// saturated area (more than 16 pixels from anything else) each channel's mean moves by less than a tenth
// of a code value and every value stays inside that area's own input range, so hue and saturation are kept where the stage does work.
TEST(clean_hdr_colour_kept_outside_smooth_regions) {
    using namespace clean_test;
    namespace e = veyra::engine;
    const float base[3] = {e::pushCleanPqEncode(400.0f / 80.0f), e::pushCleanPqEncode(60.0f / 80.0f), e::pushCleanPqEncode(15.0f / 80.0f)};
    PushCleanImage in(128, 64);
    std::mt19937 rng(17);
    std::uniform_int_distribution<int> coarse(-14, 14), fine(-1, 1);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x) {
        const bool textured = x < 40;
        const int offset = textured ? coarse(rng) : (fine(rng) * (x % 3 == 0 ? 1 : 0));
        for (int c = 0; c < 3; ++c) {
            float v = base[c] + float(offset) * kCode * (c == 1 ? 1.0f : 0.8f);
            if (x >= 104 && x < 108) v = base[c] * 0.4f;   // a dark saturated bar: strong edges on both sides
            in.set(c, x, y, quant8(v));
        }
    }
    const unsigned partSets[] = {e::kPushCleanDefaultParts, e::kPushCleanAllParts};
    for (unsigned parts : partSets)
        for (int strength : {50, 100}) {
            const auto out = run(in, strength, true, true, nullptr, 10, 3u, parts);
            for (int y = 4; y < 60; ++y) {
                for (int x = 4; x < 36; ++x) for (int c = 0; c < 3; ++c) CHECK(out.at(c, x, y) == in.at(c, x, y));
                for (int x = 103; x < 109; ++x) for (int c = 0; c < 3; ++c) CHECK(out.at(c, x, y) == in.at(c, x, y));
            }
            int changed = 0;
            for (int y = 20; y < 44; ++y) for (int x = 58; x < 86; ++x) changed += out.at(1, x, y) != in.at(1, x, y) ? 1 : 0;
            CHECK(changed > 24 * 28 / 4);   // the stage does work there
            for (int c = 0; c < 3; ++c) {
                double before = 0.0, after = 0.0; float lo = 1.0f, hi = 0.0f;
                for (int y = 20; y < 44; ++y) for (int x = 58; x < 86; ++x) {
                    before += in.at(c, x, y); after += out.at(c, x, y);
                    lo = std::min(lo, in.at(c, x, y)); hi = std::max(hi, in.at(c, x, y));
                }
                for (int y = 20; y < 44; ++y) for (int x = 58; x < 86; ++x) CHECK(out.at(c, x, y) >= lo && out.at(c, x, y) <= hi);
                CHECK(std::fabs(after - before) / (24.0 * 28.0) < 0.1 * double(kCode));
            }
        }
}

// Independent review S2: a faint 1-pixel line (4.5 to 6 code values) on a flat background must not leave a
// halo: no background pixel 2 or more pixels from it moves by more than the debanding grain (half a code
// value), and the background keeps its mean, at every strength and with all parts.
TEST(clean_faint_line_leaves_no_halo) {
    using namespace clean_test;
    namespace e = veyra::engine;
    for (float step : {4.5f, 6.0f}) {
        PushCleanImage in = grey(96, 64, quant8(0.35f));
        for (int y = 0; y < 64; ++y) setGrey(in, 48, y, quant8(0.35f + step * kCode));
        const unsigned partSets[] = {e::kPushCleanDefaultParts, e::kPushCleanAllParts};
        for (unsigned parts : partSets)
            for (int strength : {50, 75, 100}) {
                const auto out = run(in, strength, false, true, nullptr, 0, 11u, parts);
                double shift = 0.0; int count = 0;
                for (int y = 0; y < 64; ++y) for (int x = 0; x < 96; ++x) {
                    if (std::abs(x - 48) < 2) continue;
                    for (int c = 0; c < 3; ++c) CHECK(std::fabs(out.at(c, x, y) - in.at(c, x, y)) <= 0.51f * kCode);
                    shift += double(out.at(1, x, y) - in.at(1, x, y)); ++count;
                }
                CHECK(std::fabs(shift / double(count)) < 0.05 * double(kCode));
                for (int y = 0; y < 64; ++y) CHECK(out.at(1, 48, y) == in.at(1, 48, y));   // the line itself is kept
            }
    }
}

TEST(clean_hud_switch_and_params) {
    using namespace clean_test;
    PushCleanSettings a; a.strength = 20;
    PushCleanSettings b; b.strength = 80;
    const auto pa = veyra::engine::pushCleanParams(a), pb = veyra::engine::pushCleanParams(b);
    CHECK(pa.active && pb.active);
    CHECK(pb.maxStep > pa.maxStep && pb.grainTau > pa.grainTau && pb.deband > pa.deband && pb.texture > pa.texture);
    PushCleanSettings loose = b; loose.hudProtect = false;
    CHECK(veyra::engine::pushCleanParams(loose).line > pb.line);
    PushCleanSettings over; over.strength = 400;
    PushCleanSettings full; full.strength = 100;
    CHECK(veyra::engine::pushCleanParams(over).maxStep == veyra::engine::pushCleanParams(full).maxStep);
    const auto k = veyra::engine::pushCleanConstants(pb);
    CHECK(k.flat == pb.flat && k.deband == pb.deband && k.grain == pb.grain && k.radius16 == 256u && k.seed == 0u);
    CHECK(k.parts == veyra::engine::kPushCleanAllParts && k.width == 0u && k.stage == 0u);
    CHECK(veyra::engine::pushCleanConstants(veyra::engine::pushCleanParams(PushCleanSettings{})).parts == 0u);
    PushCleanSettings hdr = b; hdr.pq = true; hdr.outputBits = 10; hdr.seed = 9;
    CHECK(std::fabs(veyra::engine::pushCleanParams(hdr).grain - 1.0f / 1023.0f) < 1e-9f);
    CHECK(veyra::engine::pushCleanParams(hdr).wg == 0.6780f && veyra::engine::pushCleanParams(hdr).seed == 9u);
    CHECK(veyra::engine::pushCleanStatsLine(300, 40, 1920, 1080, 0.2, 0.35, true, false, veyra::engine::kPushCleanAllParts) ==
          "frames=300 strength=40 size=1920x1080 gpuMsP50=0.200 gpuMsP95=0.350 hudProtect=1 hdr=0 parts=15");
}

TEST(clean_parts_switch_independently) {
    using namespace clean_test;
    namespace e = veyra::engine;
    // A picture with something for every part: a banded gradient, block steps, ripples beside an edge, grain.
    PushCleanImage in(128, 64);
    std::mt19937 rng(21);
    std::uniform_int_distribution<int> code(-1, 1);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x) {
        float v;
        if (y < 16) v = 0.30f + float(x / 10) * kCode;                                           // bands
        else if (y < 32) v = 0.40f + float((x / 8) % 3) * 3.0f * kCode;                          // block steps
        else if (y < 48) v = (x < 64 ? 0.15f : 0.75f) + ((x > 56 && x < 72 && x != 63 && x != 64) ? (x % 2 ? 3.0f : -3.0f) * kCode : 0.0f);
        else v = 0.50f + float(code(rng)) * kCode;                                               // grain
        setGrey(in, x, y, quant8(v));
    }
    const auto count = [&](unsigned parts, e::PushCleanClass cls) {
        e::PushCleanStats stats; (void)run(in, 60, false, true, &stats, 0, 0, parts); return stats.count[std::size_t(cls)];
    };
    e::PushCleanStats none; const auto off = run(in, 60, false, true, &none, 0, 0, 0u);
    for (std::size_t c = 0; c < 3; ++c) CHECK(off.plane[c] == in.plane[c]);
    CHECK(none.count[std::size_t(e::PushCleanClass::Off)] == 128u * 64u);
    // Deband alone (the part ported from libplacebo): no deblock, no smooth.
    CHECK(count(e::kPushCleanDeband, e::PushCleanClass::Deband) > 0);
    CHECK(count(e::kPushCleanDeband, e::PushCleanClass::Deblock) == 0 && count(e::kPushCleanDeband, e::PushCleanClass::Smooth) == 0);
    // Each of the stage's own parts alone.
    CHECK(count(e::kPushCleanDeblock, e::PushCleanClass::Deblock) > 0);
    CHECK(count(e::kPushCleanDeblock, e::PushCleanClass::Deband) == 0 && count(e::kPushCleanDeblock, e::PushCleanClass::Smooth) == 0);
    CHECK(count(e::kPushCleanDering, e::PushCleanClass::Smooth) > 0 && count(e::kPushCleanDering, e::PushCleanClass::Deblock) == 0);
    CHECK(count(e::kPushCleanGrain, e::PushCleanClass::Smooth) > 0 && count(e::kPushCleanGrain, e::PushCleanClass::Deband) == 0);
    // Dering works only beside the strong edge, grain smoothing only away from it.
    const auto ring = run(in, 60, false, true, nullptr, 0, 0, e::kPushCleanDering), grain = run(in, 60, false, true, nullptr, 0, 0, e::kPushCleanGrain);
    bool ringFar = false, grainNear = false;
    for (int y = 50; y < 62; ++y) for (int x = 4; x < 124; ++x) ringFar = ringFar || ring.at(1, x, y) != in.at(1, x, y);
    for (int y = 34; y < 46; ++y) for (int x = 59; x < 69; ++x) grainNear = grainNear || grain.at(1, x, y) != in.at(1, x, y);
    CHECK(!ringFar && !grainNear);
    // The noise follows the frame number unless the static switch is set; the switch is not a filter part.
    CHECK(e::pushCleanFrameSeed(e::kPushCleanAllParts, 41) == 41u && e::pushCleanFrameSeed(e::kPushCleanAllParts | e::kPushCleanStaticNoise, 41) == 0u);
    PushCleanSettings st; st.strength = 50; st.parts = e::kPushCleanStaticNoise;
    CHECK(!e::pushCleanParams(st).active);
    // The live values the interface sets and the graph reads.
    const auto before = e::pushCleanLive();
    CHECK(before.strength == 0 && before.hudProtect && before.parts == e::kPushCleanDefaultParts);
    CHECK(e::kPushCleanDefaultParts == 13u && (e::kPushCleanDefaultParts & e::kPushCleanDeblock) == 0u);
    e::setPushCleanLive(140, false, e::kPushCleanDeband | e::kPushCleanStaticNoise | 64u);
    const auto live = e::pushCleanLive();
    CHECK(live.strength == 100 && !live.hudProtect && live.parts == (e::kPushCleanDeband | e::kPushCleanStaticNoise));
    e::setPushCleanLive(before.strength, before.hudProtect, before.parts);
}

TEST(clean_final_write_dither_follows_libplacebo) {
    using namespace clean_test;
    namespace e = veyra::engine;
    // The 16 x 16 ordered pattern: every bias k / 256 exactly once, repeating every 16 pixels.
    std::array<int, 256> seen{};
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        const float bias = e::pushCleanOrderedBias(x, y);
        CHECK(bias >= 0.0f && bias < 1.0f && bias * 256.0f == std::floor(bias * 256.0f));
        ++seen[std::size_t(bias * 256.0f)];
        CHECK(e::pushCleanOrderedBias(x + 16, y + 32) == bias);
    }
    for (int count : seen) CHECK(count == 1);
    CHECK(e::pushCleanOrderedBias(0, 0) == 0.0f);
    // Black stays black, a value between two codes keeps its mean, the result is always a code value.
    const float between = (100.0f + 0.3f) / 255.0f;
    double sum = 0;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        CHECK(e::pushCleanDitherRound(0.0f, x, y, 255.0f) == 0.0f);
        CHECK(e::pushCleanDitherRound(4.0e-6f, x, y, 255.0f) == 0.0f);
        const float v = e::pushCleanDitherRound(between, x, y, 255.0f) * 255.0f;
        CHECK(std::fabs(v - 100.0f) < 1e-3f || std::fabs(v - 101.0f) < 1e-3f);
        sum += double(v);
    }
    CHECK(std::fabs(sum / 256.0 - 100.3) < 0.01);
    // Review S3: a code value slightly off (as after the FP16 round trip) is written as that code everywhere.
    for (int k : {1, 37, 128, 254}) for (float off : {-0.03f, 0.0f, 0.03f})
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
            CHECK(std::fabs(e::pushCleanDitherRound((float(k) + off) / 255.0f, x, y, 255.0f) * 255.0f - float(k)) < 1e-3f);
    CHECK(std::fabs(e::pushCleanDitherRound(0.5004f, 3, 5, 1023.0f) * 1023.0f - std::round(e::pushCleanDitherRound(0.5004f, 3, 5, 1023.0f) * 1023.0f)) < 1e-3f);
    // A smooth gradient (what the debanding aims for) written to 8 bits: plain rounding makes runs of one
    // code again (bands), the dither does not.
    PushCleanImage smooth;
    (void)bandedGradient(160, 40, 12, &smooth);
    std::vector<std::uint8_t> mask(smooth.plane[0].size(), 0);
    for (int y = 0; y < 40; ++y) for (int x = 20; x < 140; ++x) mask[std::size_t(y) * 160 + std::size_t(x)] = 1;
    std::vector<float> rounded(smooth.plane[1].size()), dithered(smooth.plane[1].size());
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 160; ++x) {
        const std::size_t i = std::size_t(y) * 160 + std::size_t(x);
        rounded[i] = quant8(smooth.plane[1][i]);
        dithered[i] = e::pushCleanDitherRound(smooth.plane[1][i], x, y, 255.0f);
    }
    const double runRounded = e::pushCleanBandRun(rounded, mask, 160, 40), runDithered = e::pushCleanBandRun(dithered, mask, 160, 40);
    CHECK(runRounded > 10.0);
    CHECK(runDithered < 0.4 * runRounded);
    // The contour index says the same: band edges with flat runs on both sides exist only without the dither;
    // a flat picture and a noisy one have none.
    const double contoursRounded = e::pushCleanContourIndex(rounded, mask, 160, 40), contoursDithered = e::pushCleanContourIndex(dithered, mask, 160, 40);
    CHECK(contoursRounded > 50.0);
    CHECK(contoursDithered < 0.1 * contoursRounded);
    const std::vector<std::uint8_t> everywhere(160 * 40, 1);
    CHECK(e::pushCleanContourIndex(lumaOf(grey(160, 40, 0.4f)), everywhere, 160, 40) == 0.0);
    CHECK(e::pushCleanContourIndex(lumaOf(noise(160, 40, 0.4f, 6.0f * kCode, 2)), everywhere, 160, 40) < 1.0);
}
