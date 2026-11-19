// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushGovernorFeed.h"
#include "veyra/engine/PushGpuBudget.h"
#include "veyra/engine/PushHardwareCapabilities.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace veyra::engine {
// Latency governor, active control (experimental; VEYRA_EXP_RP_GOVERNOR=apply). Portable and header-only.
// Tests: tests/remoteplay/GovernorControlTests.inc (unit tests and invariants over every simulator scenario and
// randomized traces); closed-loop model: include/veyra/engine/PushGovernorSim.h, tools/remoteplay_latency_bench/
// governor_sim.cpp. Unverified on Windows; no threshold here has run on any GPU or PS5.
//
// It is also the one arbiter between everything that may change frame generation while a stream plays: one
// ordered decision per tick (about a second):
//   1. decode and presentation are protected first: the GPU budget (PushGpuBudget, embedded, fed with the
//      same sample) asks for less work when the decode queue waits or presentation deadlines are missed;
//   2. then frame generation for latency: the unplanned delay (the measured P95 "decoded picture to present
//      call" latency minus the hold the level plans, (M-1)/M of a source interval) above the latency budget;
//   3. enhancement (AI picture enhancement, super resolution): no step here. Their setters rebuild the
//      graph (a visible stall), so they are not live knobs; see docs/HARDWARE_COMPATIBILITY.md.
// pushFlowBudgetWatch's own tier step and the GPU budget's own apply mode stand aside while this applies;
// the optimiser and the quality test pause it and get the user's choice back first (pendingRestore()).
//
// The only settings it changes are the two the interface already sets live: the frame generation multiplier
// (setFgMultiplier; 1 = off) and PushFlow's tier (setVfgQuality). The ladder goes from the user's request
// down: tiers first, then lower multipliers, then off. Not changed, on purpose: presentation and pacing mode
// and the low-latency choices (the performance modes already set the latency-first values, and changing the
// swap chain's queue live risks recreating it), and the reorder / backlog / pad poll switches (read once when
// a PS5 session connects; they cannot change live).
//
// Rules (each is checked over all simulator scenarios and randomized traces by pushGovernorInvariantViolation):
//   - never above the user's own request (multiplier, and tier while generating);
//   - never raises latency to show more frames: a step up is only a return toward the request, only after a
//     healthy hold with the unplanned delay known and under (budget - hysteresis), no protection trouble, and
//     the level above, when it was last measured, also under that line;
//   - latency budget in ms (default 10: small, it favours responsiveness) with hysteresis;
//   - minimum interval between changes; at most maxChangesPerMinute in any 60 s;
//   - every step down is judged after evaluateMs: when what it targeted did not improve, it is rolled back
//     (to the level it left; that step is then locked out with exponential back-off);
//   - direction reversals within oscillationWindowMs: at oscillationReversals the governor locks out, the
//     lock-out doubling each time (up to 16x);
//   - unknown inputs: no action (a step needs its own trigger measured; a step up needs everything);
//   - a step up after overload waits healthyHoldMs, doubled after each failed return (up to 8x);
//   - user override wins at once: the setting the user changed becomes the request and the governor leaves
//     that setting alone until the next stream (newStream()). A setter that refuses a level is not a user
//     change: that level is not asked again for kRefusedHoldMs.

struct PushControlConfig {
    double latencyBudgetMs = 10;           // allowed unplanned delay (P95), ms
    double hysteresisMs = 3;               // a step up needs the unplanned delay under budget - hysteresis
    std::int64_t sustainMs = 3000;         // over budget this long before a latency step down
    std::int64_t minIntervalMs = 3000;     // between any two changes
    unsigned maxChangesPerMinute = 6;
    std::int64_t evaluateMs = 5000;        // a step down is judged this long after it
    double improveShare = 0.05;            // it must lower its target by this share ...
    double improveMinimumMs = 0.5;         // ... and by at least this much (in the target's unit)
    std::int64_t healthyHoldMs = 20000;    // healthy this long before a step back up (doubles after a failed return)
    std::int64_t lockoutMs = 60000;        // after a rollback or oscillation (doubles, up to 16x)
    unsigned oscillationReversals = 3;     // reversals within the window that lock the governor out
    std::int64_t oscillationWindowMs = 120000;
    std::int64_t tierIntervalMs = 60000;    // between tier changes (each rebuilds the graph)
    std::int64_t probeIntervalMs = 300000;  // while reduced and healthy now, one judged step up at most this often
                                            // (doubling after a failed probe, up to 4x), even before a full hold
};

