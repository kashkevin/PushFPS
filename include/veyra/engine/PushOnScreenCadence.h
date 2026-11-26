// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace" (25 Nov 2026; out/research/SMOOTHNESS_60_VS_90.md): how long each presented frame really stayed on the
// screen, the number the owner's "90 looks like 60" should correlate with. Present CALLS are on the 11.1 ms grid (93-96 %
// within 1 ms in his logs), but on his composed desktop the time from Present to the vblank that shows a frame varies by one
// to two refreshes within every second, so frames are held 1, 2 or 3 refreshes (5.6 / 11.1 / 16.7 ms at 179.95 Hz) instead
// of exactly 2. This window measures that per frame from the DXGI frame statistics the presenter already reads before every
// Present (PresentSink::sampleDisplayLatency):
//   - each statistics reading names the last shown present (PresentCount, the same numbering as GetLastPresentCount) and the
//     vblank it was first shown at (PresentRefreshCount). When two consecutive present ids were both named, the earlier one
//     stayed on screen exactly refresh(id+1) - refresh(id) refreshes (DXGI_FRAME_STATISTICS; flip model, windowed included).
//     Review 1 MUST 1: there is one reading per Present, so a frame held one refresh is often never named, and the frames with
//     their own known hold are biased towards long holds. Ids skipped between two named presents form a GAP whose total refreshes
//     are known; the even share is therefore reported as a range evenPctMin..evenPctMax over every covered frame, and the
//     interface shows evenPctMin only, only when at least 90 % of the frames are covered.
//   - the expected hold k = the Present-call median interval / the refresh period, rounded (2 at 90 on 180 Hz, 3 at 60);
//     even = held exactly k; judderPct = mean |hold - k| / k x 100: relative to the frame's own length
//     (a frame one refresh off = 50 % at 3X, 33 % at 2X), so NOT a fair comparison between multipliers; the fair ones are in
//     milliseconds: showJitterMs, holdSdMs, judderMs (same display jitter = same number at 2X and 3X, test onscreen_unbiased_*).
//   - the refresh clock: under variable refresh / independent flip his logs show SyncRefreshCount standing still for
//     seconds. Readings whose sync count did not move while 1.5 or more refreshes passed are counted; when most do,
//     refreshClock=frozen and holds cannot be dated: the Present-call spacing (spacingEven = within 1 ms of the median) is
//     then what the panel follows, and the line says so.
//   - the Present call's phase after the last vblank (8 bins of one refresh), the capacity waits of generated frames (the
//     waitable object under explicit VSync), and what the opt-in phase lock did.
// Cost: O(1) per present and per reading (fixed arrays, no allocation after construction); the window's sort of at most
// kIntervals values runs once per 10 s on the presenter's (engine owner) thread. Nothing here can block a Present.
// Pure and portable; tests: tests/remoteplay/OnScreenCadenceTests.inc.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {

class PushOnScreenCadence {
public:
    static constexpr std::size_t kRing = 64, kPhaseBins = 8, kMaxHold = 8, kHolds = 4096, kGaps = 2048, kIntervals = 4096, kWaits = 1024;
    static constexpr double kWindowMs = 10000.0, kPauseMs = 250.0;

