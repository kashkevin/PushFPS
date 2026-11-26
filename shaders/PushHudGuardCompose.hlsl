// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 5 (pipeline/PushHudGuard.h, PushHudGuardReference::compose): a generated frame at time t
// takes from the real frames, A + t (B - A), its protected pixels and the pixels within `grow` px of one whose own change
// is at most `move` (no growth into moving picture: review SHOULD 4). Writes only those pixels, straight into the
// generated frame (typed UAV store).
Texture2D<float4> aTex : register(t0);
Texture2D<float4> bTex : register(t1);
Texture2D<uint> maskTex : register(t2);
Texture2D<float2> changeTex : register(t3);
RWTexture2D<float4> outTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, grow, pad0;
    float t, move, pad1, pad2;
    uint pad3, pad4, pad5, pad6;
};

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
    outTex[p] = a + t * (b - a);
}