inline PushControlConfig pushSanitizedControlConfig(PushControlConfig c) noexcept {
    const PushControlConfig standard;
    const auto ms = [](std::int64_t v, std::int64_t lowest) { return v < lowest ? lowest : v > INT64_C(3600000) ? INT64_C(3600000) : v; };
    c.latencyBudgetMs = std::isfinite(c.latencyBudgetMs) && c.latencyBudgetMs >= 1 && c.latencyBudgetMs <= 200 ? c.latencyBudgetMs : standard.latencyBudgetMs;
    c.hysteresisMs = std::isfinite(c.hysteresisMs) && c.hysteresisMs >= 0 && c.hysteresisMs < c.latencyBudgetMs ? c.hysteresisMs : c.latencyBudgetMs * 0.4;
    c.sustainMs = ms(c.sustainMs, 0);
    c.minIntervalMs = ms(c.minIntervalMs, 1000);
    c.maxChangesPerMinute = c.maxChangesPerMinute == 0 ? 1u : c.maxChangesPerMinute > 30 ? 30u : c.maxChangesPerMinute;
    c.evaluateMs = ms(c.evaluateMs, 1000);
    c.improveShare = std::isfinite(c.improveShare) && c.improveShare >= 0 && c.improveShare < 1 ? c.improveShare : standard.improveShare;
    c.improveMinimumMs = std::isfinite(c.improveMinimumMs) && c.improveMinimumMs >= 0 ? c.improveMinimumMs : standard.improveMinimumMs;
    c.healthyHoldMs = ms(c.healthyHoldMs, 1000);
    c.lockoutMs = ms(c.lockoutMs, 1000);
    c.oscillationReversals = c.oscillationReversals < 2 ? 2u : c.oscillationReversals > 16 ? 16u : c.oscillationReversals;
    c.oscillationWindowMs = ms(c.oscillationWindowMs, 10000);
    c.probeIntervalMs = ms(c.probeIntervalMs, 10000);
    c.tierIntervalMs = ms(c.tierIntervalMs, 0);
    return c;
}

// a is within b: no more generated frames, and (while generating) no higher tier.
inline bool pushLevelWithin(PushGpuLevel a, PushGpuLevel b) noexcept {
    return a.multiplier <= b.multiplier && (a.multiplier < 2 || a.tier <= b.tier);
}
// The delay interpolation plans for every real picture: (M-1)/M of a source interval (PushModes.js delayMs).
inline double pushPlannedDelayMs(unsigned multiplier, double sourceFps) noexcept {
    if (multiplier < 2 || !std::isfinite(sourceFps) || sourceFps <= 0) return 0;
    return static_cast<double>(multiplier - 1) / static_cast<double>(multiplier) * 1000.0 / sourceFps;
}

// Request first (always level 0, exactly as asked), then lower tiers (unless the tier is frozen), then lower
// selectable multipliers (unless the multiplier is frozen; at tier 0, or at the request's tier when it is
// frozen), then off (unless the multiplier is frozen).
inline PushGpuBudgetLadder pushControlLadder(PushFgRequest request, bool freezeMultiplier, bool freezeTier) {
    request = pushNormalizedFgRequest(request);
    PushGpuBudgetLadder ladder;
    const auto push = [&ladder](unsigned t, unsigned m) {
        if (ladder.count < ladder.levels.size()) ladder.levels[ladder.count++] = PushGpuLevel{t, m};
    };
    push(request.tier, request.multiplier);
    if (request.multiplier < 2) return ladder;
    if (!freezeTier)
        for (unsigned t = request.tier; t-- > 0;) push(t, request.multiplier);
    if (freezeMultiplier) return ladder;
    const unsigned lowTier = freezeTier ? request.tier : 0u;
    const unsigned top = request.multiplier > kPushFgLargestMultiplier ? kPushFgLargestMultiplier + 1 : request.multiplier;
    for (unsigned m = top - 1; m >= 2; --m)
        if (pushFgMultiplierSelectable(request.method, m)) push(lowTier, m);
    push(lowTier, 1);
    return ladder;
}

// One tick's measurements. Negative or non-finite = not measured.
struct PushControlSample {
    std::int64_t nowMs = 0;              // monotonic
    double sourceFps = -1;
    double latencyP95Ms = -1;            // decoded picture to present call, P95 of the last second
    double networkJitterMs = -1;         // the stream's frame jitter: delay no frame generation setting can change
    PushGpuBudgetSample budget{};        // what PushGpuBudget is fed (its now100ns is set from nowMs)
};

