// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg", step 4: the FULLY CAUSAL schedule of "Lower delay", an EXPERIMENTAL choice, never a default.
//
// Per pair (A, B) at 3X it shows B FIRST, then P1 = B + 1/3 and P2 = B + 2/3 of the A -> B motion, both PREDICTED from
// the one B -> A field of the pair (PushWarp forward projection, PushWarp.h t > 1, acceleration from the previous pair's
// field at starts and stops). At 2X (Smoothness Mode stepped down) it shows B, P(1/2). No frame is generated before B, so
// B waits for nothing generated: arithmetic on the owner's measured parts (P15_REAL_LOG_ANALYSIS.md): the readout's
// picture 4.7 + margin 2.2 = 6.9 ms with no schedule wait, against 4.7 + 2.0 + 11.1 + 2.2 = 20.0 ms for one predicted frame
// and 31 ms measured today. Arithmetic, not a measurement and not a promise.
//
// The picture cost is real (OFFLINE, synthetic 4K scenes, CPU, OWNFG_RESULTS.md; not game footage, not any graphics
// card): two of three frames predicted, P(1/3) 27.1-27.4 dB and P(2/3) 25.3-25.5 dB against a learned interpolator's
// 30.5 dB (about -3.4 / -5.2 dB); content placed up to 32 / 47 px off (P95) at a 30 FPS game's motion; what nothing can
// predict (something entering from behind an edge) stays at B (a visible hold).
//
// This header is pure (no D3D12): the choice, the ladder between the causal and the one-predicted schedule (its own exits
// and hysteresis), the causal learner's start, the admission deadline, the per-frame verdict and the evidence text.
// EngineController and EnhanceGraph call it through short hooks marked "stream ownfg"; the one-predicted schedule
// (engine/PushLowDelay.h) stays the other engineer's and is the level the causal schedule falls back to.
// Unit tests: tests/remoteplay/CausalTests.inc.
#include "veyra/pipeline/PushOwnFrame.h"
#include "veyra/pipeline/PushPredict.h"
#include "veyra/pipeline/PushWarp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::pipeline {

// The level the causal ladder is on. Causal: B, P1, P2. OnePredicted: the other engineer's I(2/3), B, P (its own rules
// decide when it falls back to interpolation).
enum class PushCausalLevel : std::uint8_t { Causal = 0, OnePredicted = 1 };
// Why the ladder left the causal level (to one predicted frame, never further by itself).
enum class PushCausalExit : std::uint8_t { None = 0, Unusable, Late, Dear, Count };
inline const char* pushCausalExitName(PushCausalExit e) {
    switch (e) {
    case PushCausalExit::Unusable: return "unusable";   // most second predictions mostly held (their counters)
    case PushCausalExit::Late: return "late";           // predicted frames of causal pairs dropped late
    case PushCausalExit::Dear: return "dear";           // the two predictions' GPU time above two output slots
    default: return "none";
    }
}

// A predicted frame is usable when at most this share of its moving pixels was held (as the one-predicted P's rule:
// pushPredictFrameUsable's 0.25), from PushWarp's counters (blend = moved, hole = held).
constexpr double kPushCausalMaxHeldShare = 0.25;
inline bool pushCausalFrameUsable(const PushWarpCounters& c) {
    return !pushWarpWholeFrameNearest(c) && pushWarpHoleShare(c) <= kPushCausalMaxHeldShare;
}

// One finished causal pair, as the graph reads it back (EnhanceGraph::takeCausalVerdicts).
struct PushCausalVerdict {
    bool p1Usable = true, p2Usable = true, twoPredictions = true;   // twoPredictions false: a 2X pair (B, P(1/2))
    double p1Ms = -1.0, p2Ms = -1.0;                                  // GPU time of each prediction (PushWarp stamps); -1 unknown
};

