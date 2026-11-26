// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace": the present blit's downscale, CPU reference (shaders/PresentBlit.hlsl PushShrink; flags 512 / 1024 / 2048 /
// 4096, the footprint in constant 14). Pure and callable: the offline quality bench (exp/s12-bench, tools/quality_bench) uses it
// as its "downscale" stage, old and new.
//
// THE PACKAGE 16 DEFECT (his case: 3840x2160 shown at 2560x1440, f = 1.5 source pixels per screen pixel). PresentBlit takes ONE
// bilinear tap per screen pixel, at the screen pixel's centre, on the CODES the frame holds (PQ for HDR10). Screen pixel o
// samples at source position 1.5 o + 0.25 (texel centres at integers): o = 2k mixes columns 3k / 3k+1 as 75 / 25 %, o = 2k+1
// mixes 3k+1 / 3k+2 as 25 / 75 %. Column 3k+1 never reaches the screen with more than a quarter of its code; in PQ a quarter of
// a 300-nit code is about 1 nit (1.11 nits on black, 6.6 on a 1-nit background; columns 3k and 3k+2 give 65.6 nits). A thin
// HUD stroke, crosshair or text line on such a column all but vanishes, and anything thin that moves passes the three phases in
// turn: its light on screen pulses (spread 1.4 of its mean in HDR10, 0.9 in SDR). At f = 2.63 (his 1462x884 window) one tap per
// screen pixel skips whole 4K columns: thin lines vanish outright at some positions (spread 3.8).
//
// THE FIX, a visible choice "Steadier fine detail" (pushSteadyDetail, default off): every 4K texel under the screen pixel's
// footprint is decoded to linear light first (PQ, sRGB, or nothing for scRGB), weighted exactly, summed, normalised, coded again.
// Kernel chosen by numbers (tests/remoteplay/OnScreenCadenceTests.inc downscale_kernel_table): AreaSharp = the exact coverage box
// one screen pixel wide with a mild unsharp mask (s = 0.03) of the two neighbouring boxes: no thin-stroke collapse, no energy
// pulse, contrast at 4 / 3 / 2.5 screen-pixel periods equal to Package 16 at 1.5x (within 0.001), overshoot 2.9 % (HDR10).
// Catmull-Rom / Lanczos-2 overshoot 4 %; Mitchell and the plain box lose contrast. 1:1 and magnification never use it.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace veyra::engine {

enum class PushShrinkKernel : int { Bilinear1 = 0, Area = 1, CatmullRom = 2, Mitchell = 3, Lanczos2 = 4, AreaSharp = 5 };
// AreaSharp: the exact coverage box plus a mild unsharp mask made of the two neighbouring screen pixels' boxes:
// (1 + 2s) box(centre) - s box(left) - s box(right). s = kPushShrinkSharpen, chosen so contrast at 4 / 3 / 2.5 screen-pixel
// periods is not below Package 16's single tap at 1.5x while the overshoot stays under 3 % (table in the tests).
inline constexpr double kPushShrinkSharpen = 0.03;
enum class PushTransfer : int { Linear = 0, Pq = 1, Srgb = 2 };

