// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// restore_bench: the restoration stage's CPU REFERENCE (include/veyra/engine/PushClean.h) on decoded
// frames of a software-encoded stream, measured against the pristine picture (docs/RESTORATION.md).
// Linux, g++ only; run by run_bench.sh. Every number it prints is: MODEL content (gen_content.py), a
// SOFTWARE encoder (x264 / x265, not the PS5's), the CPU reference (not a GPU, not the shader).
//
//   restore_bench --content <dir> --decoded <prefix> [--first <n>] --frames <n> --label <text> [--csv <file>]
// reads <dir>/ref_NNN.rgb48, <dir>/mask.gray and <prefix>NNN.rgb24 (frames first .. first + n - 1) and
// appends one CSV row per setting.
#include "veyra/engine/PushClean.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace veyra::engine;

namespace {
constexpr int W = 1920, H = 1080;
enum Region : std::uint8_t { Other = 0, Gradient = 1, Hud = 2, Texture = 3, Lines = 4, Dark = 5, RegionCount = 6 };

bool readFile(const std::string& path, std::vector<std::uint8_t>& bytes, std::size_t expected) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    bytes.resize(expected);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(expected));
    if (std::size_t(in.gcount()) != expected) { std::fprintf(stderr, "%s: expected %zu bytes\n", path.c_str(), expected); return false; }
    return true;
}
std::string numbered(const std::string& prefix, int n, const char* ext) {
    char text[32];
    std::snprintf(text, sizeof text, "%03d%s", n, ext);
    return prefix + text;
}
std::vector<float> lumaOf(const PushCleanImage& img) {
    std::vector<float> y(img.plane[0].size());
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = 0.2126f * img.plane[0][i] + 0.7152f * img.plane[1][i] + 0.0722f * img.plane[2][i];
    return y;
}
double psnr(double mse) { return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse); }

// Mean SSIM of luma: 8x8 windows every 4 pixels, the usual constants for a 0..1 signal.
double ssim(const std::vector<float>& a, const std::vector<float>& b) {
    const double c1 = 0.01 * 0.01, c2 = 0.03 * 0.03;
    double total = 0; std::size_t windows = 0;
    for (int y = 0; y + 8 <= H; y += 4)
        for (int x = 0; x + 8 <= W; x += 4) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    const std::size_t k = std::size_t(y + j) * W + std::size_t(x + i);
                    const double va = a[k], vb = b[k];
                    sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb;
                }
            const double ma = sa / 64, mb = sb / 64, va = saa / 64 - ma * ma, vb = sbb / 64 - mb * mb, cov = sab / 64 - ma * mb;
            total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++windows;
        }
    return total / double(windows);
}

// What the final 8-bit write does to the luma: plain rounding, or the ordered dither of the last blit.
std::vector<float> written8(const PushCleanImage& img, bool dither) {
    PushCleanImage q(img.width, img.height);
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                const float v = img.at(c, x, y);
                q.set(c, x, y, dither ? pushCleanDitherRound(v, x, y, 255.0f) : float(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) / 255.0f);
            }
    return lumaOf(q);
}

struct Setting { const char* name; int strength; unsigned parts; };
struct Sums {
    double se[RegionCount]{}; std::uint64_t n[RegionCount]{};
    double seAll = 0, ssim = 0, block = 0, bandRound = 0, bandDither = 0, bandRefRound = 0, bandRefDither = 0;
    double hudStd = 0, hudStdDecoded = 0, hudChanged = 0, hudEdgeShift = 0, hudEdgeShiftDecoded = 0, flicker = 0;
    double changed = 0, cpuMs = 0;
    std::uint64_t classCount[kPushCleanClassCount]{};
    int frames = 0, flickerFrames = 0;
};

// Where the HUD's strong edges are, to a fraction of a pixel: at every pixel of the HUD region where the
// PRISTINE picture has a strong local maximum of its horizontal gradient, the peak of the gradient is
// located by a parabola through three samples, in the pristine picture and in `y`. Returns the mean
// distance between the two in pixels (0 = every edge where the console drew it).
double edgeShift(const std::vector<float>& y, const std::vector<float>& ref, const std::vector<std::uint8_t>& hudNear) {
    const auto grad = [](const std::vector<float>& v, std::size_t k) { return std::fabs(double(v[k + 1]) - double(v[k - 1])); };
    const auto peak = [&](const std::vector<float>& v, std::size_t k) {
        const double l = grad(v, k - 1), c = grad(v, k), r = grad(v, k + 1), curve = l - 2 * c + r;
        return curve < -1e-9 ? std::clamp(0.5 * (l - r) / curve, -1.0, 1.0) : 0.0;
    };
    double sum = 0; std::uint64_t n = 0;
    for (int r = 0; r < H; ++r)
        for (int x = 2; x + 2 < W; ++x) {
            const std::size_t k = std::size_t(r) * W + std::size_t(x);
            if (!hudNear[k]) continue;
            const double g = grad(ref, k);
            if (g < 0.2 || g < grad(ref, k - 1) || g < grad(ref, k + 1)) continue;
            sum += std::fabs(peak(y, k) - peak(ref, k)); ++n;
        }
    return n ? sum / double(n) : 0.0;
}
double stdIn(const std::vector<float>& y, const std::vector<std::uint8_t>& mask) {
    double s = 0, ss = 0; std::uint64_t n = 0;
    for (std::size_t i = 0; i < y.size(); ++i) if (mask[i]) { s += y[i]; ss += double(y[i]) * y[i]; ++n; }
    if (!n) return 0.0;
    const double m = s / double(n);
    return std::sqrt(std::max(0.0, ss / double(n) - m * m));
}
}