enum class PushControlCause : std::uint8_t { None = 0, Decode = 1, Presentation = 2, GpuLoad = 3, Latency = 4, Recovery = 5, Rollback = 6, Probe = 7 };
inline const char* pushControlCauseName(PushControlCause c) noexcept {
    switch (c) {
    case PushControlCause::None: return "none";
    case PushControlCause::Decode: return "decode";
    case PushControlCause::Presentation: return "presentation";
    case PushControlCause::GpuLoad: return "gpu-load";
    case PushControlCause::Latency: return "latency";
    case PushControlCause::Recovery: return "recovery";
    case PushControlCause::Rollback: return "rollback";
    case PushControlCause::Probe: return "probe";
    }
    return "none";
}

struct PushControlTick {
    bool apply = false;                  // put target in place now (one setting change)
    PushFgRequest target{};
    PushGpuLevel from{}, to{};
    std::size_t index = 0;               // ladder level now believed in force (0 = request)
    std::size_t startIndex = 0;          // the level this tick's decision started from
    std::size_t protectIndex = 0;        // what decode/presentation protection asks for
    PushControlCause cause = PushControlCause::None;
    bool protectionTrouble = false;      // the GPU budget saw trouble this tick
    double unplannedMs = -1;             // latency P95 minus the planned hold; -1 unknown
    double returnLineMs = -1;            // a step up needs the unplanned delay under this (when reduced)
    bool userOverride = false;           // the user changed a setting this tick
    bool refused = false;                // the last level asked for was reported refused (applyRefused)
    std::string_view reason = "start";
    std::string_view budgetReason = "start";
};

class PushGovernorControl {
public:
    static constexpr std::int64_t kRefusedHoldMs = 60000;
    static constexpr unsigned kMaxLockoutShift = 4, kMaxReturnShift = 3, kMaxProbeShift = 2;
    static constexpr std::int64_t kPredictionMemory = 4;   // healthy holds

    explicit PushGovernorControl(PushControlConfig settings = {}) : config_(pushSanitizedControlConfig(settings)) {
        recent_.fill(kNever);
        reversals_.fill(kNever);
    }
    const PushControlConfig& config() const noexcept { return config_; }

    // A new stream: the request stays (the caller restores it first, pendingRestore()); the user's frozen
    // settings, step locks, refusals and the return back-off start again. The rate limits and the oscillation
    // lock-out (with its back-off) are kept for the whole run.
    void newStream() noexcept { started_ = false; }
    bool started() const noexcept { return started_; }
    bool frozenMultiplier() const noexcept { return freezeMultiplier_; }
    bool frozenTier() const noexcept { return freezeTier_; }
    PushFgRequest request() const noexcept { return request_; }
    std::size_t index() const noexcept { return index_; }
    const PushGpuBudgetLadder& ladder() const noexcept { return ladder_; }
    bool lockedOut(std::int64_t nowMs) const noexcept { return nowMs < lockoutUntil_; }
    unsigned rollbacks() const noexcept { return rollbacks_; }
    unsigned lockouts() const noexcept { return lockouts_; }
    unsigned changes() const noexcept { return changeCount_; }

    // The user's request, when the settings in force are still a level this governor put in place: put it
    // back before a stop, a quit or a measurement (optimiser, quality test), then call restored().
    std::optional<PushFgRequest> pendingRestore(PushFgRequest current) const noexcept {
        if (!haveRequest_ || index_ == 0) return std::nullopt;
        current = pushNormalizedFgRequest(current);
        if (!(current == settingsOf(ladder_[index_]))) return std::nullopt;
        return request_;
    }
    void restored() noexcept { index_ = 0; applyPending_ = false; }
    // The caller's setter did not take the last level (read back right after setting it): it is not a user
    // change. The level stays where it was and that level is not asked again for kRefusedHoldMs.
    void applyRefused() noexcept {
        if (!applyPending_) return;
        applyPending_ = false;
        index_ = applyFromIndex_;
        refusedIndex_ = applyToIndex_;
        refusedUntil_ = lastNow_ + kRefusedHoldMs;
        evaluating_ = false;
        refusedReport_ = true;
    }

