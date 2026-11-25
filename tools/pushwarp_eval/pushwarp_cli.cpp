// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Offline: runs the implemented PushWarp rules (include/veyra/pipeline/PushWarp.h, pushWarpReference: the CPU reference
// the GPU passes are checked against on WARP) on frames and a field written by measure.py. No GPU.
//   pushwarp_cli <job.bin> <out.rgba>
// job.bin (little-endian): u32 W, H, gw, gh, hasPrevious; f32 t; u32 acceleration; A, B: W*H u32 (R8G8B8A8);
// field: gw*gh f32 storedX, storedY, trust; previous field (if hasPrevious): the same.
// out.rgba: W*H u32, then u32 still, blend, conflict, hole, wholeFrameNearest.
#include "veyra/pipeline/PushWarp.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace veyra::pipeline;

static bool readAll(std::FILE* f, void* p, std::size_t n) { return std::fread(p, 1, n, f) == n; }

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
    const auto r = pushWarpReference(A, B, false, field, p, h[4] ? &previous : nullptr);
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) return 8;
    std::fwrite(r.words.data(), 4, r.words.size(), o);
    const std::uint32_t c[5] = {r.counters.still, r.counters.blend, r.counters.conflict, r.counters.hole, r.wholeFrameNearest ? 1u : 0u};
    std::fwrite(c, 4, 5, o);
    std::fclose(o);
    return 0;
}
