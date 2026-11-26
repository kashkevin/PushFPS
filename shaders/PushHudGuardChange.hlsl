// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 1 (pipeline/PushHudGuard.h, PushHudGuardReference::update): per pixel the luma change of this
// real frame against the previous one and the largest luma difference to its 4 neighbours in this frame, both rounded to
// 1/255 as R8G8_UNORM stores them (BT.709 weights, BT.2020 for HDR10 PQ); per 16 x 16 tile (one thread group) how many
// pixels moved (step 2's early-out and surround share); group (0, 0) also clears the evidence counters.
Texture2D<float4> currentTex : register(t0);
Texture2D<float4> previousTex : register(t1);
RWTexture2D<float2> changeTex : register(u0);
RWTexture2D<uint> tileTex : register(u1);
RWByteAddressBuffer counters : register(u2);
cbuffer Constants : register(b0) {
    uint width, height, reset, outputMode;   // outputMode: 0 sRGB, 1 scRGB (linear), 2 PQ
    float move, still, pad1, pad2;   // tiles count change above `still` (review 2 SHOULD 2)
    uint pad3, pad4, pad5, pad6;
};
float3 perceptual(float3 v) { return outputMode == 1 ? sqrt(max(v, 0) / 2.5) : v; }
float luma(float3 v) {
    return outputMode == 2 ? 0.2627 * v.r + 0.6780 * v.g + 0.0593 * v.b : 0.2126 * v.r + 0.7152 * v.g + 0.0722 * v.b;
}
float lumaAt(int2 p) { return luma(perceptual(currentTex[clamp(p, int2(0, 0), int2(width - 1, height - 1))].rgb)); }
float quantize(float c) { return floor(saturate(c) * 255.0 + 0.5) / 255.0; }
groupshared uint gMoving;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi == 0) gMoving = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height) {
        float2 q = float2(0, 0);
        if (reset == 0) {
            const int2 p = int2(id.xy);
            const float l = lumaAt(p);
            const float c = abs(l - luma(perceptual(previousTex[id.xy].rgb)));
            const float k = max(max(abs(l - lumaAt(p + int2(1, 0))), abs(l - lumaAt(p - int2(1, 0)))),
                                max(abs(l - lumaAt(p + int2(0, 1))), abs(l - lumaAt(p - int2(0, 1)))));
            q = float2(quantize(c), quantize(k));
        }
        changeTex[id.xy] = q;
        if (q.x > still) InterlockedAdd(gMoving, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        tileTex[gid.xy] = gMoving;
        if (gid.x == 0 && gid.y == 0) { counters.Store4(0, uint4(0, 0, 0, 0)); counters.Store4(16, uint4(0, 0, 0, 0)); }
    }
}
