// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s12-pace" frame probe (engine/PushFrameThumb.h, engine/PushFrameProbe.h): a 128 x 72 luma thumbnail of one presented
// frame, 4 x 4 point taps per cell at the sub-cell centres (integer positions exactly as pushFrameThumbCpu). 147 456 texel
// loads per frame: no full read of the 4K picture.
Texture2D<float4> sourceTex : register(t0);
RWBuffer<float> thumb : register(u0);
cbuffer Params : register(b0) { uint flags; uint unused0; uint unused1; uint unused2; };
static const uint kWidth = 128, kHeight = 72, kTaps = 4;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kWidth || id.y >= kHeight) return;
    uint width, height;
    sourceTex.GetDimensions(width, height);
    float sum = 0;
    [unroll] for (uint j = 0; j < kTaps; ++j) {
        const uint sy = id.y * kTaps + j;
        const uint y = min(height - 1, (sy * 2 + 1) * height / (kHeight * kTaps * 2));
        [unroll] for (uint i = 0; i < kTaps; ++i) {
            const uint sx = id.x * kTaps + i;
            const uint x = min(width - 1, (sx * 2 + 1) * width / (kWidth * kTaps * 2));
            const float3 c = sourceTex.Load(int3(x, y, 0)).rgb;
            const float l = dot(c, float3(0.2126, 0.7152, 0.0722));
            sum += (flags & 1) ? (l > 0 ? l / (1 + l) : 0) : saturate(l);
        }
    }
    thumb[id.y * kWidth + id.x] = sum / float(kTaps * kTaps);
}
