// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {
// No-reference picture metrics for the in-app pipeline probe (S5_CONTRACT.md interface item 7; sprint 5,
// stream "pipeline"). Portable and pure: tests in tests/remoteplay/ImageMetricsTests.inc on synthetic
// pictures. Every metric is a number on 8-bit-equivalent luma ("codes": 0..255 of the ENCODED signal, sRGB
// for SDR and PQ for HDR, fractions kept), and higher means more of that defect, except `detail`.
// They are software measures of one still or moving scene. They are not a judgement of beauty, they have
// no reference picture, and each is blind to the things listed with it. Compare the same scene between
// arms; never read one number alone. Nothing here has run on a real stream.
//
//   blockiness  how much stronger the luma steps are on a regular grid (the encoder's 8-pixel transform
//               grid, `period` pixels after scaling) than everywhere else: mean step on the grid / mean step
//               off it, minus 1, best grid phase, rows and columns averaged; 0 = no grid visible.
//               Blind to: block edges that are not on a regular grid (motion-compensated blocks, HEVC's
//               larger and smaller transforms only partly), a grid blurred away by a scaler with a
//               non-integer factor, and blocking hidden by grain. A picture with real 8-pixel structure
//               (a tiled wall, a HUD grid) scores too.
//   banding     per mille of neighbour positions that are a band edge: a step of 0.75..3 codes between two
//               runs of 4 pixels that are each flat (range <= 0.26 codes). Rows and columns.
//               Blind to: bands with larger steps (taken for edges), bands narrower than 4 pixels, banding
//               under grain or dither (which is the purpose of dither). Flat-shaded art with small steps
//               (a HUD gradient) scores too.
//   ringing     mean overshoot, in codes, next to strong edges that have a flat surround: for an edge of at
//               least 24 codes, on each side whose far zone (6..9 pixels away) is flat (range <= 3 codes),
//               the largest excursion of the near zone (2..5 pixels) beyond the far zone's mean, away from
//               the other side of the edge. A soft edge's tail does not count: it approaches the far level
//               from the other side. A picture enlarged by `scale` is read in groups of round(scale) pixels,
//               so the distances stay source pixels (1.33x is read as it is). 0 = none found.
//               Blind to: ringing inside texture (no flat surround), ripples closer than 2 pixels, mosquito
//               noise of both signs (half of it counts), halos wider than 5 pixels.
//   detail      mean luma gradient (|dx| + |dy|, codes per pixel) times `scale`, so that a picture enlarged
//               by an ideal interpolator keeps roughly its number. Higher = more fine structure.
//               Blind to what the structure is: detail, grain, noise, ringing and sharpening halos all raise
//               it. Read it together with ringing and flicker.
//   flicker     between two frames: mean absolute luma change, in codes, inside 16x16 blocks that stand
//               still (block means within 0.5 codes and a mean change of at most 6 codes).
//               Blind to: instability in moving areas, brightness pumping of whole areas (their means
//               move, so they are not "still"), and anything when the two frames are the same picture
//               (a 30 FPS game in a 60 FPS stream repeats every picture: such pairs read as 0).
//   hudDelta    mean absolute difference, in codes, between the output and the raw picture (bilinearly
//               scaled to the output size) inside given rectangles (0..1 of the picture).
//               Blind to colour-only changes. Any upscaler differs from a bilinear enlargement, so the
//               number is not 0 for a faithful picture: compare arms, do not compare with 0.

// Luma picture: codes x 64 in 16 bits (0..16320), so a 3840x2160 frame is 16.6 MB.
struct PushLumaImage {
    int width = 0, height = 0;
    std::vector<std::uint16_t> q;

    bool valid() const { return width > 0 && height > 0 && q.size() == std::size_t(width) * std::size_t(height); }
    void resize(int w, int h) {
        width = std::max(w, 0);
        height = std::max(h, 0);
        q.assign(std::size_t(width) * std::size_t(height), std::uint16_t{0});
    }
    float at(int x, int y) const { return float(q[std::size_t(y) * std::size_t(width) + std::size_t(x)]) * (1.0f / 64.0f); }
    void set(int x, int y, float codes) {
        const float clamped = codes >= 0.0f ? (codes <= 255.0f ? codes : 255.0f) : 0.0f;  // NaN becomes 0
        q[std::size_t(y) * std::size_t(width) + std::size_t(x)] = static_cast<std::uint16_t>(clamped * 64.0f + 0.5f);
    }
};
struct PushRectN {
    float left = 0, top = 0, right = 0, bottom = 0;  // 0..1 of the picture
};

