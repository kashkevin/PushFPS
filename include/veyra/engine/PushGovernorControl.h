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
// Sprint 4 (review 4, 20 November 2026):
//   - a link stall (a frame networkBurstPeakMs off its cadence, PushControlSample::networkStallMs) makes the
//     network burst for stallLingerMs, like mean jitter or loss over their lines; during a burst the embedded
//     budget still sees GPU time per picture (decode wait, missed deadlines, skipped pairs are withheld);
//   - nothing is stepped for trouble GPU work cannot explain: a decode wait longer than all the GPU work in force,
//     missed deadlines with the GPU not busy, or a latency excess (less jitter) beyond all the work plus the
//     budget, unless the GPU is saturated (then a backlog can be any length);
//   - a step is credited only when its trouble went away by no more than the work it removed allows and its
//     level had no episode end on its own within quietMemoryMs; the same trouble back at the level it went to
//     withdraws the credit (rolled back, counted); probes are judged on the ticks frame generation answers for;
//   - the embedded budget's ladder starts at the level in force, so recovery and probes never wait for its level;
//   - generation on/off counts as a rebuild (spaced by tierIntervalMs like a tier change); no probe rebuilds;
//   - standing down (kMaxUncredited useless steps) restores the user's request, then nothing until the next stream.
// Sprint 4 (review 5, 20 November 2026):
//   - every step is judged only on ticks frame generation can answer for (no burst; for latency, an excess within the
//     GPU work), and a step from a level the GPU loaded (at or above the budget's loadHigh) is not refused credit by
//     the quiet-baseline rule: a real GPU overload on a stalling link is answered;
//   - a return that goes over the budget within returnFailWindowMs failed: counted then, and the excess the level
//     showed is remembered for returnMemoryMs (weight falling linearly; forgotten on a user change or a new stream);
//     a latency step down passes the rebuild spacing (latency beats the rebuild count);
//   - a credit is withdrawn when its trouble comes back at the level it went to, over the budget on every tick for
//     sustainMs (or the budget's sustained protection trouble), within creditMemoryMs, whatever explains it there,
//     unless the GPU explains it at that level.

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
    double networkBurstJitterMs = 3;        // above: the network is bursting (decode-queue growth is then its doing)
    double networkBurstLossPercent = 0.5;
    std::int64_t tierIntervalMs = 60000;    // between tier changes (each rebuilds the graph)
    std::int64_t probeIntervalMs = 300000;  // while reduced and healthy now, one judged step up at most this often
                                            // (doubling after a failed probe, up to 4x), even before a full hold
    // Sprint 4 (review 4). A frame this far off its cadence (interarrival excess, ms) is a stall of the link: the
    // network bursts for stallLingerMs after it (the mean jitter, RFC 3550 /16, hides 20-80 ms stalls).
    double networkBurstPeakMs = 8;
    std::int64_t stallLingerMs = 3000;
    // GPU work explains a wait only up to its own length, or anything once the GPU is saturated (a backlog).
    double saturatedLoad = 0.95;           // GPU time per source interval at which any wait can be the GPU's
    double busyLoad = 0.60;                // missed presentation deadlines are the GPU's only at or above this load
    double returnLoad = 0.725;             // after a protection step: the level above must be predicted under this load
    std::int64_t quietMemoryMs = 600000;   // trouble that ended on its own at a level this recently: that level's quiet
                                           // baseline returns by itself, so a step from it earns no credit
    // Sprint 4 (review 5). A return toward the request fails when the latency goes over the budget within
    // returnFailWindowMs of it; the excess the level above then showed is remembered for returnMemoryMs (its weight
    // falling linearly to nothing over that time; reset when the user changes a setting or a stream starts), so the
    // same return is not tried again into over-budget latency every few minutes.
    std::int64_t returnFailWindowMs = 40000;
    std::int64_t returnMemoryMs = 10800000;   // 3 h
    // A credited step whose trouble comes back at the level it went to within creditMemoryMs is withdrawn, whatever
    // explains the trouble there (unless the GPU explains it at that level: then more work would only add to it).
    std::int64_t creditMemoryMs = 1800000;
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
    c.networkBurstPeakMs = std::isfinite(c.networkBurstPeakMs) && c.networkBurstPeakMs > 0 ? c.networkBurstPeakMs : standard.networkBurstPeakMs;
    c.stallLingerMs = ms(c.stallLingerMs, 0);
    const auto share = [](double v, double fallback) { return std::isfinite(v) && v > 0 && v <= 4 ? v : fallback; };
    c.saturatedLoad = share(c.saturatedLoad, standard.saturatedLoad);
    c.busyLoad = share(c.busyLoad, standard.busyLoad);
    c.returnLoad = share(c.returnLoad, standard.returnLoad);
    c.quietMemoryMs = ms(c.quietMemoryMs, 0);
    c.returnFailWindowMs = ms(c.returnFailWindowMs, 1000);
    c.returnMemoryMs = c.returnMemoryMs < 0 ? standard.returnMemoryMs : c.returnMemoryMs > INT64_C(86400000) ? INT64_C(86400000) : c.returnMemoryMs;
    c.creditMemoryMs = ms(c.creditMemoryMs, 0);
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

