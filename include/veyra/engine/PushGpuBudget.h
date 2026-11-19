// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushHardwareCapabilities.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace veyra::engine {
// GPU workload budget: keeps headroom for decoding and presentation by stepping frame generation down.
//
// Portable and header-only. Tests: tests/remoteplay/GpuBudgetTests.inc. Nothing calls it yet; the hooks it
// needs are listed in docs/HARDWARE_COMPATIBILITY.md ("Engine hooks"). Unverified on Windows; the thresholds
// are starting points (estimates) that have never run on any GPU.
//
// What already exists, and what each piece does (read 19 November 2026):
//   FgRecoveryBudget (FgRecoveryBudget.h)   per PAIR. P95 of measured base (per source frame) and FG (per
//       group) GPU time over the last 1-2 s; admits a pair only if its first and last outputs can reach their
//       deadlines (10 ms or one output interval of grace), else the pair is shown without generated frames.
//       After a rejection, two admitted pairs end "limited". It never changes a setting.
//   FgCostSettling (same file)   after 1 s without an evaluation the next two are warm-up samples; failed
//       probes retry after 2, 4, then every 8 s. Stops one slow probe from starving FG for a minute.
//   VfgOverloadEstimate   VFG only: remembers a full group's cost so cheap reduced 2X groups cannot make an
//       overloaded mode look affordable; one full probe per 30 s.
//   admitLiveFg (LiveFgAdmission.h)   per pair: admit while the last generated timestamp, plus P95 completion
//       and present time, can still be met (10 ms grace).
//   LiveGpuScheduler   at most two batches in flight on the graph owner thread; waits are deadlines.
//   PushAdaptiveFg   per pair, picks the multiplier whose output is closest to the target for the measured
//       CONTENT rate (0.5 s hold). Load plays no part in it.
//   pushFlowBudgetWatch (PushFps.cpp)   UI, once a second: PushFlow only. After 8 s in which the budget skipped
//       nearly every pair (output < 1.25 x content), one tier down with a notice; on Fast a notice only. Never
//       steps back up; a tier the user raises again is left alone.
//   PushAutomaticMode (PushPerformanceMode.h, VEYRA_EXP_AUTO_MODE)   mode level: GPU busy share >= 0.90, a
//       queue of 2 or a poor network for 3 s -> one mode down; 30 s healthy -> one up (doubling on failed
//       returns), 5 s dwell. Its levels are multipliers; it has no quality tier and no decode or deadline input.
// What is missing, and is what this file adds:
//   1. Decode-queue delay and missed presentation deadlines as reasons to shed GPU work. Every piece above
//      protects frame generation's own deadline; none notices that decoding or presenting real frames waits.
//   2. A bounded ladder that sheds the cheapest-to-lose work first: quality tier, then multiplier, then
//      generation off, one step at a time, for every method (pushFlowBudgetWatch covers PushFlow's tier only).
//   3. Recovery: a step back up only after a sustained healthy period, only to what the user asked for, and
//      only when the higher level's predicted load (current load x the ratio of its last sample to this level's
//      first sample) stays below the midpoint of the healthy and trouble thresholds.
//   4. Thermal state and battery power as optional inputs.
//
// Invariants (tested):
//   - It never chooses more work than the user's request (index 0 of the ladder is the request).
//   - It never steps up on unknown input, on battery, or while any trouble signal is present; so it never
//     raises latency (a higher multiplier holds real frames longer) to show more frames under load.
//   - One step per decision; at least minDwell between ordinary steps; severe overload may step again after
//     severeHold, never faster.
//   - Unknown measurements are neither trouble nor healthy.
//
// Times are in 100 ns units, like the rest of the engine. Not thread-safe: one owner calls update().

enum class PushThermalState : std::uint8_t { Unknown = 0, Normal = 1, Throttling = 2 };

struct PushGpuLevel {
    unsigned tier = 0;
    unsigned multiplier = 1;   // 1 = frame generation off
    constexpr bool frameGeneration() const { return multiplier >= 2; }
};