    // One successful Present: its DXGI present id (0 when it cannot be dated), whether it was a generated frame, the call time
    // (ms, steady clock), the last vblank known before it (ms on the same clock, <= 0 unknown) and the refresh period (ms).
    void presented(std::uint32_t id, bool generated, double callMs, double lastVblankMs, double periodMs) {
        if (!(callMs > 0)) return;
        if (windowStartMs_ <= 0) windowStartMs_ = callMs;
        ++presents_;
        if (generated) ++generated_;
        if (lastCallMs_ > 0 && callMs > lastCallMs_ && callMs - lastCallMs_ <= kPauseMs && intervalCount_ < kIntervals)
            intervals_[intervalCount_++] = callMs - lastCallMs_;
        lastCallMs_ = callMs;
        std::int8_t bin = -1;
        if (periodMs > 0 && lastVblankMs > 0 && std::isfinite(periodMs)) {
            periodMs_ = periodMs;
            double after = std::fmod(callMs - lastVblankMs, periodMs);
            if (after < 0) after += periodMs;
            bin = static_cast<std::int8_t>(std::min<std::size_t>(kPhaseBins - 1, static_cast<std::size_t>(after / periodMs * double(kPhaseBins))));
            ++phase_[static_cast<std::size_t>(bin)];
        }
        if (!id) return;
        if (lastId_ && id < lastId_) {   // a new swap chain: its present ids start again
            ring_ = {};
            lastShown_ = 0;
            named_ = Frame{};
        }
        lastId_ = id;
        ring_[id % kRing] = Frame{id, generated, callMs, -1, bin, false};
    }
    // One frame-statistics reading (taken before a Present): the last shown present, the vblank it was first shown at, the
    // vblank of the reading and the reading's time (ms, steady clock; <= 0 unknown).
    // Review 1 MUST 1: readings come once per Present, so a frame held one refresh is often never named. When the named present
    // jumps from P to Q = P + n (n > 1), the n frames P .. Q-1 held refresh(Q) - refresh(P) refreshes together: recorded as a gap
    // with a known total, so frames are never dropped silently and the even share is reported as a range (evenPctMin..Max).
    void statistics(std::uint32_t presentCount, std::uint32_t presentRefreshCount, std::uint32_t syncRefreshCount, double syncMs) {
        stillNow_ = false;
        if (haveReading_) {
            // The refresh clock: a reading whose sync count stood still although at least 1.5 refreshes passed between the two
            // readings (by their own times when they moved, else by our call times).
            const double passed = syncMs > 0 && lastSyncMs_ > 0 && syncMs != lastSyncMs_ ? syncMs - lastSyncMs_ : lastCallMs_ - lastReadingCallMs_;
            if (periodMs_ > 0 && passed >= 1.5 * periodMs_) {
                ++clockReadings_;
                if (syncRefreshCount == lastSyncRefresh_) { ++clockStill_; stillNow_ = true; }
            }
        }
        haveReading_ = true;
        lastSyncRefresh_ = syncRefreshCount;
        lastSyncMs_ = syncMs;
        lastReadingCallMs_ = lastCallMs_;
        if (presentCount == lastShown_) return;
        lastShown_ = presentCount;
        auto& slot = ring_[presentCount % kRing];
        if (slot.id != presentCount) { named_ = Frame{}; return; }   // not ours (older than the ring) or never dated
        if (slot.refresh < 0) slot.refresh = static_cast<std::int64_t>(presentRefreshCount);
        const Frame previous = named_;
        named_ = slot;
        if (!previous.id || previous.refresh < 0 || presentCount <= previous.id) return;
        const std::uint32_t n = presentCount - previous.id;
        if (n >= kRing || slot.callMs - previous.callMs > kPauseMs * double(n)) return;   // too old, or a pause
        const std::int64_t total = slot.refresh - previous.refresh;
        if (total < 0) return;
        if (n == 1) {
            if (holdCount_ < kHolds)
                holds_[holdCount_++] = Hold{static_cast<std::uint8_t>(std::min<std::int64_t>(total, std::int64_t(kMaxHold))), previous.generated, previous.id,
                                            previous.phaseBin, previous.late, previous.callMs, previous.refresh};
            return;
        }
        if (gapCount_ < kGaps) gaps_[gapCount_++] = Gap{n, static_cast<std::uint32_t>(std::min<std::int64_t>(total, 100000))};
    }
    // True when the last reading found the refresh count standing still although 1.5 refreshes passed (variable refresh /
    // independent flip): the per-reading test the phase lock refuses on (review 1 MUST 2: not the 10 s summary).
    bool clockStillNow() const { return stillNow_; }
    // A generated frame waited this long for display-queue capacity (the waitable object) before its Present.
    void capacityWait(double ms) {
        if (!(ms >= 0) || !std::isfinite(ms)) return;
        ++capacityWaits_;
        if (waitCount_ < kWaits) waits_[waitCount_++] = ms;
    }
    // A paced frame's Present began lateMs after its deadline (called after that Present, so it marks the last presented frame).
    // Late by more than 1 ms is split by what was seen: the frame's GPU work was first SEEN done after the deadline
    // (readySeenLateMs > 0; a late thread wake also looks like this: review 1 SHOULD 5), it waited for display-queue capacity
    // (capacityMs > 0.5), or neither (thread wake / timer). subframe: 0 the real frame, 1.. the generated ones.
    void presentLateness(double lateMs, unsigned subframe, double readySeenLateMs, double capacityMs) {
        if (!(lateMs > 1.0) || !std::isfinite(lateMs)) return;
        ++late_;
        if (readySeenLateMs > 0) ++lateReadySeenLate_;
        else if (capacityMs > 0.5) ++lateCapacity_;
        else ++lateOther_;
        ++lateBySubframe_[std::min<std::size_t>(subframe, lateBySubframe_.size() - 1)];
        if (lastId_) {
            auto& f = ring_[lastId_ % kRing];
            if (f.id == lastId_) f.late = true;
        }
    }
    // The opt-in phase lock: a Present went out advanceMs before its deadline (counted at the Present itself, only when more
    // than 0.5 ms: review 1 MUST 2), or a slot had passed (missed).
    void phaseLockPresented(double advanceMs) {
        if (!(advanceMs > 0.5) || !std::isfinite(advanceMs)) return;
        ++lockMoves_;
        lockAdvanceSum_ += advanceMs;
    }
    void phaseLockMissed() { ++lockMissed_; }
    // One extra statistics reading between Presents (review 2 MUST 1) took this long (microseconds).
    void extraReading(double us) {
        ++extraReadings_;
        if (us > extraReadingMaxUs_) extraReadingMaxUs_ = us;
    }

