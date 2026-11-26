// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 4 (pipeline/PushHudGuard.h, PushHudGuardReference::compose): a generated frame at time t
// takes its protected pixels (mask grown by `grow` px) from the real frames, A + t (B - A): the console's own overlay
// pixels, in place in every frame. Writes only those pixels, straight into the generated frame (typed UAV store).
Texture2D<float4> aTex : register(t0);
Texture2D<float4> bTex : register(t1);
Texture2D<float2> stateTex : register(t2);
RWTexture2D<float4> outTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, stillFrames, grow;
    float t, pad0, pad1, pad2;
    uint pad3, pad4, pad5, pad6;
};
bool protectedAt(int2 q) {
    const float2 s = stateTex[q];
    const uint run = uint(s.x + 0.5), ev = uint(s.y + 0.5);
    return run >= stillFrames && (ev & 15u) >= 1;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    bool hit = false;
    [loop] for (int dy = -int(grow); dy <= int(grow) && !hit; ++dy)
        [loop] for (int dx = -int(grow); dx <= int(grow) && !hit; ++dx) {
            const int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
            hit = protectedAt(q);
        }
    if (!hit) return;
    const float4 a = aTex[p], b = bTex[p];
    outTex[p] = a + t * (b - a);
}
