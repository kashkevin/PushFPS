// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "present" R2 (24 Nov 2026): the always-on [fg-health] line, one per 10 s on a capture card. It gathers what the
// engine already measures into the numbers the owner asked for ("Measure average latency, median, P95, P99, frame pacing,
// dropped frames, duplicate frames ... and GPU processing time. Do not optimize averages while ignoring latency spikes"):
//   real   the real frame's delay as the badge measures it (capture callback -> return of the real frame's Present):
//          mean, P50, P95, P99, max (software timing, not photons; nothing of the console, card or display)
//   pace   the spacing of Present calls (every shown frame): P50, P95, P99, max, and the 11.1 ms grid fields of
//          [capture-timing] summed over the window (on the grid within 1 ms, shown of expected, longest, longest off-run)
//   drops  by cause over the window (deadline, capacity, output cap, suppressed, capture mailbox) and duplicates removed by
//          content dedup (repeated console pictures)
//   gpu    per stage P50 / P95 / P99 (colour, the first and second generated frame, the whole FG batch, the present blit)
//   reflex NVIDIA low latency's per-frame Sleep (R11): sleeps, their mean / max, and how far each ran PAST the arrival of
//          the sample read after it (mean, P95, P99, max, count over 0.5 ms): up to that much is added to the real frame
//          (grid slack can absorb part of it; reasoning, not a measurement); unknown when the capture mailbox replaced a
//          sample meanwhile (reflexUnknown)
// Cost: O(1) per sample (fixed rings, no allocation after construction); the sorting happens once per 10 s on at most a few
// thousand values. Everything runs on the engine's owner thread (the presenter runs there too); one object per process.
// Nothing private: numbers only.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace veyra::engine {

// A fixed ring of the last N samples of one window; summary() sorts a copy.
template <std::size_t N>
class PushSampleRing {
public:
    void add(double v) {
        if (!std::isfinite(v) || v < 0) return;
        values_[next_] = v;
        next_ = (next_ + 1) % N;
        if (count_ < N) ++count_;
        sum_ += v;
        ++total_;
    }
    void clear() { next_ = count_ = 0; sum_ = 0; total_ = 0; }
    std::size_t count() const { return count_; }
    std::uint64_t total() const { return total_; }   // samples added in the window, also those the ring dropped
    struct Summary {
        std::size_t n = 0;
        double mean = -1, p50 = -1, p95 = -1, p99 = -1, max = -1;
    };
    // Percentiles by nearest rank over the kept samples; the mean over every sample added.
    Summary summary(std::vector<double>& scratch) const {
        Summary s;
        s.n = count_;
        if (!count_) return s;
        scratch.assign(values_.begin(), values_.begin() + static_cast<std::ptrdiff_t>(count_));
        std::sort(scratch.begin(), scratch.end());
        const auto at = [&](double q) {
            const std::size_t rank = static_cast<std::size_t>(std::ceil(q * double(scratch.size())));
            return scratch[std::clamp<std::size_t>(rank, 1, scratch.size()) - 1];
        };
        s.mean = total_ ? sum_ / double(total_) : -1;
        s.p50 = at(0.50);
        s.p95 = at(0.95);
        s.p99 = at(0.99);
        s.max = scratch.back();
        return s;
    }

private:
    std::array<double, N> values_{};
    std::size_t next_ = 0, count_ = 0;
    double sum_ = 0;
    std::uint64_t total_ = 0;
};

// The GPU stages [fg-health] reports, by their index in diagnostics::GpuStage (FrameMetrics.h: Color 0, Fg1 5, Fg2 6,
// FgBatch 12, Blit 13; VideoPresenter.cpp checks these numbers with static_assert where both headers are visible).
struct PushHealthStage {
    unsigned index;
    const char* name;
};
inline constexpr std::array<PushHealthStage, 5> kPushHealthStages{{{0, "color"}, {5, "fg1"}, {6, "fg2"}, {12, "fgBatch"}, {13, "blit"}}};