    static constexpr std::size_t kHoldClasses = 4;   // held 1, 2, 3, 4+ refreshes
    struct Summary {
        double seconds = 0, outFps = 0, periodMs = -1, intervalP50Ms = -1;
        std::uint64_t presents = 0, generated = 0;
        int k = 0;                                      // expected refreshes per frame, 0 unknown
        bool holdsValid = false;                        // false while the refresh clock stands still: no hold can be dated
        std::uint64_t holds = 0, holdsReal = 0, holdsGenerated = 0, replaced = 0;
        std::array<std::uint64_t, kMaxHold + 1> histogram{};   // [h] = frames held h refreshes (h = kMaxHold: that many or more)
        std::uint64_t gaps = 0, gapFrames = 0, gapUneven = 0, gapUnevenFrames = 0, coveredFrames = 0;
        // evenPct: of the frames whose own hold is known (biased towards long holds: short ones are named less often).
        // evenPctMin / Max: of every covered frame (own hold known or inside a gap with a known total): the true share lies between.
        double evenPct = -1, evenPctMin = -1, evenPctMax = -1, shortPct = -1, longPct = -1, judderPct = -1, evenRealPct = -1, evenGeneratedPct = -1;
        double measuredPct = -1, coveredPct = -1;
        // UNBIASED scores (item 1 of the pacing stream; docs/drafts/PACE_DOWNSCALE.md section 1): in milliseconds, so the same display
        // jitter gives the same number at 2X and at 3X. judderPct above is relative to the frame's own length (a one-refresh error is
        // 50 % at 3X and 33 % at 2X): it answers "how large is the error against the step", not "which multiplier is steadier".
        //   showJitterMs: standard deviation of (vblank that first showed the frame - the frame's Present call) after removing the
        //                 linear drift of the period estimate: the display-side delay jitter, independent of the expected hold.
        //   holdSdMs:     standard deviation of the measured hold times (ms).
        //   judderMs:     mean |hold - expected hold| in ms (expected = whole refreshes of the Present spacing), -1 when no even hold exists.
        double showJitterMs = -1, holdSdMs = -1, judderMs = -1;
        std::uint64_t longEvents = 0, shortEvents = 0, longestUnevenRun = 0;
        double beatSeconds = -1;                        // window seconds per long hold, -1 none
        double beatFloorPerS = -1;                      // |refresh - k x outFps|: long (or short) holds per second no schedule can avoid
        double clockStillPct = -1;                      // readings with a still refresh count
        bool clockFrozen = false;
        double spacingEvenPct = -1;                     // Present intervals within 1 ms of their median
        std::array<std::uint64_t, kPhaseBins> phase{};
        std::array<std::array<std::uint64_t, kHoldClasses>, kPhaseBins> holdByPhase{};   // measured holds by their Present's phase bin
        std::uint64_t lateFrameHolds = 0, lateFrameUneven = 0;   // measured holds of frames whose own Present was > 1 ms late
        std::uint64_t capacityWaits = 0;
        double capacityWaitP99Ms = -1;
        std::uint64_t lockMoves = 0, lockMissed = 0;
        double lockMeanAdvanceMs = -1;
        std::uint64_t extraReadings = 0;
        double extraReadingMaxUs = -1;
        std::uint64_t late = 0, lateReadySeenLate = 0, lateCapacity = 0, lateOther = 0;
        std::array<std::uint64_t, 4> lateBySubframe{};   // real, first generated, second, third or later
    };
    bool due(double nowMs) const { return windowStartMs_ > 0 && nowMs - windowStartMs_ >= kWindowMs; }
    // Summarises the window that ends now and starts the next one. The ring of recent presents stays (a hold may span windows).
    Summary take(double nowMs) {
        Summary s;
        s.seconds = windowStartMs_ > 0 && nowMs > windowStartMs_ ? (nowMs - windowStartMs_) / 1000.0 : 0;
        s.presents = presents_;
        s.generated = generated_;
        s.outFps = s.seconds > 0 ? double(presents_) / s.seconds : 0;
        s.periodMs = periodMs_ > 0 ? periodMs_ : -1;
        if (intervalCount_) {
            std::vector<double>& v = scratch_;
            v.assign(intervals_.begin(), intervals_.begin() + static_cast<std::ptrdiff_t>(intervalCount_));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
            s.intervalP50Ms = v[v.size() / 2];
            std::size_t withinMs = 0;
            for (std::size_t i = 0; i < intervalCount_; ++i)
                if (std::abs(intervals_[i] - s.intervalP50Ms) <= 1.0) ++withinMs;
            s.spacingEvenPct = 100.0 * double(withinMs) / double(intervalCount_);
            if (periodMs_ > 0) s.k = expectedHold(s.intervalP50Ms, periodMs_);
        }
        if (s.k && periodMs_ > 0 && s.outFps > 0) s.beatFloorPerS = std::abs(1000.0 / periodMs_ - double(s.k) * s.outFps);
        s.clockStillPct = clockReadings_ ? 100.0 * double(clockStill_) / double(clockReadings_) : -1.0;
        s.clockFrozen = clockReadings_ >= 20 && clockStill_ * 2 > clockReadings_;
        s.holdsValid = !s.clockFrozen;   // review 1 SHOULD 2: a still clock dates nothing
        s.phase = phase_;
        if (s.holdsValid) summariseHolds(s);
        s.capacityWaits = capacityWaits_;
        if (waitCount_) {
            std::vector<double>& v = scratch_;
            v.assign(waits_.begin(), waits_.begin() + static_cast<std::ptrdiff_t>(waitCount_));
            const std::size_t rank = std::min(v.size() - 1, (v.size() * 99 + 99) / 100 - 1);
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(rank), v.end());
            s.capacityWaitP99Ms = v[rank];
        }
        s.lockMoves = lockMoves_;
        s.lockMissed = lockMissed_;
        s.lockMeanAdvanceMs = lockMoves_ ? lockAdvanceSum_ / double(lockMoves_) : -1.0;
        s.extraReadings = extraReadings_;
        s.extraReadingMaxUs = extraReadings_ ? extraReadingMaxUs_ : -1.0;
        s.late = late_;
        s.lateReadySeenLate = lateReadySeenLate_;
        s.lateCapacity = lateCapacity_;
        s.lateOther = lateOther_;
        s.lateBySubframe = lateBySubframe_;
        reset(nowMs);
        return s;
    }
    // Every frame and reading forgotten (a new swap chain: present ids restart).
    void clear() {
        ring_ = {};
        lastShown_ = lastId_ = 0;
        named_ = Frame{};
        haveReading_ = stillNow_ = false;
        lastCallMs_ = 0;
        reset(0);
    }
    // 2 at 90 FPS on 179.95 Hz, 3 at 60; 0 when the interval is not within 10 % of a whole number of refreshes (e.g. 120 FPS
    // on 180 Hz: no even hold exists).
    static int expectedHold(double intervalMs, double periodMs) {
        if (!(intervalMs > 0) || !(periodMs > 0)) return 0;
        const double ratio = intervalMs / periodMs, k = std::round(ratio);
        if (k < 1 || std::abs(ratio - k) > 0.1 * k) return 0;
        return int(k);
    }

