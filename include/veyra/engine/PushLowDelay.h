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
// graph's shape (pushLowDelayShapesGraph), so the FIRST switch-on with VFG at 3X rebuilds the graph as HUD Lock does (and
// runs the motion source's self-check once); the pass is then kept for the run (pushLowDelayKeepPass), so later switches
// rebuild nothing. With the row never on in a run, the pass is never created. The kit's switch creates it for the process.
//
// Rollback (review 1 SHOULD 12): parts LowDelaySchedule16 and LowDelayMigrate16 in PushLegacy.h (VEYRA_TEST_RP_LEGACY_
// LOWDELAY_SCHEDULE / _LOWDELAY_MIGRATE, both covered by _PACKAGE15 = 1). Reasoning for what they cover, with the row OFF: the admission change (SHOULD 9,
// FgRecoveryBudget::admit's groupFgMs) is -1 unless the engine asked for a predicted pair, which needs the row, so the
// row-off admission is Package 15's; the latency split (MUST 2: GetClockCalibration after the base submission, at most
// once a second) runs with the row off but only feeds [fg-health] submitToGpuStart*, a log figure no decision reads; the
// capture stall ride, the engine-stall handling, the entry guard, the grid baseline and the cost rule only feed this
// schedule. So the row-off frame path cannot change through them; the rollback part is for the row-ON schedule (Package
// 15's interruption rule and exits: setLegacyPackage15, and the entry guard in EngineController) and for the one-time
// preference reset (pushLowDelayMigrate). Package 15's motion source (the strict GPU DIS) is deliberately not offered.
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
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "veyra/pipeline/PushPredict.h"

namespace veyra::engine {

enum class PushLowDelayExit : std::uint8_t { None = 0, Off, Ineligible, Refused, Unusable, Skipped, Held, Late, Dear, Count };
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
    case PushLowDelayExit::Dear: return "dear";               // Package 16: the predicted frame's measured GPU time is above a slot
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
    // Package 16: not eligible only because this pair's multiplier is not 3 (Smoothness Mode stepped down, the adaptive
    // multiplier or the output cap). The schedule pauses (no back-off) and resumes only after kMultiplierResumePairs
    // eligible calm pairs at 3X, so a multiplier that changes back and forth cannot make the schedule switch at its pace.
    bool multiplierPause = false;
    // Capture stall ride-through (the display branch's finding: about every quarter hour his card delivers one sample
    // 92-158 ms late, then a burst of the held ones; P15_DISPLAY_AND_BASE_PATH.md): the pair is part of such a
    // discontinuity (PushCaptureStallRide). The whole stall counts as one interruption and never makes the schedule
    // leave; its pairs neither count as calm nor restart the calm run, and late predicted frames in it are not counted.
    bool stall = false;
    bool engineStall = false;   // the stall is the engine's own (PushCaptureStallRide::lastWasEngine): not even one interruption
};

