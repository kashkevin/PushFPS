// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Quality bench test helper: the vendored s12-pace downscale reference (vendor/PushDownscale.h pushBlitSample) on a plane.
//   downscale_ref <in.bin> <out.bin>; in: u32 W, H, outW, outH, kernel (PushShrinkKernel id), transfer (PushTransfer id); W*H f64. out: outW*outH f64.
#include "PushDownscale.h"
#include <cstdio>
using namespace veyra::engine;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 3;
    unsigned h[6];
    if (std::fread(h, 4, 6, f) != 6) return 4;
    PushPlane p;
    p.width = h[0]; p.height = h[1];
    p.v.resize(std::size_t(h[0]) * h[1]);
    if (std::fread(p.v.data(), 8, p.v.size(), f) != p.v.size()) return 5;
    std::fclose(f);
    std::FILE* o = std::fopen(argv[2], "wb");
    for (unsigned y = 0; y < h[3]; ++y)
        for (unsigned x = 0; x < h[2]; ++x) {
            const double v = pushShrinkSample(p, h[2], h[3], x, y, PushShrinkKernel(h[4]), PushTransfer(h[5]));
            std::fwrite(&v, 8, 1, o);
        }
    std::fclose(o);
    return 0;
}
