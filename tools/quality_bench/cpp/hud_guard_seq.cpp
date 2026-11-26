// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Quality bench (OFFLINE, CPU): runs the vendored HUD Guard CPU reference (vendor/PushHudGuard.h, stream s12-hud) over a
// sequence of real frames, as the GPU passes do once per real frame. No GPU; nothing here is a speed of anything.
//   hud_guard_seq <in.bin> <out.bin>
// in.bin:  u32 W, H, N, tenBit; then N frames of W*H u32 words (R8G8B8A8 SDR or R10G10B10A2 HDR10 PQ codes).
// out.bin: per frame i: W*H u8 (bit 0: compose mask of pair (i-1, i) = protectMask, bit 1: pre-clean mask = cleanMask),
//          then W*H u32 words: frame i as the generator's input copy (pre-cleaned). Frame 0 is a reset (nothing protected).
#include "PushHudGuard.h"
#include <cstdio>
#include <vector>
using namespace veyra::pipeline;
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: hud_guard_seq in.bin out.bin\n"); return 2; }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 3;
    std::uint32_t h[4];
    if (std::fread(h, 4, 4, f) != 4) return 4;
    const std::uint32_t W = h[0], H = h[1], N = h[2];
    const bool ten = h[3] != 0;
    std::vector<std::vector<std::uint32_t>> frames(N, std::vector<std::uint32_t>(std::size_t(W) * H));
    for (auto& fr : frames)
        if (std::fread(fr.data(), 4, fr.size(), f) != fr.size()) return 5;
    std::fclose(f);
    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) return 6;
    PushHudGuardReference g(W, H);
    std::vector<std::uint8_t> mask(std::size_t(W) * H);
    for (std::uint32_t i = 0; i < N; ++i) {
        g.update(i ? frames[i - 1] : frames[0], frames[i], ten, i == 0);
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
                mask[std::size_t(y) * W + x] = std::uint8_t((g.protectMask(int(x), int(y)) ? 1 : 0) | (g.cleanMask(int(x), int(y)) ? 2 : 0));
        const auto cleaned = g.clean(frames[i], ten);
        std::fwrite(mask.data(), 1, mask.size(), o);
        std::fwrite(cleaned.data(), 4, cleaned.size(), o);
    }
    std::fclose(o);
    return 0;
}
