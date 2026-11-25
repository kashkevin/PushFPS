// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Offline: runs the implemented PushWarp rules (include/veyra/pipeline/PushWarp.h, pushWarpReference: the CPU reference
// the GPU passes are checked against on WARP) on frames and a field written by measure.py. No GPU.
//   pushwarp_cli <job.bin> <out.rgba>
// job.bin (little-endian): u32 W, H, gw, gh, hasPrevious; f32 t; u32 acceleration; A, B: W*H u32 (R8G8B8A8);
// field: gw*gh f32 storedX, storedY, trust; previous field (if hasPrevious): the same.
// out.rgba: W*H u32, then u32 still, blend, conflict, hole, wholeFrameNearest, cutOffCells (trusted by the flow but longer
// than maxMotionShare), trustedCells.
// Offline variants (environment; nothing here changes what the GPU does):
//   PUSHWARP_MAX_MOTION_SHARE=<f>  maxMotionShare (the GPU's constant is 0.125)
//   PUSHWARP_TEN_BIT=1             frames are R10G10B10A2 words (HDR10 PQ codes), as the GPU's HDR10 path
//   PUSHWARP_LINEAR_COMPOSE=1      (ten-bit only) R3 redone here with the bilinear samples and the blend in LINEAR light
//                                  (PQ decoded, blended, PQ encoded) on the reference's own filled field: the comparison
//                                  with the implemented scheme, which samples and blends in PQ code values. With
//                                  PUSHWARP_LINEAR_COMPOSE=check the same re-run is done in code values and must give the
//                                  reference's words exactly (exit code 9 otherwise).
#include "veyra/pipeline/PushWarp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace veyra::pipeline;

static bool readAll(std::FILE* f, void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; }