// From the request down to "off": tiers first (at the requested multiplier), then each selectable multiplier
// below at tier 0, then off. A request without generation is a one-level ladder ("off").
struct PushGpuBudgetLadder {
    std::array<PushGpuLevel, 16> levels{};
    std::size_t count = 0;
    constexpr const PushGpuLevel& operator[](std::size_t index) const { return levels[index]; }
};
inline PushGpuBudgetLadder pushGpuBudgetLadder(PushFgMethod method, unsigned multiplier, unsigned tier) {
    PushGpuBudgetLadder ladder;
    const auto push = [&ladder](unsigned t, unsigned m) {
        if (ladder.count < ladder.levels.size()) ladder.levels[ladder.count++] = PushGpuLevel{t, m};
    };
    unsigned top = 0;
    for (unsigned m = multiplier > kPushFgLargestMultiplier ? kPushFgLargestMultiplier : multiplier; m >= 2; --m)
        if (pushFgMultiplierSelectable(method, m)) { top = m; break; }
    if (top >= 2) {
        const unsigned tiers = pushFgTierCount(method);
        const unsigned first = tier < tiers ? tier : tiers - 1;
        for (unsigned t = first + 1; t-- > 0;) push(t, top);
        for (unsigned m = top - 1; m >= 2; --m)
            if (pushFgMultiplierSelectable(method, m)) push(0, m);
    }
    push(0, 1);
    return ladder;
}

struct PushGpuBudgetConfig {
    std::int64_t troubleHold100ns = 20000000;    // 2 s of trouble before an ordinary step down
    std::int64_t severeHold100ns = 5000000;      // 0.5 s of severe overload before a step down, ignoring dwell
    std::int64_t minDwell100ns = 30000000;       // 3 s between ordinary steps
    std::int64_t healthyHold100ns = 200000000;   // 20 s healthy before a step up (doubles after a failed return, up to 8x)
    double loadHigh = 0.85;        // GPU time per source interval: trouble at or above
    double loadLow = 0.60;         //                                 healthy at or below
    double loadSevere = 1.25;      //                                 severe at or above (the GPU cannot keep up at all)
    double decodeHighShare = 0.50; // decode-queue delay as a share of the source interval: trouble at or above
    double decodeLowShare = 0.20;  //                                                      healthy at or below
    double decodeSevereShare = 1.0;
    double missedHigh = 0.02;      // share of output frames that missed their presentation deadline
    double missedSevere = 0.20;
    double skippedHigh = 0.25;     // share of source pairs the per-pair admission skipped
};

// One sample, typically one second of counters. Negative or non-finite = not measured.
struct PushGpuBudgetSample {
    std::int64_t now100ns = 0;
    double sourceIntervalMs = -1;     // console picture interval (33.4 for 30 fps content, 16.7 for 60)
    double outputIntervalMs = -1;     // interval between presented outputs
    double gpuFrameMs = -1;           // P95 GPU time of the per-source-frame work (FgRecoveryBudget::baseCost)
    double fgMs = -1;                 // P95 GPU time of frame generation per source pair (0 when generation is off)
    double decodeQueueDelayMs = -1;   // how long a decoded picture waited before the engine took it (P95)
    unsigned presented = 0;           // outputs presented in the sample
    unsigned missedDeadlines = 0;     // outputs that missed their presentation deadline in the sample
    double skippedPairShare = -1;     // pairs the per-pair admission skipped / pairs seen
    PushThermalState thermal = PushThermalState::Unknown;
    PushPowerSource power = PushPowerSource::Unknown;
};

struct PushGpuBudgetDecision {
    PushGpuLevel level{};
    std::size_t index = 0;   // 0 = the request; count-1 = generation off
    bool changed = false;
    std::string_view reason = "start";
};

