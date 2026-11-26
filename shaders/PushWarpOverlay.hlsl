// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg": the static-overlay mask of a pair (crosshair, HUD text, minimap ring over a moving picture), computed
// once per pair after its cells (W3) and before the first frame's compose; PushWarp.h pushWarpOverlayCells /
// pushWarpOverlayMask are the CPU reference (keep the arithmetic in the same order). `mode` selects the pass:
//   0 spread    per flow cell: the trust-weighted mean of the cell vectors within 2 cells -> overlayCells (float2)
//   1 classify  per pixel: bits 1 core, 2 extend, 4 unexplained, 8 rim candidate -> mask0
//   2 run 1     per pixel: core, or extend joined to a core pixel by an h/v run of extend pixels (<= 16) -> mask1 bit 1
//   3 run 2     the same from mask1's seeds -> mask2 bit 1
//   4 overlay   seed (mask2), or rim candidate and unexplained next to a core pixel -> mask1 bit 1
//   5 sources   bit 1 copied, bit 2 = an overlay pixel within 1 px -> mask2 (what the compose reads); counts the overlay
//               pixels (all, and within the middle eighth of the width and height) into the slot's counters
// Masks: one byte per pixel, four pixels per uint (a row of ceil(width / 4) words); every pass writes whole words, one
// thread per word, and reads only buffers it does not write. FP32 and integer arithmetic only.
#include "PushWarp.hlsli"

ByteAddressBuffer previousFrame : register(t0);   // A
ByteAddressBuffer currentFrame : register(t1);    // B
RWByteAddressBuffer scratch : register(u1);       // the pair's cells
RWByteAddressBuffer counters : register(u2);
RWByteAddressBuffer mask0 : register(u4);
RWByteAddressBuffer mask1 : register(u5);
RWByteAddressBuffer mask2 : register(u6);
RWByteAddressBuffer overlayCells : register(u7);  // per flow cell, 24 bytes: spread float2, moving mean float2, share float

static const float kOverlayStill = 3.0 / 255.0;
static const float kOverlayRim = 24.0 / 255.0;
static const float kOverlayMinMove = 1.5;
static const float kOverlayContrast = 16.0 / 255.0;
static const int kOverlayRun = 16;
static const int kOverlaySpread = 2;
static const float kOverlayStructure = 13.0 / 255.0;
static const float kOverlayShare = 0.5;
static const int kOverlayReach = 4;
static const float kOverlayAgree = 0.25;
static const float kOverlayPqScale = 0.6;
static const uint kOverlayCellBytes = 24;

float overlayScale() { return tenBit != 0 ? kOverlayPqScale : 1.0; }
float moveFlow() { return kOverlayMinMove * float(flowWidth) / float(width); }
// pushWarpOverlayMovingCell on the pair's cells (scratch, W3)
bool movingCell(uint i, out float2 v)
{
    const uint4 c = scratch.Load4(cellAddress(i));
    v = float2(asfloat(c.x), asfloat(c.y));
    const float m = moveFlow();
    return asfloat(c.z) >= 0.5 && v.x * v.x + v.y * v.y >= m * m;
}

uint rowWords() { return (width + 3u) / 4u; }
uint maskByte(RWByteAddressBuffer m, int x, int y)
{
    const uint cx = uint(clamp(x, 0, int(width) - 1)), cy = uint(clamp(y, 0, int(height) - 1));
    return (m.Load((cy * rowWords() + cx / 4u) * 4u) >> ((cx & 3u) * 8u)) & 255u;
}
bool inside(int x, int y) { return x >= 0 && y >= 0 && x < int(width) && y < int(height); }

float2 overlayCellAt(uint x, uint y) { return asfloat(overlayCells.Load2((y * flowWidth + x) * kOverlayCellBytes)); }

