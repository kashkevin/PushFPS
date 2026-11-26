// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg" R1 (PushWarp.h pushWarpReference, "R1 pass 1 and 2"): B's flow cells forward-splatted to where their
// content is at time t, p + (1 - t) g, with bilinear weights x the cell's trust. Collisions: a softmax of the speed per
// target (the faster mover in front), in the two-pass integer form: mode 0 InterlockedMax of the quantised importance per
// target cell; mode 1 InterlockedAdd of fixed-point weight x exp(importance - that max), of the weighted vector (1/32 flow
// px) and of the plain coverage. Integer atomics only.
#include "PushWarp.hlsli"

RWByteAddressBuffer scratch : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= flowWidth || id.y >= flowHeight) return;
    const uint4 cell = scratch.Load4(cellAddress(id.y * flowWidth + id.x));
    const float gx = asfloat(cell.x), gy = asfloat(cell.y), cellTrust = asfloat(cell.z);
    const uint importance = cell.w;
    if (!(cellTrust > 0)) return;
    const float qx = float(id.x) + (1.0 - frameTime) * gx, qy = float(id.y) + (1.0 - frameTime) * gy;
    const float fx = floor(qx), fy = floor(qy);
    const float tx = qx - fx, ty = qy - fy;
    const int x0 = int(fx), y0 = int(fy);
    const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    const int vx = roundHalfUp(gx * kVectorScale), vy = roundHalfUp(gy * kVectorScale);
    [unroll] for (int k = 0; k < 4; ++k) {
        const int x = x0 + (k & 1), y = y0 + (k >> 1);
        if (!(w[k] > 0) || x < 0 || y < 0 || x >= int(flowWidth) || y >= int(flowHeight)) continue;
        const uint at = accumAddress(uint(y) * flowWidth + uint(x));
        if (mode == 0) {
            scratch.InterlockedMax(at, importance);
        } else {
            const uint top = scratch.Load(at);
            const float soft = exp((float(importance) - float(top)) / kImportanceScale);
            const uint wq = uint(w[k] * cellTrust * soft * kWeightScale + 0.5);
            const uint cq = uint(w[k] * cellTrust * kWeightScale + 0.5);
            scratch.InterlockedAdd(at + 4, wq);
            scratch.InterlockedAdd(at + 8, cq);
            scratch.InterlockedAdd(at + 12, asuint(int(wq) * vx));
            scratch.InterlockedAdd(at + 16, asuint(int(wq) * vy));
        }
    }
}