int main(int argc, char** argv) {
    std::string content, decoded, label = "stream", csv;
    int frames = 1, first = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--content") content = v; else if (a == "--decoded") decoded = v; else if (a == "--frames") frames = std::atoi(v.c_str());
        else if (a == "--first") first = std::atoi(v.c_str());
        else if (a == "--label") label = v; else if (a == "--csv") csv = v;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (content.empty() || decoded.empty() || frames < 1 || first < 0) {
        std::fprintf(stderr, "usage: restore_bench --content <dir> --decoded <prefix> [--first <n>] --frames <n> --label <text> [--csv <file>]\n");
        return 2;
    }
    const std::vector<Setting> settings = {
        {"off", 0, kPushCleanAllParts},
        {"deband-25", 25, kPushCleanDeband}, {"deband-50", 50, kPushCleanDeband}, {"deband-75", 75, kPushCleanDeband}, {"deband-100", 100, kPushCleanDeband},
        {"deblock-50", 50, kPushCleanDeblock}, {"dering-50", 50, kPushCleanDering}, {"grain-50", 50, kPushCleanGrain},
        {"all-25", 25, kPushCleanAllParts}, {"all-50", 50, kPushCleanAllParts}, {"all-75", 75, kPushCleanAllParts}, {"all-100", 100, kPushCleanAllParts},
        {"all-50-static", 50, kPushCleanAllParts | kPushCleanStaticNoise},
        {"no-deblock-25", 25, kPushCleanDeband | kPushCleanDering | kPushCleanGrain},
        {"no-deblock-50", 50, kPushCleanDeband | kPushCleanDering | kPushCleanGrain},
        {"no-deblock-75", 75, kPushCleanDeband | kPushCleanDering | kPushCleanGrain},
        {"deband-grain-50", 50, kPushCleanDeband | kPushCleanGrain},
    };
    std::vector<std::uint8_t> maskBytes;
    if (!readFile(content + "/mask.gray", maskBytes, std::size_t(W) * H)) return 1;
    std::vector<std::uint8_t> gradientMask(maskBytes.size()), hudMask(maskBytes.size()), hudNear(maskBytes.size());
    for (std::size_t i = 0; i < maskBytes.size(); ++i) {
        gradientMask[i] = maskBytes[i] == Gradient || maskBytes[i] == Dark;
        hudMask[i] = maskBytes[i] == Hud;
    }
    for (int y = 0; y < H; ++y)   // the HUD and two pixels around it
        for (int x = 0; x < W; ++x) {
            bool near = false;
            for (int j = -2; j <= 2 && !near; ++j)
                for (int i = -2; i <= 2 && !near; ++i) {
                    const int xx = std::clamp(x + i, 0, W - 1), yy = std::clamp(y + j, 0, H - 1);
                    near = hudMask[std::size_t(yy) * W + std::size_t(xx)] != 0;
                }
            hudNear[std::size_t(y) * W + std::size_t(x)] = near;
        }
    std::vector<Sums> sums(settings.size());
    std::vector<std::vector<float>> previous(settings.size());
    for (int n = 0; n < frames; ++n) {
        std::vector<std::uint8_t> refBytes, decBytes;
        if (!readFile(numbered(content + "/ref_", first + n, ".rgb48"), refBytes, std::size_t(W) * H * 6)) return 1;
        if (!readFile(numbered(decoded, first + n, ".rgb24"), decBytes, std::size_t(W) * H * 3)) return 1;
        PushCleanImage ref(W, H), in(W, H);
        for (std::size_t i = 0; i < std::size_t(W) * H; ++i)
            for (std::size_t c = 0; c < 3; ++c) {
                ref.plane[c][i] = float(refBytes[(i * 3 + c) * 2] | (refBytes[(i * 3 + c) * 2 + 1] << 8)) / 65535.0f;
                in.plane[c][i] = float(decBytes[i * 3 + c]) / 255.0f;
            }
        const auto refY = lumaOf(ref), inY = lumaOf(in);
        const double inShift = edgeShift(inY, refY, hudNear);
        const double refBand = pushCleanContourIndex(written8(ref, false), gradientMask, W, H);
        const double refBandDither = pushCleanContourIndex(written8(ref, true), gradientMask, W, H);
        const double inStd = stdIn(inY, hudNear);
        for (std::size_t s = 0; s < settings.size(); ++s) {
            PushCleanSettings cs;
            cs.strength = settings[s].strength; cs.parts = settings[s].parts; cs.outputBits = 8;
            cs.seed = pushCleanFrameSeed(settings[s].parts, std::uint64_t(first + n));
            PushCleanImage out;
            PushCleanStats stats;
            const auto t0 = std::chrono::steady_clock::now();
            pushCleanApply(in, out, cs, &stats);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const auto outY = lumaOf(out);
            Sums& a = sums[s];
            for (std::size_t i = 0; i < outY.size(); ++i) {
                const double d = double(outY[i]) - double(refY[i]);
                a.se[maskBytes[i] < RegionCount ? maskBytes[i] : 0] += d * d; ++a.n[maskBytes[i] < RegionCount ? maskBytes[i] : 0];
                a.seAll += d * d;
                if (hudNear[i] && outY[i] != inY[i]) a.hudChanged += 1;
            }
            a.ssim += ssim(outY, refY);
            a.block += pushCleanBlockiness(outY, W, H);
            a.bandRound += pushCleanContourIndex(written8(out, false), gradientMask, W, H);
            a.bandDither += pushCleanContourIndex(written8(out, true), gradientMask, W, H);
            a.bandRefRound += refBand; a.bandRefDither += refBandDither;
            a.hudStd += stdIn(outY, hudNear); a.hudStdDecoded += inStd;
            a.hudEdgeShift += edgeShift(outY, refY, hudNear); a.hudEdgeShiftDecoded += inShift;
            if (!previous[s].empty()) {
                double f = 0; std::uint64_t count = 0;
                for (std::size_t i = 0; i < outY.size(); ++i) if (hudMask[i]) { f += std::fabs(double(outY[i]) - double(previous[s][i])); ++count; }
                a.flicker += count ? f / double(count) * 255.0 : 0.0; ++a.flickerFrames;
            }
            previous[s] = outY;
            a.changed += double(stats.changed);
            for (std::size_t k = 0; k < kPushCleanClassCount; ++k) a.classCount[k] += stats.count[k];
            a.cpuMs += ms; ++a.frames;
        }
        std::fprintf(stderr, "%s: frame %d of %d done\n", label.c_str(), n + 1, frames);
    }
    std::FILE* out = stdout;
    bool header = true;
    if (!csv.empty()) {
        { std::ifstream probe(csv); header = !probe.good() || probe.peek() == std::ifstream::traits_type::eof(); }
        out = std::fopen(csv.c_str(), "a");
        if (!out) { std::fprintf(stderr, "cannot write %s\n", csv.c_str()); return 1; }
    }
    if (header)
        std::fprintf(out, "stream,setting,strength,parts,frames,psnr_y,psnr_gradient,psnr_dark,psnr_texture,psnr_hud,psnr_lines,ssim,blockiness,"
                          "contours_round,contours_dither,contours_pristine_round,contours_pristine_dither,hud_contrast,hud_changed_pct,hud_edge_shift_px,hud_edge_shift_off_px,"
                          "hud_flicker_codes,changed_pct,deband_pct,deblock_pct,smooth_pct,protected_pct,cpu_ms\n");
    const double pixels = double(W) * H;
    std::uint64_t hudNearCount = 0;
    for (auto v : hudNear) hudNearCount += v;
    for (std::size_t s = 0; s < settings.size(); ++s) {
        const Sums& a = sums[s];
        const double f = double(a.frames);
        const auto region = [&](Region r) { return a.n[r] ? psnr(a.se[r] / double(a.n[r])) : 0.0; };
        const auto share = [&](PushCleanClass c) { return 100.0 * double(a.classCount[std::size_t(c)]) / (pixels * f); };
        std::fprintf(out, "%s,%s,%d,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%.3f,%.2f,%.2f,%.2f,%.2f,%.4f,%.3f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f\n",
                     label.c_str(), settings[s].name, settings[s].strength, settings[s].parts, a.frames,
                     psnr(a.seAll / (pixels * f)), region(Gradient), region(Dark), region(Texture), region(Hud), region(Lines),
                     a.ssim / f, a.block / f, a.bandRound / f, a.bandDither / f, a.bandRefRound / f, a.bandRefDither / f,
                     a.hudStdDecoded > 0 ? a.hudStd / a.hudStdDecoded : 1.0, 100.0 * a.hudChanged / (double(hudNearCount) * f),
                     a.hudEdgeShift / f, a.hudEdgeShiftDecoded / f, a.flickerFrames ? a.flicker / a.flickerFrames : 0.0,
                     100.0 * a.changed / (pixels * f), share(PushCleanClass::Deband), share(PushCleanClass::Deblock), share(PushCleanClass::Smooth),
                     share(PushCleanClass::Edge) + share(PushCleanClass::Line) + share(PushCleanClass::Texture), a.cpuMs / f);
    }
    if (out != stdout) std::fclose(out);
    return 0;
}