// pushWarpCellsAt on the spread cells: work px.
float2 overlayFieldAt(uint x, uint y)
{
    const float sx = float(flowWidth) / float(width), sy = float(flowHeight) / float(height);
    const float qx = max((float(x) + 0.5) * sx - 0.5, 0.0), qy = max((float(y) + 0.5) * sy - 0.5, 0.0);
    const float fx = floor(qx), fy = floor(qy), tx = qx - fx, ty = qy - fy;
    const uint x0 = min(uint(fx), flowWidth - 1), y0 = min(uint(fy), flowHeight - 1);
    const uint x1 = min(x0 + 1, flowWidth - 1), y1 = min(y0 + 1, flowHeight - 1);
    const float2 c00 = overlayCellAt(x0, y0), c10 = overlayCellAt(x1, y0), c01 = overlayCellAt(x0, y1), c11 = overlayCellAt(x1, y1);
    const float topX = c00.x + (c10.x - c00.x) * tx, bottomX = c01.x + (c11.x - c01.x) * tx;
    const float topY = c00.y + (c10.y - c00.y) * tx, bottomY = c01.y + (c11.y - c01.y) * tx;
    return float2((topX + (bottomX - topX) * ty) * (float(width) / float(flowWidth)), (topY + (bottomY - topY) * ty) * (float(height) / float(flowHeight)));
}

// Whether vector v (work px) explains the pixel (pushWarpOverlayMask's "explains").
bool explains(uint x, uint y, float4 a, float4 b, float2 v)
{
    const float back = maxDelta(b, bilinearAt(previousFrame, float2(float(x) + v.x, float(y) + v.y)));
    const float fwd = maxDelta(a, bilinearAt(currentFrame, float2(float(x) - v.x, float(y) - v.y)));
    return min(back, fwd) <= kOverlayContrast * overlayScale();
}
// The three gates of a core candidate (PushWarp.h "Three gates"): structure, surround, no vector.
bool gated(uint x, uint y, float4 a, float4 b)
{
    float structure = 0, structureB = 0;
    [loop] for (uint k = 0; k < 8; ++k) {
        const int r = k < 4 ? 1 : 2, d = int(k & 3u);
        const int ox = d == 0 ? r : d == 1 ? -r : 0, oy = d == 2 ? r : d == 3 ? -r : 0;
        structure = max(structure, maxDelta(a, texelAt(previousFrame, int2(int(x) + ox, int(y) + oy))));
        structureB = max(structureB, maxDelta(b, texelAt(currentFrame, int2(int(x) + ox, int(y) + oy))));
    }
    if (min(structure, structureB) < kOverlayStructure * overlayScale()) return false;
    const uint cx = min(flowWidth - 1, uint(float(x) * float(flowWidth) / float(width))), cy = min(flowHeight - 1, uint(float(y) * float(flowHeight) / float(height)));
    const uint at = (cy * flowWidth + cx) * kOverlayCellBytes;
    if (asfloat(overlayCells.Load(at + 16)) < kOverlayShare) return false;
    const float2 m = asfloat(overlayCells.Load2(at + 8));
    if (explains(x, y, a, b, float2(m.x * float(width) / float(flowWidth), m.y * float(height) / float(flowHeight)))) return false;
    [loop] for (int j = -1; j <= 1; ++j)
        [loop] for (int i = -1; i <= 1; ++i) {
            const int xx = int(cx) + i, yy = int(cy) + j;
            if (xx < 0 || yy < 0 || xx >= int(flowWidth) || yy >= int(flowHeight)) continue;
            float2 v;
            if (!movingCell(uint(yy) * flowWidth + uint(xx), v)) continue;
            if (explains(x, y, a, b, float2(v.x * float(width) / float(flowWidth), v.y * float(height) / float(flowHeight)))) return false;
        }
    return true;
}

