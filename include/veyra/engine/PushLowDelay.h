// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Package 15, "Lower delay" (the owner, 23 Nov 2026: "i get 90 frames with 30 ms i need less ms delay than this"): the
// SCHEDULING half of the one-predicted schedule (docs/drafts/FG_SCHEDULING_NEXT.md section 3, docs/drafts/LOW_DELAY_MODE.md).
// The GPU half (veyra/pipeline/PushPredict.h, EnhanceGraph::setPairSchedule) runs a pair as I(2/3), B, P(B + 1/3) when asked.
// This header decides WHEN to ask, and how the presentation phase follows. Portable and header-only; EngineController calls
// it; the scheduling simulator (tests/remoteplay/FgScheduleSim.h) runs the same code. Times in 100 ns.
//
// Capture card, VFG, 3X only. OFF unless chosen (the quick-setup row "Lower delay", pref pushLowDelay, or
// VEYRA_EXP_RP_FG_SCHEDULE=onepredicted for the kit); off, nothing here is called and the frame path is Package 14's.
// The row takes effect in the running session: EnhancementSettings::lowDelay makes the predicted-frame pass part of the
// graph's shape (pushLowDelayShapesGraph), so switching the row with VFG at 3X rebuilds the graph as HUD Lock does, with
// the pass created on demand and released when the row is off. The kit's switch still creates it for the whole process.
//
// Timing. Deadlines follow the frames' own timestamps through PresentationScheduler (anchor + (pts - b) + phase), so a
// predicted pair needs no special deadline code: I(2/3) is due at phase - T/3, B at phase, P (pts b + T/3) at phase + T/3.
// What changes is the phase: B waits ONE slot after the interpolated frame instead of two, so the phase is about T/3
// (11.1 ms at a 30 FPS game) shorter. Each schedule learns its own phase (two LivePairLatency); a learner without fresh
// samples starts from the other schedule's phase moved by T/3 (pushLowDelayPhaseArgs), so a switch aims to keep frames on
// the output grid: entering, the first predicted pair's I falls one slot after the last interpolated B (its content skips
// the 1/3 position once); leaving, the first interpolated I(1/3) falls one slot after the last P (its content repeats the
// 1/3 position once). A switch is NOT free: each moves the real frame by one slot (11.1 ms), and leaving can make the
// first pairs late when interpolation needs more than the predicted phase + T/3 (the simulator shows off-grid intervals
// and P95 tails there). The StayIn policy exists to make switches rare (docs/drafts/LOW_DELAY_MODE.md section 3).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include "veyra/pipeline/PushPredict.h"