    PushControlTick update(PushFgRequest current, const PushControlSample& in) {
        current = pushNormalizedFgRequest(current);
        PushControlTick out;
        if (started_ && in.nowMs < lastNow_) {   // a clock step back: nothing is trusted
            out.index = index_;
            out.reason = "bad-input";
            return out;
        }
        if (!started_) startStream(current);
        lastNow_ = in.nowMs;

        // Did the user change something? (A setter that refused was reported through applyRefused().)
        out.refused = refusedReport_;
        refusedReport_ = false;
        applyPending_ = false;
        if (!(current == settingsOf(ladder_[index_]))) {
            const PushFgRequest expected = settingsOf(ladder_[index_]);
            const bool multiplierChanged = current.multiplier != expected.multiplier || current.method != expected.method;
            const bool tierChanged = current.tier != expected.tier || current.method != expected.method;
            freezeMultiplier_ = freezeMultiplier_ || multiplierChanged;
            freezeTier_ = freezeTier_ || tierChanged;
            // Only what the user changed becomes the request: a setting the governor had reduced and the user left
            // alone keeps the user's own value as its ceiling (the governor never touches it again this stream,
            // but the restore puts it back).
            const bool sameMethod = current.method == request_.method;
            const PushFgRequest wanted{current.method, multiplierChanged || !sameMethod ? current.multiplier : request_.multiplier,
                                       tierChanged || !sameMethod ? current.tier : request_.tier};
            setRequest(wanted);
            // Where the settings in force now sit on the new ladder (the request itself when nothing is reduced).
            bool onLadder = false;
            for (std::size_t i = 0; i < ladder_.count && !onLadder; ++i)
                if (current == settingsOf(ladder_[i])) { index_ = i; onLadder = true; }
            if (!onLadder) setRequest(current);   // not reachable from the kept request: the user's whole choice
            evaluating_ = false;
            out.userOverride = true;
        }

        out.startIndex = index_;
        // Inputs.
        const bool rateKnown = known(in.sourceFps) && in.sourceFps > 0;
        const unsigned m = ladder_[index_].multiplier;
        // Below the planned hold counts as none: -1 stays "not measured".
        out.unplannedMs = rateKnown && known(in.latencyP95Ms) ? std::max(0.0, in.latencyP95Ms - pushPlannedDelayMs(m, in.sourceFps)) : -1.0;
        unplannedNow_ = out.unplannedMs;
        if (out.unplannedMs >= 0) {
            lastUnplanned_[index_] = out.unplannedMs;
            lastUnplannedAt_[index_] = in.nowMs;
            if (entryUnplanned_[index_] < 0) entryUnplanned_[index_] = out.unplannedMs;
        }
        PushGpuBudgetSample sample = in.budget;
        sample.now100ns = in.nowMs * 10000;
        const PushGpuBudgetDecision budget = budget_.update(sample);
        out.budgetReason = budget.reason;
        const PushControlCause protectCause = protectionCause(budget.reason);
        out.protectionTrouble = protectCause != PushControlCause::None;
        out.protectIndex = mapToLadder(budget.level);

        // Over-budget and healthy clocks.
        const double high = config_.latencyBudgetMs, low = config_.latencyBudgetMs - config_.hysteresisMs;
        if (out.unplannedMs >= 0 && rateKnown) {
            if (out.unplannedMs > high) { if (overSince_ == kNever) overSince_ = in.nowMs; }
            else if (out.unplannedMs < low) overSince_ = kNever;
        } else overSince_ = kNever;
        // The line a return must stay under: budget minus hysteresis after a latency step; after a protection step
        // the budget itself (the latency never asked for that step, so it need not be lower than before it).
        const bool protectionReturn = index_ > 0 && index_ < cameFromCause_.size() && isProtection(cameFromCause_[index_]);
        const double returnLine = protectionReturn ? high : low;
        out.returnLineMs = returnLine;
        const bool latencyHealthy = rateKnown && out.unplannedMs >= 0 && out.unplannedMs < returnLine && !out.protectionTrouble;
        if (latencyHealthy) { if (healthySince_ == kNever) healthySince_ = in.nowMs; }
        else healthySince_ = kNever;

        // Evaluation of the last step down.
        if (evaluating_ && rateKnown) {
            const double v = targetValue(evalCause_, in);
            if (v >= 0) { evalSum_ += v; ++evalCount_; }
        }
        out.index = index_;
        if (ladder_.count <= 1) return finish(out, freezeMultiplier_ || freezeTier_ ? "suspended-by-user" : "nothing-to-change");
        if (!rateKnown) return finish(out, "unknown-input");
        if (evaluating_ && in.nowMs - evalAt_ >= config_.evaluateMs) {
            if (evalCount_ == 0) { evaluating_ = false; }   // nothing measured since: no verdict, no action
            else {
                const double after = evalSum_ / static_cast<double>(evalCount_);
                const double needed = evalBefore_ - std::max(config_.improveMinimumMs, evalBefore_ * config_.improveShare);
                if (evalCause_ == PushControlCause::Probe) {
                    // A probe is kept unless the level it tried went over the budget; else back down, and the next
                    // probe waits twice as long (up to 4x).
                    if (after <= high) { evaluating_ = false; probeShift_ = 0; }
                    else if (gatesOpen(in.nowMs) && index_ == evalToIndex_) {
                        evaluating_ = false;
                        ++rollbacks_;
                        if (probeShift_ < kMaxProbeShift) ++probeShift_;
                        return change(out, evalFromIndex_, PushControlCause::Rollback, "rollback-probe-over-budget", in.nowMs);
                    } else if (index_ != evalToIndex_ || in.nowMs - evalAt_ >= 3 * config_.evaluateMs) evaluating_ = false;
                    else return finish(out, "rollback-waiting");
                } else if (after <= needed) { evaluating_ = false; }
                else if (gatesOpen(in.nowMs) && index_ == evalToIndex_ && targetValue(evalCause_, in) >= 0 && !out.protectionTrouble) {
                    evaluating_ = false;
                    ++rollbacks_;
                    lockStep(isProtection(evalCause_), evalFromIndex_, in.nowMs);
                    return change(out, evalFromIndex_, PushControlCause::Rollback, "rollback-target-not-improved", in.nowMs);
                } else if (index_ != evalToIndex_ || in.nowMs - evalAt_ >= 3 * config_.evaluateMs) evaluating_ = false;   // too late: no verdict
                else return finish(out, "rollback-waiting");
            }
        }

        // 1. protection (decode, presentation, GPU load) and 2. latency: one step down.
        // Over the budget now, and (between the thresholds the clock holds) for sustainMs.
        // Cause first: the part of the unplanned delay the network's own jitter explains is not frame generation's,
        // so a latency step needs the rest over the budget (and the jitter measured), and a level with fewer
        // generated frames to go to (never a tier: a tier change rebuilds the graph and hardly moves latency).
        const bool attributable = known(in.networkJitterMs) && out.unplannedMs - in.networkJitterMs > high;
        std::size_t fewer = SIZE_MAX;
        for (std::size_t j = index_ + 1; j < ladder_.count && fewer == SIZE_MAX; ++j)
            if (ladder_[j].multiplier < ladder_[index_].multiplier) fewer = j;
        const bool latencyOver = attributable && fewer != SIZE_MAX && overSince_ != kNever && in.nowMs - overSince_ >= config_.sustainMs;
        const bool protectDown = out.protectIndex > index_;
        if ((protectDown || latencyOver) && index_ + 1 < ladder_.count) {
            // Decode and presentation come first: their steps pass an oscillation lock-out (never the minimum
            // interval or the per-minute cap); a latency step waits it out. Each kind has its own step locks,
            // so a latency step that did not help never blocks protecting decode. One step is judged before
            // the next is made.
            if (evaluating_) return finish(out, "judging-last-step");
            if (!gatesOpen(in.nowMs, protectDown)) return finish(out, lockedOut(in.nowMs) && !protectDown ? "locked-out" : "rate-limited");
            if (stepLocked(protectDown, index_, in.nowMs)) return finish(out, "step-locked");
            const PushControlCause cause = protectDown ? (protectCause != PushControlCause::None ? protectCause : PushControlCause::GpuLoad) : PushControlCause::Latency;
            // Protection takes the next level (a lower tier sheds GPU work first). A latency step takes the next
            // level with fewer generated frames when there is one: that shortens the hold every real picture
            // waits, where a lower tier mostly saves GPU time the budget above already watches.
            const std::size_t next = protectDown ? index_ + 1 : fewer;
            // A protection step that only changes the tier rebuilds the graph: at most one per tierIntervalMs.
            if (protectDown && ladder_[next].multiplier == ladder_[index_].multiplier && haveTierChange_ && in.nowMs - lastTierChange_ < config_.tierIntervalMs)
                return finish(out, "tier-interval");
            if (next == refusedIndex_ && in.nowMs < refusedUntil_) return finish(out, "refused-hold");
            evaluating_ = true;
            evalCause_ = cause;
            evalAt_ = in.nowMs;
            evalFromIndex_ = index_;
            evalToIndex_ = next;
            evalBefore_ = targetValue(cause, in);
            evalSum_ = 0;
            evalCount_ = 0;
            if (evalBefore_ < 0) evaluating_ = false;   // its target is not measured: no verdict later
            // A return counts as failed only when the level above did not last two holds.
            if (haveReturned_ && in.nowMs - lastReturn_ < 2 * config_.healthyHoldMs) returnShift_ = returnShift_ < kMaxReturnShift ? returnShift_ + 1 : kMaxReturnShift;
            haveReturned_ = false;
            return change(out, next, cause, protectDown ? "protect-step-down" : "latency-step-down", in.nowMs);
        }
        if (out.unplannedMs > high && fewer == SIZE_MAX) return finish(out, "at-floor");

        // Two oscillation windows without a reversal (and not locked out) forget one doubling of the lock-out.
        if (lockShift_ > 0 && !lockedOut(in.nowMs) && (reversals_[0] == kNever || in.nowMs - reversals_[0] >= 2 * config_.oscillationWindowMs) &&
            (lastShiftDecay_ == kNever || in.nowMs - lastShiftDecay_ >= 2 * config_.oscillationWindowMs)) {
            --lockShift_;
            lastShiftDecay_ = in.nowMs;
        }
        // Each full (base) healthy hold, at any level, forgets one failed return: periodic short bursts cannot grow
        // the hold without bound.
        if (latencyHealthy && returnShift_ > 0 && in.nowMs - healthySince_ >= config_.healthyHoldMs &&
            (lastDecay_ == kNever || in.nowMs - lastDecay_ >= config_.healthyHoldMs)) {
            --returnShift_;
            lastDecay_ = in.nowMs;
        }
        // Recovery: one step back toward the request, to the level the step down came from (a latency step may
        // skip tier levels; coming back through a level never measured would be a guess).
        if (index_ > 0 && out.protectIndex < index_) {
            std::size_t up = cameFrom_[index_] < index_ ? cameFrom_[index_] : index_ - 1;
            if (up < out.protectIndex) up = out.protectIndex;
            if (out.protectionTrouble) return finish(out, "protection-trouble");   // (also part of latencyHealthy)
            if (!latencyHealthy) return finish(out, out.unplannedMs < 0 ? "unknown-input" : "hold");
            const std::int64_t hold = config_.healthyHoldMs << returnShift_;
            const bool holdDone = in.nowMs - healthySince_ >= hold && (!haveChanged_ || in.nowMs - lastChange_ >= hold);
            // Short bursts can keep resetting the hold: a judged probe step up then still comes, capped.
            const bool probeDue = !holdDone && !evaluating_ && haveChanged_ && in.nowMs - lastChange_ >= (config_.probeIntervalMs << probeShift_);
            if (!holdDone && !probeDue) return finish(out, "healthy-waiting");
            // The level above, predicted: now plus what the step down saved when it was made (its last sample
            // against this level's first one, which cancels what both shared, such as network jitter).
            // Measurements older than kPredictionMemory healthy holds are forgotten: the next try finds out again
            // (and a failed try doubles the hold before the one after).
            const bool fresh = lastUnplannedAt_[up] != kNever && in.nowMs - lastUnplannedAt_[up] < kPredictionMemory * config_.healthyHoldMs;
            const double aboveLast = fresh ? lastUnplanned_[up] : -1.0, hereFirst = entryUnplanned_[index_];
            const double predicted = aboveLast >= 0 && hereFirst >= 0 ? out.unplannedMs + std::max(0.0, aboveLast - hereFirst) : out.unplannedMs;
            if (!probeDue && predicted >= returnLine) return finish(out, "level-above-over-budget");
            if (!gatesOpen(in.nowMs)) return finish(out, lockedOut(in.nowMs) ? "locked-out" : "rate-limited");
            if (up == refusedIndex_ && in.nowMs < refusedUntil_) return finish(out, "refused-hold");
            if (probeDue) {
                evaluating_ = true;
                evalCause_ = PushControlCause::Probe;
                evalAt_ = in.nowMs;
                evalFromIndex_ = index_;
                evalToIndex_ = up;
                evalBefore_ = out.unplannedMs;
                evalSum_ = 0;
                evalCount_ = 0;
                return change(out, up, PushControlCause::Probe, "probe-step-up", in.nowMs);
            }
            evaluating_ = false;
            haveReturned_ = true;
            lastReturn_ = in.nowMs;
            return change(out, up, PushControlCause::Recovery, "step-up-sustained-healthy", in.nowMs);
        }
        return finish(out, index_ == 0 ? "at-request" : "hold");
    }

private:
    static constexpr std::int64_t kNever = INT64_MIN;
    static bool known(double v) noexcept { return std::isfinite(v) && v >= 0; }
    PushFgRequest settingsOf(PushGpuLevel l) const noexcept {
        return pushNormalizedFgRequest(PushFgRequest{request_.method, l.multiplier, l.tier});
    }
    void setRequest(PushFgRequest r) {
        request_ = pushNormalizedFgRequest(r);
        haveRequest_ = true;
        ladder_ = pushControlLadder(request_, freezeMultiplier_, freezeTier_);
        index_ = 0;
        budget_.reset(pushGpuBudgetLadder(request_.method, request_.multiplier, request_.tier));
        lastUnplanned_.fill(-1);
        lastUnplannedAt_.fill(kNever);
        cameFrom_.fill(SIZE_MAX);
        cameFromCause_.fill(PushControlCause::None);
        entryUnplanned_.fill(-1);
        for (auto& locks : stepLockUntil_) locks.fill(kNever);
        refusedIndex_ = SIZE_MAX;
        overSince_ = healthySince_ = kNever;
    }
    void startStream(PushFgRequest current) {
        started_ = true;
        freezeMultiplier_ = freezeTier_ = false;
        // The request survives a new stream only when the settings in force are still ours.
        const bool ours = haveRequest_ && current == settingsOf(ladder_[index_]);
        setRequest(ours ? request_ : current);
        if (ours && !(current == request_)) {
            // Find where the settings in force sit on the ladder (normally restored to 0 before).
            for (std::size_t i = 0; i < ladder_.count; ++i)
                if (current == settingsOf(ladder_[i])) { index_ = i; break; }
        }
        // The rate limits (minimum interval, changes per minute) and the oscillation lock-out with its back-off
        // carry over: they hold across streams too.
        returnShift_ = 0;
        probeShift_ = 0;
        haveReturned_ = false;
        evaluating_ = false;
        applyPending_ = false;
    }
    // GPU budget reasons -> what is protected.
    static PushControlCause protectionCause(std::string_view reason) noexcept {
        const bool trouble = reason.starts_with("trouble-") || reason.starts_with("step-down-") || reason.starts_with("severe-step-down-");
        if (!trouble) return PushControlCause::None;
        if (reason.ends_with("decode-delay")) return PushControlCause::Decode;
        if (reason.ends_with("missed-deadlines")) return PushControlCause::Presentation;
        return PushControlCause::GpuLoad;
    }
    // The first ladder level within the budget's level (the ladders differ when a setting is frozen).
    std::size_t mapToLadder(PushGpuLevel level) const noexcept {
        for (std::size_t i = 0; i < ladder_.count; ++i)
            if (pushLevelWithin(ladder_[i], level)) return i;
        return ladder_.count - 1;
    }
    // What a step targeted, measured now: lower is better; -1 = not measured.
    double targetValue(PushControlCause cause, const PushControlSample& in) const noexcept {
        const auto& b = in.budget;
        switch (cause) {
        case PushControlCause::Latency: return unplannedNow_;
        case PushControlCause::Decode: return known(b.decodeQueueDelayMs) ? b.decodeQueueDelayMs : -1.0;
        case PushControlCause::Presentation: {
            const unsigned total = b.presented + b.missedDeadlines;
            return total > 0 ? 100.0 * static_cast<double>(b.missedDeadlines) / static_cast<double>(total) : -1.0;
        }
        case PushControlCause::GpuLoad:
            return known(b.gpuFrameMs) ? b.gpuFrameMs + (known(b.fgMs) ? b.fgMs : 0.0) : -1.0;
        case PushControlCause::None:
        case PushControlCause::Recovery:
        case PushControlCause::Rollback: return -1.0;
        case PushControlCause::Probe: return unplannedNow_;
        }
        return -1.0;
    }
    bool gatesOpen(std::int64_t now, bool protection = false) const noexcept {
        if (lockedOut(now) && !protection) return false;
        if (haveChanged_ && now - lastChange_ < config_.minIntervalMs) return false;
        unsigned inWindow = 0;
        for (std::int64_t t : recent_)
            if (t != kNever && now - t < 60000) ++inWindow;
        return inWindow < config_.maxChangesPerMinute;
    }
    static bool isProtection(PushControlCause c) noexcept {
        return c == PushControlCause::Decode || c == PushControlCause::Presentation || c == PushControlCause::GpuLoad;
    }
    bool stepLocked(bool protection, std::size_t fromIndex, std::int64_t now) const noexcept {
        const auto& locks = stepLockUntil_[protection ? 1 : 0];
        return fromIndex < locks.size() && now < locks[fromIndex];
    }
    // The step down from this level is locked out (after a rollback), doubling.
    void lockStep(bool protection, std::size_t fromIndex, std::int64_t now) noexcept {
        auto& locks = stepLockUntil_[protection ? 1 : 0];
        if (fromIndex < locks.size()) locks[fromIndex] = now + (config_.lockoutMs << lockShift_);
        if (lockShift_ < kMaxLockoutShift) ++lockShift_;
    }
    PushControlTick change(PushControlTick out, std::size_t to, PushControlCause cause, std::string_view reason, std::int64_t now) {
        const int direction = to > index_ ? -1 : 1;   // -1 = less work
        out.from = ladder_[index_];
        out.to = ladder_[to];
        out.target = settingsOf(ladder_[to]);
        out.apply = true;
        out.cause = cause;
        applyPending_ = true;
        applyFromIndex_ = index_;
        applyToIndex_ = to;
        if (to < entryUnplanned_.size()) entryUnplanned_[to] = -1;
        if (to > index_ && to < cameFrom_.size()) { cameFrom_[to] = index_; cameFromCause_[to] = cause; }
        index_ = to;
        out.index = to;
        if (ladder_[to].multiplier == ladder_[applyFromIndex_].multiplier && ladder_[to].tier != ladder_[applyFromIndex_].tier) { lastTierChange_ = now; haveTierChange_ = true; }
        lastChange_ = now;
        haveChanged_ = true;
        ++changeCount_;
        for (std::size_t i = recent_.size() - 1; i > 0; --i) recent_[i] = recent_[i - 1];
        recent_[0] = now;
        if (lastDirection_ != 0 && direction != lastDirection_) {
            for (std::size_t i = reversals_.size() - 1; i > 0; --i) reversals_[i] = reversals_[i - 1];
            reversals_[0] = now;
            unsigned inWindow = 0;
            for (std::int64_t t : reversals_)
                if (t != kNever && now - t < config_.oscillationWindowMs) ++inWindow;
            if (inWindow >= config_.oscillationReversals) {
                lockoutUntil_ = now + (config_.lockoutMs << lockShift_);
                if (lockShift_ < kMaxLockoutShift) ++lockShift_;
                ++lockouts_;
                reversals_.fill(kNever);
            }
        }
        lastDirection_ = direction;
        out.reason = reason;
        return out;
    }
    PushControlTick finish(PushControlTick out, std::string_view reason) const {
        out.index = index_;
        out.reason = reason;
        return out;
    }

