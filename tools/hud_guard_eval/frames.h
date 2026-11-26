// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// tools/hud_guard_eval: frame output for the end-to-end quality bench (branch exp/s12-bench, tools/quality_bench) and the
// downscale study (downscale_eval.cpp), and a CPU copy of the two present filters for a shrinking blit:
//   singleTap  Package 16's PresentBlit: one bilinear tap per output pixel at its centre;
//   area       the stream "s12-pace" filter (include/veyra/engine/PushDownscale.h on that branch, pushBlitSample): n x n
//              bilinear taps over the output pixel's footprint, n = ceil(footprint) + 1 clamped to 3..5, equal weights,
//              in code values for SDR (this study is SDR; the bench does HDR10 in linear light).
// Frames are written as 16-bit binary PGM (P5, maxval 65535, big-endian): the grey's code value (0..1) x 65535; for an
// HDR10 scene the PQ code value. A manifest.json beside them lists every file with its frame number, kind and time.
#include "HudGuardScene.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hud_guard_frames {

// Code values (0..1) of a scene's words (grey: the red channel), not SDR-normalised: what the GPU would hold.
inline std::vector<double> codes(const hud_guard_scene::Scene& s, const std::vector<std::uint32_t>& words) {
    std::vector<double> v(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) v[i] = s.hdr() ? double(words[i] & 1023u) / 1023.0 : double(words[i] & 255u) / 255.0;
    return v;
}
inline bool writePgm16(const std::string& path, unsigned w, unsigned h, const std::vector<double>& v) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%u %u\n65535\n", w, h);
    std::vector<unsigned char> row(std::size_t(w) * 2);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            const unsigned q = unsigned(std::lround(std::clamp(v[std::size_t(y) * w + x], 0.0, 1.0) * 65535.0));
            row[std::size_t(x) * 2] = static_cast<unsigned char>(q >> 8);
            row[std::size_t(x) * 2 + 1] = static_cast<unsigned char>(q & 255u);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

struct Plane {
    unsigned w = 0, h = 0;
    std::vector<double> v;
    double at(int x, int y) const { return v[std::size_t(std::clamp(y, 0, int(h) - 1)) * w + std::size_t(std::clamp(x, 0, int(w) - 1))]; }
    double bilinear(double u, double t) const {
        const double px = u * w - 0.5, py = t * h - 0.5;
        const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
        const double fx = px - x0, fy = py - y0;
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
};
enum class Filter { SingleTap, Area };
inline Plane downscale(const Plane& src, unsigned outW, unsigned outH, Filter f) {
    Plane out{outW, outH, std::vector<double>(std::size_t(outW) * outH)};
    const double fx = double(src.w) / outW, fy = double(src.h) / outH, footprint = std::max(fx, fy);
    const unsigned n = unsigned(std::clamp(std::ceil(footprint) + 1.0, 3.0, 5.0));
    for (unsigned oy = 0; oy < outH; ++oy)
        for (unsigned ox = 0; ox < outW; ++ox) {
            const double u = (ox + 0.5) / outW, t = (oy + 0.5) / outH;
            double v = 0;
            if (f == Filter::SingleTap || footprint <= 1.001) v = src.bilinear(u, t);
            else {
                for (unsigned j = 0; j < n; ++j)
                    for (unsigned i = 0; i < n; ++i)
                        v += src.bilinear(u + ((i + 0.5) / n - 0.5) * fx / src.w, t + ((j + 0.5) / n - 0.5) * fy / src.h);
                v /= double(n * n);
            }
            out.v[std::size_t(oy) * outW + ox] = v;
        }
    return out;
}

}  // namespace hud_guard_frames