// Reasons: start | hold | dwell | at-ceiling | at-floor | healthy-waiting | on-battery | bad-input |
//   trouble-<s> | step-down-<s> | severe-step-down-<s>, where <s> is gpu-load, generated-frame-cost,
//   decode-delay, missed-deadlines, skipped-pairs, thermal |
//   step-up-sustained-healthy | step-up-predicted-over-budget (healthy, but the higher level's last measured
//   load would not be healthy: stays).
class PushGpuBudget {
public:
    explicit PushGpuBudget(PushGpuBudgetLadder ladder = pushGpuBudgetLadder(PushFgMethod::PushFlow, 1, 0), PushGpuBudgetConfig settings = {})
        : config_(sanitized(settings)) { reset(ladder); }

    // A new source, settings change or user choice: start again at the request.
    void reset(PushGpuBudgetLadder ladder) {
        ladder_ = ladder.count ? ladder : pushGpuBudgetLadder(PushFgMethod::PushFlow, 1, 0);
        index_ = 0;
        started_ = haveStepUp_ = false;
        failedReturns_ = 0;
        lastNow_ = lastSwitch_ = lastStepUp_ = 0;
        troubleSince_ = severeSince_ = healthySince_ = kNever;
        for (auto& load : loadAt_) load = -1;
        for (auto& load : entryLoad_) load = -1;
    }
    const PushGpuBudgetConfig& config() const { return config_; }
    const PushGpuBudgetLadder& ladder() const { return ladder_; }
    PushGpuLevel level() const { return ladder_[index_]; }
    std::size_t index() const { return index_; }

    PushGpuBudgetDecision update(const PushGpuBudgetSample& in) {
        if (!started_) {
            started_ = true;
            lastNow_ = lastSwitch_ = in.now100ns;
        } else if (in.now100ns < lastNow_) {
            lastNow_ = lastSwitch_ = in.now100ns;
            troubleSince_ = severeSince_ = healthySince_ = kNever;
            return decide(false, "bad-input");
        }
        lastNow_ = in.now100ns;
        const Reading r = read(in);
        if (r.load >= 0) {
            loadAt_[index_] = r.load;
            if (entryLoad_[index_] < 0) entryLoad_[index_] = r.load;
        }

        if (r.signal != Signal::None) {
            healthySince_ = kNever;
            if (troubleSince_ == kNever) troubleSince_ = in.now100ns;
            if (r.severe) { if (severeSince_ == kNever) severeSince_ = in.now100ns; }
            else severeSince_ = kNever;
            if (index_ + 1 >= ladder_.count) return decide(false, "at-floor");
            const bool severeDue = r.severe && in.now100ns - severeSince_ >= config_.severeHold100ns && in.now100ns - lastSwitch_ >= config_.severeHold100ns;
            const bool ordinaryDue = in.now100ns - troubleSince_ >= config_.troubleHold100ns;
            if (!severeDue && !ordinaryDue) return decide(false, name(r.signal, Kind::Trouble));
            if (!severeDue && in.now100ns - lastSwitch_ < config_.minDwell100ns) return decide(false, "dwell");
            if (haveStepUp_) {
                const bool soon = in.now100ns - lastStepUp_ < 8 * config_.healthyHold100ns;
                failedReturns_ = soon ? (failedReturns_ < 3 ? failedReturns_ + 1 : 3u) : 0u;
                haveStepUp_ = false;
            }
            switchTo(index_ + 1, in.now100ns);
            return decide(true, name(r.signal, severeDue ? Kind::Severe : Kind::Step));
        }
        troubleSince_ = severeSince_ = kNever;
        if (!r.healthy) { healthySince_ = kNever; return decide(false, r.onBattery ? "on-battery" : "hold"); }
        if (healthySince_ == kNever) healthySince_ = in.now100ns;
        if (index_ == 0) return decide(false, "at-ceiling");
        if (in.now100ns - healthySince_ < (config_.healthyHold100ns << failedReturns_)) return decide(false, "healthy-waiting");
        if (in.now100ns - lastSwitch_ < config_.minDwell100ns) return decide(false, "dwell");
        // The cost ratio between two levels belongs to the method more than to the scene: the higher level's load
        // is predicted as the current load times the ratio between its last sample and this level's first one
        // (seconds apart, around the step down). It must stay at least halfway between healthy and trouble.
        const double higher = loadAt_[index_ - 1], here = entryLoad_[index_];
        if (higher >= 0 && here > 0 && r.load >= 0 && r.load * (higher / here) > (config_.loadLow + config_.loadHigh) / 2)
            return decide(false, "step-up-predicted-over-budget");
        switchTo(index_ - 1, in.now100ns);
        haveStepUp_ = true;
        lastStepUp_ = in.now100ns;
        healthySince_ = in.now100ns;
        return decide(true, "step-up-sustained-healthy");
    }

private:
    enum class Signal : std::uint8_t { None, GpuLoad, GeneratedFrameCost, DecodeDelay, MissedDeadlines, SkippedPairs, Thermal };
    enum class Kind : std::uint8_t { Trouble, Step, Severe };
    struct Reading {
        Signal signal = Signal::None;
        bool severe = false;
        bool healthy = false;
        bool onBattery = false;
        double load = -1;
    };
    static constexpr std::int64_t kNever = INT64_MIN;
    static bool known(double value) { return std::isfinite(value) && value >= 0; }

