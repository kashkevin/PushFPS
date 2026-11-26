// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 5 (pipeline/PushHudGuard.h, PushHudGuardReference::compose): a generated frame at time t
// takes from the real frames, A + t (B - A), its protected pixels and the pixels within `grow` px of one whose own change
// is at most `move` (no growth into moving picture: review SHOULD 4). Review 2 SHOULD 1: when this pair's generator
// input was the cleaned copy (hasCleaned), a pixel of the clean area (bit 1 grown by `grow`) whose luma in B is within
// backgroundMatch of the cleaned copy shows background, not the overlay: it keeps the generator's output (no hard
// outline of interpolated-from-A/B background around the crosshair). Writes only the pixels it takes, straight into the
// generated frame (typed UAV store).
Texture2D<float4> aTex : register(t0);
Texture2D<float4> bTex : register(t1);
Texture2D<uint> maskTex : register(t2);
Texture2D<float2> changeTex : register(t3);
Texture2D<float4> cleanedTex : register(t4);   // the cleaned copy of B (only read when hasCleaned)
RWTexture2D<float4> outTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, grow, outputMode;   // outputMode: 0 sRGB, 1 scRGB (linear), 2 PQ
    float t, move, backgroundMatch, pad2;
    uint hasCleaned, pad4, pad5, pad6;
};
float3 perceptual(float3 v) { return outputMode == 1 ? sqrt(max(v, 0) / 2.5) : v; }
float luma(float3 v) {
    return outputMode == 2 ? 0.2627 * v.r + 0.6780 * v.g + 0.0593 * v.b : 0.2126 * v.r + 0.7152 * v.g + 0.0722 * v.b;
}
bool grownBit(int2 p, uint bit) {
    [loop] for (int dy = -int(grow); dy <= int(grow); ++dy)
        [loop] for (int dx = -int(grow); dx <= int(grow); ++dx) {
            const int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
            if ((maskTex[q] & bit) != 0) return true;
        }
    return false;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    bool hit = (maskTex[p] & 1u) != 0;
    if (!hit && changeTex[p].x <= move) {
        [loop] for (int dy = -int(grow); dy <= int(grow) && !hit; ++dy)
            [loop] for (int dx = -int(grow); dx <= int(grow) && !hit; ++dx) {
                const int2 q = p + int2(dx, dy);
                if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
                hit = (maskTex[q] & 1u) != 0;
            }
    }
    if (!hit) return;
    const float4 a = aTex[p], b = bTex[p];
    if (hasCleaned != 0 && grownBit(p, 2u) && abs(luma(perceptual(b.rgb)) - luma(perceptual(cleanedTex[p].rgb))) <= backgroundMatch) return;
    outTex[p] = a + t * (b - a);
}
