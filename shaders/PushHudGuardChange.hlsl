// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 1 (pipeline/PushHudGuard.h, pushHudGuardChange): per pixel the luma change of this real frame
// against the previous one, rounded to 1/255 as R8_UNORM stores it; per 16 x 16 tile (one thread group) how many pixels
// moved (step 2's early-out); group (0, 0) also clears the evidence counters step 2 adds to.
Texture2D<float4> currentTex : register(t0);
Texture2D<float4> previousTex : register(t1);
RWTexture2D<float> changeTex : register(u0);
RWTexture2D<uint> tileTex : register(u1);
RWByteAddressBuffer counters : register(u2);
cbuffer Constants : register(b0) {
    uint width, height, reset, outputMode;   // outputMode: 0 sRGB, 1 scRGB (linear), 2 PQ
    float move, pad0, pad1, pad2;
    uint pad3, pad4, pad5, pad6;
};
float3 perceptual(float3 v) { return outputMode == 1 ? sqrt(max(v, 0) / 2.5) : v; }
float luma(float3 v) { return 0.2126 * v.r + 0.7152 * v.g + 0.0722 * v.b; }
groupshared uint gMoving;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi == 0) gMoving = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height) {
        float q = 0;
        if (reset == 0) {
            const float c = abs(luma(perceptual(currentTex[id.xy].rgb)) - luma(perceptual(previousTex[id.xy].rgb)));
            q = floor(saturate(c) * 255.0 + 0.5) / 255.0;
        }
        changeTex[id.xy] = q;
        if (q > move) InterlockedAdd(gMoving, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        tileTex[gid.xy] = gMoving;
        if (gid.x == 0 && gid.y == 0) counters.Store4(0, uint4(0, 0, 0, 0));
    }
}