// The ladder. Enter the causal level when chosen (the one-predicted schedule decides whether the mode runs at all).
// Leave to ONE predicted frame (one slot back, never two) on a SUSTAINED failure:
//   unusable  at least kBadNeeded of the last kWindow causal pairs had their second prediction unusable;
//   late      more than kLateDrops of the last kLateWindow predicted frames of causal pairs dropped late;
//   dear      nearest-rank P95 of the predictions' GPU time above (n - 1) output slots (at least kCostMin samples).
// Return to causal after kReturnPairs pairs on the one-predicted level (doubled up to kMaxReturnPairs when it fails again
// within kQuickFailPairs causal pairs of returning: hysteresis); after kStopAfter exits of one reason it stays at one
// predicted frame for the playback (stoppedFor) until the choice is switched off and on.
class PushCausalLadder {
public:
    static constexpr unsigned kWindow = 60, kBadNeeded = 45, kLateWindow = 60, kLateDrops = 2, kCostWindow = 30, kCostMin = 20;
    static constexpr unsigned kReturnPairs = 300, kMaxReturnPairs = 1800, kQuickFailPairs = 256, kStopAfter = 3;

    // The choice (pref pushLowDelayCausal). Switching it off resets everything (a later switch-on tries afresh).
    void chosen(bool on) {
        if (on == chosen_) return;
        chosen_ = on;
        if (!on) { const auto keep = entries_; const bool test = keepOnUnusable_; *this = PushCausalLadder{}; entries_ = keep; keepOnUnusable_ = test; }
    }
    bool isChosen() const { return chosen_; }
    // TEST ONLY (VEYRA_TEST_CAUSAL_KEEP_ON_UNUSABLE=1, the e2e's mechanics runs with the zero flow, whose predictions are all
    // held by design): the unusable exit is not taken; the shares are still counted and logged.
    void keepOnUnusable(bool keep) { keepOnUnusable_ = keep; }
    PushCausalLevel level() const { return level_; }
    // Whether the next pair is asked to run causal: chosen, on the causal level, not stopped, and the mode itself would
    // run a predicted pair (`modeAsks`: the one-predicted schedule asks at 3X, or the pair is at 2X while the mode is on).
    bool ask(bool modeAsks) const { return chosen_ && level_ == PushCausalLevel::Causal && stopped_ == PushCausalExit::None && modeAsks; }

    // After each pair the mode was on for: whether it ran causal, or (on the one-predicted level) a predicted pair.
    void pair(bool ranCausal, bool ranOnePredicted) {
        if (!chosen_) return;
        if (ranCausal) {
            if (!counted_) { counted_ = true; ++entries_; }
            ++causalPairs_;
            ++stay_;
            return;
        }
        if (level_ == PushCausalLevel::OnePredicted && ranOnePredicted && stopped_ == PushCausalExit::None) {
            if (++returnHave_ >= returnNeed_) { level_ = PushCausalLevel::Causal; counted_ = false; stay_ = 0; resetWindows(); }
        }
    }
    // budgetMs: the time the predictions may take together, (n - 1) output slots (two at 3X, one at 2X).
    void verdict(const PushCausalVerdict& v, double budgetMs) {
        if (!chosen_) return;
        p1Bad_ += !v.p1Usable; ++p1Seen_;
        if (v.twoPredictions) { p2Bad_ += !v.p2Usable; ++p2Seen_; }
        // The SECOND prediction (the further one) decides "unusable"; at 2X the only one.
        const bool bad = v.twoPredictions ? !v.p2Usable : !v.p1Usable;
        push(bad_, badNext_, badCount_, badOnes_, kWindow, bad);
        if (level_ == PushCausalLevel::Causal && badCount_ >= kWindow && badOnes_ >= kBadNeeded && !keepOnUnusable_) leave(PushCausalExit::Unusable);
        const double total = (v.p1Ms >= 0 ? v.p1Ms : 0.0) + (v.p2Ms >= 0 ? v.p2Ms : 0.0);
        if (v.p1Ms >= 0 && budgetMs > 0) {
            costs_[costCount_ % kCostWindow] = total; ++costCount_;
            const unsigned n = std::min(costCount_, kCostWindow);
            if (n >= kCostMin) {
                double s[kCostWindow];
                for (unsigned i = 0; i < n; ++i) s[i] = costs_[i];
                std::sort(s, s + n);
                lastCostP95_ = s[(n * 95 + 99) / 100 - 1];
                if (level_ == PushCausalLevel::Causal && lastCostP95_ > budgetMs) leave(PushCausalExit::Dear);
            }
        }
    }
    // A predicted frame of a causal pair presented or dropped late (EngineController's outcome of a causal lease).
    void outcome(bool droppedLate) {
        if (!chosen_) return;
        ++outcomes_; lateSeen_ += droppedLate;
        if (level_ != PushCausalLevel::Causal) return;
        push(late_, lateNext_, lateCount_, lateOnes_, kLateWindow, droppedLate);
        if (lateOnes_ > kLateDrops) leave(PushCausalExit::Late);
    }

