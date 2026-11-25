// SPDX-License-Identifier: GPL-3.0-only
// TEST ONLY (TestVfgBackend, VEYRA_TEST_FAKE_VFG=1): the stand-in for a VFG Run on CI's software renderer.
// One generated frame = a per-channel linear blend of the pair's encoded inputs A (t = 0) and B (t = 1) at
// t = numerator / denominator (FrameIndex / FrameMultiplier). Both inputs and the output are the pitch-linear
// 32-bit words VfgBackend's shared buffers hold (R8G8B8A8 or R10G10B10A2). No motion, no model: a test pattern only.

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;        // bytes per row of all three buffers
    uint tenBit;       // 1: R10G10B10A2, 0: R8G8B8A8
    uint numerator;    // FrameIndex
    uint denominator;  // FrameMultiplier (>= 2)
};

ByteAddressBuffer frameA : register(t0);
ByteAddressBuffer frameB : register(t1);
RWByteAddressBuffer output : register(u0);

uint mixChannel(uint a, uint b)
{
    // Rounded integer lerp: (a * (d - n) + b * n + d / 2) / d. Values stay below 2^10 * 2^3, no overflow.
    return (a * (denominator - numerator) + b * numerator + denominator / 2) / denominator;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint offset = id.y * pitch + id.x * 4;
    const uint a = frameA.Load(offset);
    const uint b = frameB.Load(offset);
    uint word = 0;
    if (tenBit != 0) {
        word = mixChannel(a & 1023u, b & 1023u)
             | (mixChannel((a >> 10) & 1023u, (b >> 10) & 1023u) << 10)
             | (mixChannel((a >> 20) & 1023u, (b >> 20) & 1023u) << 20)
             | (mixChannel(a >> 30, b >> 30) << 30);
    } else {
        word = mixChannel(a & 255u, b & 255u)
             | (mixChannel((a >> 8) & 255u, (b >> 8) & 255u) << 8)
             | (mixChannel((a >> 16) & 255u, (b >> 16) & 255u) << 16)
             | (mixChannel(a >> 24, b >> 24) << 24);
    }
    output.Store(offset, word);
}