// --- encodings -----------------------------------------------------------------------------------------
inline float pushSrgbEncode(float linear) {
    const float c = linear >= 0.0f ? (linear <= 1.0f ? linear : 1.0f) : 0.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
// SMPTE ST 2084: nits (0..10000) to the PQ signal 0..1.
inline float pushPqEncode(float nits) {
    const float y = (nits >= 0.0f ? (nits <= 10000.0f ? nits : 10000.0f) : 0.0f) / 10000.0f;
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(y, m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}
// IEEE 754 half to float (the graph's FP16 textures), without intrinsics.
inline float pushHalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (std::uint32_t(h) & 0x8000u) << 16;
    std::uint32_t exponent = (std::uint32_t(h) >> 10) & 0x1Fu;
    std::uint32_t mantissa = std::uint32_t(h) & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while (!(mantissa & 0x400u)) { mantissa <<= 1; --exponent; }
            mantissa &= 0x3FFu;
            bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// --- read-back rows to luma ----------------------------------------------------------------------------
// 8-bit RGBA or BGRA of an SDR picture (already sRGB encoded): BT.709 weights on the encoded values.
inline bool pushLumaFromRgba8(const std::uint8_t* data, std::size_t pitch, int width, int height, bool bgr, PushLumaImage& out) {
    if (!data || width <= 0 || height <= 0 || pitch < std::size_t(width) * 4) return false;
    out.resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = data + std::size_t(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + std::size_t(x) * 4;
            const float r = float(p[bgr ? 2 : 0]), g = float(p[1]), b = float(p[bgr ? 0 : 2]);
            out.set(x, y, 0.2126f * r + 0.7152f * g + 0.0722f * b);
        }
    }
    return true;
}
// R10G10B10A2 holding HDR10 (PQ, BT.2020): BT.2020 weights on the 10-bit PQ codes, divided by 4.
inline bool pushLumaFromRgb10a2(const std::uint8_t* data, std::size_t pitch, int width, int height, PushLumaImage& out) {
    if (!data || width <= 0 || height <= 0 || pitch < std::size_t(width) * 4) return false;
    out.resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = data + std::size_t(y) * pitch;
        for (int x = 0; x < width; ++x) {
            std::uint32_t v = 0;
            std::memcpy(&v, row + std::size_t(x) * 4, 4);
            const float r = float(v & 0x3FFu), g = float((v >> 10) & 0x3FFu), b = float((v >> 20) & 0x3FFu);
            out.set(x, y, (0.2627f * r + 0.6780f * g + 0.0593f * b) * (255.0f / 1023.0f));
        }
    }
    return true;
}
// FP16 RGBA, linear BT.709 in scRGB units (1 = 80 nits): the graph's raw console picture and an scRGB output.
//   hdr = false: an SDR picture: sRGB encoding, BT.709 weights.
//   hdr = true:  to BT.2020, `gain` (203/80 for an SDR source shown in HDR, else 1), PQ per component,
//                BT.2020 weights: the same encoding as an HDR10 output, so the two can be compared.
inline bool pushLumaFromRgba16f(const std::uint8_t* data, std::size_t pitch, int width, int height, bool hdr, float gain, PushLumaImage& out) {
    if (!data || width <= 0 || height <= 0 || pitch < std::size_t(width) * 8) return false;
    out.resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = data + std::size_t(y) * pitch;
        for (int x = 0; x < width; ++x) {
            std::uint16_t h[3]{};
            std::memcpy(h, row + std::size_t(x) * 8, 6);
            const float r = pushHalfToFloat(h[0]), g = pushHalfToFloat(h[1]), b = pushHalfToFloat(h[2]);
            if (!hdr) {
                out.set(x, y, 255.0f * (0.2126f * pushSrgbEncode(r) + 0.7152f * pushSrgbEncode(g) + 0.0722f * pushSrgbEncode(b)));
            } else {
                const float k = 80.0f * gain;
                const float r2 = k * (0.6274f * r + 0.3293f * g + 0.0433f * b);
                const float g2 = k * (0.0691f * r + 0.9195f * g + 0.0114f * b);
                const float b2 = k * (0.0164f * r + 0.0880f * g + 0.8956f * b);
                out.set(x, y, 255.0f * (0.2627f * pushPqEncode(r2) + 0.6780f * pushPqEncode(g2) + 0.0593f * pushPqEncode(b2)));
            }
        }
    }
    return true;
}

