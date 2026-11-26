// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Types.h"
#include <deque>
namespace veyra::remoteplay {
class Sequence16Extender {
public:
    struct Result {std::uint64_t value=0;bool accepted=false,first=false;std::uint32_t gap=0;};
    Result observe(std::uint16_t) noexcept;
    void reset() noexcept {*this={};}
private:
    bool initialized_=false;std::uint16_t previous_=0;std::uint64_t extended_=0;
};
struct EstimatedStamp {
    std::int64_t pts100ns=0,duration100ns=0;
    bool discontinuity=false;
    TimestampProvenance provenance=TimestampProvenance::LocalEstimated;
};
// A shared local origin for audio and video, NOT independent zero-at-first-frame clocks.
// Source index gives cadence, host arrival provides an anchor. Reset after network
// discontinuity; consumer must advance its epoch and reset temporal GPU history.
class RemotePlayClock {
public:
    // followRate: experiment VEYRA_EXP_RP_CLOCK_RATE (stream "sync"). false (default): the phase moves at most
    // 0.5 % of a frame per frame, so a console that sends fewer than 59.7 indexes a second for a 60 profile
    // (Package 9: 56.4-59.5 at 15-50 Mbit/s) leaves the PTS behind its arrival by 1000 - 16.75 x rate ms every
    // second, without bound. true: 10 % per frame, so the same lower envelope is followed for any console rate
    // within 10 % of nominal (54-66 for a 60 profile); the lag stays bounded by the envelope window.
    void reset(HostTime commonOrigin,std::uint32_t fps,bool followRate=false);
    std::optional<EstimatedStamp> video(std::uint64_t index,HostTime arrival);
    std::optional<std::int64_t> audioPts(std::uint64_t firstSample,std::uint32_t rate,HostTime audioStart) const noexcept;
    // Diagnostics ([remoteplay-clock] wireFps=): wire indexes per second of arrival since the previous call (or
    // since the first frame after reset), then the mark moves here. Unknown before a frame, for no time or an
    // index that went back. What the console numbered and sent, not what was shown.
    std::optional<double> takeWireFps(std::uint64_t index,HostTime arrival) noexcept;
    bool followRate() const noexcept {return followRate_;}
private:
    HostTime origin_=0,videoAnchor_=0,lastArrival_=0;
    // Bounded lower-envelope PLL: nominal profile FPS is not a source clock
    // (a 60 profile can deliver ~59.94). Filter network jitter, slew only the
    // locally estimated timestamps, and never accumulate nominal-rate error.
    struct PhaseSample {HostTime arrival;std::int64_t error;};
    std::deque<PhaseSample> phaseWindow_;
    std::int64_t phase_=0,lastPts_=0;
    std::uint64_t firstIndex_=0,lastIndex_=0;
    std::uint32_t fps_=0;bool anchored_=false;
    bool followRate_=false,wireMarked_=false;std::uint64_t wireMarkIndex_=0;HostTime wireMarkArrival_=0;
};
} // namespace veyra::remoteplay
