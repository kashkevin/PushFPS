// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "present" R3 (24 Nov 2026), experiment VEYRA_EXP_RP_PRESENT_BLIT_AHEAD=1 (off by default): blit ahead, Present at the
// deadline (AMD's FidelityFX frame-interpolation swap chain records and finishes the UI composition before it calls Present;
// research report out/research/AMD_FG_WINDOWS_PRESENT_GPU.md section 2.3 P2). Today the presenter records and submits a
// frame's blit AT its deadline and calls Present right after, so the flip waits behind a blit that shares the GPU with the
// next pair's work. With the switch, a frame that is ready before its deadline has its blit submitted at once
// (VideoPresenter::prepare) and Present alone is called at the deadline.
//
// Two models here, both portable and unit-tested (tests/remoteplay/PresentPathTests.inc):
//  1. pushSimulateBlits: one in-order presentation queue on a GPU whose other work blocks the start of a blit (busy
//     windows). For every frame it gives the Present call and the earliest flip (both the Present called and the blit
//     done). Proven by the tests over random schedules: Present never before the deadline, frames in order, the earliest
//     flip never later than today's for any frame, and one prepared frame at a time.
//  2. PushFlipChain: DXGI FLIP_DISCARD with 3 buffers. The back buffer index changes only at Present; a blit into the
//     current index while a frame is queued (maximum frame latency 1: at most one queued) never touches the buffer on
//     screen or the queued one; a prepared frame that is dropped is overwritten by the next blit and never shown.
// What the models do NOT cover, and why the switch stays off by default: the GPU time of the blit is the same, only earlier;
// run earlier it can sit in front of the next pair's graph work (in the model that work is the "busy" input, not an
// output), so the first generated frame of the next pair could be later by up to one blit (~0.1 ms at 4K -> 1440p,
// estimate). That is not provably never-later for every frame (S5_DIRECTIVE25_FORWARD's rule), so it is not a default.
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace veyra::engine {

// Whether a frame may be prepared ahead at all (review S9): the switch is on, the presenter has its own presentation queue,
// no provider owns Present (XeSS), and NO capture consumer is active (recording, replay buffer, going live, comparison clip,
// HUD detection, quality / latency / clarity probes). The capture hook records inside the blit's list, so a prepared frame
// that is then dropped, or re-blitted after a window change, would be recorded although never shown, or twice; with a
// consumer active every frame therefore takes today's path and those outputs are exactly as with the switch off.
inline bool pushBlitAheadAllowed(bool switchOn, bool ownQueue, bool providerPresents, bool captureActive) {
    return switchOn && ownQueue && !providerPresents && !captureActive;
}

struct PushBlitFrame {
    double readyMs = 0;      // the frame's GPU work is complete (its fence), as the engine sees it
    double deadlineMs = 0;   // its Present time on the content grid
    double recordMs = 0;     // CPU time to record and submit the blit list
    double blitMs = 0;       // GPU time of the blit
};
struct PushGpuBusy {
    double startMs = 0, endMs = 0;   // other GPU work during which a presentation-queue blit cannot start
};
struct PushBlitOutcome {
    double submitMs = 0, blitStartMs = 0, blitEndMs = 0, presentCallMs = 0, flipReadyMs = 0;
    bool ahead = false;   // the blit went out before the deadline
};

namespace push_blit_detail {
inline double firstFree(double at, const std::vector<PushGpuBusy>& busy) {
    bool moved = true;
    while (moved) {
        moved = false;
        for (const auto& b : busy)
            if (at >= b.startMs && at < b.endMs) { at = b.endMs; moved = true; }
    }
    return at;
}
}  // namespace push_blit_detail

// The engine handles frames in order on one thread: frame k is looked at only after frame k-1 was presented (or dropped).
// Today: at the deadline the blit is recorded and submitted, then Present. Ahead: when frame k is ready and its deadline
// is still to come, the blit is recorded and submitted then; Present at the deadline. A frame not ready before its
// deadline is handled as today.
inline std::vector<PushBlitOutcome> pushSimulateBlits(const std::vector<PushBlitFrame>& frames, const std::vector<PushGpuBusy>& busy, bool ahead) {
    std::vector<PushBlitOutcome> out;
    out.reserve(frames.size());
    double queueFree = 0, lastPresent = 0;
    for (const auto& f : frames) {
        PushBlitOutcome o;
        const double lookAt = std::max(f.readyMs, lastPresent);   // the engine reaches this frame
        if (ahead && lookAt + f.recordMs < f.deadlineMs) {
            o.ahead = true;
            o.submitMs = lookAt + f.recordMs;
            o.presentCallMs = f.deadlineMs;
        } else {
            const double at = std::max(f.deadlineMs, lookAt);
            o.submitMs = at + f.recordMs;
            o.presentCallMs = o.submitMs;
        }
        o.blitStartMs = push_blit_detail::firstFree(std::max(o.submitMs, queueFree), busy);
        o.blitEndMs = o.blitStartMs + f.blitMs;
        queueFree = o.blitEndMs;
        o.flipReadyMs = std::max(o.presentCallMs, o.blitEndMs);
        lastPresent = o.presentCallMs;
        out.push_back(o);
    }
    return out;
}

// DXGI FLIP_DISCARD, 3 buffers, as the presenter uses it (PresentSink.cpp). blit() writes the current back buffer;
// present() queues it and moves the index; latch() is the display taking the oldest queued buffer on screen.
class PushFlipChain {
public:
    static constexpr unsigned kBuffers = 3;
    // Returns false if the blit would write a buffer that is on screen or queued (the GPU would have to wait for it).
    bool blit(int frame) {
        const bool free = current_ != onScreen_ && std::find(queued_.begin(), queued_.begin() + queuedCount_, current_) == queued_.begin() + queuedCount_;
        content_[current_] = frame;
        return free;
    }
    // Present the current buffer; false if the queue is already full (the engine waits for capacity first).
    bool present(unsigned maximumLatency) {
        if (queuedCount_ >= maximumLatency) return false;
        queued_[queuedCount_++] = current_;
        current_ = (current_ + 1) % kBuffers;
        return true;
    }
    // The display shows the oldest queued frame; returns it (-1 when nothing was queued).
    int latch() {
        if (!queuedCount_) return -1;
        onScreen_ = queued_[0];
        for (unsigned i = 1; i < queuedCount_; ++i) queued_[i - 1] = queued_[i];
        --queuedCount_;
        return content_[onScreen_];
    }
    unsigned queued() const { return queuedCount_; }
    unsigned current() const { return current_; }

private:
    std::array<int, kBuffers> content_{-1, -1, -1};
    std::array<unsigned, kBuffers> queued_{};
    unsigned queuedCount_ = 0, current_ = 0, onScreen_ = kBuffers - 1;
};

}  // namespace veyra::engine