// --- metrics -------------------------------------------------------------------------------------------
inline double pushMeanLuma(const PushLumaImage& img) {
    if (!img.valid()) return -1.0;
    double sum = 0.0;
    for (const std::uint16_t v : img.q) sum += double(v);
    return sum / (64.0 * double(img.q.size()));
}

namespace detail_metrics {
// Grid strength of one step profile: the best phase's mean step on the grid over the mean step off it, minus 1.
inline double gridExcess(const std::vector<double>& profile, double period) {
    const std::size_t n = profile.size();
    if (period < 2.0 || double(n) < period * 4.0) return -1.0;
    double total = 0.0;
    for (const double v : profile) total += v;
    double best = 0.0;
    const int phases = int(std::ceil(period));
    for (int phase = 0; phase < phases; ++phase) {
        double onSum = 0.0;
        std::size_t onCount = 0;
        std::size_t last = n;  // no index yet
        for (int k = 0;; ++k) {
            const double position = double(phase) + double(k) * period;
            const std::size_t index = std::size_t(position + 0.5);
            if (index >= n) break;
            if (index == last) continue;
            last = index;
            onSum += profile[index];
            ++onCount;
        }
        if (onCount == 0 || onCount >= n) continue;
        const double onMean = onSum / double(onCount);
        const double offMean = (total - onSum) / double(n - onCount);
        // A floor of 0.05 codes: a picture without any step off the grid cannot divide by nothing.
        best = std::max(best, onMean / std::max(offMean, 0.05) - 1.0);
    }
    return best;
}
}  // namespace detail_metrics

inline double pushBlockiness(const PushLumaImage& img, double period = 8.0) {
    if (!img.valid() || img.width < 2 || img.height < 2) return -1.0;
    std::vector<double> columns(std::size_t(img.width - 1), 0.0), rows(std::size_t(img.height - 1), 0.0);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x + 1 < img.width; ++x) columns[std::size_t(x)] += std::abs(double(img.at(x + 1, y)) - double(img.at(x, y)));
    for (int y = 0; y + 1 < img.height; ++y)
        for (int x = 0; x < img.width; ++x) rows[std::size_t(y)] += std::abs(double(img.at(x, y + 1)) - double(img.at(x, y)));
    for (double& v : columns) v /= double(img.height);
    for (double& v : rows) v /= double(img.width);
    const double horizontal = detail_metrics::gridExcess(columns, period), vertical = detail_metrics::gridExcess(rows, period);
    if (horizontal < 0.0 || vertical < 0.0) return -1.0;
    return 0.5 * (horizontal + vertical);
}

inline double pushBanding(const PushLumaImage& img) {
    constexpr int run = 4;
    constexpr float flat = 0.26f, stepMin = 0.75f, stepMax = 3.0f;
    if (!img.valid() || img.width < 2 * run || img.height < 2 * run) return -1.0;
    std::uint64_t edges = 0, positions = 0;
    const auto test = [&](int x, int y, int dx, int dy) {
        // The step lies between (x, y) and (x + dx, y + dy); a run of 4 on each side must be flat.
        const float step = std::abs(img.at(x + dx, y + dy) - img.at(x, y));
        ++positions;
        if (step < stepMin || step > stepMax) return;
        float lowA = 255.0f, highA = 0.0f, lowB = 255.0f, highB = 0.0f;
        for (int i = 0; i < run; ++i) {
            const float a = img.at(x - i * dx, y - i * dy), b = img.at(x + (i + 1) * dx, y + (i + 1) * dy);
            lowA = std::min(lowA, a); highA = std::max(highA, a);
            lowB = std::min(lowB, b); highB = std::max(highB, b);
        }
        if (highA - lowA <= flat && highB - lowB <= flat) ++edges;
    };
    for (int y = 0; y < img.height; ++y)
        for (int x = run - 1; x + run < img.width; ++x) test(x, y, 1, 0);
    for (int y = run - 1; y + run < img.height; ++y)
        for (int x = 0; x < img.width; ++x) test(x, y, 0, 1);
    return positions ? 1000.0 * double(edges) / double(positions) : -1.0;
}

