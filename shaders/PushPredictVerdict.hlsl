// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Package 15, review 2 SHOULD 3 (pipeline/PushPredictPass.cpp): the whole-frame verdict of one predicted frame P.
// Reads P's four counters (still, held, fallback, warped) after the warp and writes a 64-bit predicate: 1 when more of the
// moving pixels fell back than were moved (fallback > warped: more than half), else 0. The pass then copies B over P
// under that predicate, so a frame that is mostly held is shown as B as a whole (a uniform one-slot hold) instead of a
// torn mix of moved and held regions. PushPredict.h pushPredictWholeFrameHeld is the CPU reference.
cbuffer Constants : register(b0)
{
    uint counterOffset;   // bytes into the counter buffer: 16 x slot
    uint verdictOffset;   // bytes into the verdict buffer: 8 x slot
};
RWByteAddressBuffer counters : register(u0);
RWByteAddressBuffer verdict : register(u1);

[numthreads(1, 1, 1)]
void main()
{
    const uint fallback = counters.Load(counterOffset + 8);
    const uint warped = counters.Load(counterOffset + 12);
    verdict.Store2(verdictOffset, uint2(fallback > warped ? 1u : 0u, 0u));
}
