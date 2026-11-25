// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// How much a decoded stream picture differs from the one before it (pipeline/StreamChange.h,
// pipeline/StreamChangePass.h; experiment VEYRA_EXP_RP_STREAM_DEDUP).
//
// One thread per 16x16 block of the luma plane (the blocks at the right and bottom edges hold the
// samples the picture has there). Two comparisons with the previous picture, in 8-bit steps:
//   - the block's mean against the mean the same block had, which this pass left in blockMeans;
//   - every sample against the same sample, which this pass left in lumaBefore. A block mean
//     cannot see what moves inside the block; this can.
// Both stores are rewritten here for the next picture, each texel by the one thread that read it.
// result (twelve words, cleared before every dispatch; veyra::pipeline::StreamChangeWords):
//   [0] largest block difference x 256
//   [1] sum of all block differences x 256
//   [2] blocks that moved by more than one step
//   [3] blocks that reported in
//   [4] largest sample difference x 256
//   [5] samples that differ by more than pixelLimit steps
//   [6] samples that were read
//   [7] sum of the sample differences that brightened, 8-bit steps (each block's sum rounded)
//   [8] sum of those that darkened, the same way
//   [9] blocks in which any sample changed at all
//   [10] blocks that changed one way (veyra::pipeline::streamChangeCoherent)
//   [11] not used
// The CPU reference with the same rounding is veyra::pipeline::streamChangeReference.
// Names here avoid every HLSL keyword and reserved word (no fixed, sample, point, line, linear,
// centroid, precise, pixel, half, vector, matrix, string, pass, texture).
Texture2D<float> lumaPlane : register(t0);      // plane 0 as R8_UNORM or R16_UNORM
RWTexture2D<float> blockMeans : register(u0);   // R32_FLOAT, one texel per block
RWBuffer<uint> result : register(u1);          // R32_UINT x 12 (as HdrCurveCoverage.hlsl's stats)
RWTexture2D<float> lumaBefore : register(u2);   // R32_FLOAT, the luma plane of the previous picture
cbuffer Constants : register(b0) {
    uint blocksX, blocksY, compare, lumaWidth;  // compare 0: no previous picture, only fill the stores
    uint lumaHeight;
    float pixelLimit;                           // kStreamChangePixelLimit
    uint pad0, pad1;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= blocksX || id.y >= blocksY) return;
    const uint2 origin = id.xy * 16;
    const uint blockColumns = min(16u, lumaWidth - origin.x);
    const uint blockRows = min(16u, lumaHeight - origin.y);
    float blockSum = 0;
    float largestMove = 0;
    uint movedCount = 0;
    float brighterSum = 0;
    float darkerSum = 0;
    for (uint y = 0; y < blockRows; ++y)
        for (uint x = 0; x < blockColumns; ++x) {
            const uint2 spot = origin + uint2(x, y);
            const float lumaNow = lumaPlane.Load(int3(spot, 0));
            const float lumaWas = lumaBefore[spot];
            lumaBefore[spot] = lumaNow;
            blockSum += lumaNow;
            const float signedMove = (lumaNow - lumaWas) * 255.0;
            const float lumaMove = abs(signedMove);
            brighterSum += max(signedMove, 0.0);
            darkerSum += max(-signedMove, 0.0);
            largestMove = max(largestMove, lumaMove);
            if (lumaMove > pixelLimit) movedCount += 1;
        }
    const uint readCount = blockColumns * blockRows;
    const float mean = blockSum * 255.0 / float(readCount);
    const float previous = blockMeans[id.xy];
    blockMeans[id.xy] = mean;
    InterlockedAdd(result[3], 1);
    InterlockedAdd(result[6], readCount);
    if (compare == 0) return;
    const float difference = abs(mean - previous);
    const uint differenceQ8 = uint(min(difference, 255.0) * 256.0 + 0.5);   // 8.8, as kStreamChangeFixed
    InterlockedMax(result[0], differenceQ8);
    InterlockedAdd(result[1], differenceQ8);
    if (difference > 1.0) InterlockedAdd(result[2], 1);
    InterlockedMax(result[4], uint(min(largestMove, 255.0) * 256.0 + 0.5));
    InterlockedAdd(result[5], movedCount);
    const uint brighterSteps = uint(brighterSum + 0.5);
    const uint darkerSteps = uint(darkerSum + 0.5);
    InterlockedAdd(result[7], brighterSteps);
    InterlockedAdd(result[8], darkerSteps);
    if (largestMove > 0.5) InterlockedAdd(result[9], 1);
    const uint netSteps = brighterSteps > darkerSteps ? brighterSteps - darkerSteps : darkerSteps - brighterSteps;
    if (netSteps >= 16 && netSteps * 2 >= brighterSteps + darkerSteps) InterlockedAdd(result[10], 1);   // kStreamChangeCoherentSteps
}
