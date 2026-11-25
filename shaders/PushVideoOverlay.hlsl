// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "present" R1 (engine/PushPresentOverlay.h): the interface's in-picture items (delay badge, statistics overlay) as
// one straight-alpha sRGB RGBA8 picture, blended onto the swap chain's back buffer AFTER the picture's present blit. The
// caller sets the viewport and the scissor to the picture's rectangle only, so every pixel outside it is never written
// (bit-identical to the blit's output); inside, the fixed-function blend is src * a + dst * (1 - a) on colour and keeps the
// destination's alpha. The value written is coded like the back buffer:
//   mode 0  SDR (R8G8B8A8_UNORM, sRGB-coded G22): the item's sRGB values as they are (blended in coded values, as the
//           desktop composes the interface)
//   mode 1  scRGB FP16 (linear, 1.0 = 80 nits): linear * 203 / 80 (the item's white at 203 nits, BT.2408 reference white)
//   mode 2  HDR10 (R10G10B10A2, BT.2020 PQ): the same 203-nit white, PQ coded (blended in PQ code values)
#include "HdrColor.hlsli"
Texture2D<float4> overlayTex : register(t0);

cbuffer PushVideoOverlayConstants : register(b0)
{
    float4 place;   // x, y: the picture's top left in buffer pixels; z, w: picture pixels per buffer pixel
    float4 info;    // x, y: picture size in pixels; z: mode (0 SDR, 1 scRGB, 2 PQ)
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut vsMain(uint vertexId : SV_VertexID)
{
    // One triangle covering the viewport (= the picture's rectangle).
    VSOut o;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 psMain(VSOut input) : SV_Target
{
    const float2 p = (input.pos.xy - place.xy) * place.zw;
    const int2 last = int2(info.xy) - 1;
    const float4 o = overlayTex.Load(int3(clamp(int2(floor(p)), int2(0, 0), last), 0));
    float3 c = saturate(o.rgb);
    const uint mode = uint(info.z + 0.5);
    if (mode != 0) {
        const float3 lin = lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)) * (203.0 / 80.0);
        c = mode == 2 ? HdrEncodePq(lin) : lin;
    }
    return float4(c, saturate(o.a));
}
