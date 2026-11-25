// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "lagfix", Directive 25 (the owner's 25th message): changes to live frame-generation scheduling for the four
// fragile spots the scheduling simulator found in the default 4K 3X path (tests/remoteplay/FgScheduleSim.h; every number
// quoted below is SIMULATED: the program's own decision code with made-up 4K-like costs, never a measurement).
// Portable and header-only; EngineController.cpp reads the switches once and calls these.
//
//   robustAdmit        admission lock-out. FgRecoveryBudget admits a pair when interval + base + the FG cost's P95 over the
//                      last 2 s fit; a few slow groups lift that P95 over the deadline, and as no FG sample arrives while
//                      pairs are skipped, generation stays off until they are 2 s old. This adds a second estimate,
//                      min(P95, 1.2 x P75) of the same samples (PushFgCostWindow), so a few slow groups no longer outvote
//                      the rest; a pair the budget rejects is admitted when that estimate fits. Simulated near-limit card:
//                      generated frames 34.8 -> 59.7 a second, skipped pairs 11.2 -> 0, off-11.1 ms intervals 25.5 -> 12.4 %,
//                      longest gap 62.4 -> 33.4 ms, real frame P95 61.0 -> 46.9 ms; but its MEAN 35.6 -> 42.5 ms, because the
//                      30 FPS stretches it ends showed real frames early (LiveFgRejectionHold). ON by default on capture
//                      cards since the owner's priority order decides (kPushFgSchedCardDefault); off on streams.
//   phaseBehindWarmup  the learned phase jumping after a skip run. A warm-up group (FgCostSettling) is not a phase sample,
//                      but the pair queued behind it on the GPU was, so its readiness raised the phase to ~63 ms, which then
//                      came down 0.5 ms a pair. No phase sample from a pair submitted before an excluded group finished.
//                      Simulated near-limit card: real frame P95 61.0 -> 45.2 ms, mean 35.6 -> 33.1 ms; but 0.02 fewer
//                      generated frames a second and 1.4 % more uneven intervals (the lower phase leaves them less time).
//                      A trade-off: off by default.
//   streamSmooth       a PS5 stream's real frames presented on their decode times carry decode jitter to the panel. With
//                      this, a stream's present anchor goes through LiveArrivalSmoother as a capture card's does (its PTS
//                      line, kLead 0.5 ms earlier; generated-frame expiry keeps its distance to the decode time). Simulated
//                      (decode jitter sd 1.5 ms): off-11.1 ms intervals 21.4 -> 0.1 % (comfortable), 30.7 -> 16.5 % (drift);
//                      real frame mean 0.5 ms lower; P95 from DECODE 1.5-2.6 ms higher (a frame decoded early now waits for
//                      its even slot: the jitter moves from the panel into that measure); longest interval +0.3 ms in
//                      drift. A trade-off by the letter of the rules: off by default.
//   holdCardsOnly      LiveFgRejectionHold (real frames without the FG phase after 1 s of rejections) also ran on a stream
//                      with a learned phase. With this only capture cards use it. Simulated near-limit stream: generated
//                      frames 27.8 -> 29.1 a second, longest gap 62.4 -> 40.8 ms; mean real frame 28.5 -> 38.5 ms (the hold's
//                      30 FPS stretches are gone). A trade-off: off by default.
//
// Switches (environment only: slipstream.ini cannot set them, slipstreamFileMayApply leaves them out; the kit can):
//   VEYRA_EXP_RP_FG_SCHED=<parts>          turns parts on: "all" or a comma list of the names above.
//   VEYRA_TEST_RP_FG_SCHED_LEGACY=<parts>  turns parts off (today's behaviour), for a part that is on by default.
//   gridEarly          the dead phase on a card: a skipped (not held) pair's real frame on the earliest slot of the output
//                      grid its base cost reaches (pushGridEarlyDelay) instead of the full phase; the grid is kept.
// Defaults: capture cards kPushFgSchedCardDefault (robustAdmit and gridEarly on), every other source kPushFgSchedDefault
// (all off). Unknown or malformed values change nothing.
#include "veyra/engine/PushLegacy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veyra::engine {

