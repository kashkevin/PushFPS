// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace": the CPU reference of PresentBlit's area filter for a shrinking blit (shaders/PresentBlit.hlsl AreaSample,
// flags 512 / 1024). His screen is 2560x1440 and the picture 3840x2160, so every frame he sees is shrunk 1.5x; Package 16 took
// ONE bilinear tap per output pixel (two source pixels of every three fully, the third not at all), and thin edges in motion
// crawl and shimmer at any frame rate. The filter: n x n bilinear taps over the output pixel's footprint, n = ceil(footprint) + 1
// clamped to 3..5, equal weights; a PQ-coded source (HDR10) is averaged in linear light (SMPTE ST 2084 per channel, no gamut
// change) and coded again. 1:1 and magnification (footprint <= 1.001) keep today's path bit for bit. Pure; tests:
// tests/remoteplay/OnScreenCadenceTests.inc (downscale_*).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace veyra::engine {

// n = ceil(footprint) + 1, 3..5: a box exactly one output pixel wide made of n bilinear taps. Chosen offline (this file's tests):
// at 1.5x, 2 taps left the line's energy pulsing (0.094), 3 taps conserve it (0.000) for a 10 % lower contrast at a 4-pixel
// period; a twice-wider tent pulses least but costs a third of the contrast (too soft).
inline unsigned pushAreaTaps(double footprint) { return unsigned(std::clamp(std::ceil(footprint) + 1.0, 3.0, 5.0)); }
inline bool pushAreaApplies(bool switchOn, double footprint) { return switchOn && footprint > 1.001; }

inline double pushPqToLinear(double pq) {
    const double p = std::pow(std::clamp(pq, 0.0, 1.0), 32.0 / 2523.0);
    return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / std::max(2413.0 / 128.0 - 2392.0 / 128.0 * p, 1e-6), 16384.0 / 2610.0);
}
inline double pushLinearToPq(double l) {
    const double p = std::pow(std::clamp(l, 0.0, 1.0), 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}

// One channel image, row major, clamped addressing; uv in 0..1 as the GPU's SampleLevel with a bilinear clamp sampler.
struct PushPlane {
    unsigned width = 0, height = 0;
    std::vector<double> v;
    double at(int x, int y) const {
        x = std::clamp(x, 0, int(width) - 1);
        y = std::clamp(y, 0, int(height) - 1);
        return v[std::size_t(y) * width + std::size_t(x)];
    }
    double bilinear(double u, double t) const {
        const double px = u * width - 0.5, py = t * height - 0.5;
        const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
        const double fx = px - x0, fy = py - y0;
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
};
// The output pixel (ox, oy) of an outW x outH image covering the whole source (axis aligned, as his window: no rotation).
inline double pushBlitSample(const PushPlane& src, unsigned outW, unsigned outH, unsigned ox, unsigned oy, bool area, bool pq) {
    const double u = (ox + 0.5) / outW, t = (oy + 0.5) / outH;
    const double fx = double(src.width) / outW, fy = double(src.height) / outH;
    const double footprint = std::max(fx, fy);
    if (!pushAreaApplies(area, footprint)) return src.bilinear(u, t);
    const unsigned n = pushAreaTaps(footprint);
    double sum = 0;
    for (unsigned j = 0; j < n; ++j)
        for (unsigned i = 0; i < n; ++i) {
            const double ou = ((i + 0.5) / n - 0.5) * fx / src.width, ot = ((j + 0.5) / n - 0.5) * fy / src.height;
            const double c = src.bilinear(u + ou, t + ot);
            sum += pq ? pushPqToLinear(c) : c;
        }
    sum /= double(n * n);
    return pq ? pushLinearToPq(sum) : sum;
}

}  // namespace veyra::engine