    PushCausalExit stoppedFor() const { return stopped_; }
    PushCausalExit lastExit() const { return lastExit_; }
    std::uint64_t exits(PushCausalExit why) const { return exits_[static_cast<unsigned>(why)]; }
    std::uint64_t exitsTotal() const { return exits_[0]; }
    std::uint64_t entries() const { return entries_; }
    std::uint64_t causalPairs() const { return causalPairs_; }
    unsigned returnHave() const { return level_ == PushCausalLevel::Causal ? 0 : returnHave_; }
    unsigned returnNeed() const { return level_ == PushCausalLevel::Causal ? 0 : returnNeed_; }
    double p1UnusableShare() const { return p1Seen_ ? double(p1Bad_) / double(p1Seen_) : -1.0; }
    double p2UnusableShare() const { return p2Seen_ ? double(p2Bad_) / double(p2Seen_) : -1.0; }
    std::uint64_t lateSeen() const { return lateSeen_; }
    std::uint64_t outcomesSeen() const { return outcomes_; }
    double lastCostP95() const { return lastCostP95_; }
    unsigned costSamples() const { return std::min(costCount_, kCostWindow); }
    // The share counters of the evidence are per window: the caller reads them, then clears.
    void clearShares() { p1Bad_ = p2Bad_ = p1Seen_ = p2Seen_ = 0; }

private:
    static void push(std::array<std::uint64_t, 2>& bits, unsigned& next, unsigned& count, unsigned& ones, unsigned window, bool v) {
        const std::uint64_t bit = std::uint64_t{1} << (next % 64);
        auto& word = bits[next / 64];
        if (count >= window) { if (word & bit) --ones; } else ++count;
        if (v) { word |= bit; ++ones; } else word &= ~bit;
        next = (next + 1) % window;
    }
    void resetWindows() {
        bad_ = {}; badNext_ = badCount_ = badOnes_ = 0;
        late_ = {}; lateNext_ = lateCount_ = lateOnes_ = 0;
        costCount_ = 0;
    }
    void leave(PushCausalExit why) {
        level_ = PushCausalLevel::OnePredicted;
        lastExit_ = why;
        ++exits_[0]; ++exits_[static_cast<unsigned>(why)];
        returnNeed_ = returnNeed_ != 0 && stay_ < kQuickFailPairs ? std::min(kMaxReturnPairs, returnNeed_ * 2) : kReturnPairs;
        returnHave_ = 0;
        if (++sustained_[static_cast<unsigned>(why)] >= kStopAfter) stopped_ = why;
        resetWindows();
    }
    bool chosen_ = false, counted_ = false, keepOnUnusable_ = false;
    PushCausalLevel level_ = PushCausalLevel::Causal;
    PushCausalExit lastExit_ = PushCausalExit::None, stopped_ = PushCausalExit::None;
    std::array<std::uint64_t, static_cast<unsigned>(PushCausalExit::Count)> exits_{};
    std::array<unsigned, static_cast<unsigned>(PushCausalExit::Count)> sustained_{};
    std::array<std::uint64_t, 2> bad_{}, late_{};
    unsigned badNext_ = 0, badCount_ = 0, badOnes_ = 0, lateNext_ = 0, lateCount_ = 0, lateOnes_ = 0;
    double costs_[kCostWindow] = {};
    unsigned costCount_ = 0;
    double lastCostP95_ = -1.0;
    unsigned returnHave_ = 0, returnNeed_ = 0, stay_ = 0;
    std::uint64_t entries_ = 0, causalPairs_ = 0, p1Bad_ = 0, p2Bad_ = 0, p1Seen_ = 0, p2Seen_ = 0, lateSeen_ = 0, outcomes_ = 0;
};

