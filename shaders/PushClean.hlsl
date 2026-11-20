// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS restoration ("clean-up") of a compressed console stream before any upscaler or neural layer
// (pipeline/PushCleanPass.h, docs/RESTORATION.md). The maths is include/veyra/engine/PushClean.h
// (pushCleanPixel), mirrored here line by line; tests/integration/PushGpuTests.cpp (testClean*) compares
// the two. One shader, two dispatches:
//   stage 0  the graph's linear picture -> the encoded copy (sRGB, or BT.2020 PQ with CLEAN_PQ=1)
//   stage 1  the encoded copy -> the graph's picture, written ONLY where the filter changed the pixel:
//            every pixel the filter decides not to touch keeps its own texel, bit for bit.
// Built twice: PushCleanSdr.dxil (CLEAN_PQ=0) and PushCleanPq.dxil (CLEAN_PQ=1). Unverified on Windows.
#include "HdrColor.hlsli"

#ifndef CLEAN_PQ
#define CLEAN_PQ 0
#endif

Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);

// engine::PushCleanConstants, same order (20 words).
cbuffer CleanConstants : register(b0) {
    float kFlat; float kMinStep; float kMaxStep; float kGrainTau;
    float kRingTau; float kEdge; float kTexture; float kRingTexture;
    float kLine; float kDeband; float kGrain; float kWr;
    float kWg; float kWb; uint kRadius16; uint kSeed;
    uint kParts; uint width; uint height; uint stage;
};

// Per-component branch without a vector ternary (as NisColor.hlsl).
float3 CleanSrgbEncode(float3 c) {
    c = saturate(c);
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, step(c, 0.0031308));
}
float3 CleanSrgbDecode(float3 c) {
    c = saturate(c);
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));
}
float3 CleanEncode(float3 linearRgb) {
#if CLEAN_PQ
    return HdrEncodePq(linearRgb);
#else
    return CleanSrgbEncode(linearRgb);
#endif
}
float3 CleanDecode(float3 encoded) {
#if CLEAN_PQ
    return HdrDecodePq(encoded);
#else
    return CleanSrgbDecode(encoded);
#endif
}

float3 CleanAt(int2 p) {
    const int2 q = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return source[uint2(q)].rgb;
}
float CleanLuma(float3 c) { return kWr * c.r + kWg * c.g + kWb * c.b; }
// A thin line, a text stroke or an isolated highlight: the centre beyond both sides.
bool CleanExtreme(float c, float a, float b) {
    return (c - a) * (c - b) > 0.0 && min(abs(c - a), abs(c - b)) > kLine;
}
// Deblocking along one axis: nine pixels, the centre at index 4. Adds this pixel's share of each of the four
// edges it is part of (clean_detail::edgeShare).
float3 CleanDeblock(float3 colour[9], float luma[9], inout bool deblocked) {
    float3 delta = float3(0.0, 0.0, 0.0);
    for (int e = -2; e <= 1; ++e) {
        const int a = 4 + e;
        const float jump = luma[a + 1] - luma[a];
        const float mag = abs(jump);
        const float side = max(max(abs(luma[a] - luma[a - 1]), abs(luma[a - 1] - luma[a - 2])),
                               max(abs(luma[a + 2] - luma[a + 1]), abs(luma[a + 3] - luma[a + 2])));
        if (mag < kMinStep || mag > kMaxStep || side > kFlat || mag < 2.0 * side) continue;
        float share = -0.125;
        if (e == 0) share = 0.375;
        else if (e == -1) share = -0.375;
        else if (e == 1) share = 0.125;
        deblocked = true;
        delta += share * (colour[a + 1] - colour[a]);
    }
    return delta;
}

