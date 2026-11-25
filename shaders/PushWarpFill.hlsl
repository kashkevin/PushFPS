// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg" R2 (PushWarp.h pushWarpReference, "R2"): the push-pull fill of the splatted motion at the flow size.
// mode 0 resolve: level 0 from the accumulators (vector = acc / wsum / 32, known when the coverage reaches fillKnown,
//                 visibility = coverage / coverageFull clamped);
// mode 1 down:    this level = the mean of its existing 2 x 2 children (premultiplied vectors and known shares);
// mode 2 up:      estimate = known x own + (1 - known) x the coarser level's estimate (bilinear, edge-clamped); at the
//                 coarsest level (otherWidth 0) the own value alone.
// Pyramid cell (32 bytes): vx, vy (premultiplied), known, visibility, estimate x, estimate y, 2 unused.
#include "PushWarp.hlsli"

RWByteAddressBuffer scratch : register(u1);

float2 estimateAt(uint offset, uint w, uint x, uint y) { return asfloat(scratch.Load2(pyramidAddress(offset + y * w + x) + 16)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= levelWidth || id.y >= levelHeight) return;
    const uint here = pyramidAddress(levelOffset + id.y * levelWidth + id.x);
    if (mode == 0) {
        const uint index = id.y * flowWidth + id.x;
        const uint4 s = scratch.Load4(accumAddress(index));   // top, wsum, cov, accx
        const int accy = asint(scratch.Load(accumAddress(index) + 16));
        const float cov = float(s.z) / kWeightScale;
        const float visibility = clamp(cov / coverageFull, 0.0, 1.0);
        const bool known = s.y > 0 && cov >= fillKnown;
        float vx = 0, vy = 0;
        if (known) {
            vx = float(asint(s.w)) / float(s.y) / kVectorScale;
            vy = float(accy) / float(s.y) / kVectorScale;
        }
        scratch.Store4(here, uint4(asuint(vx), asuint(vy), asuint(known ? 1.0 : 0.0), asuint(visibility)));
        return;
    }
    if (mode == 1) {
        float vx = 0, vy = 0, k = 0, count = 0;
        for (uint j = 0; j < 4; ++j) {
            const uint cx = 2 * id.x + (j & 1), cy = 2 * id.y + (j >> 1);
            if (cx >= otherWidth || cy >= otherHeight) continue;
            const float3 child = asfloat(scratch.Load3(pyramidAddress(otherOffset + cy * otherWidth + cx)));
            vx += child.x; vy += child.y; k += child.z; count += 1.0;
        }
        scratch.Store3(here, uint3(asuint(vx / count), asuint(vy / count), asuint(k / count)));
        return;
    }
    const float3 own = asfloat(scratch.Load3(here));
    const float ox = own.z > 1e-6 ? own.x / own.z : 0.0, oy = own.z > 1e-6 ? own.y / own.z : 0.0;
    if (otherWidth == 0) {
        scratch.Store2(here + 16, uint2(asuint(ox), asuint(oy)));
        return;
    }
    const float qx = clamp((float(id.x) + 0.5) * 0.5 - 0.5, 0.0, float(otherWidth - 1));
    const float qy = clamp((float(id.y) + 0.5) * 0.5 - 0.5, 0.0, float(otherHeight - 1));
    const float fx = floor(qx), fy = floor(qy), tx = qx - fx, ty = qy - fy;
    const uint x0 = uint(fx), y0 = uint(fy);
    const uint x1 = min(x0 + 1, otherWidth - 1), y1 = min(y0 + 1, otherHeight - 1);
    const float2 c00 = estimateAt(otherOffset, otherWidth, x0, y0), c10 = estimateAt(otherOffset, otherWidth, x1, y0);
    const float2 c01 = estimateAt(otherOffset, otherWidth, x0, y1), c11 = estimateAt(otherOffset, otherWidth, x1, y1);
    const float2 top = c00 + (c10 - c00) * tx;
    const float2 up = top + ((c01 + (c11 - c01) * tx) - top) * ty;
    const float s = clamp(own.z, 0.0, 1.0);
    scratch.Store2(here + 16, uint2(asuint(s * ox + (1.0 - s) * up.x), asuint(s * oy + (1.0 - s) * up.y)));
}
