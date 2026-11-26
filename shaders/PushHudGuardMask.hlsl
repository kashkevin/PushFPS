// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 3 (pipeline/PushHudGuard.h, PushHudGuardReference::update): the mask (R8_UINT, bit 0
// protected, bit 1 cleaned). Protected = armed (run >= K, nearby >= nearFrames; K = rearmFrames while `recent`, else
// stillFrames) and a still structure (run >= its K, edge >= edgeFrames) within `gate` px; cleaned = protected and thin.
// Aim anchor: in the aim window a pixel still for anchorFrames frames with no lone mover near (state bit 28 clear) is armed too.
// Adds the protected / cleaned pixels (all, the middle tenth, the 128 x 128 aim window) and the aim window's longest run.
Texture2D<uint> stateTex : register(t0);
RWTexture2D<uint> maskTex : register(u0);
RWByteAddressBuffer counters : register(u1);
cbuffer Constants : register(b0) {
    uint width, height, stillFrames, rearmFrames;
    uint nearFrames, edgeFrames, gate, anchorFrames;
    uint pad1, pad2, pad3, pad4;
};
groupshared uint gCount[7];
uint armFrames(uint w) { return ((w >> 22) & 63u) > 0 ? rearmFrames : stillFrames; }
bool structureAt(int2 q) {
    const uint w = stateTex[q];
    return (w & 255u) >= armFrames(w) && ((w >> 16) & 15u) >= edgeFrames;
}
bool centre(uint2 p) {
    const int dx = abs(2 * int(p.x) + 1 - int(width)), dy = abs(2 * int(p.y) + 1 - int(height));
    return dx * 10 <= int(width) && dy * 10 <= int(height);
}
bool aim(uint2 p) {
    const int cx = int(width / 2), cy = int(height / 2);
    return int(p.x) >= cx - 64 && int(p.x) < cx + 64 && int(p.y) >= cy - 64 && int(p.y) < cy + 64;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 7) gCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height) {
        const uint w = stateTex[id.xy];
        const uint run = w & 255u;
        const bool inAim = aim(id.xy);
        if (inAim) InterlockedMax(gCount[6], run);
        uint m = 0;
        const bool anchored = inAim && anchorFrames > 0 && run >= anchorFrames && ((w >> 28) & 1u) == 0;
        if ((run >= armFrames(w) && ((w >> 8) & 15u) >= nearFrames) || anchored) {
            bool structure = false;
            [loop] for (int dy = -int(gate); dy <= int(gate) && !structure; ++dy)
                [loop] for (int dx = -int(gate); dx <= int(gate) && !structure; ++dx) {
                    const int2 q = int2(id.xy) + int2(dx, dy);
                    if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) continue;
                    structure = structureAt(q);
                }
            if (structure) {
                m = ((w >> 12) & 15u) >= 1 ? 3u : 1u;
                const bool mid = centre(id.xy);
                InterlockedAdd(gCount[0], 1u);
                if (mid) InterlockedAdd(gCount[2], 1u);
                if (inAim) InterlockedAdd(gCount[4], 1u);
                if (m == 3u) {
                    InterlockedAdd(gCount[1], 1u);
                    if (mid) InterlockedAdd(gCount[3], 1u);
                    if (inAim) InterlockedAdd(gCount[5], 1u);
                }
            }
        }
        maskTex[id.xy] = m;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 6 && gCount[gi] != 0) { uint previous; counters.InterlockedAdd(gi * 4, gCount[gi], previous); }
    if (gi == 6 && gCount[6] != 0) { uint previous; counters.InterlockedMax(24, gCount[6], previous); }
}