// Which pairs belong to a capture discontinuity: the pair whose real frame arrived more than kGapIntervals content
// intervals after the previous one, the burst after it (arrivals closer than half an interval, at most as many as the
// gap held plus one), and one pair more after the burst (its timing re-settles). Also an ENGINE discontinuity: a real
// frame that waited more than kLagIntervals content intervals between its arrival and the pair's decision (the engine
// itself stalled: gate round 3, run 45551814743, WARP logged [engine-stall] loops of 45-65 ms several times a second,
// and the predicted frames those delayed were dropped late; not the schedule's doing). Pure, tested.
class PushCaptureStallRide {
public:
    static constexpr std::int64_t kGapIntervals = 3, kLagIntervals = 1;
    bool pair(std::int64_t arrivalDelta100ns, std::int64_t interval100ns, std::int64_t processLag100ns = 0) {
        engine_ = false;
        if (interval100ns > 0 && processLag100ns > kLagIntervals * interval100ns) { ++engineStalls_; engine_ = true; return true; }
        if (interval100ns <= 0 || arrivalDelta100ns <= 0) { left_ = 0; grace_ = false; return false; }
        if (arrivalDelta100ns > kGapIntervals * interval100ns) {
            ++stalls_;
            left_ = static_cast<unsigned>(arrivalDelta100ns / interval100ns) + 1;
            grace_ = true;
            return true;
        }
        if (left_ > 0 && arrivalDelta100ns * 2 < interval100ns) { --left_; return true; }
        left_ = 0;
        if (grace_) { grace_ = false; return true; }
        return false;
    }
    std::uint64_t stalls() const { return stalls_; }
    std::uint64_t engineStalls() const { return engineStalls_; }
    bool lastWasEngine() const { return engine_; }
private:
    unsigned left_ = 0;
    bool grace_ = false, engine_ = false;
    std::uint64_t stalls_ = 0, engineStalls_ = 0;
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

// Package 16, item 3 (forensics F1): what a skipped, held or refused pair (an INTERRUPTION: the pair runs today's way)
// does to the schedule.
//  - Strict (Package 15): leaves at once; a stay shorter than kSettledPairs doubles the calm run needed (8 -> 16 -> ...
//    -> 240 consecutive calm pairs). Fed i.i.d. skipped pairs, that rule runs about 1 % of pairs predicted at 2 % skips
//    (decision code only; P15_NO_GAIN_FORENSICS.md section 1.9, and the test lowdelay_tolerant_rule_runs_through_2_percent_skips).
//  - Tolerant (Package 16, the default): an interruption neither ends the mode nor doubles anything. The schedule leaves
//    only when interruptions are FREQUENT: more than kLeaveInterrupts of the last kInterruptWindow pairs (7 of 30). It
//    enters when kCalmPairs of the last kEnterWindow pairs were calm (8 of 10) and at most kEnterInterrupts of the last
//    kInterruptWindow were interrupted (3 of 30: the gap between 3 and 7 is the hysteresis that keeps a GPU with frequent
//    skips from switching in and out). A sustained back-off (unusable predictions under StayIn, late frames) counts calm
//    pairs without restarting at an interruption. Off and Ineligible (row off, a pause, a settings change, the multiplier
//    leaving 3X) start the counting afresh, without back-off, as before. Unusable-prediction and late-frame exits unchanged.
enum class PushLowDelayInterrupts : std::uint8_t { Strict = 0, Tolerant = 1 };
inline const char* pushLowDelayInterruptsName(PushLowDelayInterrupts r) { return r == PushLowDelayInterrupts::Tolerant ? "tolerant" : "strict"; }

// The last n (<= 64) values of a yes/no stream.
struct PushLowDelayBits {
    std::uint64_t bits = 0;
    unsigned size = 0, filled = 0, ones = 0, next = 0;
    explicit PushLowDelayBits(unsigned n = 30) : size(std::clamp(n, 1u, 64u)) {}
    void clear() { bits = 0; filled = ones = next = 0; }
    void push(bool v) {
        const std::uint64_t bit = std::uint64_t{1} << next;
        if (filled == size) { if (bits & bit) --ones; } else ++filled;
        if (v) { bits |= bit; ++ones; } else bits &= ~bit;
        next = (next + 1) % size;
    }
};

class PushLowDelaySchedule {
public:
    static constexpr unsigned kCalmPairs = 8, kMaxCalmPairs = 240, kSettledPairs = 64;
    // Package 16: calm pairs at 3X needed after a multiplier pause (1 s at a 30 FPS game: longer than the 0.5 s gates of
    // Smoothness Mode's step up and of the adaptive multiplier, so one gated change back and forth is one pause).
    static constexpr unsigned kMultiplierResumePairs = 30;
    // Package 16, item 3 (PushLowDelayInterrupts::Tolerant).
    static constexpr unsigned kInterruptWindow = 30, kLeaveInterrupts = 6, kEnterInterrupts = 3, kEnterWindow = 10;
    void setInterruptRule(PushLowDelayInterrupts rule) { rule_ = rule; }
    // Rollback part LowDelaySchedule16 (PushLegacy.h, VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE / _PACKAGE15 = 1): Package 15's
    // schedule with the row on: the strict rule, no stall ride (a stall pair is an ordinary pair), no stop for the playback,
    // the grid exit at a fixed kOnGridMinPct when interpolation's own median is at kOnGridMinPct or better.
    void setLegacyPackage15(bool on) { legacy15_ = on; if (on) rule_ = PushLowDelayInterrupts::Strict; }
    // The user switched the row off and on again (PushLowDelayShared::retryEpoch, a real off -> on in the interface): an
    // explicit retry. The back-off of earlier exits (the long calm run, the stop for the playback) starts over, so the
    // mode tries again after kCalmPairs calm pairs. Not under the Package 15 rollback (its back-off stayed).
    void retry() {
        if (legacy15_) return;
        calmNeeded_ = kCalmPairs; sustainCalm_ = 0; resumeNeeded_ = 0; calm_ = 0;
        stopped_ = PushLowDelayExit::None; sustainedExits_ = {};
        lastExit_ = PushLowDelayExit::None;   // REL16 SHOULD 4: the row does not read "the row was switched off" while on
    }
    bool legacyPackage15() const { return legacy15_; }
    PushLowDelayInterrupts interruptRule() const { return rule_; }
    std::uint64_t interruptions() const { return interruptions_; }
    unsigned recentInterruptions() const { return interrupts_.ones; }
    // Progress of the entry (for the row): calm pairs among the last kEnterWindow, and what the entry needs.
    unsigned recentCalm() const { return recentCalm_.ones; }
    static constexpr unsigned kSustainWindow = 60, kSustainBad = 45, kStayInCalmPairs = 300, kStayInMaxCalmPairs = 1800;
    // StayIn's sustained-failure rule: at least `bad` of the last `window` (<= 128) predicted frames unusable leaves;
    // the first return needs `calm` calm pairs (doubled, up to kStayInMaxCalmPairs, if it fails again soon).
    void setSustain(unsigned window, unsigned bad, unsigned calm) {
        sustainWindow_ = std::clamp(window, 1u, 128u); sustainBadNeeded_ = std::clamp(bad, 1u, sustainWindow_); stayInCalm_ = std::max(calm, kCalmPairs);
    }
    void reset() { const auto policy = policy_; const auto rule = rule_; *this = PushLowDelaySchedule{}; policy_ = policy; rule_ = rule; }
    void setPolicy(PushLowDelayPolicy policy) { policy_ = policy; }
    PushLowDelayPolicy policy() const { return policy_; }
    std::uint64_t unusableShown() const { return unusableShown_; }
    // Review 2, SHOULD 2 (the owner's steadiness first): leave, with the sustained back-off, when more than kLateDrops of
    // the last kLateWindow predicted frames were dropped late, or after kOffGridWindows one-second windows in a row below
    // the baseline on the 1 ms grid while the mode ran for the whole window. Review 1 SHOULD 10: the baseline is the
    // MEDIAN of the last kBaselineWindows whole windows of interpolation (his own steadiness: 95-96 % on the grid in his
    // logs, where a fixed 98 % would have left all the time) minus kGridMarginPct (and the window is below kOnGridMinPct);
    // with fewer than kBaselineMin of them
    // there is no grid exit (entry does not wait for the baseline: a 3 s wait cost shown frames and steadiness in the
    // simulator, and the late and dear exits apply from the first pair). Without a comparison with interpolation the simulator left
    // near the GPU limit for a schedule that is LESS steady there (near-limit 98.2 -> 93.2 %, overload 97.3 -> 34.7 % on
    // the grid, SIMULATED).
    static constexpr unsigned kLateWindow = 60, kLateDrops = 2, kOffGridWindows = 3, kBaselineWindows = 10, kBaselineMin = 3;
    static constexpr double kGridMarginPct = 1.0, kOnGridMinPct = 98.0;
    // A predicted frame's outcome arrives a few pairs after its pair (presentation): late drops of a stall's pairs are
    // still ignored for kStallTailPairs pairs after it.
    static constexpr unsigned kStallTailPairs = 4;
    void predictedOutcome(bool droppedLate) {
        ++outcomesSeen_; if (droppedLate) ++lateSeen_;        // evidence for the log, whatever the state (not used by the rule)
        if (!active_ || inStall_ || stallTail_ > 0) return;   // a stall's late frames are the stall's, not the schedule's
        const unsigned slot = lateCount_ % kLateWindow;
        const bool was = (lateBits_[slot / 64] >> (slot % 64)) & 1u;
        if (lateCount_ >= kLateWindow && was) --lateDrops_;
        if (droppedLate) { lateBits_[slot / 64] |= std::uint64_t{1} << (slot % 64); ++lateDrops_; }
        else lateBits_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
        ++lateCount_;
        if (lateDrops_ > kLateDrops) { calm_ = 0; leave(PushLowDelayExit::Late); }
    }
    void gridWindow(double onGridPct, bool inModeWholeWindow, bool interpolatedWholeWindow) {
        if (interpolatedWholeWindow && onGridPct >= 0.0) { baseline_[baselineNext_] = onGridPct; baselineNext_ = (baselineNext_ + 1) % kBaselineWindows; baselineCount_ = std::min(baselineCount_ + 1, kBaselineWindows); }
        if (!active_ || !inModeWholeWindow || !(onGridPct >= 0.0)) { offGridRun_ = 0; return; }
        const double floor = legacy15_ ? (interpolatedOnGridBaseline() >= kOnGridMinPct ? kOnGridMinPct : -1.0)
                                       : interpolatedOnGridBaseline() - kGridMarginPct;
        // Below interpolation's own median by the margin AND below kOnGridMinPct (a window at 98 % or better is steady
        // enough whatever interpolation managed; kept from review 2's rule).
        offGridRun_ = interpolatedOnGridKnown() && onGridPct < floor && onGridPct < kOnGridMinPct ? offGridRun_ + 1 : 0;
        if (offGridRun_ >= kOffGridWindows) { ++gridExits_; calm_ = 0; leave(PushLowDelayExit::Late); }
    }
    // Evidence for the log ([fg-sched] predictedLate=late/outcomes gridExits=): every predicted frame's outcome, late or
    // shown, also outside the mode's counting (stalls), and how many "late" exits were the grid rule's.
    std::uint64_t predictedLateSeen() const { return lateSeen_; }
    std::uint64_t predictedOutcomesSeen() const { return outcomesSeen_; }
    unsigned gridExits() const { return gridExits_; }
    bool interpolatedOnGridKnown() const { return baselineCount_ >= kBaselineMin; }
    // The median of the stored interpolated windows (-1 with none).
    double interpolatedOnGridBaseline() const {
        if (baselineCount_ == 0) return -1.0;
        double v[kBaselineWindows];
        for (unsigned i = 0; i < baselineCount_; ++i) v[i] = baseline_[i];
        std::sort(v, v + baselineCount_);
        return baselineCount_ % 2 ? v[baselineCount_ / 2] : 0.5 * (v[baselineCount_ / 2 - 1] + v[baselineCount_ / 2]);
    }
    // Package 16, item 4 (forensics F2): the predicted frame's measured GPU time (PushPredictPass stage "total"). P's list
    // runs after I's on the graph queue and before the next pair's base list, so a P that needs more than one output slot
    // (T/3, `slotMs`) on most pairs takes GPU time the next pair's real frame needs. Leave, with the sustained back-off,
    // when the nearest-rank P95 of the last kCostWindow measured P is above the slot (at least kCostMinSamples of them).
    // A rule from the queue order (reasoning), not from a measurement of its effect. kCostMinSamples is 20: with fewer the
    // nearest-rank P95 is the maximum, and one slow first P after an entry (gate round 2, run 45549334998, WARP: 10-15 ms
    // for the first warp of an entry, 0.3 ms after) made the mode leave "dear" after exactly 15 predicted pairs.
    static constexpr unsigned kCostWindow = 30, kCostMinSamples = 20;
    void predictedCost(double totalMs, double slotMs) {
        if (!(totalMs >= 0) || !(slotMs > 0)) return;
        costs_[costCount_ % kCostWindow] = totalMs;
        ++costCount_;
        const unsigned n = costCount_ < kCostWindow ? costCount_ : kCostWindow;
        if (!active_ || n < kCostMinSamples) return;
        double v[kCostWindow];
        for (unsigned i = 0; i < n; ++i) v[i] = costs_[i];
        std::sort(v, v + n);
        const unsigned rank = (n * 95 + 99) / 100;
        lastCostP95_ = v[rank - 1];
        if (lastCostP95_ > slotMs) { calm_ = 0; costCount_ = 0; leave(PushLowDelayExit::Dear); }
    }
    double lastCostP95() const { return lastCostP95_; }
    bool request() const { return active_; }
    bool active() const { return active_; }
    unsigned calmNeeded() const { return calmNeeded_; }
    void pair(const PushLowDelayPair& p) {
        if (p.ranPredicted) ++predictedPairs_;
        // A capture stall (PushLowDelayPair::stall): one interruption for the whole stall, never an exit.
        if (p.wanted && p.eligible && p.stall && !legacy15_) {
            inStall_ = true;
            stallTail_ = kStallTailPairs;
            if (!stallCounted_ && !p.engineStall) {
                stallCounted_ = true;
                ++stallRides_;
                ++interruptions_;
                if (rule_ == PushLowDelayInterrupts::Tolerant) interrupts_.push(true);
            }
            return;
        }
        inStall_ = stallCounted_ = false;
        if (stallTail_ > 0) --stallTail_;
        PushLowDelayExit why = PushLowDelayExit::None;
        if (!p.wanted) { why = PushLowDelayExit::Off; stopped_ = PushLowDelayExit::None; sustainedExits_ = {}; }   // the row off: a later switch-on tries again
        else if (!p.eligible) why = PushLowDelayExit::Ineligible;
        else if (p.held) why = PushLowDelayExit::Held;
        else if (p.skipped) why = PushLowDelayExit::Skipped;
        else if (p.refused) why = PushLowDelayExit::Refused;
        if (rule_ == PushLowDelayInterrupts::Tolerant) { tolerantPair(p, why); return; }
        if (why != PushLowDelayExit::None) {
            calm_ = 0;
            if (why == PushLowDelayExit::Ineligible && p.multiplierPause) {
                resumeNeeded_ = kMultiplierResumePairs;
                if (!multiplierPaused_) ++multiplierPauses_;
                multiplierPaused_ = true;
            }
            if (active_) leave(why);
            return;
        }
        multiplierPaused_ = false;
        if (active_) {
            if (++stay_ >= kSettledPairs) calmNeeded_ = kCalmPairs;
            return;
        }
        if (stopped_ != PushLowDelayExit::None) return;   // stopped for the playback (SHOULD 4 / 5)
        if (++calm_ >= std::max(calmNeeded_, resumeNeeded_)) enter();
    }
    // The counters of a finished predicted frame (pushPredictFrameUsable). Unusable: leave at once, calm restarts.
    void usable(bool ok) {
        if (inStall_) return;   // a verdict on a predicted frame of a capture stall's burst: the stall's, not the prediction's
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
    unsigned calmCount() const { return calm_; }
    // Package 16, item 6: the last reason the schedule left (None: never), and how far the next entry is (for the row):
    // calm pairs so far against what the entry needs (a back-off or a multiplier pause: the long count; otherwise the
    // last kEnterWindow pairs). Both 0 while active.
    PushLowDelayExit lastExit() const { return lastExit_; }
    void entryProgress(unsigned& have, unsigned& need) const {
        if (active_) { have = need = 0; return; }
        const unsigned longNeed = std::max(calmNeeded_, resumeNeeded_);
        if (rule_ == PushLowDelayInterrupts::Strict || longNeed > kCalmPairs) { need = longNeed; have = std::min(calm_, need); return; }
        need = kCalmPairs; have = std::min(recentCalm_.ones, need);
    }
    std::uint64_t exits(PushLowDelayExit why) const { return exits_[static_cast<unsigned>(why)]; }
    std::uint64_t switches() const { return entries_ + exits_[0]; }
    std::uint64_t predictedPairs() const { return predictedPairs_; }
    // Package 16: how often the pair's multiplier left 3X while the row was on (Smoothness Mode, adaptive, cap).
    std::uint64_t multiplierPauses() const { return multiplierPauses_; }
    std::uint64_t stallRides() const { return stallRides_; }   // capture stalls ridden through (PushLowDelayPair::stall)
    // Review 1 SHOULD 4 / 5: after kStopAfterSustained sustained exits of the same reason (dear, late, unusable under
    // StayIn, or interruptions the mode causes itself: selfCaused) the mode stops for the rest of the playback (a new
    // playback starts a new schedule) or until the row is switched off; stoppedFor() names the reason (None: running).
    static constexpr unsigned kStopAfterSustained = 3;
    // SHOULD 5: interruptions while predicted against while interpolating, as exponential shares over about 30 s of pairs
    // at 30 FPS; "self-caused" when both have kShareMinSamples and the predicted share is kSelfMargin above the other.
    static constexpr double kShareAlpha = 1.0 / 900.0, kSelfMargin = 0.05;
    static constexpr unsigned kShareMinSamples = 90;
    PushLowDelayExit stoppedFor() const { return stopped_; }
    bool selfCaused() const {
        return activeSamples_ >= kShareMinSamples && idleSamples_ >= kShareMinSamples && activeShare_ > idleShare_ + kSelfMargin;
    }
    double activeInterruptShare() const { return activeShare_; }
    double idleInterruptShare() const { return idleShare_; }
    bool inStall() const { return inStall_; }
    unsigned resumeNeeded() const { return resumeNeeded_; }

private:
    void enter() {
        active_ = true;
        resumeNeeded_ = 0;
        stay_ = 0;
        ++entries_;
        lateCount_ = lateDrops_ = offGridRun_ = 0;
        lateBits_[0] = lateBits_[1] = 0;
    }
    void tolerantPair(const PushLowDelayPair& p, PushLowDelayExit why) {
        if (why == PushLowDelayExit::Off || why == PushLowDelayExit::Ineligible) {
            calm_ = 0;
            interrupts_.clear();
            recentCalm_.clear();
            if (why == PushLowDelayExit::Ineligible && p.multiplierPause) {
                resumeNeeded_ = kMultiplierResumePairs;
                if (!multiplierPaused_) ++multiplierPauses_;
                multiplierPaused_ = true;
            }
            if (active_) leave(why);
            return;
        }
        multiplierPaused_ = false;
        const bool interrupted = why != PushLowDelayExit::None;
        interrupts_.push(interrupted);
        recentCalm_.push(!interrupted);
        if (interrupted) ++interruptions_;
        // SHOULD 5: the two shares (held / skipped pairs count in both; refused exists only while asked).
        double& share = active_ ? activeShare_ : idleShare_;
        unsigned& samples = active_ ? activeSamples_ : idleSamples_;
        share += ((interrupted ? 1.0 : 0.0) - share) * (samples < 900 ? 1.0 / double(samples + 1) : kShareAlpha);
        if (samples < 1000000) ++samples;
        if (active_) {
            if (interrupted && interrupts_.ones > kLeaveInterrupts) { calm_ = 0; leave(why); }
            else if (!interrupted && ++stay_ >= kSettledPairs) calmNeeded_ = kCalmPairs;
            return;
        }
        if (interrupted) return;   // neither counts nor restarts the calm run
        ++calm_;
        if (stopped_ != PushLowDelayExit::None) return;   // stopped for the playback (SHOULD 4 / 5)
        if (calm_ >= std::max(calmNeeded_, resumeNeeded_) && recentCalm_.ones >= kCalmPairs && interrupts_.ones <= kEnterInterrupts) enter();
    }
    void leave(PushLowDelayExit why) {
        active_ = false;
        lastExit_ = why;
        ++exits_[0];
        ++exits_[static_cast<unsigned>(why)];
        // SHOULD 5: frequent interruptions the mode causes itself (more while predicted than while interpolating) take
        // the sustained back-off like a dear or late P, so the mode does not switch at the interruptions' pace.
        const bool interruptionExit = why == PushLowDelayExit::Refused || why == PushLowDelayExit::Skipped || why == PushLowDelayExit::Held;
        const bool selfInterrupted = rule_ == PushLowDelayInterrupts::Tolerant && interruptionExit && selfCaused();
        const bool sustained = (policy_ == PushLowDelayPolicy::StayIn && why == PushLowDelayExit::Unusable) || why == PushLowDelayExit::Late ||
                               why == PushLowDelayExit::Dear || selfInterrupted;
        if (sustained && ++sustainedExits_[static_cast<unsigned>(why)] >= kStopAfterSustained && !legacy15_) stopped_ = why;   // SHOULD 4 / 5
        if (sustained) {
            // A sustained failure: a long calm run before trying again, doubled if it failed again soon.
            // Within kSettledPairs x 4 pairs (about 8.5 s) of the last entry: twice the last wait; otherwise the first wait.
            sustainCalm_ = sustainCalm_ != 0 && stay_ < kSettledPairs * 4 ? std::min(kStayInMaxCalmPairs, sustainCalm_ * 2) : stayInCalm_;
            calmNeeded_ = sustainCalm_;
            sustainCount_ = sustainBad_ = 0;
            sustainBits_[0] = sustainBits_[1] = 0;
            return;
        }
        // Off is the user's choice and Ineligible a change of settings: no back-off for those. Package 16 (Tolerant):
        // frequent interruptions are not a back-off either unless the mode causes them itself (selfCaused, above; the
        // entry's 3-of-30 condition is the hysteresis otherwise); only an unusable prediction under LeaveOnUnusable still
        // doubles the calm run.
        if (rule_ == PushLowDelayInterrupts::Tolerant && why != PushLowDelayExit::Unusable) return;
        if (why != PushLowDelayExit::Off && why != PushLowDelayExit::Ineligible && stay_ < kSettledPairs)
            calmNeeded_ = calmNeeded_ * 2 > kMaxCalmPairs ? kMaxCalmPairs : calmNeeded_ * 2;
    }
    bool inStall_ = false, stallCounted_ = false, legacy15_ = false;
    unsigned stallTail_ = 0;
    PushLowDelayExit stopped_ = PushLowDelayExit::None;
    std::array<unsigned, static_cast<unsigned>(PushLowDelayExit::Count)> sustainedExits_{};
    double activeShare_ = 0.0, idleShare_ = 0.0;
    unsigned activeSamples_ = 0, idleSamples_ = 0;
    std::uint64_t stallRides_ = 0;
    PushLowDelayPolicy policy_ = PushLowDelayPolicy::LeaveOnUnusable;
    PushLowDelayInterrupts rule_ = PushLowDelayInterrupts::Tolerant;
    PushLowDelayBits interrupts_{kInterruptWindow}, recentCalm_{kEnterWindow};
    std::uint64_t interruptions_ = 0;
    PushLowDelayExit lastExit_ = PushLowDelayExit::None;
    double costs_[kCostWindow] = {};
    unsigned costCount_ = 0;
    double lastCostP95_ = -1.0;
    unsigned sustainCount_ = 0, sustainBad_ = 0, sustainCalm_ = 0;
    unsigned sustainWindow_ = kSustainWindow, sustainBadNeeded_ = kSustainBad, stayInCalm_ = kStayInCalmPairs;
    unsigned lateCount_ = 0, lateDrops_ = 0, offGridRun_ = 0, gridExits_ = 0;
    std::uint64_t lateSeen_ = 0, outcomesSeen_ = 0;
    double baseline_[kBaselineWindows] = {};
    unsigned baselineCount_ = 0, baselineNext_ = 0;
    std::uint64_t lateBits_[2] = {0, 0};
    std::uint64_t sustainBits_[2] = {0, 0};
    std::uint64_t unusableShown_ = 0;
    bool active_ = false;
    unsigned calm_ = 0, stay_ = 0, calmNeeded_ = kCalmPairs, resumeNeeded_ = 0;
    std::uint64_t entries_ = 0, predictedPairs_ = 0, multiplierPauses_ = 0;
    bool multiplierPaused_ = false;
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
// Package 16, item 3 (forensics F1): "the previous pair ran predicted" as pushLowDelayResets reads it is the last pair
// that GENERATED (predicted or interpolated). A skipped, held or history-reset pair in between changes nothing, so an
// isolated interruption no longer restarts the predicted learner at its seeded start (8 pairs off its learned phase).
inline bool pushLowDelayPreviousRan(bool previous, bool ranPredicted, bool generated) { return generated ? ranPredicted : previous; }
inline PushLowDelayResets pushLowDelayResets(bool askPredicted, bool previousRanPredicted) {
    return {previousRanPredicted, askPredicted && !previousRanPredicted};
}

// Admission of a pair asked to run predicted (EngineController's admitFg; the simulator calls the same). Today's check
// compares the whole group's cost (base + the FG group P95) with the deadline of the last interpolated frame, a + 2d/3.
// In the predicted schedule that frame (I) is FIRST and is followed by P, so the group cost is compared with I's
// deadline moved by what the group costs after I (group FG P95 - the Fg1 stage P95), never past P's own deadline.
// Equivalent to "base + Fg1 fits I's deadline, and the whole group fits P's". Missing estimates: I's deadline unchanged
// (stricter). Times in 100 ns, costs in ms.
// Package 16, item 4 (b): the group cost an asked pair is admitted with. The interpolated group's FG P95 (two VFG runs)
// minus Fg1 stood in for P before P was measured; once at least kMinMeasured predicted frames were timed (their stage
// "total" P95), the group is Fg1 + that. Predicted pairs never enter the interpolated FG estimate (EngineController).
inline double pushLowDelayAdmissionGroupMs(double interpolatedGroupMs, double fg1Ms, double predictedP95Ms, std::size_t predictedSamples) {
    constexpr std::size_t kMinMeasured = 8;
    if (predictedSamples >= kMinMeasured && predictedP95Ms >= 0 && fg1Ms > 0) return fg1Ms + predictedP95Ms;
    return interpolatedGroupMs;
}
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
// Package 16, item 7 (the owner's log: each toggle of the row rebuilt the graph, 0.65-1.5 s engine stalls): the pass is
// kept once the row has been on in this run of PushFPS. EnhancementSettings::lowDelay (the graph-shape setting) is "the
// row is on or was on before in this run"; the engine asks for predicted pairs only while the row is on (`wanted`, read
// once a pair). So only the first switch-on of a run rebuilds the graph (it must create the pass, its flow and, with Live
// HUD Lock, the held copy and its descriptors, which a running graph's lists use); later switches rebuild nothing. While
// kept and the row off: memory only (PushPredictPass's textures, the flow, the HUD Lock copy), no GPU work per pair.
inline bool pushLowDelayKeepPass(bool rowOn, bool wasOnThisRun) { return rowOn || wasOnThisRun; }

// Review 1 MUST 3: the owner was told to keep the row off on Package 15 (its predicted frames removed the device on WARP,
// gate run 45526571434 / e2e run 45530162944). The first start of this version switches a saved "on" off once, so nobody
// starts with it on by accident; the flag pref kPushLowDelayResetFlag records that it was done (a later switch-on stays).
constexpr const char* kPushLowDelayResetFlag = "pushLowDelayReset16";
struct PushLowDelayMigration { bool rowOn = false, writeFlag = false, logReset = false; };
// legacy: rollback part LowDelayMigrate16 (PushLegacy.h): the saved value is used as it is and nothing is written.
inline PushLowDelayMigration pushLowDelayMigrate(bool flagPresent, bool savedOn, bool legacy = false) {
    if (flagPresent || legacy) return {savedOn, false, false};
    return {false, true, savedOn};
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
    // Package 16, item 7: the row has been on in this run (the pass is then kept: pushLowDelayKeepPass).
    std::atomic<bool> passKept{false};
    // Package 16, item 6 (forensics F5): what the row shows comes from these live counters, written once a second by the
    // engine ([capture-timing] window): predicted pairs and pairs of the last second, the last stop's reason
    // (PushLowDelayExit) and its age in seconds (-1: none this session), entry progress (calm pairs / needed), and how many
    // seconds in a row no pair ran predicted while the row was on.
    // shownPerSecond (review 1 SHOULD 11): predicted frames actually shown in that second (a dropped one is not counted).
    std::atomic<std::uint32_t> predictedPerSecond{0}, pairsPerSecond{0}, shownPerSecond{0};
    std::atomic<std::uint8_t> lastStop{0};
    std::atomic<std::int32_t> lastStopAgeS{-1};
    std::atomic<std::uint16_t> calmHave{0}, calmNeed{0}, zeroSeconds{0};
    // 25 Nov 2026: the motion source's start-up self-check is running (EnhanceGraph::initPredict, the first switch-on of a
    // run for this device and size; at most kPushFlowCheckBudgetMs): the row says "checking".
    std::atomic<bool> checking{false};
    // Review 1 SHOULD 6: incremented by the interface on a real off -> on of the row; the graph then lets VFG's
    // FrameIndex-2-only ladder start over (pushVfgLadderResetDue).
    std::atomic<std::uint32_t> retryEpoch{0};
    // Review 1 SHOULD 4 / 5: the schedule stopped for this playback (PushLowDelayExit; 0: running), for the row.
    std::atomic<std::uint8_t> stoppedFor{0};
    // Release review REL16 SHOULD 3: whether P's motion pass (FidelityFX flow) should run for the next real frame
    // (pushLowDelayFlowWanted, written by the engine once per pair while the row is on; true by default so the kit's
    // switch and the graph tests keep the flow). The graph also runs it for any pair asked to run predicted.
    std::atomic<bool> flowWanted{true};
};
// Release review REL16 SHOULD 2, narrowed by the delta review's SHOULD 1: a session that ends with the graphics device
// REALLY removed (not a presentation failure with the device alive, not an engine exception), while the predicted-frame
// pass did GPU work in the last kPushPredictFaultWorkWindowMs and the row (or the kit's switch) was on, is a strike. The
// first writes the marker "suspect <identity>" (it blocks nothing); a second strike in a row writes "failed <identity>"
// (the motion source is blocked for this program file: the row reads "Cannot run on this PC or with these settings"). A
// session that ends normally with the row on and the pass present clears a "suspect". Anything else leaves the marker.
constexpr std::int64_t kPushPredictFaultWorkWindowMs = 5000;
enum class PushPlayFaultAction : std::uint8_t { None, WriteSuspect, WriteFailed, ClearSuspect };
inline PushPlayFaultAction pushPlayFaultAction(bool deviceRemoved, bool engineError, bool passExists, bool rowOn, std::int64_t workAgeMs, bool priorSuspect) {
    if (!passExists || !rowOn) return PushPlayFaultAction::None;
    if (deviceRemoved) {
        if (workAgeMs < 0 || workAgeMs > kPushPredictFaultWorkWindowMs) return PushPlayFaultAction::None;   // P did no recent GPU work
        return priorSuspect ? PushPlayFaultAction::WriteFailed : PushPlayFaultAction::WriteSuspect;
    }
    if (!engineError && priorSuspect) return PushPlayFaultAction::ClearSuspect;
    return PushPlayFaultAction::None;
}

// Release review REL16 SHOULD 3: the motion pass for P runs only while the schedule is active or about to enter: not while
// stopped for the playback, not while the pair's multiplier is not 3X, and during a back-off only for the last
// kPushLowDelayFlowLeadPairs calm pairs before it may enter, so FidelityFX (7 frames after a reset; the graph resets it
// when a frame was not fed) is warm when the schedule enters. Its GPU time on his card is unmeasured; outside these it
// costs nothing.
constexpr unsigned kPushLowDelayFlowLeadPairs = 10;
constexpr unsigned kPushFidelityWarmupFrames = 7;
static_assert(kPushLowDelayFlowLeadPairs >= kPushFidelityWarmupFrames + 2, "the flow must be warm when the schedule may enter (+1 for the one-pair lag of flowWanted)");
// REL16 delta review MUST 1: whether this real frame restarts the flow's history (a resume after the flow was stopped).
// fedNow: this frame goes into the flow; runFg: frame generation ran for this pair. A pair that GPU admission skipped
// (runFg false), a held pair and a capture stall's missing frames are a GAP (FidelityFX keeps its history and only the
// next frame is not ready), not a stop of the flow, so they leave the memory as it was.
inline bool pushFlowResume(bool fedNow, bool runFg, bool& fedLast) {
    const bool resume = fedNow && !fedLast;
    if (runFg) fedLast = fedNow;
    return resume;
}
inline bool pushLowDelayFlowWanted(const PushLowDelaySchedule& s, bool threeX) {
    if (!threeX || s.stoppedFor() != PushLowDelayExit::None) return false;
    if (s.active()) return true;
    unsigned have = 0, need = 0;
    s.entryProgress(have, need);
    return need <= have + kPushLowDelayFlowLeadPairs;
}

// Package 16, item 6: the window update of those counters (pure, so it is tested; the engine calls it once a second).
struct PushLowDelayRowFacts {
    std::uint32_t predicted = 0, pairs = 0, shown = 0;
    PushLowDelayExit lastStop = PushLowDelayExit::None;
    std::int32_t lastStopAgeS = -1;
    std::uint16_t calmHave = 0, calmNeed = 0, zeroSeconds = 0;
};
inline PushLowDelayRowFacts pushLowDelayRowFacts(const PushLowDelaySchedule& s, bool rowOn, std::uint64_t predictedThisWindow, std::uint64_t pairsThisWindow,
                                                 std::int64_t now100ns, std::int64_t lastStopAt100ns, std::uint16_t zeroSecondsBefore,
                                                 std::uint64_t shownThisWindow = 0) {
    PushLowDelayRowFacts f;
    f.predicted = static_cast<std::uint32_t>(std::min<std::uint64_t>(predictedThisWindow, 100000));
    f.pairs = static_cast<std::uint32_t>(std::min<std::uint64_t>(pairsThisWindow, 100000));
    f.shown = static_cast<std::uint32_t>(std::min<std::uint64_t>(shownThisWindow, 100000));
    f.lastStop = s.lastExit();
    f.lastStopAgeS = lastStopAt100ns > 0 && now100ns >= lastStopAt100ns ? static_cast<std::int32_t>(std::min<std::int64_t>((now100ns - lastStopAt100ns) / 10000000, 1000000)) : -1;
    unsigned have = 0, need = 0;
    s.entryProgress(have, need);
    f.calmHave = static_cast<std::uint16_t>(std::min(have, 65535u)); f.calmNeed = static_cast<std::uint16_t>(std::min(need, 65535u));
    f.zeroSeconds = rowOn && predictedThisWindow == 0 ? static_cast<std::uint16_t>(std::min<unsigned>(zeroSecondsBefore + 1u, 65535u)) : std::uint16_t{0};
    return f;
}
inline void pushLowDelayPublish(PushLowDelayShared& shared, const PushLowDelayRowFacts& f) {
    shared.predictedPerSecond.store(f.predicted, std::memory_order_relaxed);
    shared.pairsPerSecond.store(f.pairs, std::memory_order_relaxed);
    shared.shownPerSecond.store(f.shown, std::memory_order_relaxed);
    shared.lastStop.store(static_cast<std::uint8_t>(f.lastStop), std::memory_order_relaxed);
    shared.lastStopAgeS.store(f.lastStopAgeS, std::memory_order_relaxed);
    shared.calmHave.store(f.calmHave, std::memory_order_relaxed);
    shared.calmNeed.store(f.calmNeed, std::memory_order_relaxed);
    shared.zeroSeconds.store(f.zeroSeconds, std::memory_order_relaxed);
}
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

// Package 16: what the engine reports before asking a pair (EngineController's lowDelayStatic): not a capture card with
// VFG -> not-vfg, else the graph's own static refusal (EnhanceGraph::predictStaticRefusal: pass, format, present-sink,
// the pair's multiplier, VFG refused). Package 15 also refused here whenever Smoothness Mode had a target (review 2,
// SHOULD 4, because Smoothness Mode's latency fields took the real frame as the batch's last); the owner's own log of
// 24 Nov showed that refusal kept the mode from ever running with his target of 90. Smoothness Mode now only decides
// the multiplier: at 3X the pair may run predicted; below 3X the graph refuses not-3x and the schedule pauses
// (PushLowDelayPair::multiplierPause). PushPredictRefusal::SmoothnessMode is no longer produced.
inline pipeline::PushPredictRefusal pushLowDelayStaticRefusal(bool cardVfg3x, pipeline::PushPredictRefusal graphRefusal) {
    return cardVfg3x ? graphRefusal : pipeline::PushPredictRefusal::NotVfg;
}

// Review 1 SHOULD 4: the entry guard from the motion source's start-up self-check (its GPU time for one real frame and P,
// measured once on this card at graph build): above kPushLowDelayEntryCostShare of an output slot the mode is not
// entered (static refusal flow-too-slow: the row says the predicted frame is too slow on this card). A choice (P shares
// the queue with the next pair's real frame), not tuned on any GPU. Unknown cost (-1, test sources): no guard.
constexpr double kPushLowDelayEntryCostShare = 0.75;
inline pipeline::PushPredictRefusal pushLowDelayEntryCostRefusal(pipeline::PushPredictRefusal refusal, double checkCostMs, double slotMs) {
    if (refusal != pipeline::PushPredictRefusal::Ok || !(checkCostMs > 0) || !(slotMs > 0)) return refusal;
    return checkCostMs > kPushLowDelayEntryCostShare * slotMs ? pipeline::PushPredictRefusal::FlowTooSlow : refusal;
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
