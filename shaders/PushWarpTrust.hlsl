// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg" W3 (PushWarp.h pushWarpCellOf): per flow cell, the B -> A vector g = stored / trust (the field is stored
// as v x trust, GpuDisValidate / NvofDensify), the flow's own trust, and the block-SAD trust: the mean over 4 x 4 work
// pixels of the cell of the largest colour difference between B and A shifted by g (whole pixels), 1 below sadLow and 0
// above sadHigh (smoothstep). A cell below minConfidence, or whose vector is longer than maxMotion, has trust 0 and does
// not splat. Also clears the cell's accumulators for the splat that follows (one thread per cell writes both).
#include "PushWarp.hlsli"

ByteAddressBuffer previousFrame : register(t0);   // A
ByteAddressBuffer currentFrame : register(t1);    // B
Texture2D<float2> flow : register(t2);            // B -> A, flow pixels, x trust
Texture2D<float> trust : register(t3);
Texture2D<float2> previousFlow : register(t4);    // the previous pair's field (Z -> A on A's grid), only with flags bit 1
Texture2D<float> previousTrust : register(t5);
RWByteAddressBuffer scratch : register(u1);

uint footprint(uint cell, uint i, uint work, uint grid)
{
    const uint x0 = cell * work / grid;
    const uint x1 = max(x0 + 1, (cell + 1) * work / grid);
    return min(x0 + i * (x1 - x0) / 4, work - 1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowWidth || id.y >= flowHeight) return;
    const uint index = id.y * flowWidth + id.x;
    // Clear this cell's accumulators: top, wsum, cov, accx, accy (and the 3 unused words).
    scratch.Store4(accumAddress(index), uint4(0, 0, 0, 0));
    scratch.Store4(accumAddress(index) + 16, uint4(0, 0, 0, 0));
    float gx = 0, gy = 0, cellTrust = 0;
    uint importance = 0;
    const float conf = trust[id.xy];
    if (conf > 1e-6) {
        const float2 stored = flow[id.xy];
        gx = stored.x / conf;
        gy = stored.y / conf;
        const float len = sqrt(gx * gx + gy * gy);
        if (!(conf >= minConfidence) || !(len <= maxMotion)) {
            gx = 0; gy = 0;
        } else {
            const float sx = float(width) / float(flowWidth), sy = float(height) / float(flowHeight);
            const int ox = roundHalfUp(gx * sx), oy = roundHalfUp(gy * sy);
            float sum = 0;
            for (uint j = 0; j < 4; ++j)
                for (uint k = 0; k < 4; ++k) {
                    const int x = int(footprint(id.x, k, width, flowWidth)), y = int(footprint(id.y, j, height, flowHeight));
                    sum += maxDelta(texelAt(currentFrame, int2(x, y)), texelAt(previousFrame, int2(x + ox, y + oy)));
                }
            const float mad = sum / 16.0;
            const float s = saturate((mad - sadLow) / (sadHigh - sadLow));
            cellTrust = conf * (1.0 - s * s * (3.0 - 2.0 * s));
            const float v = len * beta * kImportanceScale + 0.5;
            importance = v >= float(kImportanceCap) ? kImportanceCap : uint(max(v, 0.0));
            // Prediction with acceleration (PushWarp.h pushWarpAccelerated): the previous vector where B's content was in A.
            if ((flags & 3u) == 3u) {
                const int zx = roundHalfUp(float(id.x) + gx), zy = roundHalfUp(float(id.y) + gy);
                if (zx >= 0 && zy >= 0 && zx < int(flowWidth) && zy < int(flowHeight)) {
                    const float zt = previousTrust[int2(zx, zy)];
                    if (zt >= minConfidence) {
                        const float2 z = previousFlow[int2(zx, zy)] / zt;
                        const float s = frameTime - 1.0;
                        float ax = z.x - gx, ay = z.y - gy;
                        const float alen = sqrt(ax * ax + ay * ay);
                        if (alen >= accelMin && s > 0) {
                            const float k = s * 0.5 + s * s * 0.5;
                            const float limit = sqrt(gx * gx + gy * gy) * s, termLen = alen * k;
                            const float scale = termLen > limit ? (termLen > 0 ? limit / termLen : 0.0) : 1.0;
                            ax *= scale; ay *= scale;
                            const float dx = -gx * s + ax * k, dy = -gy * s + ay * k;
                            gx = -dx / s; gy = -dy / s;
                        }
                    }
                }
            }
        }
    }
    scratch.Store4(cellAddress(index), uint4(asuint(gx), asuint(gy), asuint(cellTrust), importance));
}
