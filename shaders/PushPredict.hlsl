// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "predict" (pipeline/PushPredict.h, docs/drafts/PREDICT_GPU.md): the predicted frame P of the "one predicted"
// schedule: the real frame B advanced by `step` (1/3 at 3X) of the motion from the previous real frame A to B.
//
// Inputs: A and B as VFG's pitch-linear input buffers (R8G8B8A8_UNORM or R10G10B10A2_UNORM words); the optical flow
// B -> A in flow pixels as GpuDisValidate writes it (the vector times its own trust, 0 where the trust is 0) and that
// trust (0..1), at the flow size. flowAt divides the bilinear motion by the bilinear trust: the trust-weighted average
// of the vectors, i.e. the true motion (PushPredict.h pushPredictTrustedMotion, the CPU reference).
// Output: P at the working size, in the working format (a generated-frame texture).
//
// Warp: BACKWARD (gather): P(y) = B(y + step * m(y)), m = the B -> A flow at y in work pixels (so -m is the A -> B
// motion). Every output pixel reads exactly one bilinear sample of B: no holes, no splat, no atomics per pixel. Its
// known error is at moving edges (the flow at y belongs to whatever B shows at y, so a leading edge does not advance and
// a trailing edge drags): there the flow's trust or the photometric check below usually fails and the pixel falls back.
//
// Per pixel (PushPredict.h pushPredictPixelRule, the CPU reference):
//   Still    max |B(y) - A(y)| <= stillDelta: B(y) EXACTLY (the texel loaded and stored unchanged): HUD, letterbox
//   Fallback trust < minConfidence (tested BEFORE the length: a failed flow has motion 0 and must not count as held)
//   Held     |m| < minMotion: B(y)
//   Fallback |m| > maxMotion, a sample outside the picture, or B(y) not matching A(y + m)
//            within maxPhotoDelta: B(y) (a held pixel, never a blend: a blend makes a double image)
//   Warped   otherwise: the bilinear sample of B
// Counters (four uint32 per slot, cleared by the caller): still, held, fallback, warped pixels of the frame.
cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint rowPitch;
    uint tenBit;
    uint flowWidth;
    uint flowHeight;
    uint counterOffset;   // bytes into the counter buffer: 16 x slot
    uint unused0;
    float step;           // fraction of the A -> B motion P is advanced by
    float stillDelta;
    float minMotion;
    float maxMotion;      // work pixels per content interval
    float minConfidence;
    float maxPhotoDelta;
    float unused1;
    float unused2;
};

ByteAddressBuffer previousFrame : register(t0);   // A
ByteAddressBuffer currentFrame : register(t1);    // B
Texture2D<float2> flow : register(t2);            // B -> A, flow pixels
Texture2D<float> trust : register(t3);
RWTexture2D<float4> predicted : register(u0);
RWByteAddressBuffer counters : register(u1);

groupshared uint groupCounts[4];

float4 unpackWord(uint word)
{
    if (tenBit != 0)
        return float4(float(word & 1023u) / 1023.0, float((word >> 10) & 1023u) / 1023.0,
                      float((word >> 20) & 1023u) / 1023.0, float(word >> 30) / 3.0);
    return float4(float(word & 255u) / 255.0, float((word >> 8) & 255u) / 255.0,
                  float((word >> 16) & 255u) / 255.0, float(word >> 24) / 255.0);
}

float4 texelOf(ByteAddressBuffer frame, int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return unpackWord(frame.Load(uint(c.y) * rowPitch + uint(c.x) * 4));
}

// Bilinear in texel-index space: q = (3, 5) is exactly texel (3, 5).
float4 bilinearOf(ByteAddressBuffer frame, float2 q)
{
    const float2 f = floor(q);
    const float2 t = q - f;
    const int2 a = int2(f);
    const float4 top = lerp(texelOf(frame, a), texelOf(frame, a + int2(1, 0)), t.x);
    const float4 bottom = lerp(texelOf(frame, a + int2(0, 1)), texelOf(frame, a + int2(1, 1)), t.x);
    return lerp(top, bottom, t.y);
}

// Flow and trust at a work pixel, bilinear between flow-pixel centres, flow converted to work pixels.
void flowAt(uint2 p, out float2 motion, out float confidence)
{
    const float2 scale = float2(float(flowWidth) / float(width), float(flowHeight) / float(height));
    const float2 q = max((float2(p) + 0.5) * scale - 0.5, float2(0, 0));
    const float2 f = floor(q);
    const float2 t = q - f;
    const int2 lim = int2(int(flowWidth) - 1, int(flowHeight) - 1);
    const int2 a = min(int2(f), lim);
    const int2 b = min(a + int2(1, 0), lim), c = min(a + int2(0, 1), lim), d = min(a + int2(1, 1), lim);
    const float2 m = lerp(lerp(flow[a], flow[b], t.x), lerp(flow[c], flow[d], t.x), t.y);
    confidence = lerp(lerp(trust[a], trust[b], t.x), lerp(trust[c], trust[d], t.x), t.y);
    // The stored flow is v x trust: divide by the bilinear trust (the trust-weighted average of the vectors).
    motion = confidence > 1e-6 ? (m / confidence) / scale : float2(0, 0);
}

bool inside(float2 q)
{
    return all(q >= float2(0, 0)) && all(q <= float2(float(width) - 1.0, float(height) - 1.0));
}

float maxDelta(float4 x, float4 y)
{
    const float3 d = abs(x.rgb - y.rgb);
    return max(d.r, max(d.g, d.b));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < 4) groupCounts[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    const bool valid = id.x < width && id.y < height;
    if (valid) {
        const int2 p = int2(id.xy);
        const float4 current = texelOf(currentFrame, p);
        const float4 previous = texelOf(previousFrame, p);
        float4 result = current;
        uint kind = 0;   // 0 still, 1 held, 2 fallback, 3 warped
        if (maxDelta(current, previous) > stillDelta) {
            float2 motion;
            float confidence;
            flowAt(id.xy, motion, confidence);
            const float len = length(motion);
            kind = 2;   // untrusted (also NaN): fallback, before the length test
            if (confidence >= minConfidence) {
                kind = 1;
                if (len >= minMotion) {
                    kind = 2;
                    const float2 from = float2(p) + motion;           // where B's pixel was in A
                    const float2 source = float2(p) + step * motion;  // where P's pixel is in B
                    if (len <= maxMotion && inside(from) && inside(source)) {
                        if (maxDelta(current, bilinearOf(previousFrame, from)) <= maxPhotoDelta) {
                            result = bilinearOf(currentFrame, source);
                            kind = 3;
                        }
                    }
                }
            }
        }
        predicted[id.xy] = result;
        InterlockedAdd(groupCounts[kind], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 4 && groupCounts[index] != 0) counters.InterlockedAdd(counterOffset + index * 4, groupCounts[index]);
}