    PushControlConfig config_;
    PushGpuBudget budget_{};
    PushGpuBudgetLadder ladder_ = pushControlLadder(PushFgRequest{}, false, false);
    PushFgRequest request_{};
    std::array<std::int64_t, 16> lastUnplannedAt_{};
    std::array<PushControlCause, 16> cameFromCause_{};             // per level: what the step down to it was for
    std::array<std::size_t, 16> cameFrom_{};                       // per level: where the step down to it came from
    std::array<double, 16> lastUnplanned_{}, entryUnplanned_{};   // per level: last and first unplanned delay (-1 none)
    std::array<std::array<std::int64_t, 16>, 2> stepLockUntil_{};   // [latency, protection][from level]
    std::array<std::int64_t, 30> recent_{};      // change times, newest first (kNever-free once filled)
    std::array<std::int64_t, 16> reversals_{};   // reversal times, newest first
    std::size_t index_ = 0, applyFromIndex_ = 0, applyToIndex_ = 0, evalFromIndex_ = 0, evalToIndex_ = 0, refusedIndex_ = SIZE_MAX;
    std::int64_t lastNow_ = 0, lastChange_ = 0, overSince_ = kNever, healthySince_ = kNever, lockoutUntil_ = kNever;
    std::int64_t lastTierChange_ = 0, lastDecay_ = kNever, lastShiftDecay_ = kNever, refusedUntil_ = kNever, evalAt_ = 0, lastReturn_ = 0;
    double evalBefore_ = 0, evalSum_ = 0, unplannedNow_ = -1;
    unsigned evalCount_ = 0, lockShift_ = 0, returnShift_ = 0, probeShift_ = 0, rollbacks_ = 0, lockouts_ = 0, changeCount_ = 0;
    int lastDirection_ = 0;
    PushControlCause evalCause_ = PushControlCause::None;
    bool started_ = false, haveRequest_ = false, freezeMultiplier_ = false, freezeTier_ = false;
    bool evaluating_ = false, applyPending_ = false, haveReturned_ = false, haveChanged_ = false, refusedReport_ = false, haveTierChange_ = false;
};
} // namespace veyra::engine