// Codes <-> linear light. PQ: 0..1 code <-> 0..10000 nits / 10000 (ST 2084). sRGB: IEC 61966-2-1.
inline double pushPqToLinear(double pq) {
    const double p = std::pow(std::clamp(pq, 0.0, 1.0), 32.0 / 2523.0);
    return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / std::max(2413.0 / 128.0 - 2392.0 / 128.0 * p, 1e-6), 16384.0 / 2610.0);
}
inline double pushLinearToPq(double l) {
    const double p = std::pow(std::clamp(l, 0.0, 1.0), 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}
inline double pushSrgbToLinear(double c) { c = std::clamp(c, 0.0, 1.0); return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
inline double pushLinearToSrgb(double l) { l = std::clamp(l, 0.0, 1.0); return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055; }
inline double pushDecode(double code, PushTransfer t) { return t == PushTransfer::Pq ? pushPqToLinear(code) : t == PushTransfer::Srgb ? pushSrgbToLinear(code) : code; }
inline double pushEncode(double light, PushTransfer t) { return t == PushTransfer::Pq ? pushLinearToPq(light) : t == PushTransfer::Srgb ? pushLinearToSrgb(light) : light; }

// The kernel in screen-pixel units x (0 = the screen pixel's centre), and its radius.
inline double pushShrinkRadius(PushShrinkKernel k) {
    return k == PushShrinkKernel::Area ? 0.5 : k == PushShrinkKernel::AreaSharp ? 1.5 : k == PushShrinkKernel::Bilinear1 ? 0.0 : 2.0;
}
inline double pushCubic(double x, double b, double c) {   // Mitchell-Netravali family (B, C)
    x = std::abs(x);
    if (x < 1) return ((12 - 9 * b - 6 * c) * x * x * x + (-18 + 12 * b + 6 * c) * x * x + (6 - 2 * b)) / 6;
    if (x < 2) return ((-b - 6 * c) * x * x * x + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x + (8 * b + 24 * c)) / 6;
    return 0;
}
inline double pushSinc(double x) { const double p = 3.14159265358979323846 * x; return std::abs(x) < 1e-9 ? 1.0 : std::sin(p) / p; }
inline double pushShrinkKernel(PushShrinkKernel k, double x) {
    switch (k) {
    case PushShrinkKernel::CatmullRom: return pushCubic(x, 0.0, 0.5);
    case PushShrinkKernel::Mitchell: return pushCubic(x, 1.0 / 3.0, 1.0 / 3.0);
    case PushShrinkKernel::Lanczos2: return std::abs(x) < 2 ? pushSinc(x) * pushSinc(x / 2) : 0.0;
    default: return 0.0;
    }
}
// The weight of source texel i (covering [i, i+1) in continuous source coordinates) for a screen pixel centred at source
// coordinate c with footprint f (source pixels per screen pixel): Area = the exact overlap of the texel with [c - f/2, c + f/2];
// AreaSharp = the same with the neighbouring boxes subtracted; the others = the kernel at the texel's centre, in screen-pixel
// units. Unnormalised.
inline double pushShrinkWeight(PushShrinkKernel k, double c, double f, int i) {
    const auto box = [&](double a, double b) { return std::max(0.0, std::min(double(i) + 1, b) - std::max(double(i), a)); };
    if (k == PushShrinkKernel::Area) return box(c - f / 2, c + f / 2);
    if (k == PushShrinkKernel::AreaSharp)
        return (1 + 2 * kPushShrinkSharpen) * box(c - f / 2, c + f / 2) - kPushShrinkSharpen * (box(c - 1.5 * f, c - f / 2) + box(c + f / 2, c + 1.5 * f));
    return pushShrinkKernel(k, (double(i) + 0.5 - c) / f);
}
inline bool pushShrinkApplies(PushShrinkKernel k, double footprint) { return k != PushShrinkKernel::Bilinear1 && footprint > 1.001; }

// One channel image of CODES, row major, clamped addressing.
struct PushPlane {
    unsigned width = 0, height = 0;
    std::vector<double> v;
    double at(int x, int y) const {
        x = std::clamp(x, 0, int(width) - 1);
        y = std::clamp(y, 0, int(height) - 1);
        return v[std::size_t(y) * width + std::size_t(x)];
    }
    // Package 16: one bilinear tap on the codes at continuous coordinate (cx, cy) (texel centres at i + 0.5).
    double bilinear(double cx, double cy) const {
        const double px = cx - 0.5, py = cy - 0.5;
        const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
        const double fx = px - x0, fy = py - y0;
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
};
// The output pixel (ox, oy) of an outW x outH picture covering the whole source: the code PresentBlit writes. The new path runs
// when either axis shrinks; each axis uses its own footprint, at least one texel (an axis that is not shrunk gets a box one
// texel wide: the texel itself, or two at a seam).
inline double pushShrinkSample(const PushPlane& src, unsigned outW, unsigned outH, unsigned ox, unsigned oy, PushShrinkKernel k, PushTransfer t) {
    const double fx = double(src.width) / outW, fy = double(src.height) / outH;
    const double cx = (ox + 0.5) * fx, cy = (oy + 0.5) * fy;
    if (!pushShrinkApplies(k, std::max(fx, fy))) return src.bilinear(cx, cy);
    const double gx = std::max(fx, 1.0), gy = std::max(fy, 1.0), rx = pushShrinkRadius(k) * gx, ry = pushShrinkRadius(k) * gy;
    const int x0 = int(std::floor(cx - rx)), x1 = int(std::ceil(cx + rx)) - 1, y0 = int(std::floor(cy - ry)), y1 = int(std::ceil(cy + ry)) - 1;
    double sum = 0, weights = 0;
    for (int y = y0; y <= y1; ++y) {
        const double wy = pushShrinkWeight(k, cy, gy, y);
        if (wy == 0) continue;
        for (int x = x0; x <= x1; ++x) {
            const double w = wy * pushShrinkWeight(k, cx, gx, x);
            if (w == 0) continue;
            sum += w * pushDecode(src.at(x, y), t);
            weights += w;
        }
    }
    return pushEncode(weights != 0 ? sum / weights : 0.0, t);
}

// 1D helpers for the kernel table (a picture constant along y): one row of codes in, one row of codes out.
inline std::vector<double> pushShrinkRow(const std::vector<double>& codes, unsigned outW, PushShrinkKernel k, PushTransfer t) {
    PushPlane p{unsigned(codes.size()), 1, codes};
    std::vector<double> out(outW);
    const double f = double(codes.size()) / outW;
    for (unsigned o = 0; o < outW; ++o) {
        const double c = (o + 0.5) * f;
        if (!pushShrinkApplies(k, f)) { out[o] = p.bilinear(c, 0.5); continue; }
        const double r = pushShrinkRadius(k) * f;
        double sum = 0, weights = 0;
        for (int x = int(std::floor(c - r)); x <= int(std::ceil(c + r)); ++x) {
            const double w = pushShrinkWeight(k, c, f, x);
            if (w == 0) continue;
            sum += w * pushDecode(p.at(x, 0), t);
            weights += w;
        }
        out[o] = pushEncode(weights != 0 ? sum / weights : 0.0, t);
    }
    return out;
}

// The visible choice "Steadier fine detail" (preference pushSteadyDetail, default off = Package 16's single tap). Live: the
// presenter reads it for every frame. The shader's kernel and transfer codes (PresentBlit flags): 512 = shrink with Area,
// 512 + 2048 = AreaSharp, 1024 = the source holds PQ codes, 4096 = the source holds sRGB codes (neither = linear scRGB).
inline std::atomic<bool>& pushSteadyDetailStore() {
    static std::atomic<bool> value{false};
    return value;
}
inline void setPushSteadyDetail(bool on) { pushSteadyDetailStore().store(on); }
inline bool pushSteadyDetail() { return pushSteadyDetailStore().load(); }
inline constexpr unsigned kPushShrinkFlagOn = 512, kPushShrinkFlagPq = 1024, kPushShrinkFlagSharpen = 2048, kPushShrinkFlagSrgb = 4096;
inline unsigned pushShrinkFlags(PushShrinkKernel k, PushTransfer t) {
    if (k != PushShrinkKernel::Area && k != PushShrinkKernel::AreaSharp) return 0;
    return kPushShrinkFlagOn + (k == PushShrinkKernel::AreaSharp ? kPushShrinkFlagSharpen : 0) +
           (t == PushTransfer::Pq ? kPushShrinkFlagPq : t == PushTransfer::Srgb ? kPushShrinkFlagSrgb : 0);
}
// Source pixels per back-buffer pixel along the SOURCE's x and y (the picture shown renderW x renderH client pixels, the back
// buffer bufferW x bufferH for a clientW x clientH client area; odd quarter turns put the source's width along the screen's height).
struct PushShrinkFootprint { double x = 1, y = 1; };
inline PushShrinkFootprint pushShrinkFootprint(double workW, double workH, double renderW, double renderH, double clientW, double clientH, double bufferW, double bufferH, unsigned quarterTurns) {
    if (!(renderW > 0 && renderH > 0 && clientW > 0 && clientH > 0 && bufferW > 0 && bufferH > 0)) return {};
    const double shownW = renderW * bufferW / clientW, shownH = renderH * bufferH / clientH;
    const bool odd = (quarterTurns & 1u) != 0;
    return {workW / (odd ? shownH : shownW), workH / (odd ? shownW : shownH)};
}

}  // namespace veyra::engine
