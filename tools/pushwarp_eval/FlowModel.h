// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// A MODEL of AMD FidelityFX optical flow's block search (SDK 1.1.4 as read from its source: 8x8 blocks, +-8 search per
// level, a 2x pyramid, each level's vectors doubled into the next finer one, ties to the centre, zero fallback at level 0)
// and of PushFPS's densify (nearest block, photometric trust), at a quarter of the work size. It is NOT AMD's code and
// does not reproduce its numbers exactly: it is the field model used offline by tools/pushwarp_eval (the C++ port of
// ffx_check_sim.py flow() + hud_eval.py field_of(); same candidate order, same rounding). g++ only, no GPU.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace pushwarp_eval {

struct Plane {   // integer luma at the flow size
    std::uint32_t w = 0, h = 0;
    std::vector<int> v;
    int at(int x, int y) const { return v[std::size_t(std::clamp(y, 0, int(h) - 1)) * w + std::size_t(std::clamp(x, 0, int(w) - 1))]; }
};
struct Vec { int x = 0, y = 0; };
struct BlockField { std::uint32_t bw = 0, bh = 0; std::vector<Vec> v; };

// One level: the best offset (from the prediction) of every 8x8 block of cur in prev, SAD; candidates dx, dy in
// [-radius, radius) in the order (|dy|, |dx|, dy, dx), strictly better wins (ties keep the earlier, nearer one).
inline BlockField modelSearch(const Plane& cur, const Plane& prev, const BlockField& pred, bool zeroFallback, int radius = 8, int block = 8) {
    BlockField out;
    out.bh = std::max(1u, cur.h / std::uint32_t(block));
    out.bw = std::max(1u, cur.w / std::uint32_t(block));
    out.v.assign(std::size_t(out.bw) * out.bh, Vec{});
    std::vector<Vec> order;
    for (int dy = -radius; dy < radius; ++dy)
        for (int dx = -radius; dx < radius; ++dx) order.push_back({dx, dy});
    std::stable_sort(order.begin(), order.end(), [](const Vec& a, const Vec& b) {
        if (std::abs(a.y) != std::abs(b.y)) return std::abs(a.y) < std::abs(b.y);
        if (std::abs(a.x) != std::abs(b.x)) return std::abs(a.x) < std::abs(b.x);
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    const int off = radius + 2048;   // ffx_check_sim pads by this much (edge) and clips the window start into the padding
    for (std::uint32_t by = 0; by < out.bh; ++by)
        for (std::uint32_t bx = 0; bx < out.bw; ++bx) {
            const Vec p = pred.v.empty() ? Vec{} : pred.v[std::size_t(by) * out.bw + bx];
            const auto sad = [&](int sx, int sy) {
                sx = std::clamp(sx, -off, int(prev.w) + off - block);
                sy = std::clamp(sy, -off, int(prev.h) + off - block);
                long long s = 0;
                for (int j = 0; j < block; ++j)
                    for (int i = 0; i < block; ++i) s += std::abs(prev.at(sx + i, sy + j) - cur.at(int(bx) * block + i, int(by) * block + j));
                return s;
            };
            long long best = (std::numeric_limits<long long>::max)();
            Vec bestV{};
            for (const auto& d : order) {
                const long long s = sad(int(bx) * block + p.x + d.x, int(by) * block + p.y + d.y);
                if (s < best) { best = s; bestV = {p.x + d.x, p.y + d.y}; }
            }
            if (zeroFallback && sad(int(bx) * block, int(by) * block) <= best) bestV = {};
            out.v[std::size_t(by) * out.bw + bx] = bestV;
        }
    return out;
}

// The vector median of the 3 x 3 (edge padded): the candidate with the least sum of squared distances to all nine.
inline BlockField modelMedian3(const BlockField& f) {
    BlockField out = f;
    for (int y = 0; y < int(f.bh); ++y)
        for (int x = 0; x < int(f.bw); ++x) {
            Vec cand[9];
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    cand[k++] = f.v[std::size_t(std::clamp(y + dy, 0, int(f.bh) - 1)) * f.bw + std::size_t(std::clamp(x + dx, 0, int(f.bw) - 1))];
            long long bestD = (std::numeric_limits<long long>::max)();
            int best = 0;
            for (int i = 0; i < 9; ++i) {
                long long d = 0;
                for (int j = 0; j < 9; ++j) {
                    const long long ex = cand[i].x - cand[j].x, ey = cand[i].y - cand[j].y;
                    d += ex * ex + ey * ey;
                }
                if (d < bestD) { bestD = d; best = i; }
            }
            out.v[std::size_t(y) * f.bw + std::size_t(x)] = cand[best];
        }
    return out;
}

// The pyramid search: cur (B) against prev (A), vectors B -> A per 8x8 block at the flow size.
inline BlockField modelFlow(const Plane& cur, const Plane& prev, int levels = 7) {
    std::vector<std::vector<double>> pc{std::vector<double>(cur.v.begin(), cur.v.end())}, pp{std::vector<double>(prev.v.begin(), prev.v.end())};
    std::vector<std::uint32_t> ws{cur.w}, hs{cur.h};
    for (int l = 1; l < levels; ++l) {
        const std::uint32_t w2 = ws.back() / 2, h2 = hs.back() / 2, w = ws.back();
        for (auto* pyr : {&pc, &pp}) {
            const auto& a = pyr->back();
            std::vector<double> n(std::size_t(w2) * h2);
            for (std::uint32_t y = 0; y < h2; ++y)
                for (std::uint32_t x = 0; x < w2; ++x)
                    n[std::size_t(y) * w2 + x] = (a[(2 * y) * w + 2 * x] + a[(2 * y) * w + 2 * x + 1] + a[(2 * y + 1) * w + 2 * x] + a[(2 * y + 1) * w + 2 * x + 1]) / 4.0;
            pyr->push_back(std::move(n));
        }
        ws.push_back(w2); hs.push_back(h2);
    }
    BlockField vec;
    for (int l = levels - 1; l >= 0; --l) {
        if (ws[std::size_t(l)] < 8 || hs[std::size_t(l)] < 8) continue;
        Plane c{ws[std::size_t(l)], hs[std::size_t(l)], {}}, p{c.w, c.h, {}};
        for (double d : pc[std::size_t(l)]) c.v.push_back(int(std::nearbyint(d)));   // numpy round: half to even
        for (double d : pp[std::size_t(l)]) p.v.push_back(int(std::nearbyint(d)));
        BlockField pred;
        pred.bh = std::max(1u, c.h / 8); pred.bw = std::max(1u, c.w / 8);
        if (!vec.v.empty()) {
            pred.v.resize(std::size_t(pred.bw) * pred.bh);
            for (std::uint32_t y = 0; y < pred.bh; ++y)
                for (std::uint32_t x = 0; x < pred.bw; ++x) {
                    const auto& s = vec.v[std::size_t(std::min(y / 2, vec.bh - 1)) * vec.bw + std::min(x / 2, vec.bw - 1)];
                    pred.v[std::size_t(y) * pred.bw + x] = {s.x * 2, s.y * 2};
                }
        }
        vec = modelMedian3(modelSearch(c, p, pred, l == 0));
    }
    return vec;
}

// PushFPS's densify on the model's blocks: every flow pixel takes its block's vector; trust from how well A at the moved
// place matches B (photometric, smoothstep 0.03..0.15 of 255, in 1/255 steps). Output as PushWarpField wants it:
// stored = vector x trust, and trust.
struct ModelField { std::uint32_t w = 0, h = 0; std::vector<float> x, y, trust; };
inline ModelField modelDensify(const BlockField& blocks, const Plane& lb, const Plane& la) {
    ModelField f{lb.w, lb.h, {}, {}, {}};
    const std::size_t n = std::size_t(f.w) * f.h;
    f.x.resize(n); f.y.resize(n); f.trust.resize(n);
    for (std::uint32_t y = 0; y < f.h; ++y)
        for (std::uint32_t x = 0; x < f.w; ++x) {
            const auto& v = blocks.v[std::size_t(std::min(y / 8, blocks.bh - 1)) * blocks.bw + std::min(x / 8, blocks.bw - 1)];
            const float qx = std::clamp(float(x) + float(v.x), 0.0f, float(f.w - 1)), qy = std::clamp(float(y) + float(v.y), 0.0f, float(f.h - 1));
            const int x0 = int(std::floor(qx)), y0 = int(std::floor(qy));
            const float tx = qx - float(x0), ty = qy - float(y0);
            const auto s = [&](int xx, int yy) { return float(la.at(xx, yy)) / 255.0f; };
            const float top = s(x0, y0) + (s(x0 + 1, y0) - s(x0, y0)) * tx, bottom = s(x0, y0 + 1) + (s(x0 + 1, y0 + 1) - s(x0, y0 + 1)) * tx;
            const float prevAt = top + (bottom - top) * ty;
            const double err = std::abs(double(lb.at(int(x), int(y))) / 255.0 - double(prevAt));
            const double st = std::clamp((err - 0.03) / 0.12, 0.0, 1.0);
            const float trust = float(std::nearbyint((1.0 - st * st * (3.0 - 2.0 * st)) * 255.0) / 255.0);
            const std::size_t i = std::size_t(y) * f.w + x;
            f.x[i] = float(v.x) * trust; f.y[i] = float(v.y) * trust; f.trust[i] = trust;
        }
    return f;
}

// BT.709 luma of 8-bit RGB, box-reduced 4 x 4 to the flow size and rounded (hud_eval.py luma_small). rgb8: 3 per pixel.
inline Plane modelLumaQuarter(const std::vector<std::uint8_t>& rgb8, std::uint32_t W, std::uint32_t H) {
    Plane p{W / 4, H / 4, {}};
    p.v.resize(std::size_t(p.w) * p.h);
    for (std::uint32_t y = 0; y < p.h; ++y)
        for (std::uint32_t x = 0; x < p.w; ++x) {
            double s = 0;
            for (std::uint32_t j = 0; j < 4; ++j)
                for (std::uint32_t i = 0; i < 4; ++i) {
                    const std::uint8_t* c = &rgb8[(std::size_t(4 * y + j) * W + 4 * x + i) * 3];
                    s += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
                }
            p.v[std::size_t(y) * p.w + x] = int(std::nearbyint(s / 16.0));
        }
    return p;
}

}  // namespace pushwarp_eval
