// ScaleBlit: bilinear-resampled copy used by the present path to map the
// working-extent frame onto the swapchain backbuffer (video extent may differ
// from window extent). SDR path only: straight RGBA8 in, RGBA8 out.

#include "HdrColor.hlsli"
Texture2D<float4> sourceTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);

cbuffer ScaleBlitConstants : register(b0)
{
    uint sourceWidth;
    uint sourceHeight;
    uint outputWidth;
    uint outputHeight;
    float encodeSrgb; // 1 only at linear working -> SDR output boundary
    float ditherStep; // 1 LSB of the target (0 = no dither)
    float orderedSteps; // PushFPS restoration: 255 or 1023 = round with OrderedDitherRound below (0 = as before)
    float _pad3;
};

groupshared float4 tile[16][16];

// PushFPS restoration (pipeline/PushCleanPass.h): after the clean-up the final 8- or 10-bit write is rounded
// with a dither that keeps black exact, so that debanded gradients do not band again at the write.
// PORTED from libplacebo (https://code.videolan.org/videolan/libplacebo, commit 89b04663135b, LGPL-2.1 or
// later): src/shaders/dithering.c `sh_dither`, method PL_DITHER_ORDERED_FIXED (16 x 16 ordered pattern from
// bit operations, no texture; the same bias for R, G and B; not temporal). The original notice of that file:
/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */
// Changed for PushFPS (2026-11-20): GLSL to HLSL; the pixel position comes from the thread id; three
// components; the low-depth gamma branch (4 bits and fewer) is left out. Mirrors engine::pushCleanOrderedBias
// and engine::pushCleanDitherRound (include/veyra/engine/PushClean.h).
float3 OrderedDitherRound(float3 color, uint2 pixel, float steps)
{
    uint2 xy = pixel % 16u;
    // Bitwise merge (morton number)
    xy.x = xy.x ^ xy.y;
    xy = (xy | (xy << 2u)) & 0x33333333u;
    xy = (xy | (xy << 1u)) & 0x55555555u;
    // Bitwise inversion
    uint b = xy.x + (xy.y << 1u);
    b = ((b * 0x0802u) & 0x22110u) | ((b * 0x8020u) & 0x88440u);
    b = 0x10101u * b;
    b = (b >> 16u) & 0xFFu;
    const float bias = float(b) * (1.0 / 256.0);
    // Don't let the dither pattern lift true black off zero.
    if (abs(color.r) < 1e-5) color.r = 0.0;
    if (abs(color.g) < 1e-5) color.g = 0.0;
    if (abs(color.b) < 1e-5) color.b = 0.0;
    return floor(steps * color + bias) * (1.0 / steps);
}

[numthreads(16, 16, 1)]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID, uint3 globalId : SV_DispatchThreadID)
{
    if (globalId.x >= outputWidth || globalId.y >= outputHeight) {
        return;
    }
    float4 value;
    if(sourceWidth==outputWidth&&sourceHeight==outputHeight){
        // Avoid reciprocal/interpolation rounding on tall 1:1 images.
        value=sourceTex[globalId.xy];
    }else{
    // Map output pixel center back into source coordinates.
    const float scaleX = float(sourceWidth) / float(outputWidth);
    const float scaleY = float(sourceHeight) / float(outputHeight);
    const float sx = (float(globalId.x) + 0.5) * scaleX - 0.5;
    const float sy = (float(globalId.y) + 0.5) * scaleY - 0.5;
    const int x0 = int(floor(sx));
    const int y0 = int(floor(sy));
    const float fx = sx - float(x0);
    const float fy = sy - float(y0);
    const int x1 = min(x0 + 1, int(sourceWidth) - 1);
    const int y1 = min(y0 + 1, int(sourceHeight) - 1);
    const int xc = max(x0, 0);
    const int yc = max(y0, 0);
    const float4 s00 = sourceTex[uint2(xc, yc)];
    const float4 s10 = sourceTex[uint2(x1, yc)];
    const float4 s01 = sourceTex[uint2(xc, y1)];
    const float4 s11 = sourceTex[uint2(x1, y1)];
    const float4 top = lerp(s00, s10, fx);
    const float4 bottom = lerp(s01, s11, fx);
    value = lerp(top, bottom, fy);
    }
    if(encodeSrgb>2.5){value.rgb=HdrProxy(value.rgb);}
    // The dither runs in the *coded* domain, so it must be applied after the
    // transfer encode of whichever 8/10-bit target the graph is writing.
    if(encodeSrgb>1.5&&encodeSrgb<2.5){
        float3 pq=HdrEncodePq(value.rgb);
        if(orderedSteps>0.5)pq=OrderedDitherRound(pq,globalId.xy,orderedSteps);
        else pq+=OutputDither(globalId.xy,ditherStep);
        outputTex[globalId.xy]=float4(pq,value.a);return;
    }
    if (encodeSrgb > 0.5) {
        const float3 c = max(value.rgb, 0.0);
        value.rgb = select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0/2.4) - 0.055);
        if (orderedSteps > 0.5) value.rgb = OrderedDitherRound(value.rgb, globalId.xy, orderedSteps);
        else value.rgb += OutputDither(globalId.xy, ditherStep);
    }
    if(encodeSrgb < -0.5){const float3 c=max(value.rgb,0.0);value.rgb=select(c<=0.04045,c/12.92,pow((c+0.055)/1.055,2.4));}
    outputTex[globalId.xy] = value;
    (void)groupId;
    (void)localId;
    (void)tile;
}
