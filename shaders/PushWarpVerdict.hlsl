// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg" (PushWarp.h pushWarpWholeFrameNearest): the whole-frame verdict of one interpolated frame. Reads its four
// counters (still, blend, conflict, hole) and writes a 64-bit predicate: 1 when more moving pixels are holes than not
// (a cut, a failed flow: nothing of B landed there), else 0. The pass then copies the real frame nearer in time over the
// frame under that predicate (PushPredictVerdict.hlsl's form).
#include "PushWarp.hlsli"

RWByteAddressBuffer counters : register(u2);
RWByteAddressBuffer verdict : register(u3);

[numthreads(1, 1, 1)]
void main()
{
    const uint4 c = counters.Load4(counterOffset);
    verdict.Store2(verdictOffset, uint2(c.w > c.y + c.z ? 1u : 0u, 0u));
}