    static PushGpuBudgetConfig sanitized(PushGpuBudgetConfig c) {
        const PushGpuBudgetConfig standard;
        const std::int64_t longest = INT64_C(864000000000);   // a day: keeps the back-off shift far from overflow
        const auto time = [longest](std::int64_t v) { return v < 0 ? std::int64_t(0) : v > longest ? longest : v; };
        c.troubleHold100ns = time(c.troubleHold100ns);
        c.severeHold100ns = time(c.severeHold100ns);
        c.minDwell100ns = time(c.minDwell100ns);
        c.healthyHold100ns = time(c.healthyHold100ns);
        const auto share = [](double v, double fallback) { return std::isfinite(v) && v > 0 ? v : fallback; };
        c.loadHigh = share(c.loadHigh, standard.loadHigh);
        c.loadLow = std::isfinite(c.loadLow) && c.loadLow >= 0 && c.loadLow <= c.loadHigh ? c.loadLow : (standard.loadLow <= c.loadHigh ? standard.loadLow : c.loadHigh);
        c.loadSevere = std::isfinite(c.loadSevere) && c.loadSevere >= c.loadHigh ? c.loadSevere : c.loadHigh;
        c.decodeHighShare = share(c.decodeHighShare, standard.decodeHighShare);
        c.decodeLowShare = std::isfinite(c.decodeLowShare) && c.decodeLowShare >= 0 && c.decodeLowShare <= c.decodeHighShare ? c.decodeLowShare : c.decodeHighShare;
        c.decodeSevereShare = std::isfinite(c.decodeSevereShare) && c.decodeSevereShare >= c.decodeHighShare ? c.decodeSevereShare : c.decodeHighShare;
        c.missedHigh = share(c.missedHigh, standard.missedHigh);
        c.missedSevere = std::isfinite(c.missedSevere) && c.missedSevere >= c.missedHigh ? c.missedSevere : c.missedHigh;
        c.skippedHigh = share(c.skippedHigh, standard.skippedHigh);
        return c;
    }