private:
    struct Frame { std::uint32_t id = 0; bool generated = false; double callMs = 0; std::int64_t refresh = -1; std::int8_t phaseBin = -1; bool late = false; };
    struct Hold { std::uint8_t refreshes = 0; bool generated = false; std::uint32_t id = 0; std::int8_t phaseBin = -1; bool late = false; double callMs = 0; std::int64_t refresh = -1; };
    struct Gap { std::uint32_t frames = 0, refreshes = 0; };
    void summariseHolds(Summary& s) {
        std::uint64_t even = 0, shorter = 0, longer = 0, evenReal = 0, evenGen = 0, run = 0, deviation = 0;
        double holdMsSum = 0, holdMsSq = 0, dn = 0, dx = 0, dy = 0, dxx = 0, dxy = 0, dyy = 0;
        std::uint32_t lastId = 0;
        for (std::size_t i = 0; i < holdCount_; ++i) {
            const auto& h = holds_[i];
            ++s.histogram[h.refreshes];
            holdMsSum += double(h.refreshes) * periodMs_;
            holdMsSq += double(h.refreshes) * periodMs_ * double(h.refreshes) * periodMs_;
            if (h.refresh >= 0 && h.callMs > 0) {
                const double x = h.callMs, y = double(h.refresh) * periodMs_ - h.callMs;
                ++dn; dx += x; dy += y; dxx += x * x; dxy += x * y; dyy += y * y;
            }
            if (h.refreshes == 0) { ++s.replaced; continue; }
            ++s.holds;
            ++(h.generated ? s.holdsGenerated : s.holdsReal);
            if (h.phaseBin >= 0) ++s.holdByPhase[static_cast<std::size_t>(h.phaseBin)][std::min<std::size_t>(h.refreshes, kHoldClasses) - 1];
            if (!s.k) continue;
            const int d = int(h.refreshes) - s.k;
            if (h.late) { ++s.lateFrameHolds; if (d) ++s.lateFrameUneven; }
            deviation += std::uint64_t(d < 0 ? -d : d);
            if (lastId && h.id != lastId + 1) run = 0;   // an unmeasured frame between: a new run
            lastId = h.id;
            if (d == 0) {
                ++even;
                ++(h.generated ? evenGen : evenReal);
                run = 0;
                continue;
            }
            ++(d < 0 ? shorter : longer);
            s.longestUnevenRun = std::max(s.longestUnevenRun, ++run);
        }
        if (holdCount_ > 1 && periodMs_ > 0) {
            const double n = double(holdCount_), mean = holdMsSum / n;
            s.holdSdMs = std::sqrt(std::max(0.0, holdMsSq / n - mean * mean));
            if (s.k && s.holds) s.judderMs = double(deviation) * periodMs_ / double(s.holds);
        }
        if (dn > 2) {   // residual standard deviation of y = (first shown vblank time - Present call) against the call time: a line removes the period drift
            const double vx = dxx - dx * dx / dn, vy = dyy - dy * dy / dn, cxy = dxy - dx * dy / dn;
            s.showJitterMs = std::sqrt(std::max(0.0, vy - (vx > 1e-9 ? cxy * cxy / vx : 0.0)) / dn);
        }
        std::uint64_t consistentGapFrames = 0;   // gap frames that MAY be even
        for (std::size_t i = 0; i < gapCount_; ++i) {
            const auto& g = gaps_[i];
            ++s.gaps;
            s.gapFrames += g.frames;
            if (!s.k) continue;
            // A gap whose total is n x k may be all even; one whose total differs holds at least one uneven frame, at most n - 1 even.
            if (g.refreshes == g.frames * std::uint32_t(s.k)) consistentGapFrames += g.frames;
            else { ++s.gapUneven; s.gapUnevenFrames += g.frames; consistentGapFrames += g.frames - 1; }
        }
        s.coveredFrames = s.holds + s.replaced + s.gapFrames;
        if (s.holds && s.k) {
            s.evenPct = 100.0 * double(even) / double(s.holds);
            s.shortPct = 100.0 * double(shorter) / double(s.holds);
            s.longPct = 100.0 * double(longer) / double(s.holds);
            s.judderPct = 100.0 * double(deviation) / double(s.holds) / double(s.k);
            s.longEvents = longer;
            s.shortEvents = shorter;
            s.beatSeconds = longer ? s.seconds / double(longer) : -1.0;
            if (s.holdsReal) s.evenRealPct = 100.0 * double(evenReal) / double(s.holdsReal);
            if (s.holdsGenerated) s.evenGeneratedPct = 100.0 * double(evenGen) / double(s.holdsGenerated);
        }
        if (s.coveredFrames && s.k) {
            s.evenPctMin = 100.0 * double(even) / double(s.coveredFrames);
            s.evenPctMax = 100.0 * double(even + consistentGapFrames) / double(s.coveredFrames);
        }
        s.measuredPct = presents_ ? 100.0 * double(s.holds) / double(presents_) : -1.0;
        s.coveredPct = presents_ ? std::min(100.0, 100.0 * double(s.coveredFrames) / double(presents_)) : -1.0;
    }
    void reset(double nowMs) {
        windowStartMs_ = nowMs;
        presents_ = generated_ = 0;
        intervalCount_ = holdCount_ = gapCount_ = waitCount_ = 0;
        phase_ = {};
        clockReadings_ = clockStill_ = 0;
        capacityWaits_ = lockMoves_ = lockMissed_ = extraReadings_ = 0;
        lockAdvanceSum_ = extraReadingMaxUs_ = 0;
        late_ = lateReadySeenLate_ = lateCapacity_ = lateOther_ = 0;
        lateBySubframe_ = {};
    }
    std::array<Frame, kRing> ring_{};
    Frame named_{};
    std::array<double, kIntervals> intervals_{};
    std::array<Hold, kHolds> holds_{};
    std::array<Gap, kGaps> gaps_{};
    std::array<double, kWaits> waits_{};
    std::array<std::uint64_t, kPhaseBins> phase_{};
    std::vector<double> scratch_ = std::vector<double>(kIntervals);
    std::size_t intervalCount_ = 0, holdCount_ = 0, gapCount_ = 0, waitCount_ = 0;
    std::uint64_t presents_ = 0, generated_ = 0, clockReadings_ = 0, clockStill_ = 0, capacityWaits_ = 0, lockMoves_ = 0, lockMissed_ = 0;
    double lockAdvanceSum_ = 0, extraReadingMaxUs_ = 0;
    std::uint64_t extraReadings_ = 0;
    std::uint64_t late_ = 0, lateReadySeenLate_ = 0, lateCapacity_ = 0, lateOther_ = 0;
    std::array<std::uint64_t, 4> lateBySubframe_{};
    double windowStartMs_ = 0, lastCallMs_ = 0, lastReadingCallMs_ = 0, lastSyncMs_ = 0, periodMs_ = 0;
    std::uint32_t lastShown_ = 0, lastSyncRefresh_ = 0, lastId_ = 0;
    bool haveReading_ = false, stillNow_ = false;
};