uint classify(uint x, uint y)
{
    const float4 a = texelAt(previousFrame, int2(x, y)), b = texelAt(currentFrame, int2(x, y));
    const float delta = maxDelta(a, b);
    const float2 g = overlayFieldAt(x, y);
    const float back = maxDelta(b, bilinearAt(previousFrame, float2(float(x) + g.x, float(y) + g.y)));
    const float fwd = maxDelta(a, bilinearAt(currentFrame, float2(float(x) - g.x, float(y) - g.y)));
    const float u = min(back, fwd);
    const bool moving = g.x * g.x + g.y * g.y >= kOverlayMinMove * kOverlayMinMove;
    const float scale = overlayScale();
    uint bits = 0;
    if (delta <= kOverlayStill * scale && moving && u > kOverlayContrast * scale && gated(x, y, a, b)) bits |= 1u;
    if (delta <= kOverlayStill * scale && moving) bits |= 2u;
    if (u > kOverlayContrast * scale) bits |= 4u;
    if (delta <= kOverlayRim * scale) bits |= 8u;
    return bits;
}

// An extend pixel joined to a seed (bit 1 of `seeds`, or a core pixel of mask0 when seedsFromCore) by an unbroken h/v
// run of extend pixels.
bool joined(int x, int y, bool seedsFromCore)
{
    [loop] for (uint d = 0; d < 4; ++d) {
        const int dx = d == 0 ? 1 : d == 1 ? -1 : 0, dy = d == 2 ? 1 : d == 3 ? -1 : 0;
        [loop] for (int k = 1; k <= kOverlayRun; ++k) {
            const int xx = x + k * dx, yy = y + k * dy;
            if (!inside(xx, yy)) break;
            const bool seed = seedsFromCore ? (maskByte(mask0, xx, yy) & 1u) != 0 : (maskByte(mask1, xx, yy) & 1u) != 0;
            if (seed) return true;
            if ((maskByte(mask0, xx, yy) & 2u) == 0) break;
        }
    }
    return false;
}

bool besideBit(RWByteAddressBuffer m, uint bit, int x, int y)
{
    bool found = false;
    [loop] for (int j = -1; j <= 1; ++j)
        [loop] for (int i = -1; i <= 1; ++i)
            found = found || (inside(x + i, y + j) && (maskByte(m, x + i, y + j) & bit) != 0);
    return found;
}