// Session totals the engine keeps (cumulative); the window reports their growth. A total that falls (a new session) counts
// from zero again.
struct PushHealthTotals {
    std::uint64_t dropDeadline = 0, dropCapacity = 0, dropCap = 0, suppressed = 0, mailboxDropped = 0, duplicates = 0;
};
// One second of the grid window of [capture-timing] (PushGridWindow::Stats), summed over 10 s.
struct PushHealthGrid {
    std::uint64_t intervals = 0, within1ms = 0, presents = 0;
    double expected = 0, longestMs = 0;
    unsigned offRunMax = 0;
};

class PushFgHealth {
public:
    static constexpr std::int64_t kWindow100ns = 100000000;   // 10 s
    // A real frame's delay (ms), as the badge measures it.
    void real(double ms) { real_.add(ms); }
    // Every Present call (any frame), host time in 100 ns; spacing over 250 ms is a break (pause), not an interval.
    void presented(std::int64_t call100ns, bool generated) {
        if (generated) ++generated_; else ++realPresents_;
        if (lastPresent100ns_ > 0 && call100ns > lastPresent100ns_ && call100ns - lastPresent100ns_ <= 2500000)
            interval_.add(double(call100ns - lastPresent100ns_) / 10000.0);
        lastPresent100ns_ = call100ns;
    }
    void gpu(unsigned stageIndex, double ms) {
        for (std::size_t i = 0; i < kPushHealthStages.size(); ++i)
            if (kPushHealthStages[i].index == stageIndex) { gpu_[i].add(ms); return; }
    }
    // One NVIDIA low-latency Sleep (ms) and how far it ran past the arrival of the sample then read (ms, 0 when it ended
    // before; -1 when no sample was read after it).
    void reflexSleep(double sleepMs, double overrunMs) {
        ++sleeps_;
        sleepSum_ += std::max(0.0, sleepMs);
        sleepMax_ = std::max(sleepMax_, sleepMs);
        if (overrunMs >= 0) {
            overrun_.add(overrunMs);
            if (overrunMs > 0.5) ++overruns_;
        } else ++unknown_;
    }
    // Low-latency state for the line: 0 off, 1 on (Sleep called), 2 ended by a guard this session.
    // reflexSleep's overrunMs: -1 when nothing could be compared (counted as reflexUnknown).
    void reflexState(int state) { reflexState_ = state; }

