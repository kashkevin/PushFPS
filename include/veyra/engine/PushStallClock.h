// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Package 16, the owner's priority "improving frame pacing and eliminating latency fluctuations" (no added buffering),
// from the stall investigator's proposal 1 (QUARTER_HOUR_STALLS.md, Part 5): a capture stall (a callback gap of more
// than 3 intervals and the burst of late frames after it, PushCaptureStallRide) is ONE discontinuity of the clock:
//  - the presentation anchor of its pairs stays on the line the smoother had before the gap (LiveArrivalSmoother::held:
//    no learning, so no snap to a late burst callback and no snaps back afterwards); a lost-frames stall (the line
//    really moved) snaps once, after the ride, as today;
//  - its pairs teach neither the phase learners (LivePairLatency) nor the deadline margin (LiveDeadlineMargin): their
//    readiness against the anchor is the stall's, not the pipeline's, and one such sample raised the phase for seconds.
// The anchor alone: a burst callback is never before its line (frames arrive late, not early), so today's anchor during the
// burst is the held one or later (a snap moves it to the late callback); every deadline of the stall's pairs is the same
// or earlier, and a real frame is presented at max(ready, deadline). Leaving the stall's samples out only lowers the
// learned phase. tests/remoteplay/LowDelaySimTests.inc (stall section) checks it per real frame in the scheduling model.
// Applies with "Lower delay" on and off. OPT-IN (VEYRA_EXP_RP_STALL_CLOCK=1): in the scheduling model (FgScheduleSim.h,
// stall section of LowDelaySimTests.inc) it removes every snap and lowers the real frames' delay on average, but it is
// NOT never-later for every real frame: second-order effects (a generated frame shown that was dropped before, a pair
// admitted differently) leave single real frames up to about 0.4 ms later, and with "Lower delay" on a different
// schedule state can leave a real frame one slot later. So it stays opt-in until the engine test and his log say more.
#include <cstdint>

#include "veyra/engine/LivePresentationTiming.h"
#include "veyra/engine/PushLowDelay.h"

namespace veyra::engine {

class PushStallClock {
public:
    // Once per capture pair, before its anchor: is this pair part of a capture stall (gap, burst, one grace pair)? An
    // engine stall (the engine late, the callbacks on time) is not a clock discontinuity: no lag is passed.
    bool pair(std::int64_t arrival100ns, std::int64_t interval100ns, bool legacy) {
        const std::int64_t delta = last_ ? arrival100ns - last_ : 0;
        last_ = arrival100ns;
        in_ = ride_.pair(delta, interval100ns) && !legacy;
        if (in_) ++pairs_;
        return in_;
    }
    bool inStall() const { return in_; }
    std::uint64_t stalls() const { return ride_.stalls(); }
    std::uint64_t pairs() const { return pairs_; }
    void reset() { last_ = 0; in_ = false; }

private:
    PushCaptureStallRide ride_;
    std::int64_t last_ = 0;
    bool in_ = false;
    std::uint64_t pairs_ = 0;
};

// The anchor of a pair: held on the line in a stall (once a line exists), else the smoother's own.
inline std::int64_t pushStallAnchor(LiveArrivalSmoother& s, bool inStall, std::int64_t source, std::int64_t callback, std::int64_t interval) {
    return inStall && s.hasLine() ? s.held(source) : s.anchor(source, callback, interval);
}
inline std::int64_t pushStallContentAnchor(LiveArrivalSmoother& s, bool inStall, std::int64_t contentPts, std::int64_t samplePts, std::int64_t callback,
                                           std::int64_t transportInterval) {
    return inStall && s.hasLine() ? s.heldContent(contentPts, samplePts, transportInterval) : s.contentAnchor(contentPts, samplePts, callback, transportInterval);
}

}  // namespace veyra::engine