groupshared uint groupOverlay[2];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (mode == 0) {
        if (id.x >= flowWidth || id.y >= flowHeight) return;
        float sx = 0, sy = 0, sw = 0;
        for (int j = -kOverlaySpread; j <= kOverlaySpread; ++j)
            for (int i = -kOverlaySpread; i <= kOverlaySpread; ++i) {
                const int xx = int(id.x) + i, yy = int(id.y) + j;
                if (xx < 0 || yy < 0 || xx >= int(flowWidth) || yy >= int(flowHeight)) continue;
                const uint4 c = scratch.Load4(cellAddress(uint(yy) * flowWidth + uint(xx)));
                const float gx = asfloat(c.x), gy = asfloat(c.y), t = asfloat(c.z);
                sx += gx * t; sy += gy * t; sw += t;
            }
        const float2 o = sw > 1e-6 ? float2(sx / sw, sy / sw) : float2(0, 0);
        // pushWarpOverlayAgrees and pushWarpOverlayMovingShare
        float n = 0, mx = 0, my = 0, qx = 0, qy = 0;
        for (int j2 = -kOverlaySpread; j2 <= kOverlaySpread; ++j2)
            for (int i2 = -kOverlaySpread; i2 <= kOverlaySpread; ++i2) {
                const int xx = int(id.x) + i2, yy = int(id.y) + j2;
                if (xx < 0 || yy < 0 || xx >= int(flowWidth) || yy >= int(flowHeight)) continue;
                float2 v;
                if (!movingCell(uint(yy) * flowWidth + uint(xx), v)) continue;
                n += 1.0; mx += v.x; my += v.y; qx += v.x * v.x; qy += v.y * v.y;
            }
        float share = 0;
        float2 mean = float2(0, 0);
        if (n >= 1.0) {
            mx /= n; my /= n;
            mean = float2(mx, my);
            const float spread2 = max(0.0, qx / n - mx * mx) + max(0.0, qy / n - my * my);
            const float limit = max(0.75, kOverlayAgree * sqrt(mx * mx + my * my));
            if (spread2 <= limit * limit) {
                float moving = 0, all = 0;
                for (int j3 = -kOverlayReach; j3 <= kOverlayReach; ++j3)
                    for (int i3 = -kOverlayReach; i3 <= kOverlayReach; ++i3) {
                        const int xx = int(id.x) + i3, yy = int(id.y) + j3;
                        if (xx < 0 || yy < 0 || xx >= int(flowWidth) || yy >= int(flowHeight)) continue;
                        all += 1.0;
                        float2 v;
                        moving += movingCell(uint(yy) * flowWidth + uint(xx), v) ? 1.0 : 0.0;
                    }
                share = all > 0 ? moving / all : 0.0;
            }
        }
        const uint at = (id.y * flowWidth + id.x) * kOverlayCellBytes;
        overlayCells.Store2(at, asuint(o));
        overlayCells.Store2(at + 8, asuint(mean));
        overlayCells.Store(at + 16, asuint(share));
        return;
    }
    if (mode == 5 && index < 2) groupOverlay[index] = 0;
    if (mode == 5) GroupMemoryBarrierWithGroupSync();
    const uint word = id.x, y = id.y;
    const bool active = word < rowWords() && y < height;
    if (active) {
        uint packed = 0;
        uint overlayPixels = 0, centrePixels = 0;
        [loop] for (uint k = 0; k < 4; ++k) {   // a loop, not unrolled: one copy of the passes' code (WARP compiles it at the first dispatch)
            const uint x = word * 4u + k;
            uint bits = 0;
            if (x < width) {
            if (mode == 1) bits = classify(x, y);
            else if (mode == 2 || mode == 3) {
                const uint c = maskByte(mask0, int(x), int(y));
                const bool seed = mode == 2 ? (c & 1u) != 0 : (maskByte(mask1, int(x), int(y)) & 1u) != 0;
                bits = (seed || ((c & 2u) != 0 && joined(int(x), int(y), mode == 2))) ? 1u : 0u;
            } else if (mode == 4) {
                const uint c = maskByte(mask0, int(x), int(y));
                const bool seed = (maskByte(mask2, int(x), int(y)) & 1u) != 0;
                bits = (seed || ((c & 8u) != 0 && (c & 4u) != 0 && besideBit(mask0, 1u, int(x), int(y)))) ? 1u : 0u;
            } else {
                const bool on = (maskByte(mask1, int(x), int(y)) & 1u) != 0;
                bits = (on ? 1u : 0u) | (besideBit(mask1, 1u, int(x), int(y)) ? 2u : 0u);
                if (on) {
                    ++overlayPixels;
                    const int dx = int(x) - int(width / 2u), dy = int(y) - int(height / 2u);
                    if (abs(dx) * 16 < int(width) && abs(dy) * 16 < int(height)) ++centrePixels;
                }
            }
            }
            packed |= bits << (k * 8u);
        }
        const uint at = (y * rowWords() + word) * 4u;
        if (mode == 1) mask0.Store(at, packed);
        else if (mode == 2 || mode == 4) mask1.Store(at, packed);
        else mask2.Store(at, packed);
        if (mode == 5) { InterlockedAdd(groupOverlay[0], overlayPixels); InterlockedAdd(groupOverlay[1], centrePixels); }
    }
    if (mode == 5) {
        GroupMemoryBarrierWithGroupSync();
        if (index < 2 && groupOverlay[index] != 0) counters.InterlockedAdd(counterOffset + 16u + 32u * 4u + index * 4u, groupOverlay[index]);
    }
}
