// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink: the one place where the running PS5 stream's latest SmartLink decision is shared between the parts
// of the program (the network session owner publishes; the audio sink, the latency governor and the interface
// read), and where the audio sink reports its buffer back for the next observation window.
//
// Header-only and portable. Process-wide because there is one PS5 stream at a time. The lock is held for a copy
// of a few hundred bytes, at most a few times per second by the writer, so a reader never waits measurably.
// With SmartLink off nothing is ever published: latestShared().sequence stays 0 and every reader must then behave
// exactly as it did before SmartLink existed.
//
// This file is shared by several work streams: copy it UNCHANGED (ask the orchestrator for changes).
#include "veyra/remoteplay/SmartLinkTypes.h"

#include <cstdint>
#include <mutex>

namespace veyra::remoteplay::smartlink {

struct SharedState {
    Decision decision{};
    Mode mode = Mode::Off;          // Off: nothing here is meaningful
    std::uint64_t sequence = 0;     // 0 = nothing published since the stream started (or SmartLink is off)
    std::int64_t updatedMs = -1;    // host monotonic milliseconds of the last publish
};

// What the audio sink knows and the observation window needs (SmartLinkTypes.h Sample::audioBufferedMs /
// audioUnderruns). Negative = unknown.
struct AudioFeed {
    double bufferedMs = -1;         // PCM queued for playout right now
    std::int64_t underruns = -1;    // playout gaps since the stream started (cumulative)
    std::uint64_t sequence = 0;     // 0 = the sink has not reported
};

namespace detail {
struct SharedBox {
    std::mutex mutex;
    SharedState state;
    AudioFeed audio;
};
inline SharedBox& sharedBox() noexcept {
    static SharedBox box;
    return box;
}
} // namespace detail

// Session owner thread, after each decision (observe and apply modes alike).
inline void publishShared(const Decision& decision, Mode mode, std::int64_t nowMs) noexcept {
    auto& box = detail::sharedBox();
    const std::lock_guard<std::mutex> lock(box.mutex);
    box.state.decision = decision;
    box.state.mode = mode;
    box.state.updatedMs = nowMs;
    ++box.state.sequence;
}

// Any thread. A reader that acts on the decision must check mode == Mode::Apply AND that the state is fresh
// (isFresh below): a stale decision is treated as "leave everything as configured".
inline SharedState latestShared() noexcept {
    auto& box = detail::sharedBox();
    const std::lock_guard<std::mutex> lock(box.mutex);
    return box.state;
}

// A decision older than this is ignored by every reader (the publisher normally refreshes it every 200 ms).
inline constexpr std::int64_t kSharedFreshMs = 2000;
inline bool isFresh(const SharedState& state, std::int64_t nowMs) noexcept {
    return state.sequence != 0 && state.mode != Mode::Off && state.updatedMs >= 0 && nowMs >= state.updatedMs &&
           nowMs - state.updatedMs <= kSharedFreshMs;
}

// Audio sink thread (any cadence; a few times per second is enough).
inline void publishAudioFeed(double bufferedMs, std::int64_t underruns) noexcept {
    auto& box = detail::sharedBox();
    const std::lock_guard<std::mutex> lock(box.mutex);
    box.audio.bufferedMs = bufferedMs;
    box.audio.underruns = underruns;
    ++box.audio.sequence;
}

inline AudioFeed latestAudioFeed() noexcept {
    auto& box = detail::sharedBox();
    const std::lock_guard<std::mutex> lock(box.mutex);
    return box.audio;
}

// Stream start and stream end: forget everything, so nothing from one stream reaches the next.
inline void clearShared() noexcept {
    auto& box = detail::sharedBox();
    const std::lock_guard<std::mutex> lock(box.mutex);
    box.state = SharedState{};
    box.audio = AudioFeed{};
}

} // namespace veyra::remoteplay::smartlink
