// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 3 (pipeline/PushHudGuard.h, PushHudGuardReference::clean): the copy of the real frame frame
// generation reads. Pixels of the cleaned mask (grown by `grow` px) take the weighted mean (1 / distance) of the nearest
// unmasked pixel along 8 directions within fillReach px, so the generator never sees the overlay; every other pixel is
// copied unchanged. Never shown: the generated frame gets the overlay back from the real frames (PushHudGuardCompose).
Texture2D<float4> currentTex : register(t0);
Texture2D<float2> stateTex : register(t1);
RWTexture2D<float4> outTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, stillFrames, grow;
    uint fillReach, pad0, pad1, pad2;
    uint pad3, pad4, pad5, pad6;
};
bool cleanedAt(int2 q) {
    const float2 s = stateTex[q];
    const uint run = uint(s.x + 0.5), ev = uint(s.y + 0.5);
    return run >= stillFrames && (ev & 15u) >= 1 && (ev >> 4) >= 1;
}
bool cleanMask(int2 p) {
    [loop] for (int dy = -int(grow); dy <= int(grow); ++dy)
        [loop] for (int dx = -int(grow); dx <= int(grow); ++dx) {
            const int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
            if (cleanedAt(q)) return true;
        }
    return false;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const float4 c = currentTex[p];
    if (!cleanMask(p)) { outTex[p] = c; return; }
    const int2 dirs[8] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, -1), int2(1, -1), int2(-1, 1)};
    float3 sum = 0;
    float weight = 0;
    [loop] for (int d = 0; d < 8; ++d)
        [loop] for (uint k = 1; k <= fillReach; ++k) {
            const int2 q = p + dirs[d] * int(k);
            if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) break;
            if (cleanMask(q)) continue;
            const float w = 1.0 / float(k);
            sum += w * currentTex[q].rgb;
            weight += w;
            break;
        }
    outTex[p] = weight > 0 ? float4(sum / weight, c.a) : c;
}
