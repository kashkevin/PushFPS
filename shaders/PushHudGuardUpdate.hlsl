// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 2 (pipeline/PushHudGuard.h, pushHudGuardStep and PushHudGuardReference::update): per pixel
// the run of still real frames and the evidence of motion around it (one side: near, both sides of one axis: thin),
// state R16G16_FLOAT (x = run, y = near + 16 x thin; exact integers). The scan for moving pixels (reach px along 4 axes)
// runs only when one of the 3 x 3 tiles around the pixel's own tile moved: tiles are 16 px and reach <= 16, so the
// early-out never changes the result. Adds the protected / cleaned pixels (all and in the middle tenth) to `counters`.
Texture2D<float> changeTex : register(t0);
Texture2D<uint> tileTex : register(t1);
RWTexture2D<float2> stateTex : register(u0);
RWByteAddressBuffer counters : register(u1);
cbuffer Constants : register(b0) {
    uint width, height, reset, tilesX;
    uint tilesY, stillFrames, reach, pad0;
    float still, hold, move, pad1;
};
groupshared uint gCount[4];

bool movingAt(int2 q) {
    if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) return false;
    return changeTex[q] > move;
}
bool centre(uint2 p) {
    const int dx = abs(2 * int(p.x) + 1 - int(width)), dy = abs(2 * int(p.y) + 1 - int(height));
    return dx * 10 <= int(width) && dy * 10 <= int(height);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi < 4) gCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height) {
        if (reset != 0) {
            stateTex[id.xy] = float2(0, 0);
        } else {
            const float2 s = stateTex[id.xy];
            uint run = uint(s.x + 0.5), ev = uint(s.y + 0.5);
            uint nearCount = ev & 15u, thinCount = ev >> 4;
            const bool was = run >= stillFrames && nearCount >= 1;
            const float c = changeTex[id.xy];
            bool keep = true;
            if (c <= still) run = min(run + 1u, 255u);
            else if (!(was && c <= hold)) { keep = false; run = 0; nearCount = 0; thinCount = 0; }
            if (keep) {
                bool anyTile = false;
                [unroll] for (int ty = -1; ty <= 1; ++ty)
                    [unroll] for (int tx = -1; tx <= 1; ++tx) {
                        const int2 t = int2(gid.xy) + int2(tx, ty);
                        if (t.x >= 0 && t.y >= 0 && t.x < int(tilesX) && t.y < int(tilesY) && tileTex[t] > 0) anyTile = true;
                    }
                bool nearMoving = false, thinMoving = false;
                if (anyTile) {
                    const int2 axes[4] = {int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1)};
                    [unroll] for (int a = 0; a < 4; ++a) {
                        bool plus = false, minus = false;
                        [loop] for (uint k = 1; k <= reach && !plus; ++k) plus = movingAt(int2(id.xy) + axes[a] * int(k));
                        [loop] for (uint k2 = 1; k2 <= reach && !minus; ++k2) minus = movingAt(int2(id.xy) - axes[a] * int(k2));
                        nearMoving = nearMoving || plus || minus;
                        thinMoving = thinMoving || (plus && minus);
                    }
                }
                if (nearMoving) nearCount = min(nearCount + 1u, 15u);
                if (thinMoving) thinCount = min(thinCount + 1u, 15u);
            }
            stateTex[id.xy] = float2(float(run), float(nearCount + 16u * thinCount));
            if (run >= stillFrames && nearCount >= 1) {
                const bool mid = centre(id.xy);
                InterlockedAdd(gCount[0], 1u);
                if (mid) InterlockedAdd(gCount[2], 1u);
                if (thinCount >= 1) { InterlockedAdd(gCount[1], 1u); if (mid) InterlockedAdd(gCount[3], 1u); }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 4 && gCount[gi] != 0) {
        uint previous;
        counters.InterlockedAdd(gi * 4, gCount[gi], previous);
    }
}