    // Trouble signals in order of what they protect: decoding first, then presentation, then the GPU itself.
    Reading read(const PushGpuBudgetSample& in) const {
        Reading r;
        const bool interval = known(in.sourceIntervalMs) && in.sourceIntervalMs > 0;
        const unsigned m = ladder_[index_].multiplier;
        if (interval && known(in.gpuFrameMs)) r.load = (in.gpuFrameMs + (m >= 2 && known(in.fgMs) ? in.fgMs : 0.0)) / in.sourceIntervalMs;
        const double decodeShare = interval && known(in.decodeQueueDelayMs) ? in.decodeQueueDelayMs / in.sourceIntervalMs : -1;
        const unsigned total = in.presented + in.missedDeadlines;
        const double missed = total > 0 ? static_cast<double>(in.missedDeadlines) / static_cast<double>(total) : -1;
        // A generated frame that costs more than an output interval cannot keep the output cadence even when the
        // whole group fits the source interval.
        const double perGenerated = m >= 2 && known(in.fgMs) ? in.fgMs / static_cast<double>(m - 1) : -1;
        const bool outputKnown = known(in.outputIntervalMs) && in.outputIntervalMs > 0;

        const auto trouble = [&r](Signal s, bool severe) {
            if (r.signal == Signal::None) r.signal = s;
            r.severe = r.severe || severe;
        };
        if (decodeShare >= config_.decodeHighShare) trouble(Signal::DecodeDelay, decodeShare >= config_.decodeSevereShare);
        if (missed >= config_.missedHigh) trouble(Signal::MissedDeadlines, missed >= config_.missedSevere);
        if (r.load >= config_.loadHigh) trouble(Signal::GpuLoad, r.load >= config_.loadSevere);
        if (outputKnown && perGenerated >= in.outputIntervalMs * config_.loadHigh) trouble(Signal::GeneratedFrameCost, false);
        if (m >= 2 && known(in.skippedPairShare) && in.skippedPairShare >= config_.skippedHigh) trouble(Signal::SkippedPairs, false);
        if (in.thermal == PushThermalState::Throttling) trouble(Signal::Thermal, false);
        if (r.signal != Signal::None) return r;

        // Healthy needs every core measurement known and comfortably inside its budget.
        r.onBattery = in.power == PushPowerSource::Battery;
        r.healthy = !r.onBattery && r.load >= 0 && r.load <= config_.loadLow && decodeShare >= 0 && decodeShare <= config_.decodeLowShare &&
                    missed == 0.0 && (m < 2 || known(in.fgMs));
        return r;
    }

    static std::string_view name(Signal s, Kind kind) {
        static constexpr std::array<std::array<std::string_view, 3>, 7> names{{
            {{"hold", "hold", "hold"}},
            {{"trouble-gpu-load", "step-down-gpu-load", "severe-step-down-gpu-load"}},
            {{"trouble-generated-frame-cost", "step-down-generated-frame-cost", "severe-step-down-generated-frame-cost"}},
            {{"trouble-decode-delay", "step-down-decode-delay", "severe-step-down-decode-delay"}},
            {{"trouble-missed-deadlines", "step-down-missed-deadlines", "severe-step-down-missed-deadlines"}},
            {{"trouble-skipped-pairs", "step-down-skipped-pairs", "severe-step-down-skipped-pairs"}},
            {{"trouble-thermal", "step-down-thermal", "severe-step-down-thermal"}},
        }};
        return names[static_cast<std::size_t>(s)][static_cast<std::size_t>(kind)];
    }
    void switchTo(std::size_t next, std::int64_t now) {
        index_ = next;
        entryLoad_[next] = -1;
        lastSwitch_ = now;
        troubleSince_ = severeSince_ = kNever;
        healthySince_ = kNever;
    }
    PushGpuBudgetDecision decide(bool changed, std::string_view reason) const { return PushGpuBudgetDecision{ladder_[index_], index_, changed, reason}; }

    PushGpuBudgetConfig config_;
    PushGpuBudgetLadder ladder_{};
    std::array<double, 16> loadAt_{};      // the last load measured at each level (-1 = never)
    std::array<double, 16> entryLoad_{};   // the first load measured since entering each level (-1 = none yet)
    std::size_t index_ = 0;
    bool started_ = false;
    bool haveStepUp_ = false;
    unsigned failedReturns_ = 0;
    std::int64_t lastNow_ = 0;
    std::int64_t lastSwitch_ = 0;
    std::int64_t lastStepUp_ = 0;
    std::int64_t troubleSince_ = kNever;
    std::int64_t severeSince_ = kNever;
    std::int64_t healthySince_ = kNever;
};
} // namespace veyra::engine
