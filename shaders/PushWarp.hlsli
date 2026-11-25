// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg": what every PushWarp shader shares (include/veyra/pipeline/PushWarp.h is the CPU reference of all of
// them; keep the arithmetic in the same order). One root signature for all passes (src/pipeline/PushWarpPass.cpp):
//   b0 constants (28), t0 / t1 raw buffers A / B (root SRVs), table {t2 flow, t3 trust, t4 previous flow, t5 previous trust,
//   u0 output}, u1 scratch, u2 counters,
//   u3 verdict (root UAVs). FP32 and integer arithmetic only: no doubles, no wave operations, no float atomics, no typed UAV
//   loads (the scratch and counters are raw buffers; the output texture is only stored to).
cbuffer Constants : register(b0)
{
    uint width;           // work size
    uint height;
    uint rowPitch;        // bytes per row of A and B
    uint tenBit;          // R10G10B10A2_UNORM (HDR10 PQ codes) instead of R8G8B8A8_UNORM
    uint flowWidth;
    uint flowHeight;
    uint counterOffset;   // bytes: 16 x slot
    uint verdictOffset;   // bytes: 8 x slot
    float frameTime;      // t
    float stillDelta;
    float conflictDelta;
    float minConfidence;
    float maxMotion;      // flow pixels per interval
    float sadLow;
    float sadHigh;
    float coverageFull;
    float holeVisibility;
    float fillKnown;
    float beta;
    uint mode;            // splat: 0 max, 1 add; fill: 0 resolve, 1 down, 2 up
    uint levelWidth;      // fill: this level
    uint levelHeight;
    uint levelOffset;     // cells
    uint otherOffset;     // fill down: the finer level; up: the coarser one
    uint otherWidth;      // 0 in "up" at the coarsest level
    uint otherHeight;
    uint flags;           // bit 0: predicting (frameTime > 1, from B alone); bit 1: acceleration from the previous field (t4, t5)
    float accelMin;       // flow px per interval: a smaller change of motion is not used (noise)
};

static const float kWeightScale = 256.0;
static const float kVectorScale = 32.0;
static const float kImportanceScale = 64.0;
static const uint kImportanceCap = 1u << 20;

uint cellCount() { return flowWidth * flowHeight; }
uint cellAddress(uint i) { return i * 16u; }
uint accumAddress(uint i) { return cellCount() * 16u + i * 32u; }
uint pyramidAddress(uint i) { return cellCount() * 48u + i * 32u; }

float4 unpackWord(uint word)
{
    if (tenBit != 0)
        return float4(float(word & 1023u) / 1023.0, float((word >> 10) & 1023u) / 1023.0,
                      float((word >> 20) & 1023u) / 1023.0, float(word >> 30) / 3.0);
    return float4(float(word & 255u) / 255.0, float((word >> 8) & 255u) / 255.0,
                  float((word >> 16) & 255u) / 255.0, float(word >> 24) / 255.0);
}

uint wordAt(ByteAddressBuffer frame, int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return frame.Load(uint(c.y) * rowPitch + uint(c.x) * 4);
}

float4 texelAt(ByteAddressBuffer frame, int2 p) { return unpackWord(wordAt(frame, p)); }

// Bilinear in texel-index space (q = (3, 5) is exactly texel (3, 5)), clamped to the edge: PushPredict.hlsl's.
float4 bilinearAt(ByteAddressBuffer frame, float2 q)
{
    const float2 f = floor(q);
    const float2 t = q - f;
    const int2 a = int2(f);
    const float4 c00 = texelAt(frame, a), c10 = texelAt(frame, a + int2(1, 0));
    const float4 c01 = texelAt(frame, a + int2(0, 1)), c11 = texelAt(frame, a + int2(1, 1));
    const float4 top = c00 + (c10 - c00) * t.x;
    const float4 bottom = c01 + (c11 - c01) * t.x;
    return top + (bottom - top) * t.y;
}

float maxDelta(float4 x, float4 y)
{
    const float3 d = abs(x.rgb - y.rgb);
    return max(d.r, max(d.g, d.b));
}

int roundHalfUp(float x) { return int(floor(x + 0.5)); }
