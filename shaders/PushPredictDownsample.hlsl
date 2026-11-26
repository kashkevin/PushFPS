// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "predict" (pipeline/PushPredict.h): one enhanced real frame, as VFG's pitch-linear input buffer holds it
// (R8G8B8A8_UNORM or R10G10B10A2_UNORM words, rowPitch bytes per row), box-filtered into a small BGRA8 picture for
// the optical flow (GpuDisOpticalFlow). Encoded values are averaged as they are (PQ for HDR10): the flow only needs
// structure, not light.
cbuffer Constants : register(b0)
{
    uint sourceWidth;
    uint sourceHeight;
    uint rowPitch;
    uint tenBit;
    uint targetWidth;
    uint targetHeight;
    uint unused0;
    uint unused1;
};

ByteAddressBuffer source : register(t0);
RWTexture2D<float4> target : register(u0);

float3 texelAt(uint x, uint y)
{
    const uint word = source.Load(y * rowPitch + x * 4);
    if (tenBit != 0)
        return float3(float(word & 1023u), float((word >> 10) & 1023u), float((word >> 20) & 1023u)) / 1023.0;
    return float3(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u)) / 255.0;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= targetWidth || id.y >= targetHeight) return;
    const uint x0 = id.x * sourceWidth / targetWidth, x1 = max(x0 + 1, (id.x + 1) * sourceWidth / targetWidth);
    const uint y0 = id.y * sourceHeight / targetHeight, y1 = max(y0 + 1, (id.y + 1) * sourceHeight / targetHeight);
    float3 sum = 0;
    for (uint y = y0; y < min(y1, sourceHeight); ++y)
        for (uint x = x0; x < min(x1, sourceWidth); ++x)
            sum += texelAt(x, y);
    const float count = float((min(y1, sourceHeight) - y0) * (min(x1, sourceWidth) - x0));
    target[id.xy] = float4(sum / max(count, 1.0), 1.0);
}
