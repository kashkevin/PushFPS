// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 4 (pipeline/PushHudGuard.h, PushHudGuardReference::clean): the copy of the real frame frame
// generation reads. Pixels of the cleaned mask (bit 1, grown by `grow` px) take the weighted mean (1 / distance) of the
// nearest pixel outside it along 8 directions within fillReach px, so the generator never sees the overlay; every other
// pixel is copied unchanged. Never shown: the generated frame gets the overlay back from the real frames (Compose).
Texture2D<float4> currentTex : register(t0);
Texture2D<uint> maskTex : register(t1);
RWTexture2D<float4> outTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, grow, fillReach;
    uint pad0, pad1, pad2, pad3;
    uint pad4, pad5, pad6, pad7;
};
bool cleanMask(int2 p) {
    [loop] for (int dy = -int(grow); dy <= int(grow); ++dy)
        [loop] for (int dx = -int(grow); dx <= int(grow); ++dx) {
            const int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
            if ((maskTex[q] & 2u) != 0) return true;
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