    // Once a second (the [capture-timing] block). Returns the line when 10 s have passed since the window began, else "".
    std::string second(std::int64_t now100ns, const PushHealthGrid& grid, const PushHealthTotals& totals) {
        grid_.intervals += grid.intervals;
        grid_.within1ms += grid.within1ms;
        grid_.presents += grid.presents;
        grid_.expected += grid.expected;
        grid_.longestMs = std::max(grid_.longestMs, grid.longestMs);
        grid_.offRunMax = std::max(grid_.offRunMax, grid.offRunMax);
        if (!windowStart100ns_) {
            windowStart100ns_ = now100ns;
            base_ = totals;
            return {};
        }
        if (now100ns - windowStart100ns_ < kWindow100ns) return {};
        const double seconds = double(now100ns - windowStart100ns_) / 1e7;
        const auto grow = [](std::uint64_t now, std::uint64_t then) { return now >= then ? now - then : now; };
        PushHealthTotals d;
        d.dropDeadline = grow(totals.dropDeadline, base_.dropDeadline);
        d.dropCapacity = grow(totals.dropCapacity, base_.dropCapacity);
        d.dropCap = grow(totals.dropCap, base_.dropCap);
        d.suppressed = grow(totals.suppressed, base_.suppressed);
        d.mailboxDropped = grow(totals.mailboxDropped, base_.mailboxDropped);
        d.duplicates = grow(totals.duplicates, base_.duplicates);
        std::string line = format(seconds, d);
        reset(now100ns, totals);
        return line;
    }

private:
    static std::string num(double v, int digits = 2) {
        if (v < 0) return "-1";
        return std::format("{:.{}f}", v, digits);
    }
    std::string format(double seconds, const PushHealthTotals& d) {
        std::string t;
        const auto r = real_.summary(scratch_);
        t += std::format("windowS={:.1f} realFrames={} realMeanMs={} realP50Ms={} realP95Ms={} realP99Ms={} realMaxMs={}", seconds, real_.total(),
                         num(r.mean), num(r.p50), num(r.p95), num(r.p99), num(r.max));
        const auto p = interval_.summary(scratch_);
        t += std::format(" presents={} generatedPresents={} intervalP50Ms={} intervalP95Ms={} intervalP99Ms={} intervalMaxMs={}", realPresents_ + generated_,
                         generated_, num(p.p50), num(p.p95), num(p.p99), num(p.max));
        const double onGrid = grid_.intervals ? 100.0 * double(grid_.within1ms) / double(grid_.intervals) : -1.0;
        const double shown = grid_.expected > 0 ? 100.0 * double(grid_.presents) / grid_.expected : -1.0;
        t += std::format(" onGrid1msPct={} shownPct={} gridLongestMs={} gridOffRunMax={}", num(onGrid), num(shown), num(grid_.intervals ? grid_.longestMs : -1.0),
                         grid_.offRunMax);
        t += std::format(" dropDeadline={} dropCapacity={} dropCap={} suppressed={} mailboxDropped={} duplicates={}", d.dropDeadline, d.dropCapacity, d.dropCap,
                         d.suppressed, d.mailboxDropped, d.duplicates);
        for (std::size_t i = 0; i < kPushHealthStages.size(); ++i) {
            const auto g = gpu_[i].summary(scratch_);
            const std::string n = kPushHealthStages[i].name;
            t += std::format(" gpu_{}P50Ms={} gpu_{}P95Ms={} gpu_{}P99Ms={}", n, num(g.p50, 3), n, num(g.p95, 3), n, num(g.p99, 3));
        }
        const auto o = overrun_.summary(scratch_);
        t += std::format(" reflex={} reflexSleeps={} reflexSleepMeanMs={} reflexSleepMaxMs={} reflexOverrunMeanMs={} reflexOverrunP95Ms={} reflexOverrunP99Ms={} "
                         "reflexOverrunMaxMs={} reflexOverruns={} reflexUnknown={}",
                         reflexState_, sleeps_, num(sleeps_ ? sleepSum_ / double(sleeps_) : -1.0, 3), num(sleeps_ ? sleepMax_ : -1.0, 3), num(o.mean, 3),
                         num(o.p95, 3), num(o.p99, 3), num(o.max, 3), overruns_, unknown_);
        t += " (software timing, not photons: the real frame's delay is capture callback -> return of its Present; interval = spacing of Present calls; "
             "reflexOverrun = how far NVIDIA low latency's Sleep ran past the arrival of the sample read after it: up to that much is added to that frame)";
        return t;
    }
    void reset(std::int64_t now100ns, const PushHealthTotals& totals) {
        real_.clear();
        interval_.clear();
        overrun_.clear();
        for (auto& g : gpu_) g.clear();
        grid_ = {};
        generated_ = realPresents_ = sleeps_ = overruns_ = unknown_ = 0;
        sleepSum_ = 0;
        sleepMax_ = 0;
        windowStart100ns_ = now100ns;
        base_ = totals;
    }

    PushSampleRing<1024> real_;
    PushSampleRing<4096> interval_;
    PushSampleRing<1024> overrun_;
    std::array<PushSampleRing<1024>, kPushHealthStages.size()> gpu_{};
    std::vector<double> scratch_ = std::vector<double>(4096);
    PushHealthGrid grid_{};
    PushHealthTotals base_{};
    std::int64_t windowStart100ns_ = 0, lastPresent100ns_ = 0;
    std::uint64_t generated_ = 0, realPresents_ = 0, sleeps_ = 0, overruns_ = 0, unknown_ = 0;
    double sleepSum_ = 0, sleepMax_ = 0;
    int reflexState_ = 0;
};

