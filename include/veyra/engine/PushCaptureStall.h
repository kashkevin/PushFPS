// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s10-display" (24 Nov 2026), from the owner's Package 14 and 15 logs (review of s10-display, PART A 2): pairs of
// capture stalls about 17 s apart near every quarter hour by the wall clock, in both sessions and across a restart of the
// program: callback gaps of 100-158 ms while the device timestamps keep their 33.34 ms steps, the callback's own copy time
// 6.5-9.5 ms instead of 1.1, the delayed samples then delivered as a burst. The cause is outside what the log shows.
// This is the always-on fact line for such a moment, [capture-stall] (at most a few lines a quarter hour; numbers only):
//   kind=gap    a callback came more than kGapIntervals nominal intervals after the previous one: the gap, the timestamp
//               step, this callback's copy time and the copy times just before it, the session's counters;
//   kind=burst  the first callback after the gap that arrives at least half an interval after its predecessor: how many
//               callbacks came closer than that (the burst), their longest copy, the burst's span.
// Portable, header-only, O(1) per callback, no allocation. Called on the capture callback thread only.
// Tests: tests/remoteplay/DisplayPathTests.inc.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace veyra::engine {

class PushCaptureStall {
public:
    static constexpr double kGapIntervals = 3.0;
    // arrivalDeltaMs: host time since the previous callback (<= 0: the first); ptsDeltaMs: the device timestamp step;
    // copyMs: this callback's time from entry to its copied frame; intervalMs: the nominal frame interval; received: the
    // source's callback counter.
    std::optional<std::string> observe(double arrivalDeltaMs, double ptsDeltaMs, double copyMs, double intervalMs, std::uint64_t received) {
        std::optional<std::string> line;
        if (!(intervalMs > 0) || !(arrivalDeltaMs > 0)) { remember(copyMs); return line; }
        if (arrivalDeltaMs > kGapIntervals * intervalMs) {
            ++gaps_;
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "kind=gap gapMs=%.1f ptsStepMs=%.3f intervals=%.1f copyMs=%.2f previousCopyMaxMs=%.2f received=%llu gaps=%llu "
                          "(a capture callback came late; the device timestamps say whether frames were missing: ptsStepMs near one interval = "
                          "late, not lost; the wall-clock time of this line is the fact to compare with other programs' schedules)",
                          arrivalDeltaMs, ptsDeltaMs, arrivalDeltaMs / intervalMs, copyMs, recentCopyMax(), static_cast<unsigned long long>(received),
                          static_cast<unsigned long long>(gaps_));
            line = std::string(buf);
            inBurst_ = true;
            burstCallbacks_ = 0;
            burstCopyMaxMs_ = copyMs;
            burstSpanMs_ = 0;
        } else if (inBurst_) {
            if (arrivalDeltaMs < 0.5 * intervalMs) {
                ++burstCallbacks_;
                burstCopyMaxMs_ = std::max(burstCopyMaxMs_, copyMs);
                burstSpanMs_ += arrivalDeltaMs;
            } else {
                char buf[256];
                std::snprintf(buf, sizeof buf, "kind=burst callbacks=%u spanMs=%.1f copyMaxMs=%.2f nextDeltaMs=%.1f (callbacks closer than half an interval after the gap)",
                              burstCallbacks_, burstSpanMs_, burstCopyMaxMs_, arrivalDeltaMs);
                line = std::string(buf);
                inBurst_ = false;
            }
        }
        remember(copyMs);
        return line;
    }
    std::uint64_t gaps() const { return gaps_; }

private:
    void remember(double copyMs) {
        if (!(copyMs >= 0)) return;
        copies_[next_] = copyMs;
        next_ = (next_ + 1) % copies_.size();
    }
    double recentCopyMax() const { return *std::max_element(copies_.begin(), copies_.end()); }
    std::array<double, 8> copies_{};
    std::size_t next_ = 0;
    std::uint64_t gaps_ = 0;
    bool inBurst_ = false;
    unsigned burstCallbacks_ = 0;
    double burstCopyMaxMs_ = 0, burstSpanMs_ = 0;
};

}  // namespace veyra::engine
