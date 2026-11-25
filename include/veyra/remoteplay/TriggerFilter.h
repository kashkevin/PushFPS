// SPDX-License-Identifier: GPL-3.0-only
// From Veyra 17e54e24 ("Remote Play: keep L2/R2 releases from being lost", cherry-pick of 261be9d8)
// with rule 3 as corrected by Veyra 15adac8f (release 2.2.0), the rules unchanged. PushFPS: off unless
// VEYRA_EXP_RP_TRIGGER_FILTER=1 (ControllerInput.cpp), because rules 2 and 3 change which trigger
// values are sent.
#pragma once
#include "Types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace veyra::remoteplay {

// L2/R2 for PS5 Remote Play.
//
// Every change of an analog trigger value becomes one Chiaki feedback-history
// event, and each history packet repeats only the newest 8 events. The
// controller owner submits every 4 ms, so a trigger that jitters by a step or
// two (worn trigger, or a game's adaptive-trigger vibration) produces a stream
// of events. When one trigger is released while the other is still held, that
// stream pushes the release out of the 8-event window within ~30 ms. If those
// few packets are lost, the console never sees the release and keeps the
// trigger held until it moves again (reported: press L2 and R2 together, L2
// stays held; pressing L2 again releases it). Chiaki's tail resend of the last
// history packet does not help here, because it only runs while nothing new is
// queued.
//
// The filter removes the jitter:
//  1. Ends snap: a value within kDeadzone of 0 or 255 reads as 0 or 255.
//  2. Between the ends, a value is only sent once it moves kStep or more from
//     the last sent value. 0 and 255 are always sent at once.
//  3. For kEdgeHold after a trigger is released, raise the other trigger's
//     movement threshold. Real trigger travel and both endpoints still go
//     through immediately, while small noise leaves the release in history.
// Presses, releases, full pulls and travel of 2 * kStep or more are never
// delayed. A smaller in-between movement is not sent until it has grown to
// kStep (2 * kStep during rule 3's window), so between the snapped ends the
// value the console holds can be up to kStep - 1 (2 * kStep - 1) steps of 255
// away from the trigger.
class TriggerFilter {
public:
    static constexpr std::uint8_t kDeadzone = 6;
    static constexpr int kStep = 4;
    static constexpr HostTime kEdgeHold = 1200000; // 120 ms in 100 ns units

    struct Values { std::uint8_t l2 = 0, r2 = 0; };

    Values apply(std::uint8_t l2, std::uint8_t r2, HostTime now) {
        const std::array<std::uint8_t, 2> raw{snap(l2), snap(r2)};
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t other = 1 - i;
            const std::uint8_t v = raw[i];
            std::uint8_t& sent = sent_[i];
            if (v == sent) continue;
            if (v == 0 || v == 255 || sent == 0) {
                // Release, full press or first press: an edge, sent at once.
                if (v == 0 && sent != 0) holdUntil_[other] = now + kEdgeHold;
                sent = v;
                continue;
            }
            const int step = now < holdUntil_[i] ? 2 * kStep : kStep;
            if (std::abs(int(v) - int(sent)) >= step) sent = v;
        }
        return {sent_[0], sent_[1]};
    }

    // Focus or device loss: the next poll starts from released triggers.
    void reset() { *this = TriggerFilter{}; }

private:
    static std::uint8_t snap(std::uint8_t v) {
        if (v <= kDeadzone) return 0;
        if (v >= 255 - kDeadzone) return 255;
        return v;
    }
    std::array<std::uint8_t, 2> sent_{};
    std::array<HostTime, 2> holdUntil_{};
};

} // namespace veyra::remoteplay