// The causal learner's start and ceiling (LivePairLatency::select, as pushLowDelayPhaseArgs gives the predicted learner).
// At 3X one slot (T/3) below the one-predicted phase (B moves one slot earlier again), or Package 14's legacy phase minus
// two slots without one; at 2X ([B, P(1/2)] against [I(1/2), B]) the legacy phase minus one slot (T/2). Never above the
// legacy ceiling minus (n - 1) slots. predictedPhase: the one-predicted learner's last phase (0: none).
struct PushCausalPhaseArgs { std::int64_t start = 0, allowance = 0; };
inline PushCausalPhaseArgs pushCausalPhaseArgs(std::int64_t interval100ns, unsigned multiplier, std::int64_t legacy100ns, std::int64_t allowance100ns,
                                               std::int64_t predictedPhase100ns) {
    const std::int64_t n = multiplier >= 2 ? multiplier : 2, slot = interval100ns / n, back = (n - 1) * slot;
    const std::int64_t ceiling = legacy100ns + allowance100ns - back;
    std::int64_t start = n == 3 && predictedPhase100ns > slot ? predictedPhase100ns - slot : legacy100ns - back;
    if (start > ceiling) start = ceiling;
    if (start < 0) start = 0;
    return {start, ceiling > start ? ceiling - start : 0};
}

// The graph's per-pair decision (EnhanceGraph, PushPredictEligibility filled as for the one-predicted pair): the same
// reasons, at 2X or 3X, with PushWarp making every generated frame (no VFG request, so VFG's refusal does not apply).
// `served`: the generator is VFG or PushFlow and PushWarp passed its self-check with both real frames as inputs.
inline PushPredictRefusal pushCausalPairRefusal(PushPredictEligibility e, bool served) {
    if (e.requested != PushPairSchedule::Causal) return PushPredictRefusal::SwitchOff;
    if (e.presentSinkFg) return PushPredictRefusal::PresentSinkFg;
    if (!served) return PushPredictRefusal::NotVfg;
    if (e.multiplier != 2 && e.multiplier != 3) return PushPredictRefusal::NotThreeX;
    e.requested = PushPairSchedule::OnePredicted; e.multiplier = 3; e.vfg = true; e.vfgRefused = false;
    return pushPredictPairRefusal(e);
}

// The predictions' time budget of a pair (ms): (n - 1) output slots of its content interval b - a (100 ns).
inline double pushCausalBudgetMs(std::int64_t a100ns, std::int64_t b100ns, unsigned multiplier) {
    if (multiplier < 2 || b100ns <= a100ns) return 0.0;
    return double(b100ns - a100ns) / 10000.0 * double(multiplier - 1) / double(multiplier);
}

// The predicted frames' timestamps: B + d j / n (j = 1 .. n - 1), rounded as FrameBatch::interpolate (100 ns).
inline std::int64_t pushCausalPts(std::int64_t a, std::int64_t b, unsigned index, unsigned multiplier) {
    const std::int64_t n = std::max<std::int64_t>(1, multiplier), j = index, d = b - a;
    return b + (d / n) * j + ((d % n) * j + n / 2) / n;
}
// The step of prediction j at multiplier n (t = 1 + j / n for PushWarpParams).
inline float pushCausalStep(unsigned index, unsigned multiplier) { return multiplier ? float(index) / float(multiplier) : 0.0f; }