// The process's one window (engine owner thread only).
inline PushFgHealth& pushFgHealth() {
    static PushFgHealth health;
    return health;
}

// ---- R11: NVIDIA low latency's per-frame Sleep on a source-paced capture card --------------------------------------------
// How far one Sleep ran past the arrival of the sample read after it: the part of the Sleep that ended after the sample was
// already there (it can be no longer than the Sleep itself). All times host 100 ns; -1 when there is nothing to compare.
inline double pushReflexOverrunMs(std::int64_t sleepStart100ns, std::int64_t sleepEnd100ns, std::int64_t arrival100ns) {
    if (sleepStart100ns <= 0 || sleepEnd100ns < sleepStart100ns || arrival100ns <= 0) return -1.0;
    const std::int64_t past = std::min(sleepEnd100ns - arrival100ns, sleepEnd100ns - sleepStart100ns);
    return past > 0 ? double(past) / 10000.0 : 0.0;
}
// The bound (see VideoPresenter::sourceArrived for why it is a bound and not a skip), with warm-up and hysteresis (review
// S7): on a capture card with frame generation, low latency ends only when the Sleep KEEPS running past the sample's
// arrival: in kBadBlocks consecutive blocks of kBlock measured sleeps, each block with at least kBadInBlock sleeps more than
// kOverrunMs past the arrival (at 30-60 accepted frames a second: sustained over about 2-4 s). Ignored: the first kWarmupMs
// after the first sample, after restartWarmup() (a presentation reconfiguration) and after a gap of more than kGapMs between
// samples (a source change, a pause, a stall); and every Sleep whose overrun is unknown (-1). Sleeps that end before the
// sample arrives overlap the engine's idle wait: they never count. Once ended it stays ended for the presenter's life (one
// source session; EngineController makes a new presenter for each).
class PushReflexOverrunGuard {
public:
    static constexpr unsigned kBlock = 60, kBadInBlock = 4, kBadBlocks = 2;
    static constexpr double kOverrunMs = 0.5, kWarmupMs = 3000.0, kGapMs = 250.0;
    void restartWarmup(double nowMs) {
        warmupUntilMs_ = nowMs + kWarmupMs;
        count_ = bad_ = badBlocks_ = 0;
    }
    // atMs: the sample's arrival (ms, any steady origin); overrunMs: pushReflexOverrunMs or -1 when unknown. Returns true
    // once, when low latency must end now; ended() stays true afterwards.
    bool observe(double atMs, double overrunMs) {
        if (ended_) return false;
        if (lastAtMs_ <= 0 || atMs - lastAtMs_ > kGapMs || atMs < lastAtMs_) restartWarmup(atMs);
        lastAtMs_ = atMs;
        if (atMs < warmupUntilMs_ || overrunMs < 0) return false;
        ++count_;
        if (overrunMs > kOverrunMs) ++bad_;
        if (count_ < kBlock) return false;
        badBlocks_ = bad_ >= kBadInBlock ? badBlocks_ + 1 : 0;
        count_ = bad_ = 0;
        if (badBlocks_ < kBadBlocks) return false;
        ended_ = true;
        return true;
    }
    bool ended() const { return ended_; }
    bool warmingUp(double nowMs) const { return lastAtMs_ <= 0 || nowMs < warmupUntilMs_; }

private:
    double lastAtMs_ = 0, warmupUntilMs_ = 0;
    unsigned count_ = 0, bad_ = 0, badBlocks_ = 0;
    bool ended_ = false;
};
// The guard applies to a live capture card with frame generation on, unless VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP=1 (or a
// master) restores Package 14. Files, PS5 streams and screen capture keep the old behaviour.
inline bool pushReflexGuardApplies(bool physicalCapture, bool frameGeneration, bool legacyRestored) {
    return physicalCapture && frameGeneration && !legacyRestored;
}

}  // namespace veyra::engine