struct PushFgSched {
    bool robustAdmit = false, phaseBehindWarmup = false, streamSmooth = false, holdCardsOnly = false, gridEarly = false;
    bool understood = true;   // false: a value was set but not understood (nothing changed by it)
};
inline constexpr PushFgSched kPushFgSchedDefault{};   // PS5 streams and other sources: every part off (unchanged)
// Capture cards (the owner's scope, 23 Nov 2026 22:51 ET): robustAdmit and gridEarly ON by default. Judged by his priority
// ORDER, per card cost set of the simulator: not worse on (1)+(2) (generated frames shown, uneven intervals, longest gap,
// skipped pairs) in any set, identical in every set without skipped pairs, and better in near-limit, overload and heavy.
// robustAdmit's real frames in pairs shown WITH generation are earlier than today's (near-limit mean 47.1 -> 42.5 ms,
// P95 63.5 -> 46.9 ms): its higher overall mean is generation no longer dropping out, not a later frame.
// phaseBehindWarmup stays off: alone it is worse in near-limit and overload (fewer generated frames, more uneven
// intervals), and with robustAdmit on it changes nothing.
inline constexpr PushFgSched kPushFgSchedCardDefault{true, false, false, false, true, true};

namespace fg_sched_detail {
// Parses "all" or a comma list; false on anything else (then nothing is applied).
inline bool parts(std::wstring_view value, PushFgSched& out) {
    if (value.empty() || value.size() > 127) return false;
    if (value == L"all") { out.robustAdmit = out.phaseBehindWarmup = out.streamSmooth = out.holdCardsOnly = out.gridEarly = true; return true; }
    PushFgSched p{};
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = value.find(L',', start);
        const std::wstring_view part = value.substr(start, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - start);
        if (part == L"robustAdmit") p.robustAdmit = true;
        else if (part == L"phaseBehindWarmup") p.phaseBehindWarmup = true;
        else if (part == L"streamSmooth") p.streamSmooth = true;
        else if (part == L"holdCardsOnly") p.holdCardsOnly = true;
        else if (part == L"gridEarly") p.gridEarly = true;
        else return false;
        if (comma == std::wstring_view::npos) break;
        start = comma + 1;
    }
    out = p;
    return true;
}
}  // namespace fg_sched_detail

// on / legacy: the two variables' values (empty when unset). Defaults first, then on, then legacy (legacy wins).
inline PushFgSched pushFgSchedResolve(std::wstring_view on, std::wstring_view legacy, PushFgSched defaults = kPushFgSchedDefault) {
    PushFgSched out = defaults;
    out.understood = true;
    if (!on.empty()) {
        PushFgSched p;
        if (fg_sched_detail::parts(on, p)) {
            out.robustAdmit = out.robustAdmit || p.robustAdmit;
            out.phaseBehindWarmup = out.phaseBehindWarmup || p.phaseBehindWarmup;
            out.streamSmooth = out.streamSmooth || p.streamSmooth;
            out.holdCardsOnly = out.holdCardsOnly || p.holdCardsOnly;
            out.gridEarly = out.gridEarly || p.gridEarly;
        } else out.understood = false;
    }
    if (!legacy.empty()) {
        PushFgSched p;
        if (fg_sched_detail::parts(legacy, p)) {
            out.robustAdmit = out.robustAdmit && !p.robustAdmit;
            out.phaseBehindWarmup = out.phaseBehindWarmup && !p.phaseBehindWarmup;
            out.streamSmooth = out.streamSmooth && !p.streamSmooth;
            out.holdCardsOnly = out.holdCardsOnly && !p.holdCardsOnly;
            out.gridEarly = out.gridEarly && !p.gridEarly;
        } else out.understood = false;
    }
    return out;
}
// Package 14's rollback (PushLegacy.h): VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT / _GRID_EARLY / _PACKAGE13 = 1 turn those parts
// off whatever the defaults or VEYRA_EXP_RP_FG_SCHED say.
inline PushFgSched pushFgSchedApplyLegacy(PushFgSched s, const PushEnvLookup& env) {
    if (pushLegacyRestored(PushLegacyPart::RobustAdmit, env)) s.robustAdmit = false;
    if (pushLegacyRestored(PushLegacyPart::GridEarly, env)) s.gridEarly = false;
    return s;
}
inline std::string pushFgSchedDescribe(const PushFgSched& s) {
    std::string t = "robustAdmit=";
    t += s.robustAdmit ? '1' : '0';
    t += " phaseBehindWarmup=";
    t += s.phaseBehindWarmup ? '1' : '0';
    t += " streamSmooth=";
    t += s.streamSmooth ? '1' : '0';
    t += " holdCardsOnly=";
    t += s.holdCardsOnly ? '1' : '0';
    t += " gridEarly=";
    t += s.gridEarly ? '1' : '0';
    t += s.understood ? "" : " (a value of VEYRA_EXP_RP_FG_SCHED / VEYRA_TEST_RP_FG_SCHED_LEGACY was not understood and changed nothing)";
    return t;
}