// Admission of an asked causal pair: nothing generated precedes B, so the group (base + both predictions) is judged
// against the LAST prediction's deadline; the predictions' measured P95 once at least kMinMeasured were timed, else the
// interpolated group's cost (two generated frames either way).
inline double pushCausalAdmissionGroupMs(double interpolatedGroupMs, double causalP95Ms, std::size_t causalSamples) {
    constexpr std::size_t kMinMeasured = 8;
    return causalSamples >= kMinMeasured && causalP95Ms >= 0 ? causalP95Ms : interpolatedGroupMs;
}

// For [capture-timing] / [fg-health] / [fg-sched] (engine side): " schedule=<interpolate|onepredicted|causal>
// causalChosen= causalLevel= causalPairs= causalExitsUnusable= causalExitsLate= causalExitsDear= causalStopped=
// causalReturn=have/need causalP1Unusable= causalP2Unusable= causalLate=late/outcomes causalCostP95Ms=" (no commas: the
// log readers split at them). Empty when the choice was never on.
inline std::string pushCausalText(const PushCausalLadder& l, bool everChosen, const char* scheduleNow) {
    if (!everChosen) return std::string();
    char t[512];
    const int n = std::snprintf(t, sizeof t,
        " schedule=%s causalChosen=%d causalLevel=%s causalPairs=%llu causalEntries=%llu causalExitsUnusable=%llu causalExitsLate=%llu causalExitsDear=%llu causalLastExit=%s "
        "causalStopped=%s causalReturn=%u/%u causalP1Unusable=%.3f causalP2Unusable=%.3f causalLate=%llu/%llu causalCostP95Ms=%.2f",
        scheduleNow, l.isChosen() ? 1 : 0, l.level() == PushCausalLevel::Causal ? "causal" : "onepredicted",
        static_cast<unsigned long long>(l.causalPairs()), static_cast<unsigned long long>(l.entries()),
        static_cast<unsigned long long>(l.exits(PushCausalExit::Unusable)), static_cast<unsigned long long>(l.exits(PushCausalExit::Late)),
        static_cast<unsigned long long>(l.exits(PushCausalExit::Dear)), pushCausalExitName(l.lastExit()), pushCausalExitName(l.stoppedFor()),
        l.returnHave(), l.returnNeed(), l.p1UnusableShare(), l.p2UnusableShare(), static_cast<unsigned long long>(l.lateSeen()),
        static_cast<unsigned long long>(l.outcomesSeen()), l.lastCostP95());
    return n > 0 ? std::string(t, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof t) - 1))) : std::string();
}

// The graph's side (EnhanceGraph::causalTimingText): P1's and P2's GPU time apart, P50/P95 over 10 s from PushWarp's own
// timestamps (" causalP1GpuMs=p50/p95 causalP2GpuMs=p50/p95 causalGpuSamples=n1/n2"; -1 unknown).
inline std::string pushCausalCostText(const PushWarpCostWindow& p1, const PushWarpCostWindow& p2) {
    char t[160];
    const int n = std::snprintf(t, sizeof t, " causalP1GpuMs=%.2f/%.2f causalP2GpuMs=%.2f/%.2f causalGpuSamples=%zu/%zu",
                                p1.percentile(PushWarpStage::Total, 0.5), p1.percentile(PushWarpStage::Total, 0.95),
                                p2.percentile(PushWarpStage::Total, 0.5), p2.percentile(PushWarpStage::Total, 0.95), p1.size(), p2.size());
    return n > 0 ? std::string(t, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof t) - 1))) : std::string();
}

// Shared with the bridge (the row): the choice (written by the bridge) and the ladder's facts (written by the engine).
struct PushCausalShared {
    std::atomic<bool> chosen{false};
    std::atomic<std::uint8_t> level{0};        // PushCausalLevel
    std::atomic<std::uint8_t> lastExit{0};     // PushCausalExit
    std::atomic<std::uint8_t> stoppedFor{0};   // PushCausalExit
    std::atomic<std::uint32_t> returnHave{0}, returnNeed{0};
    std::atomic<std::uint32_t> causalPerSecond{0};   // causal pairs run in the last second
};
inline PushCausalShared& pushCausalShared() { static PushCausalShared s; return s; }

}  // namespace veyra::pipeline