// What the window cannot know by itself: the display path as the driver reports it, NVIDIA low latency, the phase lock.
struct PushCadenceContext {
    int directPath = -1;   // NVIDIA sleep status: fullscreen independent flip (1/0, -1 not read)
    int driverVrr = -1;    // NVIDIA sleep status: fullscreen VRR (1/0, -1 not read)
    int foreground = -1;   // the video's top-level window is the foreground window (1/0, -1 unknown)
    int reflex = 0;        // NVIDIA low latency active (Sleep called)
    int lock = 0;          // VEYRA_EXP_RP_PHASE_LOCK in force
    int syncInterval = -1;
};
// Review 1 MUST 3 / SHOULD 1: "direct" only when the driver reports independent flip; a still refresh clock alone is "frozen"
// (variable refresh or independent flip LIKELY, not reported); "composed": the clock runs and no direct flip is reported.
inline const char* pushCadencePath(const PushOnScreenCadence::Summary& s, const PushCadenceContext& c) {
    if (c.directPath == 1) return "direct";
    if (s.clockFrozen) return "frozen";
    if (s.clockStillPct >= 0) return "composed";
    return "unknown";
}
// The board's path code: 0 composed, 1 direct (driver), 2 frozen clock, -1 unknown.
inline int pushCadencePathCode(const PushOnScreenCadence::Summary& s, const PushCadenceContext& c) {
    const std::string_view p = pushCadencePath(s, c);
    return p == "composed" ? 0 : p == "direct" ? 1 : p == "frozen" ? 2 : -1;
}