// --- robustAdmit -------------------------------------------------------------------------------------------------------
// The same FG samples FgRecoveryBudget keeps (fed where the engine calls FgRecoveryBudget::complete: evaluated, comparable,
// not warm-up groups; at most 64, 2 s), and the spike-robust estimate min(P95, 1.2 x P75). Times in 100 ns, costs in ms.
class PushFgCostWindow {
public:
    static constexpr std::size_t kMax = 64;
    static constexpr std::int64_t kAge100ns = 20000000;
    void reset() { samples_.clear(); }
    void add(std::int64_t now, double ms) {
        if (!std::isfinite(ms) || ms < 0) return;
        while (!samples_.empty() && (samples_.size() >= kMax || samples_.front().first < now - kAge100ns)) samples_.pop_front();
        samples_.emplace_back(now, ms);
    }
    // -1 without samples.
    double robust(std::int64_t now) const {
        std::vector<double> v;
        v.reserve(samples_.size());
        for (const auto& s : samples_)
            if (s.first >= now - kAge100ns) v.push_back(s.second);
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        const auto at = [&v](double q) {
            const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
            return v[std::min(v.size() - 1, rank ? rank - 1 : 0)];
        };
        return std::min(at(0.95), 1.2 * at(0.75));
    }
    std::size_t size() const { return samples_.size(); }

private:
    std::deque<std::pair<std::int64_t, double>> samples_;
};
// FgRecoveryBudget::canAdmit's rule with the robust FG estimate: the deadline plus 10 ms covers base + FG - CPU progress
// + the blit. baseMs: FgRecoveryBudget::baseCost; fgMs: PushFgCostWindow::robust (< 0: no sample, nothing admitted here).
inline bool pushRobustAdmitFits(std::int64_t now, std::int64_t deadline, double elapsedMs, double presentMs, std::optional<double> baseMs, double fgMs) {
    if (!baseMs || fgMs < 0 || !std::isfinite(fgMs)) return false;
    const double progress = std::clamp(elapsedMs, 0.0, *baseMs);
    return double(deadline - now) / 10000 + 10 >= std::max(0.0, *baseMs + fgMs - progress) + std::max(0.0, presentMs);
}

// --- robustAdmit's stop for sustained slowness (review of Package 14, 24 Nov 2026) -------------------------------------
// A pair robustAdmit lets through (the P95 rule rejected it) is "bad" when one of its generated frames was dropped at its
// deadline or presented more than 1 ms after it. Over the last kWindow such pairs, more than kBadShare bad ones suspend
// robust admits for kSuspend100ns, doubling (to kMaxSuspend100ns) when the next window trips again within a minute; the
// window restarts empty after a suspension (hysteresis: it cannot flap pair by pair). While suspended, admission is the
// budget's own P95 rule, exactly as Package 13.
class PushRobustAdmitGuard {
public:
    static constexpr std::size_t kDefaultWindow = 16;
    static constexpr double kDefaultBadShare = 0.25;
    explicit PushRobustAdmitGuard(std::size_t window = kDefaultWindow, double badShare = kDefaultBadShare) : window_(window), badShare_(badShare) {}
    static constexpr std::int64_t kSuspend100ns = 20000000, kMaxSuspend100ns = 80000000, kCalm100ns = 600000000;
    void reset() { *this = PushRobustAdmitGuard(window_, badShare_); }
    bool allowed(std::int64_t now) const { return now >= suspendedUntil_; }
    // One call per robust-admitted pair, when its real frame was presented.
    void observe(std::int64_t now, bool bad) {
        outcomes_.push_back(bad);
        if (outcomes_.size() > window_) outcomes_.pop_front();
        if (outcomes_.size() < window_) return;
        std::size_t badCount = 0;
        for (const bool b : outcomes_) badCount += b ? 1u : 0u;
        if (double(badCount) <= badShare_ * double(window_)) return;
        const std::int64_t length = lastTrip_ && now - lastTrip_ < kCalm100ns ? std::min(kMaxSuspend100ns, lastLength_ * 2) : kSuspend100ns;
        suspendedUntil_ = now + length;
        lastTrip_ = now;
        lastLength_ = length;
        ++trips_;
        outcomes_.clear();
    }
    std::uint64_t trips() const { return trips_; }

private:
    std::size_t window_;
    double badShare_;
    std::deque<bool> outcomes_;
    std::int64_t suspendedUntil_ = 0, lastTrip_ = 0, lastLength_ = kSuspend100ns;
    std::uint64_t trips_ = 0;
};