inline double pushRinging(const PushLumaImage& img, double scale = 1.0) {
    constexpr float edgeMin = 24.0f, farFlat = 3.0f;
    // A picture enlarged by `scale` is read in groups of round(scale) pixels along each line (their mean),
    // so that the distances below stay distances in source pixels.
    const int s = std::max(1, int(scale + 0.5));
    if (!img.valid() || img.width < 20 * s || img.height < 20 * s) return -1.0;
    double sum = 0.0;
    std::uint64_t sides = 0;
    // One line of the picture: `length` samples read through `sample`.
    const auto line = [&](int length, const auto& sample) {
        const auto get = [&](int j) {
            if (s == 1) return sample(j);
            float mean = 0.0f;
            for (int k = 0; k < s; ++k) mean += sample(j * s + k);
            return mean / float(s);
        };
        const int groups = length / s;
        // The edge lies between group i and group i + 1.
        const auto edge = [&](int i) { return get(i + 1) - get(i); };
        for (int i = 9; i + 10 < groups; ++i) {
            const float e = edge(i);
            if (std::abs(e) < edgeMin || std::abs(edge(i - 1)) > std::abs(e) || std::abs(edge(i + 1)) >= std::abs(e)) continue;
            for (int side = 0; side < 2; ++side) {
                // side 0: after the edge (indices rise from i); side 1: before it (indices fall from i + 1).
                const int origin = side == 0 ? i : i + 1, direction = side == 0 ? 1 : -1;
                float low = 255.0f, high = 0.0f, mean = 0.0f;
                for (int k = 6; k <= 9; ++k) {
                    const float v = get(origin + direction * k);
                    low = std::min(low, v); high = std::max(high, v); mean += v;
                }
                if (high - low > farFlat) continue;
                mean *= 0.25f;
                // On the brighter side an overshoot is above the far level, on the darker side below it.
                const float sign = (side == 0) == (e > 0.0f) ? 1.0f : -1.0f;
                float over = 0.0f;
                for (int k = 2; k <= 5; ++k) over = std::max(over, sign * (get(origin + direction * k) - mean));
                sum += double(over);
                ++sides;
            }
        }
    };
    for (int y = 0; y < img.height; ++y) line(img.width, [&](int i) { return img.at(i, y); });
    for (int x = 0; x < img.width; ++x) line(img.height, [&](int i) { return img.at(x, i); });
    return sides ? sum / double(sides) : 0.0;
}

inline double pushDetail(const PushLumaImage& img, double scale = 1.0) {
    if (!img.valid() || img.width < 2 || img.height < 2) return -1.0;
    double sum = 0.0;
    for (int y = 0; y + 1 < img.height; ++y)
        for (int x = 0; x + 1 < img.width; ++x) {
            const float c = img.at(x, y);
            sum += double(std::abs(img.at(x + 1, y) - c)) + double(std::abs(img.at(x, y + 1) - c));
        }
    return sum / (double(img.width - 1) * double(img.height - 1)) * std::max(scale, 0.0);
}

inline double pushFlicker(const PushLumaImage& a, const PushLumaImage& b) {
    constexpr int block = 16;
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height || a.width < block || a.height < block) return -1.0;
    double sum = 0.0;
    std::uint64_t still = 0;
    for (int by = 0; by + block <= a.height; by += block)
        for (int bx = 0; bx + block <= a.width; bx += block) {
            double meanA = 0.0, meanB = 0.0, change = 0.0;
            for (int y = by; y < by + block; ++y)
                for (int x = bx; x < bx + block; ++x) {
                    const double va = double(a.at(x, y)), vb = double(b.at(x, y));
                    meanA += va; meanB += vb; change += std::abs(va - vb);
                }
            const double pixels = double(block) * double(block);
            if (std::abs(meanA - meanB) / pixels > 0.5 || change / pixels > 6.0) continue;
            sum += change / pixels;
            ++still;
        }
    return still ? sum / double(still) : -1.0;
}

