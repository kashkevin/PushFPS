// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Package 15, TASK 3 (the owner, 24 Nov 2026: "verify exactly what the reported latency measures and distinguish
// frame-generation processing time from scheduling delay and total input latency"): where the delay readout goes.
//
// The readout (PushLatencyBadge, pushLiveStats latencyMs, FrameFlowWindow::latency) is, per REAL frame, the time from the
// capture callback (arrival) to the return of its Present call, averaged over the last second. Each real frame's total
// is split here into four parts that add up to it exactly:
//   picture         arrival -> the real frame seen ready (its own processing: pickup, upload, colour, SR / NR, the base
//                   GPU work, as the engine thread saw it);
//   firstGenerated  the real frame seen ready -> its pair's first generated frame seen ready (the first generated frame's
//                   processing; 0 when the pair generated nothing);
//   wait            the schedule's wait for the frames shown before the real one: (generated frames before it in its
//                   batch) / (frames in the batch) x the pair's content interval: 2T/3 when interpolating at 3X, T/3 in
//                   the "Lower delay" schedule, 0 for a pair without generated frames. A formula from the pair's own
//                   timestamps, not a reading;
//   margin          the rest: what the learned phase holds beyond those, the Present call's own time and any lateness.
// Parts are clamped in that order so they never exceed the total (a late look at readiness moves time between picture
// and firstGenerated; the sum is unchanged). A frame whose readiness was not seen is not split: its whole total is
// counted as margin and as unsplit (splitShare says how many were split).
// What the number is NOT: the game's own delay, the console's video output, the capture card, the display and the
// mouse / pad are outside it (software cannot see them).
// Portable, header-only, O(1) per frame, no allocation. Tests: tests/remoteplay/DelaySplitTests.inc.
#include <algorithm>
#include <cstdint>

namespace veyra::engine {

struct PushDelayParts {
    double pictureMs = 0, firstGeneratedMs = 0, waitMs = 0, marginMs = 0;
    bool split = false;   // readiness was seen: the four parts are meaningful
};

// totalMs: Present returned - arrival; readyMs, firstMs: seen-ready times after arrival (negative: unknown / none);
// generatedBefore, frames: the real frame's batch layout; intervalMs: the pair's content interval (b - a).
inline PushDelayParts pushDelayParts(double totalMs, double readyMs, double firstMs, unsigned generatedBefore, unsigned frames, double intervalMs) {
    PushDelayParts p;
    if (!(totalMs >= 0.0)) return p;
    if (!(readyMs >= 0.0)) { p.marginMs = totalMs; return p; }
    p.split = true;
    double left = totalMs;
    p.pictureMs = std::min(readyMs, left);
    left -= p.pictureMs;
    if (generatedBefore > 0 && firstMs >= readyMs) p.firstGeneratedMs = std::min(firstMs - readyMs, left);
    left -= p.firstGeneratedMs;
    if (generatedBefore > 0 && frames > generatedBefore && intervalMs > 0.0)
        p.waitMs = std::min(double(generatedBefore) / double(frames) * intervalMs, left);
    left -= p.waitMs;
    p.marginMs = left;
    return p;
}

// Sums over one window of frames (FrameFlowWindow keeps one per readout window). Means of the parts add up to the mean
// of the totals (the readout) to within floating-point rounding, because every frame's parts add up to its total.
struct PushDelaySplitSums {
    double total = 0, picture = 0, firstGenerated = 0, wait = 0, margin = 0;
    std::uint64_t frames = 0, split = 0;
    void add(double totalMs, const PushDelayParts& p) {
        total += totalMs; picture += p.pictureMs; firstGenerated += p.firstGeneratedMs; wait += p.waitMs; margin += p.marginMs;
        ++frames; split += p.split ? 1u : 0u;
    }
    double mean(double sum) const { return frames ? sum / double(frames) : -1.0; }
    double splitShare() const { return frames ? double(split) / double(frames) : 0.0; }
};

}  // namespace veyra::engine