// --- phaseBehindWarmup -----------------------------------------------------------------------------------------------
// Called per completed group in completion order. submit: when the pair was submitted (processStart); done: when its
// completion was observed; warmup: FgCostSettling marked it; evaluated: generated frames were made. Returns true when the
// pair must not be a phase sample: it was submitted before an excluded group finished, so it queued behind it.
class PushPhaseQueueGuard {
public:
    void reset() { excludedDone_ = 0; }
    bool excluded(std::int64_t submit, std::int64_t done, bool warmup, bool evaluated) {
        const bool behind = submit < excludedDone_;
        if ((warmup || behind) && evaluated) excludedDone_ = std::max(excludedDone_, done);
        return behind;
    }

private:
    std::int64_t excludedDone_ = 0;
};

// --- gridEarly (the dead phase on a card, kept on the output grid) -------------------------------------------------------
// A capture pair whose generated frames are skipped (and not held by LiveFgRejectionHold) still shows its real frame at the
// full FG phase. Its generated frames' slots are empty either way, so the real frame may take the earliest of them it can
// reach: phase - j x interval / M, j = M-1 ... 0, the largest j whose slot is at least the base cost's P95 + 1 ms after
// the callback (anchorLead100ns: callback - presentation anchor, so the slot is measured from the callback). The output
// stays on its interval/M grid; the long gap moves from before the real frame to after it. Without a base cost: the phase.
inline std::int64_t pushGridEarlyDelay(std::int64_t phase100ns, std::int64_t interval100ns, unsigned multiplier, std::optional<double> baseP95Ms,
                                       std::int64_t anchorLead100ns) {
    if (!baseP95Ms || multiplier < 2 || interval100ns <= 0 || phase100ns <= 0) return phase100ns;
    const std::int64_t slot = interval100ns / std::int64_t(multiplier);
    // A deadline is anchor + delay, and the anchor lies anchorLead100ns before the callback.
    const std::int64_t need = std::int64_t((*baseP95Ms + 1.0) * 10000.0) + anchorLead100ns;
    for (std::int64_t j = std::int64_t(multiplier) - 1; j > 0; --j)
        if (phase100ns - j * slot >= need) return phase100ns - j * slot;
    return phase100ns;
}

// --- steadiness counters (the owner, 23 Nov 2026 23:59 ET: "99 % steady") ------------------------------------------------
// Always on, engine thread: every Present call of a frame-generation output with its grid slot (content interval / M).
// take() gives one window: intervals between Present calls, the share within 1 ms and within half a slot of the slot,
// frames shown against the frames the grid expects in the window's time, the longest interval and the longest run of
// consecutive intervals more than 1 ms off. Logged in [capture-timing] (onGrid1msPct, onGridHalfPct, shownPct, ...).
// Times in 100 ns; Present call times, not photons.
class PushGridWindow {
public:
    struct Stats {
        std::uint64_t intervals = 0, within1ms = 0, withinHalf = 0, presents = 0;
        double expected = 0, longestMs = 0;
        unsigned offRunMax = 0;
        double onGrid1msPct() const { return intervals ? 100.0 * double(within1ms) / double(intervals) : -1.0; }
        double onGridHalfPct() const { return intervals ? 100.0 * double(withinHalf) / double(intervals) : -1.0; }
        double shownPct() const { return expected > 0 ? 100.0 * double(presents) / expected : -1.0; }
    };
    void reset() { *this = PushGridWindow{}; }
    // A gap longer than kBreak100ns (a pause, a settings change) ends the run: not an interval.
    static constexpr std::int64_t kBreak100ns = 2500000;
    void observe(std::int64_t present100ns, std::int64_t slot100ns) {
        if (slot100ns <= 0) { last_ = 0; return; }
        ++s_.presents;
        if (last_ && present100ns > last_ && present100ns - last_ <= kBreak100ns) {
            const std::int64_t gap = present100ns - last_, off = gap > slot100ns ? gap - slot100ns : slot100ns - gap;
            ++s_.intervals;
            s_.expected += double(gap) / double(slot100ns);
            s_.longestMs = std::max(s_.longestMs, double(gap) / 10000.0);
            if (off <= 10000) { ++s_.within1ms; run_ = 0; }
            else s_.offRunMax = std::max(s_.offRunMax, ++run_);
            if (off * 2 <= slot100ns) ++s_.withinHalf;
        } else s_.expected += 1.0;   // the first present of a run counts itself
        last_ = present100ns;
    }
    Stats take() { const Stats out = s_; s_ = Stats{}; return out; }
    const Stats& peek() const { return s_; }

private:
    Stats s_;
    std::int64_t last_ = 0;
    unsigned run_ = 0;
};

}  // namespace veyra::engine
