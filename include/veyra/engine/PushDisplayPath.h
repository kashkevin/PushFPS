// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s10-display" (24 Nov 2026, from the owner's Package 15 logs; out/research/P15_DISPLAY_AND_BASE_PATH.md):
//   1. "To screen": the time from a Present call to the vblank that showed it, from DXGI frame statistics (PresentSink
//      dates each shown present). On his PC about 10 ms in every second of play (a composed window), and NOT in the delay
//      readout, which ends at the return of the real frame's Present. Shown as its own figure (badge tooltip, [fg-health]),
//      never added into the readout: software timers, not photons.
//   2. The refresh-phase test (A3): for each dated present, the refreshes it waited (floor(ms / period)) and how long before
//      the next vblank it was called (ms mod period), in 0.5 ms bins. If presents called close before a vblank wait about
//      one refresh more than the others, they miss the compositor's cut-off and a phase experiment is justified; if not,
//      moving Present calls within the refresh cannot shorten the time to the screen.
//   3. The precise-wait hold (VEYRA_EXP_RP_DEADLINE_HOLD=1, off by default): within kPushDeadlineHold100ns of a frame's
//      deadline, a GPU-ready event no longer ends the owner thread's precise wait (the completion poll it triggers then
//      runs after that Present instead of before it). The capture arrival event still ends the wait, as before.
// Portable, header-only, no allocation per sample. Tests: tests/remoteplay/DisplayPathTests.inc.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::engine {

// Review SHOULD 6: one threshold dilutes the test when the compositor's cut-off is not where the threshold is; the window
// keeps the mean whole refreshes waited per 0.5 ms bin of "time from the Present call to the next vblank" instead:
// bins [0, 0.5), [0.5, 1), ... [3, 3.5) and one for 3.5 ms and more. A cut-off at c ms shows as about one refresh more in
// the bins below c than in those above it.
inline constexpr double kPushPhaseBinMs = 0.5;
inline constexpr std::size_t kPushPhaseBins = 8;
struct PushRefreshPhase {
    std::array<std::uint64_t, kPushPhaseBins> count{};
    std::array<double, kPushPhaseBins> refreshes{};
    void add(double ms, double periodMs) {
        if (!(ms >= 0) || !(periodMs > 0) || !std::isfinite(ms)) return;
        const double whole = std::floor(ms / periodMs);
        const double toNext = ms - whole * periodMs;   // a dated present's vblank lies on the refresh grid
        const std::size_t bin = std::min<std::size_t>(kPushPhaseBins - 1, static_cast<std::size_t>(toNext / kPushPhaseBinMs));
        ++count[bin];
        refreshes[bin] += whole;
    }
    double mean(std::size_t bin) const { return bin < kPushPhaseBins && count[bin] ? refreshes[bin] / double(count[bin]) : -1.0; }
    // "0-0.5:1.000/12,0.5-1:-1/0,...,3.5+:1.000/40": per bin the mean whole refreshes waited and the presents in it.
    std::string describe() const {
        std::string t;
        char buf[64];
        for (std::size_t i = 0; i < kPushPhaseBins; ++i) {
            const double lo = double(i) * kPushPhaseBinMs;
            if (i + 1 < kPushPhaseBins) std::snprintf(buf, sizeof buf, "%s%g-%g:", i ? "," : "", lo, lo + kPushPhaseBinMs);
            else std::snprintf(buf, sizeof buf, ",%g+:", lo);
            t += buf;
            if (count[i]) std::snprintf(buf, sizeof buf, "%.3f/%llu", mean(i), static_cast<unsigned long long>(count[i]));
            else std::snprintf(buf, sizeof buf, "-1/0");
            t += buf;
        }
        return t;
    }
    void clear() { *this = PushRefreshPhase{}; }
};

