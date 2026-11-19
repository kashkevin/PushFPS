// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Picture numbers for a hardware-decoded stream that shows every game picture twice.
//
// A console game running at 30 fps inside a 60 Hz output sends A A B B C C. The second copy is a
// new encoded frame, and a constant-bitrate encoder spends its bits on it too: it refines the first
// copy. So the second copy is neither small nor identical after decoding. Measured on libx264 and
// libx265 low-delay streams (tools/remoteplay_latency_bench/cadence_streams.py): a repeat was 0.2
// to 1.1 times the size of a new frame, and a block of the decoded repeat differed from the first
// copy by up to 28 of 255 steps on a starved stream, more than a new picture differs in a slow
// scene. Neither size nor a fixed difference threshold separates them.
//
// What does separate them is the rhythm. `change` is how much the decoded picture differs from the
// previous decoded picture (largest difference of the mean luma of any 16x16 block, in 8-bit
// steps). In a doubled stream it goes high, low, high, low. Once that has held for `lockSteps`
// steps, a picture whose change drops below `ratio` times the change before it is a repeat. A
// picture that did not change at all is a repeat with or without the rhythm.
//
// Errors are one-sided on purpose: when unsure the picture counts as new, and the frame is kept.
// A kept repeat costs nothing visible; a dropped new picture is a lost game frame.
//
// Portable and free of platform types, so the unit tests run anywhere. It only numbers pictures;
// include/veyra/engine/ContentDedup.h decides what to do with repeats.
#include <cstdint>

namespace veyra::remoteplay {

struct StreamCadenceConfig {
    float ratio = 0.4f;       // a repeat changes less than this share of the change before it
    float identical = 0.6f;   // below this many 8-bit steps nothing changed at all
    unsigned lockSteps = 6;   // alternating steps before the rhythm is trusted
    unsigned unlockMisses = 2; // steps without a clear rise or fall the rhythm survives
};

class StreamCadence {
public:
    struct Result {
        std::uint64_t picture = 0; // equal numbers show the same picture; never 0
        bool repeat = false;
        bool locked = false;       // the doubled rhythm is currently trusted
    };
    struct Stats { std::uint64_t pictures = 0, repeats = 0, identical = 0, locks = 0, unlocks = 0; };

    explicit StreamCadence(StreamCadenceConfig config = {}) : config_(config) {}

    // change: largest block difference to the previous decoded picture, 8-bit steps.
    // boundary: first picture, decoder reset, lost frames or a seek: the previous picture is not
    // comparable, the picture is new and the rhythm starts again.
    Result observe(float change, bool boundary) {
        ++stats_.pictures;
        if (boundary || !havePrevious_ || !(change >= 0.0f)) {  // also rejects NaN
            reset();
            havePrevious_ = change >= 0.0f; previous_ = havePrevious_ ? change : 0.0f;
            return {++picture_, false, false};
        }
        const bool down = change < config_.ratio * previous_;
        const bool up = previous_ < config_.ratio * change;
        bool repeat = false;
        if (change < config_.identical) { repeat = true; ++stats_.identical; }
        if (down || up) {
            run_ = haveStep_ && down != lastDown_ ? run_ + 1 : 1;
            haveStep_ = true; lastDown_ = down;
        } else {
            run_ = 0; haveStep_ = false;
        }
        if (!locked_ && run_ >= config_.lockSteps) { locked_ = true; misses_ = 0; ++stats_.locks; }
        if (locked_) {
            if (down) { repeat = true; misses_ = 0; }
            else if (up) misses_ = 0;
            else if (++misses_ > config_.unlockMisses) { locked_ = false; run_ = 0; ++stats_.unlocks; }
        }
        previous_ = change;
        if (repeat) { ++stats_.repeats; return {picture_ ? picture_ : ++picture_, true, locked_}; }
        return {++picture_, false, locked_};
    }

    void reset() { havePrevious_ = false; previous_ = 0.0f; run_ = 0; haveStep_ = false; lastDown_ = false; locked_ = false; misses_ = 0; }
    bool locked() const { return locked_; }
    const Stats& stats() const { return stats_; }

private:
    StreamCadenceConfig config_;
    Stats stats_;
    std::uint64_t picture_ = 0;
    float previous_ = 0.0f;
    unsigned run_ = 0, misses_ = 0;
    bool havePrevious_ = false, haveStep_ = false, lastDown_ = false, locked_ = false;
};

} // namespace veyra::remoteplay