inline std::string formatPushCadence(const PushOnScreenCadence::Summary& s, const PushCadenceContext& c) {
    const auto num = [](double v, int digits) {
        char b[32];
        if (v < 0 || !std::isfinite(v)) return std::string("-1");
        std::snprintf(b, sizeof b, "%.*f", digits, v);
        return std::string(b);
    };
    std::string hist, phase, byPhase;
    if (!s.holdsValid) hist = "n/a";
    else
        for (std::size_t h = 0; h <= PushOnScreenCadence::kMaxHold; ++h) {
            if (!s.histogram[h]) continue;
            hist += (hist.empty() ? "" : ",") + std::to_string(h) + (h == PushOnScreenCadence::kMaxHold ? "+" : "") + ":" + std::to_string(s.histogram[h]);
        }
    for (std::size_t b = 0; b < PushOnScreenCadence::kPhaseBins; ++b) {
        phase += (b ? "," : "") + std::to_string(s.phase[b]);
        if (!s.holdsValid) continue;
        byPhase += b ? "," : "";
        for (std::size_t h = 0; h < PushOnScreenCadence::kHoldClasses; ++h) byPhase += (h ? "/" : "") + std::to_string(s.holdByPhase[b][h]);
    }
    char line[2400];
    std::snprintf(line, sizeof line,
        "windowS=%.1f path=%s refreshClock=%s clockStillPct=%s outFps=%s periodMs=%s intervalP50Ms=%s expectedRefreshes=%d beatFloorPerS=%s "
        "holds=%llu measuredPct=%s gaps=%llu gapFrames=%llu gapUneven=%llu coveredPct=%s evenPctMin=%s evenPctMax=%s evenPct=%s shortPct=%s longPct=%s "
        "judderPct=%s evenRealPct=%s evenGeneratedPct=%s longestUnevenRun=%llu longHolds=%llu shortHolds=%llu beatS=%s replaced=%llu "
        "holdHistogram=%s holdsByPhase=%s lateFrameHolds=%llu lateFrameUneven=%llu spacingEvenPct=%s phaseAfterVblank=%s capacityWaits=%llu "
        "capacityWaitP99Ms=%s lock=%d earlyPresents=%llu lockMissed=%llu earlyMeanAdvanceMs=%s extraReadings=%llu extraReadingMaxUs=%s lateOver1ms=%llu lateReadySeenLate=%llu lateCapacity=%llu "
        "lateOther=%llu lateBySubframe=real:%llu,g1:%llu,g2:%llu,g3+:%llu directFlip=%d driverVrr=%d foreground=%d reflex=%d syncInterval=%d "
        "showJitterMs=%s holdSdMs=%s judderMs=%s",
        s.seconds, pushCadencePath(s, c), s.clockFrozen ? "frozen" : s.clockStillPct >= 0 ? "fixed" : "unknown", num(s.clockStillPct, 1).c_str(),
        num(s.outFps, 2).c_str(), num(s.periodMs, 3).c_str(), num(s.intervalP50Ms, 3).c_str(), s.k, num(s.beatFloorPerS, 2).c_str(),
        static_cast<unsigned long long>(s.holds), num(s.measuredPct, 1).c_str(), static_cast<unsigned long long>(s.gaps),
        static_cast<unsigned long long>(s.gapFrames), static_cast<unsigned long long>(s.gapUneven), num(s.coveredPct, 1).c_str(),
        num(s.evenPctMin, 2).c_str(), num(s.evenPctMax, 2).c_str(), num(s.evenPct, 2).c_str(), num(s.shortPct, 2).c_str(), num(s.longPct, 2).c_str(),
        num(s.judderPct, 2).c_str(), num(s.evenRealPct, 2).c_str(), num(s.evenGeneratedPct, 2).c_str(), static_cast<unsigned long long>(s.longestUnevenRun),
        static_cast<unsigned long long>(s.longEvents), static_cast<unsigned long long>(s.shortEvents), num(s.beatSeconds, 2).c_str(),
        static_cast<unsigned long long>(s.replaced), hist.empty() ? "none" : hist.c_str(), byPhase.empty() ? "n/a" : byPhase.c_str(),
        static_cast<unsigned long long>(s.lateFrameHolds), static_cast<unsigned long long>(s.lateFrameUneven), num(s.spacingEvenPct, 2).c_str(),
        phase.c_str(), static_cast<unsigned long long>(s.capacityWaits), num(s.capacityWaitP99Ms, 3).c_str(), c.lock,
        static_cast<unsigned long long>(s.lockMoves), static_cast<unsigned long long>(s.lockMissed), num(s.lockMeanAdvanceMs, 3).c_str(),
        static_cast<unsigned long long>(s.extraReadings), num(s.extraReadingMaxUs, 1).c_str(), static_cast<unsigned long long>(s.late), static_cast<unsigned long long>(s.lateReadySeenLate), static_cast<unsigned long long>(s.lateCapacity),
        static_cast<unsigned long long>(s.lateOther), static_cast<unsigned long long>(s.lateBySubframe[0]), static_cast<unsigned long long>(s.lateBySubframe[1]),
        static_cast<unsigned long long>(s.lateBySubframe[2]), static_cast<unsigned long long>(s.lateBySubframe[3]), c.directPath, c.driverVrr, c.foreground,
        c.reflex, c.syncInterval, num(s.showJitterMs, 3).c_str(), num(s.holdSdMs, 3).c_str(), num(s.judderMs, 3).c_str());
    return std::string(line) +
        " (how long each frame stayed on the screen, from DXGI frame statistics, not photons: PresentRefreshCount of two shown presents named by "
        "consecutive readings; one reading per Present, so short holds are named less often: evenPct (frames with their own hold) is biased "
        "upwards; gaps = runs of n frames between two named presents with their total refreshes known (gapUneven: total != n x expected, at least "
        "one uneven frame inside); the true even share lies in evenPctMin..evenPctMax over coveredPct of the presents; expectedRefreshes = "
        "Present-call median interval / refresh period; judderPct = mean |hold - expected| / expected x 100 over frames with their own hold; "
        "beatFloorPerS = |refresh - expected x outFps|: uneven holds per second the source rate forces on a fixed refresh; holdsByPhase = per "
        "eighth of a refresh after the vblank of the frame's own Present, holds of 1/2/3/4+ refreshes; lateFrame* = holds of frames whose own "
        "Present was over 1 ms late; path direct = the driver reports independent flip; frozen = the vblank count stood still (variable refresh "
        "or independent flip, not reported): no hold can be dated (holdHistogram n/a), spacingEvenPct = Present intervals within 1 ms of their "
        "median, a measure before the screen; capacityWaits = generated frames that waited for display-queue capacity; late* = paced Presents "
        "over 1 ms after their deadline: GPU work first seen done after the deadline (a late thread wake looks the same), a capacity wait over "
        "0.5 ms, or neither; showJitterMs / holdSdMs / judderMs = the same evenness in milliseconds, the figures to compare 2X with 3X: the display-side delay jitter, the spread of hold times and the mean hold error, none of them relative to the frame length)";
}

