// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS HUD Guard, step 2 (pipeline/PushHudGuard.h, pushHudGuardStep and PushHudGuardReference::update): per pixel
// the state word (R32_UINT: run 8 bits | nearby 4 | thin 4 | edge 4 | held 2 | recent 6 | iso 1 at bit 28), from this frame's change and
// contrast, the moving pixels within `reach` px along 4 axes, the share of moving pixels in the 5 x 5 tiles around, and
// the pixel's mask bit of the previous frame. The axis scan runs only when one of the 3 x 3 tiles around the pixel's own
// moved (tiles are 16 px, reach <= 16: the early-out never changes the result).
Texture2D<float2> changeTex : register(t0);
Texture2D<uint> tileTex : register(t1);
Texture2D<uint> maskTex : register(t2);   // the previous frame's mask (bit 0 protected)
RWTexture2D<uint> stateTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, reset, tilesX;
    uint tilesY, reach, shareMille, holdFrames;
    float still, hold, move, edge;
    uint recentFrames, anchorMargin, anchorFrames, pad2;
};
// The aim anchor's busy test (PushHudGuardReference::update): a changed pixel in the tiles that overlap the 128 x 128 aim
// window grown by anchorMargin px.
bool aimBusy() {
    const int cx = int(width / 2), cy = int(height / 2), half = 64 + int(anchorMargin);
    const int x0 = max(0, cx - half) / 16, x1 = min(int(width) - 1, cx + half - 1) / 16;
    const int y0 = max(0, cy - half) / 16, y1 = min(int(height) - 1, cy + half - 1) / 16;
    [loop] for (int qy = y0; qy <= y1; ++qy)
        [loop] for (int qx = x0; qx <= x1; ++qx)
            if (tileTex[int2(qx, qy)] > 0) return true;
    return false;
}
bool inAim(uint2 p) {
    const int cx = int(width / 2), cy = int(height / 2);
    return int(p.x) >= cx - 64 && int(p.x) < cx + 64 && int(p.y) >= cy - 64 && int(p.y) < cy + 64;
}
bool movingAt(int2 q) {
    if (q.x < 0 || q.y < 0 || q.x >= int(width) || q.y >= int(height)) return false;
    return changeTex[q].x > move;
}
bool surroundMoving(int2 t) {
    uint moved = 0, pixels = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            const int2 q = t + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(tilesX) || q.y >= int(tilesY)) continue;
            moved += tileTex[q];
            pixels += min(16u, width - uint(q.x) * 16u) * min(16u, height - uint(q.y) * 16u);
        }
    return pixels > 0 && moved * 1000u >= shareMille * pixels;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    if (id.x >= width || id.y >= height) return;
    if (reset != 0) { stateTex[id.xy] = 0; return; }
    const uint w = stateTex[id.xy];
    uint run = w & 255u, nearby = (w >> 8) & 15u, thin = (w >> 12) & 15u, edges = (w >> 16) & 15u, held = (w >> 20) & 3u, recent = (w >> 22) & 63u;
    const bool was = (maskTex[id.xy] & 1u) != 0;
    const float2 ck = changeTex[id.xy];
    if (!was && recent > 0) --recent;
    if (ck.x <= still) {
        run = min(run + 1u, 255u);
        held = 0;
    } else if (was && ck.x <= hold && held < holdFrames) {
        ++held;
    } else {
        const uint r = was ? min(recentFrames, 63u) : recent;
        stateTex[id.xy] = r > 0 ? ((nearby << 8) | (thin << 12) | (edges << 16) | (r << 22)) : 0u;
        return;
    }
    bool nearMoving = false, thinMoving = false, iso = false;
    bool anyTile = false;
    [unroll] for (int ty = -1; ty <= 1; ++ty)
        [unroll] for (int tx = -1; tx <= 1; ++tx) {
            const int2 t = int2(gid.xy) + int2(tx, ty);
            if (t.x >= 0 && t.y >= 0 && t.x < int(tilesX) && t.y < int(tilesY) && tileTex[t] > 0) anyTile = true;
        }
    if (anyTile) {
        const int2 axes[4] = {int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1)};
        [unroll] for (int a = 0; a < 4; ++a) {
            bool plus = false, minus = false;
            [loop] for (uint k = 1; k <= reach && !plus; ++k) plus = movingAt(int2(id.xy) + axes[a] * int(k));
            [loop] for (uint k2 = 1; k2 <= reach && !minus; ++k2) minus = movingAt(int2(id.xy) - axes[a] * int(k2));
            nearMoving = nearMoving || plus || minus;
            thinMoving = thinMoving || (plus && minus);
        }
        if (nearMoving && !surroundMoving(int2(gid.xy))) { nearMoving = false; thinMoving = false; }   // a lone mover
    }
    // Anything changed around the aim window this frame: the aim anchor is off for it. Only pixels still long enough for the
    // anchor read the bit (pushHudGuardStep), so only they pay for the scan.
    [branch] if (anchorFrames > 0 && run >= anchorFrames && inAim(id.xy)) iso = aimBusy();
    if (nearMoving) nearby = min(nearby + 1u, 15u);
    if (thinMoving) thin = min(thin + 1u, 15u);
    if (ck.y >= edge) edges = min(edges + 1u, 15u);
    else if (edges > 0) --edges;
    stateTex[id.xy] = run | (nearby << 8) | (thin << 12) | (edges << 16) | (held << 20) | (recent << 22) | (iso ? (1u << 28) : 0u);
}
