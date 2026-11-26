// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <cmath>

namespace veyra::engine {
// Link stalls for the latency governor (VEYRA_EXP_RP_GOVERNOR=apply), sprint 4. Portable and header-only; tests in
// tests/remoteplay/GovernorStallTests.inc. Unverified on Windows.
//
// The stream's mean jitter (RFC 3550, J += (|D| - J) / 16) hides the stalls Wi-Fi shows: a 20-40 ms stall lifts it
// to about 1.3-2.5 ms (review 4, finding 1). What the inbox snapshot also carries is the PEAK interarrival excess
// over the last 256..512 frames (remoteplay::FrameJitter::peakMs: two buckets of 256 frames, the older one replaced
// in turn): a window maximum, not a per-second value. This turns it into the per-tick value the governor takes
// (PushControlSample::networkStallMs): the excess first seen this tick.
//   - The window maximum can only rise when a frame with a larger excess has arrived since the last tick: then
//     that frame is a stall seen now, and its excess is the new peak.
//   - When it stays or falls (a bucket was replaced) no new frame is known to be late: 0.
//   - Limit: a stall smaller than the larger one still held in the window is not seen (up to ~8.5 s at 60 fps).
//     The governor does not rely on this alone: decode waits longer than all the GPU work in force, and latency
//     excesses longer than it, are not frame generation's whatever this says.
//   - Unknown (not measured, negative, not finite): -1, and the next known value is compared with nothing (a
//     restart reports the held peak once: a stall in the window is then treated as seen, the cautious side).
class PushStallFromPeak {
public:
    void reset() noexcept { have_ = false; last_ = 0; }
    double update(double peakMs) noexcept {
        if (!std::isfinite(peakMs) || peakMs < 0) { reset(); return -1.0; }
        double seen = 0;
        if (!have_ || peakMs > last_ + kEpsilonMs) seen = peakMs;
        have_ = true;
        last_ = peakMs;
        return seen;
    }

private:
    static constexpr double kEpsilonMs = 0.01;
    bool have_ = false;
    double last_ = 0;
};
} // namespace veyra::engine