// The last window's result for the interface (written by the presenter, read by the bridge once a second).
class PushOnScreenBoard {
public:
    // Review 2 MUST 1: both bounds of the even share. The composed-path line is shown only when the range is at most 10 points
    // wide, as a range; the reminder uses the UPPER bound (certainly uneven). directReported: the driver reported independent
    // flip AND fullscreen VRR while PushFPS was in the foreground: remembered for the process.
    void publish(int path, int expectedRefreshes, double evenPctMin, double evenPctMax, double spacingEvenPct, bool directReported, double nowMs) {
        path_.store(path, std::memory_order_relaxed);
        k_.store(expectedRefreshes, std::memory_order_relaxed);
        min_.store(evenPctMin, std::memory_order_relaxed);
        max_.store(evenPctMax, std::memory_order_relaxed);
        spacing_.store(spacingEvenPct, std::memory_order_relaxed);
        if (directReported) directSeen_.store(true, std::memory_order_relaxed);
        at_.store(nowMs, std::memory_order_release);
    }
    static constexpr double kNarrowPoints = 10.0;
    struct Reading {
        bool known = false;            // something can be shown
        bool narrow = false;           // composed path: max - min <= kNarrowPoints
        int path = -1, expectedRefreshes = 0;
        double evenPctMin = -1, evenPctMax = -1, spacingEvenPct = -1;
        bool directSeen = false;
    };
    // Known while the last window is at most 25 s old (one window is 10 s).
    Reading read(double nowMs) const {
        Reading r;
        r.directSeen = directSeen_.load(std::memory_order_relaxed);
        const double at = at_.load(std::memory_order_acquire);
        if (!(at > 0) || nowMs - at > 25000.0 || nowMs < at - 1000.0) return r;
        r.path = path_.load(std::memory_order_relaxed);
        r.expectedRefreshes = k_.load(std::memory_order_relaxed);
        r.evenPctMin = min_.load(std::memory_order_relaxed);
        r.evenPctMax = max_.load(std::memory_order_relaxed);
        r.spacingEvenPct = spacing_.load(std::memory_order_relaxed);
        r.narrow = r.evenPctMin >= 0 && r.evenPctMax >= r.evenPctMin && r.evenPctMax - r.evenPctMin <= kNarrowPoints;
        r.known = (r.path == 1 || r.path == 2) ? r.spacingEvenPct >= 0 : (r.path == 0 && r.evenPctMax >= 0 && r.expectedRefreshes > 0);
        return r;
    }
    void clear() { at_.store(0, std::memory_order_release); }

private:
    std::atomic<int> path_{-1}, k_{0};
    std::atomic<double> min_{-1.0}, max_{-1.0}, spacing_{-1.0}, at_{0.0};
    std::atomic<bool> directSeen_{false};
};
inline PushOnScreenBoard& pushOnScreenBoard() {
    static PushOnScreenBoard board;
    return board;
}

}  // namespace veyra::engine