// SMPTE ST 2084 on 0..1 code values <-> 0..1 of 10 000 nits.
static float pqToLinear(float e) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::max(double(e), 0.0), 1.0 / m2);
    return float(std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1));
}
static float linearToPq(float y) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    const double p = std::pow(std::max(double(y), 0.0), m1);
    return float(std::pow((c1 + c2 * p) / (1 + c3 * p), m2));
}
static PushWarpColor toLinear(PushWarpColor c) { return {pqToLinear(c.r), pqToLinear(c.g), pqToLinear(c.b), c.a}; }
static PushWarpColor toPq(PushWarpColor c) { return {linearToPq(c.r), linearToPq(c.g), linearToPq(c.b), c.a}; }
// pushWarpBilinear with the four taps in linear light (or in code values when linear is false: the reference's own).
static PushWarpColor bilinearIn(const PushWarpFrame& f, float qx, float qy, bool linear) {
    if (!linear) return pushWarpBilinear(f, true, qx, qy);
    const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
    const int ax = int(fx), ay = int(fy);
    const auto c00 = toLinear(pushWarpUnpack(f.at(ax, ay), true)), c10 = toLinear(pushWarpUnpack(f.at(ax + 1, ay), true));
    const auto c01 = toLinear(pushWarpUnpack(f.at(ax, ay + 1), true)), c11 = toLinear(pushWarpUnpack(f.at(ax + 1, ay + 1), true));
    const auto mix = [&](float PushWarpColor::*m) {
        const float top = c00.*m + (c10.*m - c00.*m) * tx, bottom = c01.*m + (c11.*m - c01.*m) * tx;
        return top + (bottom - top) * ty;
    };
    return {mix(&PushWarpColor::r), mix(&PushWarpColor::g), mix(&PushWarpColor::b), mix(&PushWarpColor::a)};
}
// R3 of pushWarpReference (interpolation only, t < 1) on its filled level-0 field, in code values or in linear light.
// The still test, the conflict test and the hole rule stay in code values (as the GPU decides them); only the samples
// and the blend change domain.
static std::vector<std::uint32_t> recompose(const PushWarpFrame& a, const PushWarpFrame& b, const PushWarpResult& r, const PushWarpParams& p,
                                            std::uint32_t gw, std::uint32_t gh, bool linear) {
    const std::uint32_t W = b.width, H = b.height;
    std::vector<std::uint32_t> out(std::size_t(W) * H);
    const float sx = float(gw) / float(W), sy = float(gh) / float(H), scaleX = float(W) / float(gw), scaleY = float(H) / float(gh);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint32_t aw = a.at(int(x), int(y)), bw = b.at(int(x), int(y));
            if (pushWarpMaxDelta(pushWarpUnpack(aw, true), pushWarpUnpack(bw, true)) <= p.stillDelta) { out[std::size_t(y) * W + x] = aw; continue; }
            const float qx = std::max((float(x) + 0.5f) * sx - 0.5f, 0.0f), qy = std::max((float(y) + 0.5f) * sy - 0.5f, 0.0f);
            const float fx = std::floor(qx), fy = std::floor(qy), tx = qx - fx, ty = qy - fy;
            const std::uint32_t x0 = std::min(std::uint32_t(fx), gw - 1), y0 = std::min(std::uint32_t(fy), gh - 1);
            const std::uint32_t x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
            const auto& f00 = r.field[y0 * gw + x0]; const auto& f10 = r.field[y0 * gw + x1];
            const auto& f01 = r.field[y1 * gw + x0]; const auto& f11 = r.field[y1 * gw + x1];
            const auto bl = [&](float PushWarpLevelCell::*m) {
                const float top = f00.*m + (f10.*m - f00.*m) * tx, bottom = f01.*m + (f11.*m - f01.*m) * tx;
                return top + (bottom - top) * ty;
            };
            const float gx = bl(&PushWarpLevelCell::ex) * scaleX, gy = bl(&PushWarpLevelCell::ey) * scaleY;
            float vis = bl(&PushWarpLevelCell::visibility);
            if (vis < p.holeVisibility) vis = 1.0f;
            const float ax = float(x) + p.t * gx, ay = float(y) + p.t * gy, bx = float(x) - (1.0f - p.t) * gx, by = float(y) - (1.0f - p.t) * gy;
            const auto c0 = pushWarpBilinear(a, true, ax, ay), c1 = pushWarpBilinear(b, true, bx, by);   // the decisions, in codes
            const auto s0 = bilinearIn(a, ax, ay, linear), s1 = bilinearIn(b, bx, by, linear);
            const float w0 = (1.0f - p.t) * vis, w1 = p.t * vis;
            PushWarpColor o;
            if (pushWarpMaxDelta(c0, c1) > p.conflictDelta) o = w0 >= w1 ? s0 : s1;
            else {
                const float d = w0 + w1;
                o = {(s0.r * w0 + s1.r * w1) / d, (s0.g * w0 + s1.g * w1) / d, (s0.b * w0 + s1.b * w1) / d, (s0.a * w0 + s1.a * w1) / d};
            }
            out[std::size_t(y) * W + x] = pushWarpPack(linear ? toPq(o) : o, true);
        }
    if (r.wholeFrameNearest) {
        const PushWarpFrame& nearest = p.t >= 0.5f ? b : a;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) out[std::size_t(y) * W + x] = nearest.at(int(x), int(y));
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: pushwarp_cli job.bin out.rgba\n"); return 2; }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 3;
    std::uint32_t h[5]; float t = 0; std::uint32_t accel = 0;
    if (!readAll(f, h, sizeof h) || !readAll(f, &t, 4) || !readAll(f, &accel, 4)) return 4;
    const std::uint32_t W = h[0], H = h[1], gw = h[2], gh = h[3];
    std::vector<std::uint32_t> a(std::size_t(W) * H), b(a.size());
    std::vector<float> fx(std::size_t(gw) * gh), fy(fx.size()), ft(fx.size()), px(fx.size()), py(fx.size()), pt(fx.size());
    if (!readAll(f, a.data(), a.size() * 4) || !readAll(f, b.data(), b.size() * 4)) return 5;
    if (!readAll(f, fx.data(), fx.size() * 4) || !readAll(f, fy.data(), fy.size() * 4) || !readAll(f, ft.data(), ft.size() * 4)) return 6;
    if (h[4] && (!readAll(f, px.data(), px.size() * 4) || !readAll(f, py.data(), py.size() * 4) || !readAll(f, pt.data(), pt.size() * 4))) return 7;
    std::fclose(f);
    const PushWarpFrame A{a.data(), W, H, W}, B{b.data(), W, H, W};
    const PushWarpField field{fx.data(), fy.data(), ft.data(), gw, gh}, previous{px.data(), py.data(), pt.data(), gw, gh};
    PushWarpParams p;
    p.t = t;
    p.acceleration = accel != 0;
    if (const char* share = std::getenv("PUSHWARP_ACCEL_SHARE")) p.accelShare = float(std::atof(share));   // offline variants only
    if (const char* share = std::getenv("PUSHWARP_MAX_MOTION_SHARE")) p.maxMotionShare = float(std::atof(share));
    const char* ten = std::getenv("PUSHWARP_TEN_BIT");
    const bool tenBit = ten && ten[0] == '1';
    const auto r = pushWarpReference(A, B, tenBit, field, p, h[4] ? &previous : nullptr);
    std::vector<std::uint32_t> words = r.words;
    if (const char* lin = std::getenv("PUSHWARP_LINEAR_COMPOSE"); lin && tenBit && t < 1.0f) {
        const bool check = std::strcmp(lin, "check") == 0;
        words = recompose(A, B, r, p, gw, gh, !check);
        if (check) {
            std::size_t differing = 0;
            for (std::size_t i = 0; i < words.size(); ++i) differing += words[i] != r.words[i];
            if (differing) { std::fprintf(stderr, "recompose in code values differs from the reference at %zu pixels\n", differing); return 9; }
        }
    }
    std::uint32_t cutOff = 0, trusted = 0;
    for (std::uint32_t y = 0; y < gh; ++y)
        for (std::uint32_t x = 0; x < gw; ++x) {
            float vx = 0, vy = 0;
            if (!pushWarpVectorAt(field, int(x), int(y), p.minConfidence, vx, vy)) continue;
            ++trusted;
            if (std::hypot(vx, vy) > p.maxMotionShare * float(gw)) ++cutOff;
        }
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) return 8;
    std::fwrite(words.data(), 4, words.size(), o);
    const std::uint32_t c[7] = {r.counters.still, r.counters.blend, r.counters.conflict, r.counters.hole, r.wholeFrameNearest ? 1u : 0u, cutOff, trusted};
    std::fwrite(c, 4, 7, o);
    std::fclose(o);
    return 0;
}