// ---------------------------------------------------------------------------------------------------------
// PORTED from libplacebo (https://code.videolan.org/videolan/libplacebo, commit 89b04663135b, LGPL-2.1 or
// later): the generator of src/shaders.c `sh_prng` (pcg3d, http://jcgt.org/published/0009/03/02/) and, in
// main below ("Deband"), the debanding of src/shaders/sampling.c `pl_shader_deband`. The original notice:
/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */
// Changed for PushFPS (2026-11-20): see the same part of include/veyra/engine/PushClean.h.
uint3 CleanRandom(uint3 s) {
    s = 1664525u * s + 1013904223u;
    s.x += s.y * s.z;
    s.y += s.z * s.x;
    s.z += s.x * s.y;
    s ^= s >> 16u;
    s.x += s.y * s.z;
    s.y += s.z * s.x;
    s.z += s.x * s.y;
    return s;
}
// 256 * cos((k + 0.5) * pi / 32), k = 0..15 (kPushCleanQuarterCos).
static const int kCleanQuarterCos[16] = {256, 253, 248, 241, 231, 220, 206, 190, 172, 152, 132, 109, 86, 62, 38, 13};
// ---- end of the generator ported from libplacebo ----------------------------------------------------------

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    if (stage == 0u) {
        const float4 linearColour = source[id.xy];
        target[id.xy] = float4(CleanEncode(linearColour.rgb), linearColour.a);
        return;
    }
    if (kParts == 0u) return;
    const int2 p = int2(id.xy);
    // 5x5 window, row-major, centre at 12.
    float3 px[25];
    float w[25];
    for (int wj = 0; wj < 5; ++wj) {
        for (int wi = 0; wi < 5; ++wi) {
            const float3 v = CleanAt(p + int2(wi - 2, wj - 2));
            px[wj * 5 + wi] = v;
            w[wj * 5 + wi] = CleanLuma(v);
        }
    }
    const float3 centre = px[12];
    const float c0 = w[12];
    // Protection 1: a strong edge through the pixel.
    if (max(abs(w[13] - w[11]), abs(w[17] - w[7])) > kEdge) return;
    // Protection 2: a thin line, a text stroke or an isolated highlight.
    if (CleanExtreme(c0, w[11], w[13]) || CleanExtreme(c0, w[7], w[17]) || CleanExtreme(c0, w[6], w[18]) ||
        CleanExtreme(c0, w[8], w[16]) || CleanExtreme(c0, w[11], w[14]) || CleanExtreme(c0, w[10], w[13]) ||
        CleanExtreme(c0, w[7], w[22]) || CleanExtreme(c0, w[2], w[17])) return;
    // Neighbourhood activity: adjacent differences inside the 5x5, the strong ones (edges) set apart.
    float activity = 0.0;
    float weak = 0.0;
    bool nearEdge = false;
    for (int aj = 0; aj < 5; ++aj) {
        for (int ai = 0; ai < 5; ++ai) {
            const float v = w[aj * 5 + ai];
            if (ai < 4) {
                const float d = abs(v - w[aj * 5 + ai + 1]);
                if (d > kEdge) nearEdge = true;
                else { activity += d; weak += 1.0; }
            }
            if (aj < 4) {
                const float d = abs(v - w[(aj + 1) * 5 + ai]);
                if (d > kEdge) nearEdge = true;
                else { activity += d; weak += 1.0; }
            }
        }
    }
    activity = weak > 0.0 ? activity / weak : 0.0;
    // Protection 3: texture (busy small differences); a wider allowance beside a strong edge (ringing).
    if (activity > (nearEdge ? kRingTexture : kTexture)) return;
    // 3x3 range of each channel: the clamp of the deblock and smooth results.
    float3 mn = centre;
    float3 mx = centre;
    for (int rj = 1; rj <= 3; ++rj) {
        for (int ri = 1; ri <= 3; ++ri) {
            mn = min(mn, px[rj * 5 + ri]);
            mx = max(mx, px[rj * 5 + ri]);
        }
    }
    float3 result = centre;
    bool deblocked = false;
    if ((kParts & 2u) != 0u) {
        // Deblock: the four edges along the row and the four along the column that this pixel is part of.
        float3 rowColour[9];
        float rowLuma[9];
        float3 colColour[9];
        float colLuma[9];
        for (int k = 0; k < 9; ++k) {
            if (k >= 2 && k <= 6) {
                rowColour[k] = px[8 + k];
                rowLuma[k] = w[8 + k];
                colColour[k] = px[(k - 2) * 5 + 2];
                colLuma[k] = w[(k - 2) * 5 + 2];
            } else {
                rowColour[k] = CleanAt(p + int2(k - 4, 0));
                rowLuma[k] = CleanLuma(rowColour[k]);
                colColour[k] = CleanAt(p + int2(0, k - 4));
                colLuma[k] = CleanLuma(colColour[k]);
            }
        }
        float3 delta = CleanDeblock(rowColour, rowLuma, deblocked);
        delta += CleanDeblock(colColour, colLuma, deblocked);
        if (deblocked) result = clamp(centre + delta, mn, mx);
    }
    if (!deblocked && (kParts & (nearEdge ? 4u : 8u)) != 0u) {
        // Smooth: range-limited mean of the 5x5, guided by luma (dering beside a strong edge, grain elsewhere).
        const float tau = nearEdge ? kRingTau : kGrainTau;
        float3 sum = float3(0.0, 0.0, 0.0);
        float n = 0.0;
        for (int sk = 0; sk < 25; ++sk) {
            if (abs(w[sk] - c0) <= tau) { sum += px[sk]; n += 1.0; }
        }
        result = clamp(sum / n, mn, mx);
    }
    if ((kParts & 1u) != 0u) {
        // Deband (ported from libplacebo's pl_shader_deband, one iteration): four taps at one random distance
        // and angle, the tap mean replaces each component that is within the threshold of it, then the grain.
        uint3 state = CleanRandom(uint3(id.x, id.y, kSeed));
        const int dist = int(((state.x >> 16u) * kRadius16) >> 16u);
        const uint turn = state.y >> 28u;
        const int dx = (dist * kCleanQuarterCos[turn] + 2048) >> 12;
        const int dy = (dist * kCleanQuarterCos[15u - turn] + 2048) >> 12;
        const float3 t0 = CleanAt(p + int2(dx, dy));
        const float3 t1 = CleanAt(p + int2(-dx, dy));
        const float3 t2 = CleanAt(p + int2(-dx, -dy));
        const float3 t3 = CleanAt(p + int2(dx, -dy));
        const float3 before = result;
        const float3 lo = min(min(before, t0), min(min(t1, t2), t3));
        const float3 hi = max(max(before, t0), max(max(t1, t2), t3));
        const float3 avg = (t0 + t1 + t2 + t3) * 0.25;
        const float3 diff = abs(before - avg);
        bool debanded = false;
        if (diff.r <= kDeband) { result.r = avg.r; if (avg.r != before.r) debanded = true; }
        if (diff.g <= kDeband) { result.g = avg.g; if (avg.g != before.g) debanded = true; }
        if (diff.b <= kDeband) { result.b = avg.b; if (avg.b != before.b) debanded = true; }
        if (debanded) {
            state = CleanRandom(state);
            const float3 grainRandom = float3(state >> 8u) * (1.0 / 16777216.0);
            // No grain near true black: its strength is at most the value itself.
            result = clamp(result + min(abs(result), kGrain) * (grainRandom - 0.5), lo, hi);
        }
    }
    // Untouched: no write, the picture keeps its own texel.
    if (result.r == centre.r && result.g == centre.g && result.b == centre.b) return;
    target[id.xy] = float4(CleanDecode(result), source[id.xy].a);
}