// The last second's "to screen" figure for the interface (written by the presenter, read by the bridge).
class PushToScreenBoard {
public:
    void publish(double p50Ms, double p95Ms, std::uint64_t samples, double nowMs) {
        p50_.store(samples ? p50Ms : -1.0, std::memory_order_relaxed);
        p95_.store(samples ? p95Ms : -1.0, std::memory_order_relaxed);
        at_.store(nowMs, std::memory_order_release);
    }
    // Review SHOULD 5: the window facts the presenter measured with it (-1 unknown): the window covers its monitor; DXGI's
    // CheckHardwareCompositionSupport has the WINDOWED bit (hardware composition for windows).
    void publishWindow(int coversMonitor, int windowedHardwareComposition) {
        covers_.store(coversMonitor, std::memory_order_relaxed);
        windowedHw_.store(windowedHardwareComposition, std::memory_order_relaxed);
    }
    struct Reading { bool known = false; double p50Ms = -1, p95Ms = -1; int coversMonitor = -1, windowedHw = -1; };
    // Known while the last publication is at most 3 s old and had samples.
    Reading read(double nowMs) const {
        Reading r;
        const double at = at_.load(std::memory_order_acquire);
        if (!(at > 0) || nowMs - at > 3000.0 || nowMs < at - 1000.0) return r;
        r.p50Ms = p50_.load(std::memory_order_relaxed);
        r.p95Ms = p95_.load(std::memory_order_relaxed);
        r.known = r.p50Ms >= 0 && r.p95Ms >= 0;
        r.coversMonitor = covers_.load(std::memory_order_relaxed);
        r.windowedHw = windowedHw_.load(std::memory_order_relaxed);
        return r;
    }
    void clear() { at_.store(0, std::memory_order_release); }

private:
    std::atomic<double> p50_{-1.0}, p95_{-1.0}, at_{0.0};
    std::atomic<int> covers_{-1}, windowedHw_{-1};
};
inline PushToScreenBoard& pushToScreenBoard() {
    static PushToScreenBoard board;
    return board;
}

// The hold rule of VEYRA_EXP_RP_DEADLINE_HOLD: a GPU-ready event that arrives this close to the deadline does not end the
// wait. 2 ms: the late presents in his log are 1-1.8 ms late (intervalP99 12.0-12.9 ms on an 11.1 ms grid).
inline constexpr std::int64_t kPushDeadlineHold100ns = 20000;
inline bool pushDeadlineHoldsReadyWake(bool switchOn, std::int64_t remaining100ns) {
    return switchOn && remaining100ns > 0 && remaining100ns <= kPushDeadlineHold100ns;
}

// 4. The app's fullscreen, OPT-IN (VEYRA_EXP_RP_FULLSCREEN_NO_REGION=1; review SHOULD 1: no evidence yet that a rectangle
//    region equal to the window loses the fast path, and the region would then change on every pointer move): an interface
//    item that cuts nothing out of the video window (the retracted dock, wholly above it) is not a cover there, so the
//    window gets NO region (SetWindowRgn(nullptr), [present-path] region=full windowRgn=0) instead of a rectangle region
//    of its own size. Pixels are unchanged; windowed play is unchanged. The same intersection test as syncVideoCovers
//    (apps/veyra-qml/main.cpp) uses for coversInPicture.
inline bool pushCoverCutsWindow(int left, int top, int width, int height, int windowW, int windowH) {
    return std::min(left + width, windowW) > std::max(left, 0) && std::min(top + height, windowH) > std::max(top, 0);
}
inline bool pushFullscreenDropsOutsideCovers(bool fullscreen, bool switchOn) { return fullscreen && switchOn; }

