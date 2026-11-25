// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg" R3 (PushWarp.h pushWarpReference, "R3"): the interpolated frame I(t) at the work size.
//   still     max |A - B| <= stillDelta: A exactly (HUD, letterbox)
//   otherwise the filled field g (B -> A) and the visibility at p, bilinear between flow-cell centres (PushPredict.hlsl's
//             mapping); one bilinear tap of A at p + t g and one of B at p - (1 - t) g (work pixels); a hole (visibility
//             below holeVisibility: nothing of B landed there) uses visibility 1; weights (1 - t) and t x visibility;
//             samples further apart than conflictDelta: the larger weight alone (no double image), else their blend.
// Code values are blended as they are (PQ codes for HDR10). Counters (four uint32 per slot): still, blend, conflict, hole.
#include "PushWarp.hlsli"

ByteAddressBuffer previousFrame : register(t0);   // A
ByteAddressBuffer currentFrame : register(t1);    // B
RWTexture2D<float4> output : register(u0);
RWByteAddressBuffer scratch : register(u1);
RWByteAddressBuffer counters : register(u2);

groupshared uint groupCounts[4];

float3 fieldAt(uint x, uint y)
{
    const uint a = pyramidAddress(y * flowWidth + x);
    return float3(asfloat(scratch.Load2(a + 16)), asfloat(scratch.Load(a + 12)));   // estimate x, y; visibility
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < 4) groupCounts[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height) {
        const int2 p = int2(id.xy);
        const float4 a = texelAt(previousFrame, p);
        const float4 b = texelAt(currentFrame, p);
        const bool predicting = (flags & 1u) != 0;
        float4 result = predicting ? b : a;   // still: A exactly (interpolated), B exactly (predicted)
        uint kind = 0;
        if (maxDelta(a, b) > stillDelta) {
            const float sx = float(flowWidth) / float(width), sy = float(flowHeight) / float(height);
            const float qx = max((float(id.x) + 0.5) * sx - 0.5, 0.0), qy = max((float(id.y) + 0.5) * sy - 0.5, 0.0);
            const float fx = floor(qx), fy = floor(qy), tx = qx - fx, ty = qy - fy;
            const uint x0 = min(uint(fx), flowWidth - 1), y0 = min(uint(fy), flowHeight - 1);
            const uint x1 = min(x0 + 1, flowWidth - 1), y1 = min(y0 + 1, flowHeight - 1);
            const float3 f00 = fieldAt(x0, y0), f10 = fieldAt(x1, y0), f01 = fieldAt(x0, y1), f11 = fieldAt(x1, y1);
            const float3 top = f00 + (f10 - f00) * tx;
            const float3 f = top + ((f01 + (f11 - f01) * tx) - top) * ty;
            const float gx = f.x * (float(width) / float(flowWidth)), gy = f.y * (float(height) / float(flowHeight));
            float vis = f.z;
            const bool hole = vis < holeVisibility;
            if (predicting) {
                // From B alone: nothing landed here (revealed or untrusted) -> B held; else B moved forward.
                result = hole ? b : bilinearAt(currentFrame, float2(float(id.x) - (1.0 - frameTime) * gx, float(id.y) - (1.0 - frameTime) * gy));
                kind = hole ? 3 : 1;
            } else {
            if (hole) vis = 1.0;
            const float4 s0 = bilinearAt(previousFrame, float2(float(id.x) + frameTime * gx, float(id.y) + frameTime * gy));
            const float4 s1 = bilinearAt(currentFrame, float2(float(id.x) - (1.0 - frameTime) * gx, float(id.y) - (1.0 - frameTime) * gy));
            const float w0 = (1.0 - frameTime) * vis, w1 = frameTime * vis;
            const bool conflict = maxDelta(s0, s1) > conflictDelta;
            if (conflict) result = w0 >= w1 ? s0 : s1;
            else result = (s0 * w0 + s1 * w1) / (w0 + w1);
            kind = hole ? 3 : conflict ? 2 : 1;
            }
        }
        output[id.xy] = result;
        InterlockedAdd(groupCounts[kind], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 4 && groupCounts[index] != 0) counters.InterlockedAdd(counterOffset + index * 4, groupCounts[index]);
}