inline double pushHudDelta(const PushLumaImage& raw, const PushLumaImage& output, const PushRectN* rects, std::size_t count) {
    if (!raw.valid() || !output.valid() || !rects || !count) return -1.0;
    double sum = 0.0;
    std::uint64_t pixels = 0;
    const auto unit = [](float v) { return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; };
    for (std::size_t r = 0; r < count; ++r) {
        if (!(rects[r].right > rects[r].left) || !(rects[r].bottom > rects[r].top)) continue;  // empty, inverted or NaN
        const int x0 = int(unit(rects[r].left) * float(output.width)), x1 = int(std::ceil(unit(rects[r].right) * float(output.width)));
        const int y0 = int(unit(rects[r].top) * float(output.height)), y1 = int(std::ceil(unit(rects[r].bottom) * float(output.height)));
        for (int y = y0; y < std::min(y1, output.height); ++y) {
            const float v = std::clamp((float(y) + 0.5f) * float(raw.height) / float(output.height) - 0.5f, 0.0f, float(raw.height - 1));
            const int ya = int(v), yb = std::min(ya + 1, raw.height - 1);
            const float fy = v - float(ya);
            for (int x = x0; x < std::min(x1, output.width); ++x) {
                const float u = std::clamp((float(x) + 0.5f) * float(raw.width) / float(output.width) - 0.5f, 0.0f, float(raw.width - 1));
                const int xa = int(u), xb = std::min(xa + 1, raw.width - 1);
                const float fx = u - float(xa);
                const float top = raw.at(xa, ya) * (1.0f - fx) + raw.at(xb, ya) * fx, bottom = raw.at(xa, yb) * (1.0f - fx) + raw.at(xb, yb) * fx;
                sum += double(std::abs(output.at(x, y) - (top * (1.0f - fy) + bottom * fy)));
                ++pixels;
            }
        }
    }
    return pixels ? sum / double(pixels) : -1.0;
}

// How many frame pairs a probe of `seconds` takes: one per 2 s, at least 2, at most 6 (bounded work and memory).
constexpr std::uint32_t pushPipelineProbePairs(std::uint32_t seconds) {
    const std::uint32_t pairs = seconds / 2;
    return pairs < 2 ? 2u : pairs > 6 ? 6u : pairs;
}

// --- one probe's numbers: the mean over its frames of each metric that could be computed ------------------
struct PushMetricMean {
    double sum = 0.0;
    std::uint32_t count = 0;
    void add(double v) {
        if (v >= 0.0 && v < 1.0e9) { sum += v; ++count; }
    }
    double value() const { return count ? sum / double(count) : -1.0; }
};
struct PushQualityStats {
    std::uint32_t frames = 0;
    std::uint32_t width = 0, height = 0;
    PushMetricMean blockiness, banding, ringing, detail, hudDelta, flicker, meanLuma;
    bool hdr = false;
};
// The `[slipstream-quality]` line (S5_CONTRACT.md item 7). A number that could not be computed is -1.
inline std::string pushQualityLine(const PushQualityStats& s, std::string_view stage) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "frames=%u size=%ux%u blockiness=%.4f banding=%.4f ringing=%.4f detail=%.4f hudDelta=%.4f flicker=%.4f "
                  "meanLuma=%.3f hdr=%d stage=%.*s",
                  unsigned(s.frames), unsigned(s.width), unsigned(s.height), s.blockiness.value(), s.banding.value(), s.ringing.value(),
                  s.detail.value(), s.hudDelta.value(), s.flicker.value(), s.meanLuma.value(), s.hdr ? 1 : 0,
                  int(std::min<std::size_t>(stage.size(), 16)), stage.data());
    return text;
}
}  // namespace veyra::engine