// 5. VEYRA_EXP_RP_PRESENT_BEFORE_POLL=1 (off by default; the review's "never-later reorder"): when the owner thread comes
//    back to the scheduler with the head frame's precise deadline at most hold100ns() away, it stamps the pending frames'
//    readiness from their fences (the same reads the completion poll starts with), waits out that deadline WITHOUT ending
//    the wait on a GPU-ready or capture-arrival event, presents the head frame, and only then runs the completion poll.
//    Today the poll runs first and the head goes out at max(deadline, now + poll). The hold is sized from the measured
//    poll durations: their P99 + 0.25 ms, at least 0.25 ms, at most 2 ms (2 ms until 64 polls were measured).
//    What is proven (tests/remoteplay/DisplayPathTests.inc, a model of the owner loop, randomized): the head frame is never
//    presented later; a capture sample arriving inside the hold is read at most hold later, so a frame of the next pair is
//    not presented later when its own deadline slack exceeds the hold. Not proven: anything about the real owner loop on
//    Windows (code reading and the model only).
class PushPresentBeforePoll {
public:
    static constexpr std::int64_t kMaxHold100ns = 20000, kMinHold100ns = 2500, kGuard100ns = 2500;
    static constexpr std::size_t kRing = 256, kMinPolls = 64;
    void observePoll(double ms) {
        if (!(ms >= 0) || !std::isfinite(ms)) return;
        ring_[next_] = ms;
        next_ = (next_ + 1) % kRing;
        if (count_ < kRing) ++count_;
        dirty_ = true;
    }
    std::int64_t hold100ns() {
        if (count_ < kMinPolls) return kMaxHold100ns;
        if (dirty_) {
            std::array<double, kRing> v{};
            std::copy(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(count_), v.begin());
            const std::size_t rank = (count_ * 99 + 99) / 100 - 1;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(rank), v.begin() + static_cast<std::ptrdiff_t>(count_));
            hold_ = std::clamp(static_cast<std::int64_t>(v[rank] * 10000.0) + kGuard100ns, kMinHold100ns, kMaxHold100ns);
            dirty_ = false;
        }
        return hold_;
    }
    // Is the head's deadline close enough to hold for it?
    bool applies(bool switchOn, std::int64_t now100ns, std::int64_t due100ns) {
        return switchOn && due100ns > now100ns && due100ns - now100ns <= hold100ns();
    }

private:
    std::array<double, kRing> ring_{};
    std::size_t next_ = 0, count_ = 0;
    std::int64_t hold_ = kMaxHold100ns;
    bool dirty_ = false;
};
inline PushPresentBeforePoll& pushPresentBeforePoll() {
    static PushPresentBeforePoll p;
    return p;
}

// The model the test checks (times in ms on one owner thread). t0: the thread comes back to the scheduler (a ready wake),
// due: the head's deadline, poll: the completion poll's cost, arrival: a capture sample's arrival (< 0: none),
// process: reading and submitting a sample. Today: poll first, then the head if due; a sample that arrived is read after.
struct PushOwnerCase { double t0 = 0, due = 0, poll = 0, arrival = -1, process = 0, hold = 2; };
struct PushOwnerOutcome { double headPresent = 0, arrivalRead = -1; };
inline PushOwnerOutcome pushOwnerToday(const PushOwnerCase& c) {
    PushOwnerOutcome o;
    double now = c.t0 + c.poll;                       // advanceLive: pollCompletions, then advance
    if (c.due <= now) { o.headPresent = now; if (c.arrival >= 0) o.arrivalRead = std::max(c.arrival, now); return o; }
    if (c.arrival >= 0 && c.arrival < c.due) {        // waitLive ends on the arrival: read, process, advanceLive again
        o.arrivalRead = std::max(c.arrival, now);
        now = o.arrivalRead + c.process;
        o.headPresent = c.due <= now ? now : std::max(c.due, now + c.poll);
        return o;
    }
    o.headPresent = c.due;                            // the precise wait reaches the deadline
    if (c.arrival >= 0) o.arrivalRead = std::max(c.arrival, c.due + c.poll);
    return o;
}
inline PushOwnerOutcome pushOwnerPresentBeforePoll(const PushOwnerCase& c) {
    if (!(c.due > c.t0 && c.due - c.t0 <= c.hold)) return pushOwnerToday(c);
    PushOwnerOutcome o;
    o.headPresent = c.due;                            // stamp readiness, hold to the deadline, present the head
    if (c.arrival >= 0) o.arrivalRead = std::max(c.arrival, c.due + c.poll);   // then the poll, then the outer loop reads
    return o;
}

}  // namespace veyra::engine