// Request first (always level 0, exactly as asked), then lower selectable multipliers at the request's tier
// (unless the multiplier is frozen: a multiplier change does not rebuild the graph), then lower tiers at the
// lowest multiplier (unless the tier is frozen: each tier change rebuilds the graph), then off (unless the
// multiplier is frozen).
inline PushGpuBudgetLadder pushControlLadder(PushFgRequest request, bool freezeMultiplier, bool freezeTier) {
    request = pushNormalizedFgRequest(request);
    PushGpuBudgetLadder ladder;
    const auto push = [&ladder](unsigned t, unsigned m) {
        if (ladder.count < ladder.levels.size()) ladder.levels[ladder.count++] = PushGpuLevel{t, m};
    };
    push(request.tier, request.multiplier);
    if (request.multiplier < 2) return ladder;
    unsigned lowest = request.multiplier;
    if (!freezeMultiplier) {
        const unsigned top = request.multiplier > kPushFgLargestMultiplier ? kPushFgLargestMultiplier + 1 : request.multiplier;
        for (unsigned m = top - 1; m >= 2; --m)
            if (pushFgMultiplierSelectable(request.method, m)) { push(request.tier, m); lowest = m; }
    }
    if (!freezeTier)
        for (unsigned t = request.tier; t-- > 0;) push(t, lowest);
    if (!freezeMultiplier) push(freezeTier ? request.tier : 0u, 1);
    return ladder;
}

// One tick's measurements. Negative or non-finite = not measured.
struct PushControlSample {
    std::int64_t nowMs = 0;              // monotonic
    double sourceFps = -1;
    double latencyP95Ms = -1;            // decoded picture to present call, P95 of the last second
    double networkJitterMs = -1;         // the stream's frame jitter: delay no frame generation setting can change
    double lossPercent = -1;             // the stream's packet loss, percent
    double networkStallMs = -1;          // the largest frame interarrival excess first seen this tick (a stall of the
                                         // link; PushGovernorStall.h derives it from the windowed peak); -1 unknown
    PushGpuBudgetSample budget{};        // what PushGpuBudget is fed (its now100ns is set from nowMs)
    // SmartLink's view of the link (sprint 4, phase 2), passed as plain values by the caller (the app reads
    // remoteplay::smartlink::latestShared(); this header stays free of it). linkKnown false = no fresh decision:
    // exactly the behaviour without SmartLink.
    bool linkKnown = false;
    bool linkLimited = false;            // the stutter comes from the link: lowering graphics would not help
    bool linkHeadroom = false;           // the link carries the requested rate with margin
};

enum class PushControlCause : std::uint8_t { None = 0, Decode = 1, Presentation = 2, GpuLoad = 3, Latency = 4, Recovery = 5, Rollback = 6, Probe = 7, StandDown = 8 };
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
    case PushControlCause::StandDown: return "stand-down";
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
    bool protectionTrouble = false;      // the GPU budget saw trouble this tick (or the network bursts)
    bool networkBurst = false;           // jitter, loss or a stall seen within stallLingerMs: waits are the link's
    bool gpuExplains = true;             // the trouble this tick is one GPU work can account for (see update())
    bool gpuTrouble = false;             // protection trouble that GPU work explains (what a step down may answer)
    bool linkLimited = false;            // SmartLink said the link limits the stream: only GPU evidence acts
    bool linkHeadroom = false;           // SmartLink's headroom held with the GPU healthy: the shorter return hold
    double unplannedMs = -1;             // latency P95 minus the planned hold; -1 unknown
    double returnLineMs = -1;            // a step up needs the unplanned delay under this (when reduced)
    bool userOverride = false;           // the user changed a setting this tick
    bool refused = false;                // the last level asked for was reported refused (applyRefused)
    bool credited = false;               // this tick credited the last step down (its trouble went away by its doing)
    std::string_view reason = "start";
    std::string_view budgetReason = "start";
};

class PushGovernorControl {
public:
    static constexpr std::int64_t kRefusedHoldMs = 60000;
    static constexpr unsigned kMaxLockoutShift = 4, kMaxReturnShift = 3, kMaxProbeShift = 2, kMaxUncredited = 2;
    static constexpr std::int64_t kPredictionMemory = 4;   // healthy holds