namespace veyra::engine {

enum class PushLowDelayExit : std::uint8_t { None = 0, Off, Ineligible, Refused, Unusable, Skipped, Held, Late, Count };
inline const char* pushLowDelayExitName(PushLowDelayExit e) {
    switch (e) {
    case PushLowDelayExit::None: return "none";
    case PushLowDelayExit::Off: return "off";
    case PushLowDelayExit::Ineligible: return "ineligible";   // not a card, not VFG, not 3X, no predict pass, HUD Lock ...
    case PushLowDelayExit::Refused: return "refused";         // the graph ran today's pair (no history, shot change, ...)
    case PushLowDelayExit::Unusable: return "unusable";       // the last predicted frame was mostly held (its counters)
    case PushLowDelayExit::Skipped: return "skipped";         // admission skipped the pair
    case PushLowDelayExit::Held: return "held";               // the rejection hold or a FgCostSettling hold
    case PushLowDelayExit::Late: return "late";               // predicted frames dropped late, or the output off the grid
    default: return "?";
    }
}

// What the engine knows about one pair after process() (and its admission).
struct PushLowDelayPair {
    bool wanted = false;      // the row or the kit's switch asks for it
    bool eligible = false;    // capture card, VFG at 3X, the predict pass exists, no HUD Lock / exclusion, not present-sink
    bool ranPredicted = false;   // out.predictedPair: the batch is [I, B, P]
    bool refused = false;     // asked for, but the graph ran today's pair (lastPredictRefusal other than ok / switch-off)
    bool skipped = false;     // admission skipped the pair's generation
    bool held = false;        // a hold shows real frames without the phase (LiveFgRejectionHold, FgCostSettling)
};

// Enter after a run of calm pairs, leave at once (FG_SCHEDULING_NEXT.md 3.4). request(): what to ask the graph for the
// NEXT pair. The run needed starts at kCalmPairs and doubles (up to kMaxCalmPairs) each time the schedule leaves within
// kSettledPairs pairs of entering, so motion that keeps defeating the prediction does not make it switch every few pairs;
// it goes back to kCalmPairs after a stay of kSettledPairs. Unusable counters arrive only from predicted frames, so they
// leave the schedule but never block a later entry on their own.
// Two policies for a prediction found unusable (scheduling review of 24 Nov, item A):
//  - LeaveOnUnusable (as first built): leave the schedule at once; each exit repeats a content position and each
//    re-entry skips one, and after an exit real frames can be later for some pairs.
//  - StayIn: an unusable prediction does not change the schedule; that P is shown with its bad regions held at B (a
//    wholly bad P is B for one more slot: one held frame, no change of delay). The schedule leaves on a SUSTAINED
//    failure only: at least kSustainBad of the last kSustainWindow predicted frames unusable; it then needs a calm run of
//    at least kStayInCalmPairs (doubling up to kStayInMaxCalmPairs if it fails again soon) before it returns.
// The other exits (row off, not eligible, a hold, a skipped pair, a graph refusal) are the same in both.
enum class PushLowDelayPolicy : std::uint8_t { LeaveOnUnusable = 0, StayIn = 1 };
// The engine's policy (orchestrator's decision of 24 Nov): StayIn, unless VEYRA_TEST_RP_LOWDELAY_LEAVE_ON_UNUSABLE is set
// (environment or slipstream.ini, a rollback switch: present = set; the start-up switch line names it).
inline PushLowDelayPolicy pushLowDelayPolicyFor(bool leaveOnUnusableSwitch) {
    return leaveOnUnusableSwitch ? PushLowDelayPolicy::LeaveOnUnusable : PushLowDelayPolicy::StayIn;
}
inline const char* pushLowDelayPolicyName(PushLowDelayPolicy p) { return p == PushLowDelayPolicy::StayIn ? "stay-in" : "leave-on-unusable"; }

class PushLowDelaySchedule {
public:
    static constexpr unsigned kCalmPairs = 8, kMaxCalmPairs = 240, kSettledPairs = 64;
    static constexpr unsigned kSustainWindow = 60, kSustainBad = 45, kStayInCalmPairs = 300, kStayInMaxCalmPairs = 1800;
    // StayIn's sustained-failure rule: at least `bad` of the last `window` (<= 128) predicted frames unusable leaves;
    // the first return needs `calm` calm pairs (doubled, up to kStayInMaxCalmPairs, if it fails again soon).
    void setSustain(unsigned window, unsigned bad, unsigned calm) {
        sustainWindow_ = std::clamp(window, 1u, 128u); sustainBadNeeded_ = std::clamp(bad, 1u, sustainWindow_); stayInCalm_ = std::max(calm, kCalmPairs);
    }
    void reset() { const auto policy = policy_; *this = PushLowDelaySchedule{}; policy_ = policy; }
    void setPolicy(PushLowDelayPolicy policy) { policy_ = policy; }
    PushLowDelayPolicy policy() const { return policy_; }
    std::uint64_t unusableShown() const { return unusableShown_; }
    // Review 2, SHOULD 2 (the owner's steadiness first): leave, with the sustained back-off, when more than kLateDrops of
    // the last kLateWindow predicted frames were dropped late, or after kOffGridWindows one-second windows in a row below
    // kOnGridMinPct on the 1 ms grid while the mode ran for the whole window AND interpolation is not known to be worse:
    // its own last whole window was at kOnGridMinPct or better, or it has not been measured yet (then the exit is the
    // measurement). Without that condition the simulator left near the GPU limit for a schedule that is LESS steady there
    // (near-limit 98.2 -> 93.2 %, overload 97.3 -> 34.7 % on the grid, SIMULATED).
    static constexpr unsigned kLateWindow = 60, kLateDrops = 2, kOffGridWindows = 3;
    static constexpr double kOnGridMinPct = 98.0;
    void predictedOutcome(bool droppedLate) {
        if (!active_) return;
        const unsigned slot = lateCount_ % kLateWindow;
        const bool was = (lateBits_[slot / 64] >> (slot % 64)) & 1u;
        if (lateCount_ >= kLateWindow && was) --lateDrops_;
        if (droppedLate) { lateBits_[slot / 64] |= std::uint64_t{1} << (slot % 64); ++lateDrops_; }
        else lateBits_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
        ++lateCount_;
        if (lateDrops_ > kLateDrops) { calm_ = 0; leave(PushLowDelayExit::Late); }
    }
    void gridWindow(double onGridPct, bool inModeWholeWindow, bool interpolatedWholeWindow) {
        if (interpolatedWholeWindow && onGridPct >= 0.0) { interpolatedOnGrid_ = onGridPct; interpolatedKnown_ = true; }
        if (!active_ || !inModeWholeWindow || !(onGridPct >= 0.0)) { offGridRun_ = 0; return; }
        offGridRun_ = onGridPct < kOnGridMinPct ? offGridRun_ + 1 : 0;
        if (offGridRun_ >= kOffGridWindows && interpolatedKnown_ && interpolatedOnGrid_ >= kOnGridMinPct) { calm_ = 0; leave(PushLowDelayExit::Late); }
    }
    bool interpolatedOnGridKnown() const { return interpolatedKnown_; }
    bool request() const { return active_; }
    bool active() const { return active_; }
    unsigned calmNeeded() const { return calmNeeded_; }
    void pair(const PushLowDelayPair& p) {
        if (p.ranPredicted) ++predictedPairs_;
        PushLowDelayExit why = PushLowDelayExit::None;
        if (!p.wanted) why = PushLowDelayExit::Off;
        else if (!p.eligible) why = PushLowDelayExit::Ineligible;
        else if (p.held) why = PushLowDelayExit::Held;
        else if (p.skipped) why = PushLowDelayExit::Skipped;
        else if (p.refused) why = PushLowDelayExit::Refused;
        if (why != PushLowDelayExit::None) {
            calm_ = 0;
            if (active_) leave(why);
            return;
        }
        if (active_) {
            if (++stay_ >= kSettledPairs) calmNeeded_ = kCalmPairs;
            return;
        }
        if (++calm_ >= calmNeeded_) {
            active_ = true;
            stay_ = 0;
            ++entries_;
            lateCount_ = lateDrops_ = offGridRun_ = 0;
            lateBits_[0] = lateBits_[1] = 0;
        }
    }
    // The counters of a finished predicted frame (pushPredictFrameUsable). Unusable: leave at once, calm restarts.
    void usable(bool ok) {
        if (policy_ == PushLowDelayPolicy::StayIn) {
            if (!ok) ++unusableShown_;
            if (!ok && !active_) calm_ = 0;   // a verdict arriving after the schedule left: the calm run starts again
            // The last kSustainWindow verdicts as a ring of bits.
            const unsigned slot = sustainCount_ % sustainWindow_;
            const bool was = (sustainBits_[slot / 64] >> (slot % 64)) & 1u;
            if (sustainCount_ >= sustainWindow_ && was) --sustainBad_;
            if (!ok) { sustainBits_[slot / 64] |= std::uint64_t{1} << (slot % 64); ++sustainBad_; }
            else sustainBits_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
            ++sustainCount_;
            if (active_ && sustainBad_ >= sustainBadNeeded_) {
                calm_ = 0;
                leave(PushLowDelayExit::Unusable);
            }
            return;
        }
        if (ok) return;
        calm_ = 0;
        if (active_) leave(PushLowDelayExit::Unusable);
    }
    std::uint64_t entries() const { return entries_; }
    std::uint64_t exits() const { return exits_[0]; }
    std::uint64_t exits(PushLowDelayExit why) const { return exits_[static_cast<unsigned>(why)]; }
    std::uint64_t switches() const { return entries_ + exits_[0]; }
    std::uint64_t predictedPairs() const { return predictedPairs_; }

private:
    void leave(PushLowDelayExit why) {
        active_ = false;
        ++exits_[0];
        ++exits_[static_cast<unsigned>(why)];
        if ((policy_ == PushLowDelayPolicy::StayIn && why == PushLowDelayExit::Unusable) || why == PushLowDelayExit::Late) {
            // A sustained failure: a long calm run before trying again, doubled if it failed again soon.
            // Within kSettledPairs x 4 pairs (about 8.5 s) of the last entry: twice the last wait; otherwise the first wait.
            sustainCalm_ = sustainCalm_ != 0 && stay_ < kSettledPairs * 4 ? std::min(kStayInMaxCalmPairs, sustainCalm_ * 2) : stayInCalm_;
            calmNeeded_ = sustainCalm_;
            sustainCount_ = sustainBad_ = 0;
            sustainBits_[0] = sustainBits_[1] = 0;
            return;
        }
        // Off is the user's choice and Ineligible a change of settings: no back-off for those.
        if (why != PushLowDelayExit::Off && why != PushLowDelayExit::Ineligible && stay_ < kSettledPairs)
            calmNeeded_ = calmNeeded_ * 2 > kMaxCalmPairs ? kMaxCalmPairs : calmNeeded_ * 2;
    }
    PushLowDelayPolicy policy_ = PushLowDelayPolicy::LeaveOnUnusable;
    unsigned sustainCount_ = 0, sustainBad_ = 0, sustainCalm_ = 0;
    unsigned sustainWindow_ = kSustainWindow, sustainBadNeeded_ = kSustainBad, stayInCalm_ = kStayInCalmPairs;
    unsigned lateCount_ = 0, lateDrops_ = 0, offGridRun_ = 0;
    double interpolatedOnGrid_ = 0.0;
    bool interpolatedKnown_ = false;
    std::uint64_t lateBits_[2] = {0, 0};
    std::uint64_t sustainBits_[2] = {0, 0};
    std::uint64_t unusableShown_ = 0;
    bool active_ = false;
    unsigned calm_ = 0, stay_ = 0, calmNeeded_ = kCalmPairs;
    std::uint64_t entries_ = 0, predictedPairs_ = 0;
    std::uint64_t exits_[static_cast<unsigned>(PushLowDelayExit::Count)] = {};
};

// What a schedule's learner is given (LivePairLatency::select's legacy delay and sampling allowance); while it has fewer
// than 8 fresh samples it returns `start`, and the most it may ever select is start + allowance. `interpolatedLearned`:
// the interpolate learner's phase on the last pair timed with it after at least 8 interpolated pairs in a row (0: none).
//  - predicted: start = interpolatedLearned - T/3 (Package 14's legacy - T/3 before any); the most it may select is
//    Package 14's ceiling (legacy + allowance) - T/3, so a predicted pair is never given more delay.
//  - interpolate, the predicted schedule ran within the last second: start = the larger of its phase + T/3 (the first
//    interpolated frame one slot after the last P) and interpolatedLearned (what interpolation needed before: starting
//    lower would show up to 8 pairs of late frames); the most it may select stays Package 14's ceiling.
//  - interpolate otherwise (and always with the mode off): Package 14's legacy and allowance, exactly.
// A pair is timed with the predicted learner only when it RAN predicted (out.predictedPair); a refused or skipped pair is
// timed with the interpolate learner, so gridEarly and the rejection hold work on it as in Package 14.
struct PushLowDelayPhaseArgs { std::int64_t start = 0, allowance = 0; };
inline PushLowDelayPhaseArgs pushLowDelayPhaseArgs(bool predicted, std::int64_t now100ns, std::int64_t interval100ns, std::int64_t legacy100ns,
                                                   std::int64_t allowance100ns, std::int64_t interpolatedLearned100ns,
                                                   std::int64_t predictedPhase100ns, std::int64_t lastPredictedAt100ns) {
    const std::int64_t slot = interval100ns / 3, ceiling = legacy100ns + allowance100ns;
    if (predicted) {
        // Never above Package 14's ceiling minus a slot (no ratchet through the other learner's start).
        std::int64_t start = (interpolatedLearned100ns > slot ? interpolatedLearned100ns : legacy100ns) - slot;
        if (start > ceiling - slot) start = ceiling - slot;
        return {start, ceiling - slot - start};
    }
    if (lastPredictedAt100ns > 0 && now100ns - lastPredictedAt100ns < 10000000 && predictedPhase100ns > 0) {
        std::int64_t start = predictedPhase100ns + slot;
        if (interpolatedLearned100ns > start) start = interpolatedLearned100ns;
        if (start > ceiling) start = ceiling;
        return {start, ceiling - start};
    }
    return {legacy100ns, allowance100ns};
}

// The interpolate learner's phase is recorded as `interpolatedLearned` only on a pair timed with it after at least this
// many interpolated pairs in a row (its samples are its own by then).
constexpr unsigned kPushLowDelayLearnedRun = 8;

// Which learner starts afresh before this pair's phase is chosen, so a switch lands on the grid (its first 8 pairs use
// the start pushLowDelayPhaseArgs gives, then it moves down at most LivePairLatency's step a pair, under 1 ms):
//  - after a pair that ran predicted, the interpolate learner (its samples are from before; a refused or skipped pair
//    falls back to it within the same pair);
//  - when asking for a predicted pair after one that was not, the predicted learner.
// Never with the mode off (no pair ran predicted, none is asked).
struct PushLowDelayResets { bool interpolate = false, predicted = false; };
inline PushLowDelayResets pushLowDelayResets(bool askPredicted, bool previousRanPredicted) {
    return {previousRanPredicted, askPredicted && !previousRanPredicted};
}

// Admission of a pair asked to run predicted (EngineController's admitFg; the simulator calls the same). Today's check
// compares the whole group's cost (base + the FG group P95) with the deadline of the last interpolated frame, a + 2d/3.
// In the predicted schedule that frame (I) is FIRST and is followed by P, so the group cost is compared with I's
// deadline moved by what the group costs after I (group FG P95 - the Fg1 stage P95), never past P's own deadline.
// Equivalent to "base + Fg1 fits I's deadline, and the whole group fits P's". Missing estimates: I's deadline unchanged
// (stricter). Times in 100 ns, costs in ms.
inline std::int64_t pushLowDelayAdmissionDeadline(std::int64_t deadlineI100ns, std::int64_t deadlineP100ns, double groupFgMs, double fg1Ms) {
    if (!(groupFgMs > 0) || !(fg1Ms > 0)) return deadlineI100ns;
    const double after = groupFgMs > fg1Ms ? groupFgMs - fg1Ms : 0.0;
    const std::int64_t moved = deadlineI100ns + static_cast<std::int64_t>(after * 10000.0);
    return moved < deadlineP100ns ? moved : deadlineP100ns;
}

// Whether the row changes the graph's shape (EnhancementSettings::lowDelayShapesGraph, requiresGraphRebuild): the pass
// exists only where VFG generates at 3X or more (pushPredictCreate), so elsewhere switching the row rebuilds nothing.
inline bool pushLowDelayShapesGraph(bool row, bool vfg, std::uint32_t configuredMultiplier) {
    return row && vfg && configuredMultiplier >= 3;
}

// Process-wide state between the interface and the engine (as pushSmoothShared). The interface writes `wanted` (pref
// pushLowDelay); the engine reads it once per pair and writes back `state`. Relaxed atomics: one value each.
enum class PushLowDelayState : std::uint8_t {
    Idle = 0,         // no live capture-card pair since start, or the row is off
    Calm,             // on, waiting for a calm run of pairs (or after a fallback)
    Active,           // on, pairs run predicted
    Starting,         // on, the running graph was built before the row was on: the rebuild that adds the pass is on its way
    NotHere,          // on, but this source / settings cannot use it (reason: PushLowDelayShared::refusal)
    Unavailable,      // on, the graph was built for it but has no pass (creation failed: the log)
    Suspended,        // on, paused while a measurement or a comparison clip runs (reason: PushLowDelayShared::suspend)
};
// What pauses the mode (wanted = false for the schedule: it leaves without back-off and comes back after a calm run):
// the frame quality test (it withholds frames), the optimiser's benchmark, automatic HUD detection, a comparison clip.
enum class PushLowDelaySuspend : std::uint8_t { None = 0, QualityTest, Benchmark, HudDetection, ComparisonClip };
inline PushLowDelaySuspend pushLowDelaySuspendFor(bool qualityTest, bool benchmark, bool hudDetection, bool comparisonClip) {
    if (qualityTest) return PushLowDelaySuspend::QualityTest;
    if (benchmark) return PushLowDelaySuspend::Benchmark;
    if (hudDetection) return PushLowDelaySuspend::HudDetection;
    if (comparisonClip) return PushLowDelaySuspend::ComparisonClip;
    return PushLowDelaySuspend::None;
}
inline const char* pushLowDelaySuspendName(PushLowDelaySuspend s) {
    switch (s) {
    case PushLowDelaySuspend::None: return "none";
    case PushLowDelaySuspend::QualityTest: return "quality-test";
    case PushLowDelaySuspend::Benchmark: return "benchmark";
    case PushLowDelaySuspend::HudDetection: return "hud-detection";
    case PushLowDelaySuspend::ComparisonClip: return "comparison-clip";
    }
    return "?";
}
struct PushLowDelayShared {
    std::atomic<bool> wanted{false};               // the row (pref pushLowDelay)
    std::atomic<std::uint8_t> state{0};            // PushLowDelayState, written by the engine once a pair
    std::atomic<std::uint8_t> refusal{0};          // pipeline::PushPredictRefusal behind NotHere / Unavailable
    std::atomic<std::uint8_t> suspend{0};          // PushLowDelaySuspend behind Suspended
    // Review 2, SHOULD 9: VFG refused or failed a FrameIndex-2-only request (set by the graph, once); the mode is then
    // unavailable for the session. The bridge clears it when the row is switched, which allows one new attempt.
    std::atomic<bool> vfgRefused{false};
};
// Whether the engine wants the schedule: the row, or the kit's switch (VEYRA_EXP_RP_FG_SCHEDULE=onepredicted set before
// PushFPS started; the interface never sets it).
inline bool pushLowDelayWanted(bool switchInEnvironment, const PushLowDelayShared& shared) {
    return shared.wanted.load(std::memory_order_relaxed) || switchInEnvironment;
}
inline PushLowDelayShared& pushLowDelayShared() {
    static PushLowDelayShared shared;
    return shared;
}
inline const char* pushLowDelayStateName(PushLowDelayState s) {
    switch (s) {
    case PushLowDelayState::Idle: return "idle";
    case PushLowDelayState::Calm: return "calm";
    case PushLowDelayState::Active: return "active";
    case PushLowDelayState::Starting: return "starting";
    case PushLowDelayState::NotHere: return "not-here";
    case PushLowDelayState::Unavailable: return "unavailable";
    case PushLowDelayState::Suspended: return "suspended";
    default: return "?";
    }
}
// What the engine reports for one pair. cardVfg3x: a capture card with VFG at 3X; passAsked: the running graph was built
// wanting the pass (the row's setting or the kit's switch: EnhanceGraph::predictAsked); staticRefusal: what the graph
// would refuse before any per-pair reason (EnhanceGraph::predictStaticRefusal, pushPredictStaticRefusal).
inline PushLowDelayState pushLowDelayStateFor(bool wanted, PushLowDelaySuspend suspend, bool cardVfg3x, bool passAsked,
                                              pipeline::PushPredictRefusal staticRefusal, bool active) {
    using pipeline::PushPredictRefusal;
    if (!wanted) return PushLowDelayState::Idle;
    if (!cardVfg3x) return PushLowDelayState::NotHere;
    if (staticRefusal == PushPredictRefusal::SwitchOff || staticRefusal == PushPredictRefusal::PassUnavailable)
        return passAsked ? PushLowDelayState::Unavailable : PushLowDelayState::Starting;
    if (staticRefusal == PushPredictRefusal::VfgRefused) return PushLowDelayState::Unavailable;
    if (staticRefusal != PushPredictRefusal::Ok) return PushLowDelayState::NotHere;
    if (suspend != PushLowDelaySuspend::None) return PushLowDelayState::Suspended;
    return active ? PushLowDelayState::Active : PushLowDelayState::Calm;
}

// For the [fg-sched] and [capture-timing] lines.
inline std::string pushLowDelayExitsText(const PushLowDelaySchedule& s) {
    std::string t;
    for (unsigned i = static_cast<unsigned>(PushLowDelayExit::Off); i < static_cast<unsigned>(PushLowDelayExit::Count); ++i) {
        if (!t.empty()) t += ',';
        t += pushLowDelayExitName(static_cast<PushLowDelayExit>(i));
        t += ':';
        t += std::to_string(s.exits(static_cast<PushLowDelayExit>(i)));
    }
    return t;
}

}  // namespace veyra::engine