    explicit PushGovernorControl(PushControlConfig settings = {}) : config_(pushSanitizedControlConfig(settings)) {
        recent_.fill(kNever);
        reversals_.fill(kNever);
        failedExcess_.fill(-1);
        failedExcessAt_.fill(kNever);
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
    bool stoodDown() const noexcept { return standDown_; }

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
        resetBudget();
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
        // GPU work per source picture in force now (ms) and its share of the source interval; -1 unknown.
        const double workMs = known(in.budget.gpuFrameMs) ? in.budget.gpuFrameMs + (m >= 2 && known(in.budget.fgMs) ? in.budget.fgMs : 0.0) : -1.0;
        const double load = workMs >= 0 && rateKnown ? workMs * in.sourceFps / 1000.0 : -1.0;
        workNow_ = workMs;
        if (out.unplannedMs >= 0) {
            lastUnplanned_[index_] = out.unplannedMs;
            lastUnplannedAt_[index_] = in.nowMs;
            if (entryUnplanned_[index_] < 0) entryUnplanned_[index_] = out.unplannedMs;
        }
        if (load >= 0) {
            lastLoad_[index_] = load;
            lastLoadAt_[index_] = in.nowMs;
            if (entryLoad_[index_] < 0) entryLoad_[index_] = load;
        }
        // The network bursts while the mean jitter or the loss is over its line, and for stallLingerMs after a frame
        // came networkBurstPeakMs off its cadence (a stall: the mean jitter hides those). A growing decode queue and
        // late presents are then the clump of late frames, not the GPU.
        if (known(in.networkStallMs) && in.networkStallMs >= config_.networkBurstPeakMs) lastStallAt_ = in.nowMs;
        const bool stallRecent = lastStallAt_ != kNever && in.nowMs - lastStallAt_ < std::max<std::int64_t>(config_.stallLingerMs, 1);
        const bool networkBurst = (known(in.networkJitterMs) && in.networkJitterMs > config_.networkBurstJitterMs) ||
                                  (known(in.lossPercent) && in.lossPercent > config_.networkBurstLossPercent) || stallRecent;
        out.networkBurst = networkBurst;
        // SmartLink saying the link limits the stream: no reduction for the stutter the link explains (decode-queue
        // waits, missed deadlines, skipped pairs: the budget sees GPU time only). GPU load still acts, and a
        // latency excess still needs to be within the GPU work in force (latencyFixable) as always.
        const bool linkLimited = in.linkKnown && in.linkLimited;
        out.linkLimited = linkLimited;
        // The budget is fed every tick. During a burst only what the network cannot move is given (GPU time per
        // picture, generated-frame cost): decode-queue delay, missed deadlines and skipped pairs are withheld, so a
        // genuinely overloaded GPU is still seen, and the budget cannot read the burst as healthy either.
        PushGpuBudgetSample sample = in.budget;
        sample.now100ns = in.nowMs * 10000;
        if (networkBurst || linkLimited) sample = pushGpuOnlySample(sample);
        const PushGpuBudgetDecision budget = budget_.update(sample);
        out.budgetReason = linkLimited ? std::string_view("link-limited-gpu-only") : networkBurst ? std::string_view("network-burst-gpu-only") : budget.reason;
        // The signal itself (also at the budget's floor or in its dwell), not only its decision.
        const PushControlCause protectCause = protectionCause(budget_.lastTrouble());
        // The embedded budget runs on a ladder whose top is the level in force (reset at every change), so its own
        // level never holds the governor: a step of its own means "trouble sustained here", and is then forgotten.
        if (budget.index > 0) { resetBudget(); protectDue_ = true; }
        if (protectCause == PushControlCause::None) protectDue_ = false;
        // Trouble GPU work can explain: GPU load or generated-frame cost (the GPU's own); a decode-queue wait no
        // longer than one source picture's GPU work (or any wait once the GPU is saturated); missed deadlines only
        // with the GPU busy. A decoder or link stall longer than all the work in force is not frame generation's.
        const bool loadTrouble = load >= 0 && load >= budget_.config().loadHigh;
        const auto explains = [&](PushControlCause cause) {
            switch (cause) {
            case PushControlCause::GpuLoad: return true;
            case PushControlCause::Decode:
                return loadTrouble || load >= config_.saturatedLoad || (workMs >= 0 && known(sample.decodeQueueDelayMs) && sample.decodeQueueDelayMs <= workMs);
            case PushControlCause::Presentation: return loadTrouble || load >= config_.busyLoad;
            default: return false;
            }
        };
        out.gpuExplains = protectCause == PushControlCause::None || explains(protectCause);
        out.protectIndex = protectDue_ && out.gpuExplains ? std::min(index_ + 1, ladder_.count - 1) : index_;
        out.protectionTrouble = protectCause != PushControlCause::None || networkBurst;
        out.gpuTrouble = protectCause != PushControlCause::None && out.gpuExplains;
        // Healthy for a return: the budget's own reading; while the link limits the stream (its decode waits withheld)
        // the GPU side alone: no trouble and the load comfortably low.
        const bool budgetHealthy = !networkBurst && (linkLimited ? protectCause == PushControlCause::None && load >= 0 && load <= budget_.config().loadLow
                                                                 : budget.reason == "at-ceiling");
        // Episodes of trouble (over the latency budget, or protection trouble) at each level: one that ended with no
        // change made since it began ended on its own, which is that level's quiet baseline coming back.
        // An episode counts when it lasted sustainMs and ended clearly (under budget minus hysteresis, no protection
        // trouble): noise around the line is not an episode ending.
        const bool troubleNow = out.unplannedMs > config_.latencyBudgetMs || protectCause != PushControlCause::None;
        const bool clearNow = out.unplannedMs >= 0 && out.unplannedMs < config_.latencyBudgetMs - config_.hysteresisMs && protectCause == PushControlCause::None;
        if (troubleNow) {
            if (episodeSince_ == kNever) { episodeSince_ = in.nowMs; episodeIndex_ = index_; episodeChanges_ = changeCount_; }
        } else if (episodeSince_ != kNever && clearNow) {
            if (changeCount_ == episodeChanges_ && episodeIndex_ < selfEndedAt_.size() && in.nowMs - episodeSince_ >= std::max<std::int64_t>(config_.sustainMs, 1000))
                selfEndedAt_[episodeIndex_] = in.nowMs;
            episodeSince_ = kNever;
        }
        if (budgetHealthy) { if (budgetHealthySince_ == kNever) budgetHealthySince_ = in.nowMs; }
        else budgetHealthySince_ = kNever;
        // SmartLink's headroom with the GPU comfortably inside its budget: a return may come after the short hold.
        const bool headroom = in.linkKnown && in.linkHeadroom && !linkLimited && budgetHealthy && load >= 0 && load <= budget_.config().loadLow;
        out.linkHeadroom = headroom;
        if (headroom) { if (headroomSince_ == kNever) headroomSince_ = in.nowMs; }
        else headroomSince_ = kNever;

        // Over-budget and healthy clocks.
        const double high = config_.latencyBudgetMs, low = config_.latencyBudgetMs - config_.hysteresisMs;
        if (out.unplannedMs >= 0 && rateKnown) {
            if (out.unplannedMs > high) { if (overSince_ == kNever) overSince_ = in.nowMs; }
            else if (out.unplannedMs < low) overSince_ = kNever;
        } else overSince_ = kNever;
        // Over the budget on every tick (no hysteresis band): what a credit withdrawal needs, so isolated stall
        // seconds between quieter ones are not "the same trouble back".
        if (out.unplannedMs > high && rateKnown) { if (overEverySince_ == kNever) overEverySince_ = in.nowMs; }
        else overEverySince_ = kNever;
        // A return that goes over the budget within returnFailWindowMs failed (review 5, blocker 2): counted now, not
        // at the later step down (which may wait), and the excess the level showed is remembered (returnMemoryMs).
        if (haveReturned_ && index_ == returnedTo_ && in.nowMs - lastReturn_ < config_.returnFailWindowMs && out.unplannedMs > high) {
            if (!returnFailed_) { returnFailed_ = true; returnShift_ = returnShift_ < kMaxReturnShift ? returnShift_ + 1 : kMaxReturnShift; }
            if (index_ < failedExcess_.size() && (failedExcessAt_[index_] == kNever || in.nowMs - failedExcessAt_[index_] > config_.returnFailWindowMs ||
                                                  out.unplannedMs > failedExcess_[index_])) failedExcess_[index_] = out.unplannedMs;
            if (index_ < failedExcessAt_.size()) failedExcessAt_[index_] = in.nowMs;
        }
        // The line a return must stay under: budget minus hysteresis after a latency step; after a protection step
        // the budget itself (the latency never asked for that step, so it need not be lower than before it).
        const bool protectionReturn = index_ > 0 && index_ < cameFromCause_.size() && isProtection(cameFromCause_[index_]);
        const double returnLine = protectionReturn ? high : low;
        out.returnLineMs = returnLine;
        const bool latencyHealthy = rateKnown && out.unplannedMs >= 0 && out.unplannedMs < returnLine && !out.protectionTrouble;
        if (latencyHealthy) { if (healthySince_ == kNever) healthySince_ = in.nowMs; }
        else healthySince_ = kNever;
        // The latency the GPU work in force can explain: the unplanned delay less the network's jitter less all the
        // work per picture is what no frame generation level can remove; over the budget, a step cannot fix it
        // (unless the GPU is saturated: then a backlog can be any length).
        const bool latencyFixable = workMs >= 0 && known(in.networkJitterMs) &&
                                    (load >= config_.saturatedLoad || out.unplannedMs - in.networkJitterMs - workMs <= high);

        // Evaluation of the last step down (and of a probe).
        if (evaluating_ && rateKnown) {
            const double v = targetValue(evalCause_, in);
            // A step (and a probe) is judged only on ticks frame generation can answer for: a stall that would also
            // have come at the level it left (the link bursting, or a latency excess longer than all the GPU work) is
            // not the step's (review 5, blocker 1: Wi-Fi stalls in the means rolled back real GPU relief). A GPU-load
            // step is judged on GPU time per picture, which the link does not move: every tick counts.
            const bool latencyTarget = evalCause_ == PushControlCause::Latency || evalCause_ == PushControlCause::Probe;
            const bool counts = evalCause_ == PushControlCause::GpuLoad || (!networkBurst && (!latencyTarget || v <= high || latencyFixable));
            if (v >= 0 && counts) { evalSum_ += v; ++evalCount_; }
            if (workMs >= 0) { evalWorkSum_ += workMs; ++evalWorkCount_; }
            if (evalCause_ == PushControlCause::Probe && protectCause != PushControlCause::None && out.gpuExplains) evalTrouble_ = true;
        }
        out.index = index_;
        if (ladder_.count <= 1) return finish(out, freezeMultiplier_ || freezeTier_ ? "suspended-by-user" : "nothing-to-change");
        if (!rateKnown) return finish(out, "unknown-input");
        if (evaluating_ && in.nowMs - evalAt_ >= config_.evaluateMs) {
            // A rollback that rebuilds the graph waits out the rebuild spacing rather than giving up its verdict.
            const bool waitingForRebuild = index_ == evalToIndex_ && tierBlocked(evalFromIndex_, in.nowMs);
            const bool tooLate = index_ != evalToIndex_ || (in.nowMs - evalAt_ >= 3 * config_.evaluateMs && !waitingForRebuild);
            if (evalCause_ == PushControlCause::Probe) {
                // A probe is kept unless, on the ticks that count, the level it tried went over the budget or brought
                // GPU trouble; else back down, and the next probe waits twice as long (up to 4x).
                const double after = evalCount_ > 0 ? evalSum_ / static_cast<double>(evalCount_) : 0.0;
                if (after <= high && !evalTrouble_) { evaluating_ = false; probeShift_ = 0; }
                else if (gatesOpen(in.nowMs) && index_ == evalToIndex_ && !tierBlocked(evalFromIndex_, in.nowMs)) {
                    evaluating_ = false;
                    ++rollbacks_;
                    if (probeShift_ < kMaxProbeShift) ++probeShift_;
                    return change(out, evalFromIndex_, PushControlCause::Rollback, evalTrouble_ ? "rollback-probe-gpu-trouble" : "rollback-probe-over-budget", in.nowMs);
                } else if (tooLate) evaluating_ = false;
                else return finish(out, "rollback-waiting");
            } else if (evalCount_ == 0) { evaluating_ = false; }   // nothing measured since: no verdict, no action
            else {
                const double after = evalSum_ / static_cast<double>(evalCount_);
                const double needed = evalBefore_ - std::max(config_.improveMinimumMs, evalBefore_ * config_.improveShare);
                // Credit only what the step can explain: the trouble was there before it (its trigger was sustained)
                // and it went away by about what the step took off the GPU. A stall that ended on its own during
                // the evaluation (the quiet baseline returning) is not the step's doing.
                const double workAfter = evalWorkCount_ > 0 ? evalWorkSum_ / static_cast<double>(evalWorkCount_) : -1.0;
                const double workDrop = evalWorkBefore_ >= 0 && workAfter >= 0 ? evalWorkBefore_ - workAfter : -1.0;
                // (a) the GPU work went down, (b) by no less than the delay that went away unless the GPU was saturated
                // (no step removes more waiting than all the work in force before it), and (c) the level it left has
                // not had trouble end on its own within quietMemoryMs (then a quiet spell proves nothing).
                bool explained = true;
                if (evalCause_ == PushControlCause::Latency || evalCause_ == PushControlCause::Decode)
                    explained = workDrop > 0 && (evalLoadBefore_ >= config_.saturatedLoad || evalBefore_ - after <= evalWorkBefore_ + config_.improveMinimumMs);
                else if (evalCause_ == PushControlCause::Presentation) explained = workDrop > 0;
                const std::int64_t selfEnded = evalFromIndex_ < selfEndedAt_.size() ? selfEndedAt_[evalFromIndex_] : kNever;
                // Not when the GPU was loaded before the step (review 5, blocker 1): then the GPU explains the trouble, and
                // stall episodes that ended on their own say nothing about it.
                const bool loadedBefore = evalLoadBefore_ >= budget_.config().loadHigh;
                if (evalCause_ != PushControlCause::GpuLoad && !loadedBefore && selfEnded != kNever && evalAt_ - selfEnded < config_.quietMemoryMs) explained = false;
                if (after <= needed && explained) {
                    evaluating_ = false;
                    // Credited, provisionally: should the same trouble come back at the level it went to within
                    // quietMemoryMs, about as large, the step did not fix it after all (see below).
                    haveCredit_ = true;
                    out.credited = true;
                    creditAt_ = in.nowMs;
                    creditCause_ = evalCause_;
                    creditBefore_ = evalBefore_;
                    creditFrom_ = evalFromIndex_;
                    creditTo_ = evalToIndex_;
                }
                else if (gatesOpen(in.nowMs) && index_ == evalToIndex_ && targetValue(evalCause_, in) >= 0 && !out.gpuTrouble &&
                         !tierBlocked(evalFromIndex_, in.nowMs)) {
                    evaluating_ = false;
                    ++rollbacks_;
                    // K steps in a stream that did not earn their keep: stop acting for the rest of the stream (the
                    // user's request is put back first).
                    if (++uncredited_ >= kMaxUncredited) standDown_ = true;
                    lockStep(isProtection(evalCause_), evalFromIndex_, in.nowMs);
                    return change(out, evalFromIndex_, PushControlCause::Rollback, after <= needed ? "rollback-improvement-not-explained" : "rollback-target-not-improved", in.nowMs);
                } else if (tooLate) evaluating_ = false;   // too late: no verdict
                else return finish(out, "rollback-waiting");
            }
        }

        // Stood down (kMaxUncredited steps did not earn their keep): the user's request back, then nothing until the
        // next stream. The restore keeps the minimum interval, the per-minute cap and the rebuild spacing, and waits
        // while the GPU is in trouble it can explain.
        if (standDown_) {
            if (index_ == 0) return finish(out, "stood-down");
            if (evaluating_) return finish(out, "stood-down-judging");
            if (out.gpuTrouble || !gatesOpen(in.nowMs, true) || tierBlocked(0, in.nowMs) ||
                (refusedIndex_ == 0 && in.nowMs < refusedUntil_))
                return finish(out, "stood-down-restore-waiting");
            return change(out, 0, PushControlCause::StandDown, "stood-down-restore-request", in.nowMs);
        }

        // 1. protection (decode, presentation, GPU load) and 2. latency: one step down.
        // Over the budget now, and (between the thresholds the clock holds) for sustainMs.
        // Cause first: the part of the unplanned delay the network's own jitter explains is not frame generation's,
        // so a latency step needs the rest over the budget (and the jitter measured), a level with fewer generated
        // frames to go to (never a tier: a tier change rebuilds the graph and hardly moves latency), and the excess
        // within what the GPU work in force can explain.
        // While the network bursts nothing is attributed to frame generation (a decode stall then is the clump).
        const bool attributable = known(in.networkJitterMs) && !networkBurst && out.unplannedMs - in.networkJitterMs > high && latencyFixable;
        std::size_t fewer = SIZE_MAX;
        for (std::size_t j = index_ + 1; j < ladder_.count && fewer == SIZE_MAX; ++j)
            if (ladder_[j].multiplier < ladder_[index_].multiplier) fewer = j;
        const bool latencyOver = attributable && fewer != SIZE_MAX && overSince_ != kNever && in.nowMs - overSince_ >= config_.sustainMs;
        // The trouble a credited step went away from is back at the level it went to, sustained and about as large,
        // and the GPU does not explain it there: the quiet spell that credited it was the baseline, not the step. That
        // step is withdrawn (rolled back, locked, counted as one that did not earn its keep), whatever explains the
        // trouble (review 5: a stall the GPU work cannot explain at the lower level kept the credit for good).
        if (haveCredit_ && (index_ != creditTo_ || in.nowMs - creditAt_ >= config_.creditMemoryMs)) haveCredit_ = false;
        if (haveCredit_ && !evaluating_ && creditCause_ != PushControlCause::GpuLoad && !out.gpuTrouble && !(load >= config_.saturatedLoad)) {
            const bool sustained = creditCause_ == PushControlCause::Latency
                                       ? overEverySince_ != kNever && in.nowMs - overEverySince_ >= config_.sustainMs
                                       : protectDue_ && protectCause == creditCause_;
            const double v = targetValue(creditCause_, in);
            if (sustained && v >= 0 && v >= 0.8 * creditBefore_) {
                if (!gatesOpen(in.nowMs)) return finish(out, "trouble-returned-waiting");
                if (tierBlocked(creditFrom_, in.nowMs)) return finish(out, "trouble-returned-tier-interval");
                haveCredit_ = false;
                ++rollbacks_;
                if (++uncredited_ >= kMaxUncredited) standDown_ = true;
                lockStep(isProtection(creditCause_), creditFrom_, in.nowMs);
                return change(out, creditFrom_, PushControlCause::Rollback, "rollback-trouble-returned", in.nowMs);
            }
        }
        const bool protectDown = out.protectIndex > index_ && protectCause != PushControlCause::None;
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
            // A step that rebuilds the graph (a tier change, or generation on/off) waits tierIntervalMs after the last,
            // except a latency step down: latency beats the rebuild count (review 5, blocker 2: a failed return waited
            // 60 s over the budget before it could be undone).
            if (!latencyOver || protectDown) { if (tierBlocked(next, in.nowMs)) return finish(out, "tier-interval"); }
            if (next == refusedIndex_ && in.nowMs < refusedUntil_) return finish(out, "refused-hold");
            evaluating_ = true;
            evalCause_ = cause;
            evalAt_ = in.nowMs;
            evalFromIndex_ = index_;
            evalToIndex_ = next;
            evalBefore_ = targetValue(cause, in);
            evalWorkBefore_ = workMs;
            evalLoadBefore_ = load;
            evalSum_ = evalWorkSum_ = 0;
            evalCount_ = evalWorkCount_ = 0;
            evalTrouble_ = false;
            if (evalBefore_ < 0) evaluating_ = false;   // its target is not measured: no verdict later
            // A return counts as failed when the level above did not last two holds (unless already counted above).
            if (haveReturned_ && !returnFailed_ && in.nowMs - lastReturn_ < 2 * config_.healthyHoldMs) returnShift_ = returnShift_ < kMaxReturnShift ? returnShift_ + 1 : kMaxReturnShift;
            haveReturned_ = false;
            return change(out, next, cause, protectDown ? "protect-step-down" : "latency-step-down", in.nowMs);
        }
        if (out.unplannedMs > high && fewer == SIZE_MAX) return finish(out, "at-floor");
        if (protectDue_ && !out.gpuExplains) return finish(out, "trouble-not-gpu");
        if (out.unplannedMs > high && !latencyFixable && known(in.networkJitterMs)) return finish(out, "over-budget-not-gpu");
        if (linkLimited && known(in.budget.decodeQueueDelayMs) && in.sourceFps > 0 &&
            in.budget.decodeQueueDelayMs * in.sourceFps / 1000.0 >= budget_.config().decodeHighShare)
            return finish(out, "link-limited-no-reduction");

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
        // skip tier levels; coming back through a level never measured would be a guess). It does not wait for
        // the embedded budget's own level: after a protection step the budget's healthy reading (every tick of the
        // hold) and the GPU load predicted for the level above stand in for it.
        if (index_ > 0) {
            const std::size_t up = cameFrom_[index_] < index_ ? cameFrom_[index_] : index_ - 1;
            if (out.protectionTrouble) return finish(out, "protection-trouble");   // (also part of latencyHealthy)
            if (!latencyHealthy) return finish(out, out.unplannedMs < 0 ? "unknown-input" : "hold");
            if (protectionReturn && !budgetHealthy) return finish(out, "protection-not-healthy");
            const std::int64_t since = protectionReturn ? std::max(healthySince_, budgetHealthySince_) : healthySince_;
            const std::int64_t hold = config_.healthyHoldMs << returnShift_;
            // With SmartLink's headroom held (and the GPU healthy) through it, half a base hold is enough.
            const std::int64_t shortHold = std::max(config_.healthyHoldMs / 2, config_.minIntervalMs);
            const bool headroomHeld = headroomSince_ != kNever && in.nowMs - std::max(since, headroomSince_) >= shortHold && shortHold < hold;
            const std::int64_t needed = headroomHeld ? shortHold : hold;
            const bool holdDone = in.nowMs - since >= needed && (!haveChanged_ || in.nowMs - lastChange_ >= needed);
            // Short bursts can keep resetting the hold: a judged probe step up then still comes, capped. Never one
            // that rebuilds the graph (generation on, a tier): its rollback would be a second rebuild, or would have to
            // wait tierIntervalMs over the budget. Those levels come back after a full healthy hold only.
            const bool probeDue = !holdDone && !evaluating_ && haveChanged_ && in.nowMs - lastChange_ >= (config_.probeIntervalMs << probeShift_) &&
                                  !changesTier(index_, up);
            if (!holdDone && !probeDue) return finish(out, "healthy-waiting");
            // The level above, predicted: now plus what the step down saved when it was made (its last sample
            // against this level's first one, which cancels what both shared, such as network jitter).
            // Measurements older than kPredictionMemory healthy holds are forgotten: the next try finds out again
            // (and a failed try doubles the hold before the one after).
            const bool fresh = lastUnplannedAt_[up] != kNever && in.nowMs - lastUnplannedAt_[up] < kPredictionMemory * config_.healthyHoldMs;
            const double aboveLast = fresh ? lastUnplanned_[up] : -1.0, hereFirst = entryUnplanned_[index_];
            const double predicted = aboveLast >= 0 && hereFirst >= 0 ? out.unplannedMs + std::max(0.0, aboveLast - hereFirst) : out.unplannedMs;
            if (!probeDue && predicted >= returnLine) return finish(out, "level-above-over-budget");
            // A return to the level above that failed (over the budget soon after it) is remembered for returnMemoryMs,
            // its weight falling linearly: the same return waits until the excess, so weighted, fits (probes too).
            if (up < failedExcess_.size() && failedExcessAt_[up] != kNever && config_.returnMemoryMs > 0 && in.nowMs - failedExcessAt_[up] < config_.returnMemoryMs) {
                const double weight = 1.0 - static_cast<double>(in.nowMs - failedExcessAt_[up]) / static_cast<double>(config_.returnMemoryMs);
                const double base = hereFirst >= 0 ? hereFirst : out.unplannedMs;
                if (out.unplannedMs + weight * std::max(0.0, failedExcess_[up] - base) >= returnLine) return finish(out, "level-above-failed-return");
            }
            // After a protection step, the GPU load above as the budget predicts it: now x (its last / here first).
            if (protectionReturn && !probeDue) {
                const bool loadFresh = lastLoadAt_[up] != kNever && in.nowMs - lastLoadAt_[up] < kPredictionMemory * config_.healthyHoldMs;
                const double loadAbove = loadFresh ? lastLoad_[up] : -1.0, loadHere = entryLoad_[index_];
                if (load >= 0 && loadAbove >= 0 && loadHere > 0 && load * (loadAbove / loadHere) > config_.returnLoad) return finish(out, "level-above-gpu-over-budget");
            }
            if (!gatesOpen(in.nowMs)) return finish(out, lockedOut(in.nowMs) ? "locked-out" : "rate-limited");
            if (up == refusedIndex_ && in.nowMs < refusedUntil_) return finish(out, "refused-hold");
            if (tierBlocked(up, in.nowMs)) return finish(out, "tier-interval");
            if (probeDue) {
                evaluating_ = true;
                evalCause_ = PushControlCause::Probe;
                evalAt_ = in.nowMs;
                evalFromIndex_ = index_;
                evalToIndex_ = up;
                evalBefore_ = out.unplannedMs;
                evalWorkBefore_ = workMs;
                evalLoadBefore_ = load;
                evalSum_ = evalWorkSum_ = 0;
                evalCount_ = evalWorkCount_ = 0;
                evalTrouble_ = false;
                return change(out, up, PushControlCause::Probe, "probe-step-up", in.nowMs);
            }
            evaluating_ = false;
            haveReturned_ = true;
            returnFailed_ = false;
            returnedTo_ = up;
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
        resetBudget();
        lastUnplanned_.fill(-1);
        lastUnplannedAt_.fill(kNever);
        lastLoad_.fill(-1);
        entryLoad_.fill(-1);
        lastLoadAt_.fill(kNever);
        selfEndedAt_.fill(kNever);
        episodeSince_ = kNever;
        haveCredit_ = false;
        budgetHealthySince_ = kNever;
        headroomSince_ = kNever;
        cameFrom_.fill(SIZE_MAX);
        cameFromCause_.fill(PushControlCause::None);
        entryUnplanned_.fill(-1);
        failedExcess_.fill(-1);
        failedExcessAt_.fill(kNever);
        for (auto& locks : stepLockUntil_) locks.fill(kNever);
        refusedIndex_ = SIZE_MAX;
        overSince_ = healthySince_ = overEverySince_ = kNever;
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
            resetBudget();
        }
        // The rate limits (minimum interval, changes per minute) and the oscillation lock-out with its back-off
        // carry over: they hold across streams too.
        returnShift_ = 0;
        probeShift_ = 0;
        uncredited_ = 0;
        standDown_ = false;
        lastStallAt_ = kNever;
        haveReturned_ = false;
        returnFailed_ = false;
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
        case PushControlCause::StandDown:
        case PushControlCause::Rollback: return -1.0;
        case PushControlCause::Probe: return unplannedNow_;
        }
        return -1.0;
    }
    // The graph is rebuilt (a hitch) when generation turns on or off, or, both generating, the tier differs.
    // Every such step, whatever its reason, waits tierIntervalMs after the last one.
    bool changesTier(std::size_t from, std::size_t to) const noexcept {
        const PushGpuLevel a = ladder_[from], b = ladder_[to];
        return (a.multiplier >= 2) != (b.multiplier >= 2) || (a.multiplier >= 2 && b.multiplier >= 2 && a.tier != b.tier);
    }
    // The embedded budget's ladder starts at the level in force: its step down means "sustained trouble here".
    void resetBudget() {
        const PushGpuLevel l = ladder_[index_ < ladder_.count ? index_ : 0];
        budget_.reset(pushGpuBudgetLadder(request_.method, l.multiplier, l.tier));
        protectDue_ = false;
        budgetHealthySince_ = kNever;
        headroomSince_ = kNever;
    }
    bool tierBlocked(std::size_t to, std::int64_t now) const noexcept {
        return to < ladder_.count && changesTier(index_, to) && haveTierChange_ && now - lastTierChange_ < config_.tierIntervalMs;
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
        if (to < entryUnplanned_.size()) { entryUnplanned_[to] = -1; entryLoad_[to] = -1; }
        if (to > index_ && to < cameFrom_.size()) { cameFrom_[to] = index_; cameFromCause_[to] = cause; }
        index_ = to;
        out.index = to;
        resetBudget();
        if (changesTier(applyFromIndex_, to)) { lastTierChange_ = now; haveTierChange_ = true; }
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
    // Its own dwell is not used: the governor's minimum interval and rate cap space the steps.
    static PushGpuBudgetConfig embeddedBudgetConfig() noexcept {
        PushGpuBudgetConfig c;
        c.minDwell100ns = 0;
        return c;
    }
    PushGpuBudget budget_{pushGpuBudgetLadder(PushFgMethod::PushFlow, 1, 0), embeddedBudgetConfig()};
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
    std::array<double, 16> lastLoad_{}, entryLoad_{};             // per level: last and first GPU load (-1 none)
    std::array<std::int64_t, 16> lastLoadAt_{};
    std::array<std::int64_t, 16> selfEndedAt_{};                  // per level: when trouble there last ended on its own
    std::array<double, 16> failedExcess_{};                       // per level: the excess a failed return to it showed
    std::array<std::int64_t, 16> failedExcessAt_{};               // ... and when (kNever: none)
    std::size_t returnedTo_ = 0;
    std::int64_t overEverySince_ = kNever;
    bool returnFailed_ = false;
    std::int64_t lastStallAt_ = kNever, budgetHealthySince_ = kNever, episodeSince_ = kNever, headroomSince_ = kNever;
    std::size_t episodeIndex_ = 0;
    unsigned episodeChanges_ = 0;
    double evalBefore_ = 0, evalSum_ = 0, unplannedNow_ = -1, workNow_ = -1, evalWorkBefore_ = -1, evalLoadBefore_ = -1, evalWorkSum_ = 0;
    unsigned evalWorkCount_ = 0;
    bool protectDue_ = false, evalTrouble_ = false, haveCredit_ = false;
    std::int64_t creditAt_ = 0;
    double creditBefore_ = 0;
    std::size_t creditFrom_ = 0, creditTo_ = 0;
    PushControlCause creditCause_ = PushControlCause::None;
    unsigned evalCount_ = 0, lockShift_ = 0, returnShift_ = 0, probeShift_ = 0, uncredited_ = 0, rollbacks_ = 0, lockouts_ = 0, changeCount_ = 0;
    int lastDirection_ = 0;
    PushControlCause evalCause_ = PushControlCause::None;
    bool started_ = false, haveRequest_ = false, freezeMultiplier_ = false, freezeTier_ = false;
    bool evaluating_ = false, applyPending_ = false, haveReturned_ = false, haveChanged_ = false, refusedReport_ = false, haveTierChange_ = false, standDown_ = false;
};
} // namespace veyra::engine
