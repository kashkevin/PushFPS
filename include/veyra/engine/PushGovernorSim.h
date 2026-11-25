// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushGovernorControl.h"
#include "veyra/engine/PushGovernorStall.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace veyra::engine {
// A LINUX MODEL, not a measurement. Closed-loop trace simulator for PushGovernorControl: the governor's
// decisions change the simulated pipeline, whose next second's numbers the governor then reads. Used by the
// tests (tests/remoteplay/GovernorControlTests.inc: every scenario and randomized traces must keep every
// invariant) and by tools/remoteplay_latency_bench/governor_sim.cpp (the table in docs). Never compiled into
// the app. Every cost below is invented to exercise the rules; none was measured on any GPU.
//
// Model (one tick = one second, PushFlow, the user asks for 3X Quality on a 30 fps source):
//   work per source picture = base (3 ms x gpuScale) + (M-1) x per-generated cost (tier 2.0 / 3.2 / 5.0 ms x
//   gpuScale); load = work / source interval. Over 0.98 a GPU backlog grows (half the overrun per tick, at most
//   two source intervals) and drains when under. Latency P95 (decoded picture to present call) = the planned
//   hold (M-1)/M x interval + base + half a generated frame's cost + backlog + 0.4 x network jitter + decode
//   stall + up to 1 ms of noise. Missed presentation deadlines from load 0.95; decode-queue delay = 0.8 ms +
//   decode stall + loss recovery + half the backlog when the GPU is over 1. Network jitter and loss raise
//   latency but no frame generation step can fix them: the rollback rule must notice that.

struct PushSimWorld {
    double sourceFps = 30;
    double gpuScale = 1;
    double jitterMs = 0.3;
    double lossPercent = 0;
    double decodeStallMs = 0;
    double latencyStallMs = 0;   // delay seen in the latency only (not in the decode queue, not in the arrival times)
    double arrivalStallMs = 0;   // a link stall seen in the frames' arrival times (the governor sees it only through
                                 // the windowed interarrival peak, PushSimPeakWindow, as the app does)
    bool peakKnown = true;       // the stream reports its interarrival peak
    bool linkKnown = false;      // a fresh SmartLink decision (phase 2); false = none, as without SmartLink
    bool linkLimited = false, linkHeadroom = false;
    bool latencyKnown = true;
    bool decodeKnown = true;
    bool rateKnown = true;
};

// Deterministic generator (same numbers on every compiler; std distributions are not).
class PushSimRandom {
public:
    explicit PushSimRandom(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    unsigned below(unsigned n) { return n ? static_cast<unsigned>(next() % n) : 0u; }

private:
    std::uint64_t state_;
};

struct PushSimMeasure {
    double latencyP95Ms = 0;
    double outputFps = 0;
    double load = 0;
    PushGpuBudgetSample budget{};
};

// remoteplay::FrameJitter's peak, modelled per tick: 60 stream frames a second, the max |D| over two buckets of 256
// frames (the older one replaced in turn). The second's largest excess is the stall, else three times the mean jitter.
class PushSimPeakWindow {
public:
    double update(const PushSimWorld& w) {
        const double largest = std::max(w.arrivalStallMs, 3.0 * w.jitterMs);
        for (int f = 0; f < 60; ++f) {
            const double d = f == 0 ? largest : w.jitterMs;
            if (d > peak_[bucket_]) peak_[bucket_] = d;
            if (++count_ >= 256) { bucket_ ^= 1u; peak_[bucket_] = 0; count_ = 0; }
        }
        return std::max(peak_[0], peak_[1]);
    }

private:
    double peak_[2] = {0, 0};
    unsigned bucket_ = 0, count_ = 0;
};

class PushSimPipeline {
public:
    static constexpr std::array<double, 3> kTierCostMs{2.0, 3.2, 5.0};
    static constexpr double kBaseMs = 3.0;
    // steady: no backlog history (the "ideal" oracle's view).
    PushSimMeasure measure(PushGpuLevel level, const PushSimWorld& w, double noise, bool steady = false) {
        PushSimMeasure out;
        const double interval = 1000.0 / w.sourceFps;
        const unsigned m = level.multiplier;
        const double base = kBaseMs * w.gpuScale;
        const double perGenerated = m >= 2 ? kTierCostMs[level.tier < 3 ? level.tier : 2] * w.gpuScale : 0.0;
        const double fg = m >= 2 ? static_cast<double>(m - 1) * perGenerated : 0.0;
        const double work = base + fg;
        out.load = work / interval;
        if (steady) backlogMs_ = out.load > 0.98 ? 2 * interval : 0;
        else backlogMs_ = std::clamp(backlogMs_ + (work - 0.98 * interval) * 0.5, 0.0, 2 * interval);
        out.latencyP95Ms = pushPlannedDelayMs(m, w.sourceFps) + base + 0.5 * perGenerated + backlogMs_ + 0.4 * w.jitterMs + w.decodeStallMs + w.latencyStallMs + noise;
        const double missed = out.load > 0.95 ? std::min(0.4, (out.load - 0.95) * 1.5) : 0.0;
        const double outputs = w.sourceFps * static_cast<double>(m >= 2 ? m : 1u);
        out.outputFps = outputs * (1 - missed);
        auto& b = out.budget;
        b.sourceIntervalMs = interval;
        b.outputIntervalMs = 1000.0 / outputs;
        b.gpuFrameMs = base;
        b.fgMs = fg;
        b.decodeQueueDelayMs = w.decodeKnown ? 0.8 + w.decodeStallMs + 0.5 * w.lossPercent + (out.load > 1 ? 0.5 * backlogMs_ : 0.0) : -1.0;
        b.presented = static_cast<unsigned>(std::lround(outputs * (1 - missed)));
        b.missedDeadlines = static_cast<unsigned>(std::lround(outputs * missed));
        b.skippedPairShare = m >= 2 ? (out.load > 1 ? std::min(1.0, out.load - 1) : 0.0) : -1.0;
        b.power = PushPowerSource::Mains;
        if (!w.rateKnown) b.sourceIntervalMs = -1;
        return out;
    }
    void reset() { backlogMs_ = 0; }

private:
    double backlogMs_ = 0;
};

enum class PushSimScenario : std::uint8_t {
    CleanLan, JitterSpikes, BurstLoss, CongestionRecovery, GpuOverloadRamp, ThermalDrift, DecodeStall, CadenceSwitch,
    FlappingThreshold, UnknownInputs, EverythingAtOnce
};
inline constexpr std::size_t kPushSimScenarioCount = 11;
inline const char* pushSimScenarioName(PushSimScenario s) {
    constexpr std::array<const char*, kPushSimScenarioCount> names{"clean-lan", "jitter-spikes", "burst-loss", "congestion-then-recovery",
        "gpu-overload-ramp-and-release", "thermal-like-slow-drift", "decode-stall", "cadence-switch-60-30", "flapping-at-threshold",
        "unknown-or-missing-inputs", "everything-at-once"};
    return names[static_cast<std::size_t>(s)];
}

// The world at second t of a scenario (300 s long).
inline PushSimWorld pushSimWorld(PushSimScenario s, int t) {
    PushSimWorld w;
    const auto between = [t](int a, int b) { return t >= a && t < b; };
    const auto jitterSpikes = [&] { if (t % 40 >= 20 && t % 40 < 26) w.jitterMs = 18; };
    const auto burstLoss = [&] { if (t % 50 == 25 || t % 50 == 26) { w.lossPercent = 8; w.decodeStallMs = 12; } };
    const auto congestion = [&] { if (between(60, 180)) { w.jitterMs = 10; w.lossPercent = 1.5; } };
    const auto overload = [&] {
        if (between(60, 120)) w.gpuScale = 1 + 1.6 * (t - 60) / 60.0;
        else if (between(120, 180)) w.gpuScale = 2.6;
        else if (between(180, 210)) w.gpuScale = 2.6 - 1.6 * (t - 180) / 30.0;
    };
    switch (s) {
    case PushSimScenario::CleanLan: break;
    case PushSimScenario::JitterSpikes: jitterSpikes(); break;
    case PushSimScenario::BurstLoss: burstLoss(); break;
    case PushSimScenario::CongestionRecovery: congestion(); break;
    case PushSimScenario::GpuOverloadRamp: overload(); break;
    case PushSimScenario::ThermalDrift: w.gpuScale = 1 + 1.2 * t / 300.0; break;
    case PushSimScenario::DecodeStall: if (between(100, 130)) w.decodeStallMs = 25; break;
    case PushSimScenario::CadenceSwitch: w.sourceFps = (t / 60) % 2 == 0 ? 59.94 : 29.97; break;
    case PushSimScenario::FlappingThreshold:
        // The unplanned delay sits right at the budget at 3X Quality: alternately just over and just under.
        w.gpuScale = t % 2 == 0 ? 1.47 : 1.40;
        break;
    case PushSimScenario::UnknownInputs:
        w.latencyKnown = !between(30, 120) && !(t % 7 == 3);
        w.decodeKnown = !between(150, 200);
        w.rateKnown = !between(220, 240);
        if (between(60, 140)) w.gpuScale = 2.6;   // overload while latency is unknown
        break;
    case PushSimScenario::EverythingAtOnce:
        jitterSpikes(); burstLoss(); overload();
        if (between(200, 230)) w.decodeStallMs = 25;
        w.sourceFps = (t / 75) % 2 == 0 ? 29.97 : 59.94;
        w.gpuScale *= 1 + 0.3 * t / 300.0;
        w.latencyKnown = !(t % 11 == 5);
        break;
    }
    return w;
}

struct PushSimRecord {
    std::int64_t nowMs = 0;
    PushGpuLevel before{}, after{};
    std::size_t beforeIndex = 0;
    PushFgRequest userRequest{};          // what the user asked for (the harness's own record)
    bool userFrozenMultiplier = false, userFrozenTier = false;
    bool rateKnown = true, latencyKnown = true, budgetAnyKnown = true;
    bool changed = false;
    bool newStream = false;               // a new stream started this second (the harness's own record)
    PushControlTick tick{};
    double latencyMs = 0, outputFps = 0;
    double sourceFps = -1;                // the rate the governor was given (-1 unknown)
    double jitterMs = -1;                 // the network jitter it was given (-1 unknown)
    bool linkHeadroom = false;            // SmartLink's headroom it was given (known and set)
    // Review 5 (the checker's own evidence, never the controller's internals):
    double stallPeakMs = -1;              // the interarrival peak window the app reports (-1 unknown)
    double load = -1;                     // GPU work in force per source interval before the tick (-1 unknown)
    double latencyRequestMs = -1;         // the latency P95 the user's request would have had (no governor)
    bool restoreDue = false;              // a new stream: the settings in force must be the previous request ...
    PushGpuLevel priorRequest{};          // ... this one
};

// The invariants of PushGovernorControl, checked from a trace alone (independent of the class's internals).
// Returns "" when every one holds, else the first violation.
inline std::string pushGovernorInvariantViolation(const std::vector<PushSimRecord>& trace, const PushControlConfig& rawConfig) {
    const PushControlConfig c = pushSanitizedControlConfig(rawConfig);
    std::vector<std::int64_t> changes;
    std::vector<std::int64_t> reversals;
    int lastDirection = 0;
    std::int64_t lastChange = INT64_MIN;
    PushGpuLevel lastDownFrom{}, lastDownTo{}, lastProbeFrom{}, lastProbeTo{};
    bool haveDown = false, haveProbe = false;
    // The checker's own copy of the stand-down rule (never the class's constant): at most this many steps per stream
    // rolled back as useless; after that the only change allowed is one restore of the request.
    constexpr unsigned kUncreditedPerStream = 2;
    unsigned uncredited = 0;
    std::int64_t lastRebuild = INT64_MIN;
    const auto at = [](const PushSimRecord& r, const char* what) { return std::string(what) + " at " + std::to_string(r.nowMs) + " ms"; };
    // The unplanned delay recomputed from the record (never the controller's own figure).
    const auto unplanned = [](const PushSimRecord& r) {
        return r.latencyKnown && r.sourceFps > 0 ? std::max(0.0, r.latencyMs - pushPlannedDelayMs(r.before.multiplier, r.sourceFps)) : -1.0;
    };
    // What the step down to each level was for (a return after a protection step may use the budget itself).
    std::vector<std::pair<PushGpuLevel, PushControlCause>> downCause;
    const auto causeAt = [&downCause](PushGpuLevel l) {
        for (auto it = downCause.rbegin(); it != downCause.rend(); ++it)
            if (it->first.multiplier == l.multiplier && it->first.tier == l.tier) return it->second;
        return PushControlCause::None;
    };
    // Review 5. (a) Link stalls, recomputed from the reported peak window: a rise of the window's maximum is a frame
    // that came that far off its cadence since the last tick. No step down for decode, presentation or latency may
    // follow one by less than stallLingerMs (those are the link's).
    std::int64_t lastStallSeen = INT64_MIN;
    double lastPeak = -1;
    // (b) Episodes of over-budget latency that ended on their own (no change, no protection trouble, clear on both
    // sides, at least sustainMs long), per level: a step from that level within quietMemoryMs, with the GPU under
    // the budget's high load, may not be credited.
    const double loadHigh = PushGpuBudgetConfig{}.loadHigh;
    std::vector<std::pair<PushGpuLevel, std::int64_t>> selfEnded;
    std::int64_t episodeStart = INT64_MIN;
    bool episodeClean = false, prevClear = false;
    bool lastDownQuietBaseline = false;
    // (c) Held below the request for an hour or more with no latency gain over the request (reduced for nothing).
    std::size_t reducedFrom = SIZE_MAX;
    const auto reducedForNothing = [&](std::size_t from, std::size_t to) {
        if (from == SIZE_MAX || to <= from || trace[to - 1].nowMs - trace[from].nowMs < 3600000) return false;
        double gov = 0, req = 0;
        unsigned n = 0;
        for (std::size_t j = from; j < to; ++j) {
            const auto& h = trace[j];
            if (!h.latencyKnown || h.sourceFps <= 0 || h.latencyRequestMs < 0) continue;
            gov += std::max(0.0, h.latencyMs - pushPlannedDelayMs(h.before.multiplier, h.sourceFps));
            req += std::max(0.0, h.latencyRequestMs - pushPlannedDelayMs(h.userRequest.multiplier, h.sourceFps));
            ++n;
        }
        return n > 0 && gov / n > req / n - 0.5;
    };
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const auto& r = trace[i];
        const PushGpuLevel request{r.userRequest.tier, r.userRequest.multiplier};
        if (!pushLevelWithin(r.after, request)) return at(r, "above the user's request");
        // A new stream starts at the previous request (the governor's level is put back first).
        if (r.restoreDue && i > 0 && !(r.before.multiplier == r.priorRequest.multiplier && (r.before.multiplier < 2 || r.before.tier == r.priorRequest.tier)))
            return at(r, "a new stream started without the request put back");
        {
            const bool reducedNow = !(r.before.multiplier == r.userRequest.multiplier && (r.before.multiplier < 2 || r.before.tier == r.userRequest.tier));
            const bool cut = r.tick.userOverride || r.newStream;
            if (reducedNow && !cut) { if (reducedFrom == SIZE_MAX) reducedFrom = i; }
            else {
                if (reducedForNothing(reducedFrom, i)) return at(trace[i - 1], "held below the request for an hour with no latency gain");
                reducedFrom = reducedNow ? i : SIZE_MAX;
            }
            if (i + 1 == trace.size() && reducedForNothing(reducedFrom, i + 1)) return at(r, "held below the request for an hour with no latency gain");
        }
        if (r.stallPeakMs >= 0) {
            if ((lastPeak < 0 || r.stallPeakMs > lastPeak + 0.01) && r.stallPeakMs >= c.networkBurstPeakMs) lastStallSeen = r.nowMs;
            lastPeak = r.stallPeakMs;
        } else lastPeak = -1;
        {
            if (r.tick.userOverride || r.newStream) { selfEnded.clear(); episodeStart = INT64_MIN; }
            const double u = r.latencyKnown && r.sourceFps > 0 ? std::max(0.0, r.latencyMs - pushPlannedDelayMs(r.before.multiplier, r.sourceFps)) : -1.0;
            const bool clear = u >= 0 && u < c.latencyBudgetMs - c.hysteresisMs && !r.tick.protectionTrouble;
            if (episodeStart == INT64_MIN) {
                if (u > c.latencyBudgetMs && prevClear && !r.tick.protectionTrouble) { episodeStart = r.nowMs; episodeClean = true; }
            } else if (clear) {
                if (episodeClean && r.nowMs - episodeStart >= std::max<std::int64_t>(c.sustainMs, 1000)) selfEnded.emplace_back(r.before, r.nowMs);
                episodeStart = INT64_MIN;
            } else if (r.tick.protectionTrouble || r.changed || !(u >= 0)) episodeClean = false;
            if (episodeStart != INT64_MIN && r.changed) episodeClean = false;
            prevClear = clear && !r.changed;
        }
        if (r.tick.credited && lastDownQuietBaseline) return at(r, "a step from a level whose trouble ended on its own was credited");
        if (r.tick.userOverride || i == 0) { lastDirection = 0; }
        if (r.newStream) uncredited = 0;
        // A verdict of "useless" counts whether or not the setter then took the rollback.
        const bool uselessVerdict = r.tick.apply && r.tick.cause == PushControlCause::Rollback &&
                                    (r.tick.reason == "rollback-target-not-improved" || r.tick.reason == "rollback-improvement-not-explained" ||
                                     r.tick.reason == "rollback-trouble-returned");
        if (uselessVerdict && uncredited >= kUncreditedPerStream) return at(r, "more steps rolled back as useless in a stream than allowed");
        if (!r.changed) { if (uselessVerdict) ++uncredited; continue; }
        // Every change that rebuilds the graph (generation on/off, or a tier change while generating) is spaced.
        const bool rebuild = (r.after.multiplier >= 2) != (r.before.multiplier >= 2) ||
                             (r.after.multiplier >= 2 && r.before.multiplier >= 2 && r.after.tier != r.before.tier);
        // A latency step down may pass the spacing (review 5: latency beats the rebuild count); it still counts as one.
        const bool latencyDown = r.tick.cause == PushControlCause::Latency && pushLevelWithin(r.after, r.before);
        if (rebuild) {
            if (!latencyDown && lastRebuild != INT64_MIN && r.nowMs - lastRebuild < c.tierIntervalMs) return at(r, "graph rebuilds closer than the tier interval");
            lastRebuild = r.nowMs;
        }
        if (uncredited >= kUncreditedPerStream && !uselessVerdict) {
            const bool restore = r.tick.cause == PushControlCause::StandDown && r.after.multiplier == r.userRequest.multiplier &&
                                 (r.after.multiplier < 2 || r.after.tier == r.userRequest.tier);
            if (!restore) return at(r, "a change after standing down other than restoring the request");
        }
        if (uselessVerdict) ++uncredited;

        const bool down = pushLevelWithin(r.after, r.before) && !(r.after.multiplier == r.before.multiplier && r.after.tier == r.before.tier);
        const bool rollback = r.tick.cause == PushControlCause::Rollback;
        if (lastChange != INT64_MIN && r.nowMs - lastChange < c.minIntervalMs) return at(r, "changes closer than the minimum interval");
        changes.push_back(r.nowMs);
        unsigned inMinute = 0;
        for (std::int64_t t : changes) if (r.nowMs - t < 60000) ++inMinute;
        if (inMinute > c.maxChangesPerMinute) return at(r, "more changes in a minute than allowed");
        if (!r.rateKnown) return at(r, "a change without a known source rate");
        if (!r.latencyKnown && !r.budgetAnyKnown) return at(r, "a change with every input unknown");
        if (r.userFrozenMultiplier && r.after.multiplier != r.before.multiplier) return at(r, "changed a multiplier the user set");
        if (r.userFrozenTier && r.after.multiplier >= 2 && r.before.multiplier >= 2 && r.after.tier != r.before.tier) return at(r, "changed a tier the user set");
        const auto same = [](PushGpuLevel x, PushGpuLevel y) { return x.multiplier == y.multiplier && x.tier == y.tier; };
        if (r.tick.cause == PushControlCause::StandDown) {
            // Standing down puts the user's request back (exactly), and only after the steps that did not earn it.
            if (uncredited < kUncreditedPerStream) return at(r, "a stand-down restore before the steps that call for it");
            if (!(r.after.multiplier == request.multiplier && (r.after.multiplier < 2 || r.after.tier == request.tier))) return at(r, "a stand-down that does not restore the request");
            lastDirection = 0;
            lastChange = r.nowMs;
            continue;
        }
        if (rollback && down) {
            // Only the reverse of the last probe.
            if (!haveProbe || !same(r.before, lastProbeTo) || !same(r.after, lastProbeFrom)) return at(r, "a rollback down that is not the reverse of the last probe");
            haveProbe = false;
        } else if (down) {
            const double u = unplanned(r);
            const bool overBudget = u >= 0 && u > c.latencyBudgetMs;
            if (!overBudget && r.tick.protectIndex <= r.beforeIndex) return at(r, "a step down without a measured reason");
            if (r.tick.cause == PushControlCause::Latency) {
                if (!overBudget) return at(r, "a latency step down under the budget");
                if (r.jitterMs < 0 || u - r.jitterMs <= c.latencyBudgetMs) return at(r, "a latency step down the network's jitter explains");
                if (r.jitterMs > c.networkBurstJitterMs) return at(r, "a latency step down during a network burst");
                if (r.after.multiplier >= r.before.multiplier) return at(r, "a latency step down that keeps the multiplier");
            }
            const bool linkCause = r.tick.cause == PushControlCause::Decode || r.tick.cause == PushControlCause::Presentation || r.tick.cause == PushControlCause::Latency;
            if (linkCause && lastStallSeen != INT64_MIN && r.nowMs - lastStallSeen < std::max<std::int64_t>(c.stallLingerMs, 1))
                return at(r, "a step down during a link stall");
            lastDownQuietBaseline = false;
            if (r.tick.cause != PushControlCause::GpuLoad && r.load >= 0 && r.load < loadHigh)
                for (const auto& e : selfEnded)
                    if (e.first.multiplier == r.before.multiplier && e.first.tier == r.before.tier && r.nowMs - e.second < c.quietMemoryMs) lastDownQuietBaseline = true;
            haveDown = true; lastDownFrom = r.before; lastDownTo = r.after;
            downCause.emplace_back(r.after, r.tick.cause);
        } else if (rollback) {
            if (!haveDown || !(r.before.multiplier == lastDownTo.multiplier && r.before.tier == lastDownTo.tier) ||
                !(r.after.multiplier == lastDownFrom.multiplier && r.after.tier == lastDownFrom.tier)) return at(r, "a rollback that is not the reverse of the last step down");
            // More work again while the GPU is in trouble it explains is not allowed; trouble that is the link's or
            // a decoder's (longer than all the GPU work) does not hold a useless step in place.
            if (r.tick.gpuTrouble) return at(r, "a rollback (more work) during GPU trouble");
            haveDown = false;
        } else {
            // A step up raises latency to show more frames: only when everything says there is room.
            const PushControlCause from = causeAt(r.before);
            const bool afterProtection = from == PushControlCause::Decode || from == PushControlCause::Presentation || from == PushControlCause::GpuLoad;
            const double line = afterProtection ? c.latencyBudgetMs : c.latencyBudgetMs - c.hysteresisMs;
            const double u = unplanned(r);
            if (u < 0) return at(r, "a step up with the latency unknown");
            if (u >= line) return at(r, "a step up near or over the latency budget");
            if (r.tick.protectionTrouble) return at(r, "a step up during protection trouble");
            if (pushLevelWithin(r.before, r.after) == false) return at(r, "a step up that is not toward the request");
            if (r.tick.cause == PushControlCause::Probe) {
                // A probe: healthy now, at most one per probe interval since the last change, judged afterwards.
                if (lastChange != INT64_MIN && r.nowMs - lastChange < c.probeIntervalMs) return at(r, "a probe sooner than the probe interval");
                haveProbe = true; lastProbeFrom = r.before; lastProbeTo = r.after;
                const int direction = 1;
                if (lastDirection != 0 && direction != lastDirection) reversals.push_back(r.nowMs);
                lastDirection = direction; lastChange = r.nowMs;
                continue;
            }
            // The hold: healthyHoldMs, or half of it (at least minIntervalMs) when SmartLink's headroom was given on
            // every tick of that shorter hold (phase 2).
            const std::int64_t shortHold = std::max(c.healthyHoldMs / 2, c.minIntervalMs);
            bool headroomHeld = r.linkHeadroom;
            for (std::size_t j = i; headroomHeld && j-- > 0 && trace[j].nowMs > r.nowMs - shortHold;) headroomHeld = trace[j].linkHeadroom;
            if (headroomHeld && (i < 1 || trace[0].nowMs > r.nowMs - shortHold)) headroomHeld = false;   // the trace must cover it
            const std::int64_t hold = headroomHeld ? shortHold : c.healthyHoldMs;
            if (lastChange != INT64_MIN && r.nowMs - lastChange < hold) return at(r, "a step up before the healthy hold");
            // ... and every tick of the healthy hold before it was healthy and measured.
            for (std::size_t j = i; j-- > 0 && trace[j].nowMs > r.nowMs - hold;) {
                const auto& h = trace[j];
                const double hu = unplanned(h);
                if (!h.rateKnown || hu < 0 || hu >= line || h.tick.protectionTrouble)
                    return at(r, "a step up without a whole healthy hold before it");
            }
            if (pushLevelWithin(r.before, r.after) == false) return at(r, "a step up that is not toward the request");
        }
        const int direction = down ? -1 : 1;
        // A protection step down (decode, presentation, GPU load) may pass a lock-out by design: it is not
        // counted as the governor's own reversal.
        const bool protection = down && (r.tick.cause == PushControlCause::Decode || r.tick.cause == PushControlCause::Presentation ||
                                         r.tick.cause == PushControlCause::GpuLoad);
        if (lastDirection != 0 && direction != lastDirection && !protection) {
            reversals.push_back(r.nowMs);
            // More than oscillationReversals reversals within the window need a lock-out between them.
            const std::size_t k = c.oscillationReversals;
            if (reversals.size() > k) {
                const std::int64_t first = reversals[reversals.size() - 1 - k];
                if (r.nowMs - first < c.oscillationWindowMs) {
                    std::int64_t gap = 0, previous = first;
                    for (std::int64_t t : changes) if (t > first) { gap = std::max(gap, t - previous); previous = t; }
                    if (gap < c.lockoutMs) return at(r, "oscillation without a lock-out");
                }
            }
        }
        lastDirection = direction;
        lastChange = r.nowMs;
    }
    return {};
}

struct PushSimSummary {
    std::string scenario;
    unsigned changes = 0, rollbacks = 0, lockouts = 0, oscillations = 0, downs = 0, ups = 0;
    unsigned rebuilds = 0;   // changes of the tier or generation on/off: each rebuilds the graph (a hitch)
    std::array<unsigned, 16> secondsAtIndex{};
    double latencyGovernor = 0, latencyNone = 0, latencyIdeal = 0;   // mean modelled latency P95, ms
    double p95Governor = 0, p95None = 0, p95Ideal = 0;               // 95th percentile over the run of those
    double outputGovernor = 0, outputNone = 0, outputIdeal = 0;      // mean output fps
    unsigned overBudgetGovernor = 0, overBudgetNone = 0, overBudgetIdeal = 0;   // seconds with unplanned > budget
    std::string firstViolation;
    std::vector<PushSimRecord> trace;
};

// User actions a randomized trace may add (the scenarios have none).
struct PushSimUserAction {
    int second = -1;
    enum class Kind : std::uint8_t { SetRequest, NewStream, RefuseNext } kind = Kind::SetRequest;
    PushFgRequest request{};
};

inline double pushSimPercentile95(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(std::floor(0.95 * static_cast<double>(v.size() - 1)))];
}

// Runs one closed loop. worldAt(t) gives the world; actions are applied at their second.
template <typename WorldAt>
PushSimSummary pushRunGovernorSim(const char* name, int seconds, WorldAt worldAt, std::uint64_t seed, const PushControlConfig& config,
                                  PushFgRequest request = PushFgRequest{PushFgMethod::PushFlow, 3, 2}, const std::vector<PushSimUserAction>& actions = {}) {
    PushSimSummary out;
    out.scenario = name;
    PushGovernorControl control(config);
    const PushControlConfig used = control.config();
    PushSimPipeline governed, none, ideal;
    PushSimRandom random(seed);
    PushFgRequest user = pushNormalizedFgRequest(request), current = user;
    bool frozenM = false, frozenT = false, refuseNext = false;
    std::vector<double> latG, latN, latI;
    int lastDirection = 0;
    PushSimPeakWindow window;
    PushStallFromPeak stalls;
    PushGpuLevel priorRequest{};
    int lastChangeSecond = -1;   // a world may answer to the governor's last change (review 5, trace c)
    for (int t = 0; t < seconds; ++t) {
        PushSimWorld w;
        if constexpr (std::is_invocable_v<WorldAt, int, int>) w = worldAt(t, lastChangeSecond);
        else w = worldAt(t);
        bool newStreamNow = false, userSetNow = false;
        bool restoreExact = false;
        for (const auto& a : actions) {
            if (a.second != t) continue;
            if (a.kind == PushSimUserAction::Kind::SetRequest) {
                const PushFgRequest r = pushNormalizedFgRequest(a.request);
                // Choosing what is already in force changes nothing anyone can observe: not a new request.
                if (r == current) continue;
                userSetNow = true;
                // Before the stream's first tick the settings in force are simply the request.
                const bool changedM = r.multiplier != current.multiplier, changedT = r.tier != current.tier;
                if (control.started() && changedM) frozenM = true;
                if (control.started() && changedT) frozenT = true;
                // Only the setting the user changed becomes their request (the other keeps its own value).
                if (control.started()) user = PushFgRequest{r.method, changedM ? r.multiplier : user.multiplier, changedT ? r.tier : user.tier};
                else user = r;
                current = r;
            } else if (a.kind == PushSimUserAction::Kind::NewStream) {
                // The checker's restore rule applies when the harness's record of the request is exact (no setting frozen).
                restoreExact = !frozenM && !frozenT;
                if (const auto back = control.pendingRestore(current)) { current = *back; control.restored(); }
                control.newStream();
                newStreamNow = true;
                priorRequest = PushGpuLevel{user.tier, user.multiplier};
                user = current;
                frozenM = frozenT = false;
                governed.reset();
            } else refuseNext = true;
        }
        const double noise = random.uniform();
        const PushGpuLevel before{current.tier, current.multiplier};
        const PushSimMeasure mg = governed.measure(before, w, noise);
        const PushSimMeasure mn = none.measure(PushGpuLevel{user.tier, user.multiplier}, w, noise);
        // Ideal: the most work whose steady unplanned delay fits the budget with the GPU under 0.95.
        const PushGpuBudgetLadder lad = pushControlLadder(user, false, false);
        PushSimMeasure mi{};
        unsigned idealMultiplier = 1;
        for (std::size_t i = 0; i < lad.count; ++i) {
            mi = ideal.measure(lad[i], w, noise, true);
            idealMultiplier = lad[i].multiplier;
            const double unplanned = mi.latencyP95Ms - pushPlannedDelayMs(lad[i].multiplier, w.sourceFps);
            if (unplanned <= used.latencyBudgetMs && mi.load < 0.95) break;
        }
        PushControlSample sample;
        sample.nowMs = static_cast<std::int64_t>(t) * 1000;
        sample.sourceFps = w.rateKnown ? w.sourceFps : -1.0;
        sample.latencyP95Ms = w.latencyKnown ? mg.latencyP95Ms : -1.0;
        sample.budget = mg.budget;
        sample.networkJitterMs = w.jitterMs;
        sample.lossPercent = w.lossPercent;
        sample.linkKnown = w.linkKnown;
        sample.linkLimited = w.linkLimited;
        sample.linkHeadroom = w.linkHeadroom;
        double peakNow = -1;
        {
            const double peak = window.update(w);
            peakNow = w.peakKnown ? peak : -1.0;
            sample.networkStallMs = w.peakKnown ? stalls.update(peak) : (stalls.reset(), -1.0);
        }
        PushSimRecord rec;
        rec.stallPeakMs = peakNow;
        rec.load = w.rateKnown ? mg.load : -1.0;
        rec.latencyRequestMs = mn.latencyP95Ms;
        rec.restoreDue = newStreamNow && !userSetNow && restoreExact;
        rec.priorRequest = priorRequest;
        rec.nowMs = sample.nowMs;
        rec.newStream = newStreamNow;
        rec.linkHeadroom = w.linkKnown && w.linkHeadroom;
        rec.before = before;
        rec.rateKnown = w.rateKnown;
        rec.latencyKnown = w.latencyKnown;
        rec.budgetAnyKnown = w.rateKnown;   // the budget's load and misses need the source interval
        const PushControlTick tick = control.update(current, sample);
        rec.beforeIndex = tick.startIndex;
        rec.tick = tick;
        if (tick.apply) {
            if (refuseNext) { refuseNext = false; control.applyRefused(); }
            else current = tick.target;
        }
        rec.after = PushGpuLevel{current.tier, current.multiplier};
        rec.changed = tick.apply && !(rec.after.multiplier == before.multiplier && rec.after.tier == before.tier);
        rec.userRequest = user;
        rec.userFrozenMultiplier = frozenM;
        rec.userFrozenTier = frozenT;
        rec.latencyMs = mg.latencyP95Ms;
        rec.sourceFps = w.rateKnown ? w.sourceFps : -1.0;
        rec.jitterMs = w.jitterMs;
        rec.outputFps = mg.outputFps;
        out.trace.push_back(rec);
        if (rec.changed) {
            lastChangeSecond = t;
            ++out.changes;
            const bool onOff = (rec.after.multiplier >= 2) != (rec.before.multiplier >= 2);
            if (onOff || (rec.after.multiplier >= 2 && rec.before.multiplier >= 2 && rec.after.tier != rec.before.tier)) ++out.rebuilds;
            const bool down = pushLevelWithin(rec.after, rec.before);
            const int direction = down ? -1 : 1;
            if (down) ++out.downs; else ++out.ups;
            if (lastDirection != 0 && direction != lastDirection) ++out.oscillations;
            lastDirection = direction;
        }
        if (tick.index < out.secondsAtIndex.size()) ++out.secondsAtIndex[tick.index];
        latG.push_back(mg.latencyP95Ms); latN.push_back(mn.latencyP95Ms); latI.push_back(mi.latencyP95Ms);
        out.outputGovernor += mg.outputFps; out.outputNone += mn.outputFps; out.outputIdeal += mi.outputFps;
        const auto over = [&](const PushSimMeasure& x, unsigned m) { return x.latencyP95Ms - pushPlannedDelayMs(m, w.sourceFps) > used.latencyBudgetMs; };
        out.overBudgetGovernor += over(mg, before.multiplier) ? 1u : 0u;
        out.overBudgetNone += over(mn, user.multiplier) ? 1u : 0u;
        out.overBudgetIdeal += over(mi, idealMultiplier) ? 1u : 0u;
    }
    out.rollbacks = control.rollbacks();
    out.lockouts = control.lockouts();
    const auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / static_cast<double>(v.size()); };
    out.latencyGovernor = mean(latG); out.latencyNone = mean(latN); out.latencyIdeal = mean(latI);
    out.p95Governor = pushSimPercentile95(latG); out.p95None = pushSimPercentile95(latN); out.p95Ideal = pushSimPercentile95(latI);
    const double n = seconds > 0 ? static_cast<double>(seconds) : 1.0;
    out.outputGovernor /= n; out.outputNone /= n; out.outputIdeal /= n;
    out.firstViolation = pushGovernorInvariantViolation(out.trace, config);
    return out;
}

inline PushSimSummary pushRunGovernorScenario(PushSimScenario s, const PushControlConfig& config = {}) {
    return pushRunGovernorSim(pushSimScenarioName(s), 300, [s](int t) { return pushSimWorld(s, t); }, 0x5150u + static_cast<std::uint64_t>(s), config);
}

// Periodic short network bursts (review finding): 18 ms of jitter for 6 s, every 30-150 s (fixed seed), on a
// clean LAN otherwise. The governor may step down in a burst; the bound is how long it then stays reduced.
// kind 0: jitter only; 1: jitter with a decode stall during each burst; 2: jitter, 8 % loss and a decode stall.
inline PushSimSummary pushRunBurstSim(int seconds, const PushControlConfig& config = {}, int kind = 0) {
    std::vector<char> burst(static_cast<std::size_t>(seconds > 0 ? seconds : 0), 0);
    PushSimRandom r(4242);
    for (int t = 30; t < seconds;) {
        for (int k = 0; k < 6 && t + k < seconds; ++k) burst[static_cast<std::size_t>(t + k)] = 1;
        t += 30 + static_cast<int>(r.below(121));
    }
    return pushRunGovernorSim(kind == 0 ? "periodic-short-bursts" : kind == 1 ? "bursts-with-decode-stall" : "bursts-with-loss-and-stall", seconds, [&burst, kind](int t) {
        PushSimWorld w;
        if (burst[static_cast<std::size_t>(t)]) {
            w.jitterMs = 18;
            if (kind >= 1) w.decodeStallMs = 25;
            if (kind >= 2) w.lossPercent = 8;
        }
        return w;
    }, 99u, config);
}

// The 24 h traces of review 4 (20 November 2026; the reviewer's harness rebuilt here so they stay as tests and in
// the tool). Seed 7 for the noise, as the reviewer ran them. Kinds:
//   0 pure bursts (18 ms jitter, 3-10 s every 20-140 s)   1 bursts + 25 ms decode stall   2 bursts + 8 % loss
//   3 stall-only: the same bursts as a 25 ms decode stall with no jitter and nothing in the interarrival peak
//   4 slow GPU (scale 2.2)   5 GPU load sine (1.6 +- 0.9, 5 min period)   6 overload 2.6 for 10 min every 2 h
//   7 user changes mid-reduction (4X/t2 at the top of each hour, 3X/t1 at half past; overload 2.4 at 5-25 min)
//   8 Wi-Fi stalls seen as jitter (20-80 ms every 2-8 s as the mean jitter, 2.5 ms between)
//   9 Wi-Fi stalls (20-80 ms every 2-8 s) as decode stalls with 2.5 ms mean jitter; the peak sees 7.5 ms only
//  10 the same with the stall visible in the interarrival peak (what the app's snapshot carries)
//  11 the same as 10 on a slow GPU (scale 2.2): the GPU evidence must still be acted on
inline constexpr int kPushSimReviewTraceCount = 12;
inline const char* pushSimReviewTraceName(int kind) {
    constexpr std::array<const char*, kPushSimReviewTraceCount> names{"pure-bursts", "bursts+decode-stall", "bursts+loss", "stall-only",
        "slow-gpu-2.2", "gpu-sine-5min", "overload-then-clean-x12", "user-changes-mid-reduction", "wifi-stalls-as-jitter",
        "wifi-stalls-decode-peak-unseen", "wifi-stalls-peak-seen", "wifi-stalls-peak-seen+slow-gpu"};
    return kind >= 0 && kind < kPushSimReviewTraceCount ? names[static_cast<std::size_t>(kind)] : "unknown";
}
inline PushSimSummary pushRunReviewTrace(int kind, int seconds = 86400, const PushControlConfig& config = {}) {
    const std::size_t n = static_cast<std::size_t>(seconds > 0 ? seconds : 0);
    std::vector<char> burst(n, 0);
    PushSimRandom r(4711);
    for (int t = 30; t < seconds;) {
        const int length = 3 + static_cast<int>(r.below(8));
        for (int k = 0; k < length && t + k < seconds; ++k) burst[static_cast<std::size_t>(t + k)] = 1;
        t += 20 + static_cast<int>(r.below(120));
    }
    std::vector<double> stall(n, 0);
    PushSimRandom q(99);
    for (int t = 0; t < seconds;) {
        stall[static_cast<std::size_t>(t)] = 20 + q.below(61);
        t += 2 + static_cast<int>(q.below(7));
    }
    std::vector<PushSimUserAction> actions;
    if (kind == 7)
        for (int t = 600; t < seconds; t += 3600) {
            actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::SetRequest, PushFgRequest{PushFgMethod::PushFlow, 4, 2}});
            actions.push_back(PushSimUserAction{t + 1800, PushSimUserAction::Kind::SetRequest, PushFgRequest{PushFgMethod::PushFlow, 3, 1}});
        }
    return pushRunGovernorSim(pushSimReviewTraceName(kind), seconds, [&burst, &stall, kind](int t) {
        PushSimWorld w;
        const std::size_t i = static_cast<std::size_t>(t);
        const bool b = burst[i] != 0;
        switch (kind) {
        case 0: if (b) w.jitterMs = 18; break;
        case 1: if (b) { w.jitterMs = 18; w.decodeStallMs = 25; } break;
        case 2: if (b) { w.jitterMs = 18; w.lossPercent = 8; } break;
        case 3: if (b) w.decodeStallMs = 25; break;
        case 4: w.gpuScale = 2.2; break;
        case 5: w.gpuScale = 1.6 + 0.9 * std::sin(2 * 3.14159265358979323846 * t / 300.0); break;
        case 6: { const int p = t % 7200; if (p >= 300 && p < 900) w.gpuScale = 2.6; } break;
        case 7: { const int p = t % 3600; if (p >= 300 && p < 1500) w.gpuScale = 2.4; } break;
        case 8: w.jitterMs = stall[i] > 0 ? stall[i] : 2.5; break;
        case 9: w.jitterMs = 2.5; w.decodeStallMs = stall[i]; break;
        case 10: w.jitterMs = 2.5; w.decodeStallMs = stall[i]; w.arrivalStallMs = stall[i]; break;
        case 11: w.jitterMs = 2.5; w.decodeStallMs = stall[i]; w.arrivalStallMs = stall[i]; w.gpuScale = 2.2; break;
        default: break;
        }
        return w;
    }, 7u, config, PushFgRequest{PushFgMethod::PushFlow, 3, 2}, actions);
}
// The traces of review 5 (20 November 2026; the reviewer's adversarial traces rebuilt here, stall schedule and seed as
// in pushRunReviewTrace). Kinds:
//   0-2 Wi-Fi stalls seen in the interarrival peak + the GPU over budget half the time (30 min on, 30 off) at scale
//       2.0 / 2.2 / 2.6        3-5 the same with the peak not seeing the stalls (decode stalls only)
//   6   a steady GPU at scale 2.0, nothing else: a return to generation goes over the budget (the loop of blocker 2)
//   7-9 a decoder stall (17 ms, scale 1.5) that stops when the governor changes a level and comes back 9 / 10 / 11 min
//       later (the credit must be withdrawn)
//   10  runs of link stalls (rising 9 to 17 ms, every second for 1 min, every 30 min) seen in the peak: short enough
//       for the GPU work to "explain" them, so only the stall detector keeps the governor still
//   11  the same as 6 with a new stream every 6 h (the request must be put back at each)
//   12  overload (scale 2.6) for 10 min every 2 h with a new stream 7 min into each (reduced then: put back)
//   13  latency-only episodes (11 ms for 3 s and 5 s in turn, every 150-400 s, nothing in the peak or the decode
//       queue) that end on their own: the quiet baseline returning is not a step's doing
inline constexpr int kPushSimReview5TraceCount = 14;
inline const char* pushSimReview5TraceName(int kind) {
    constexpr std::array<const char*, kPushSimReview5TraceCount> names{"stalls-seen+gpu-2.0-half", "stalls-seen+gpu-2.2-half", "stalls-seen+gpu-2.6-half",
        "stalls-unseen+gpu-2.0-half", "stalls-unseen+gpu-2.2-half", "stalls-unseen+gpu-2.6-half", "steady-gpu-2.0", "stall-stops-at-step-back-9min",
        "stall-stops-at-step-back-10min", "stall-stops-at-step-back-11min", "stall-runs-seen-short", "steady-gpu-2.0+new-streams",
        "overload-x12+new-streams", "latency-episodes-end-on-their-own"};
    return kind >= 0 && kind < kPushSimReview5TraceCount ? names[static_cast<std::size_t>(kind)] : "unknown";
}
inline PushSimSummary pushRunReview5Trace(int kind, int seconds = 86400, const PushControlConfig& config = {}) {
    const std::size_t n = static_cast<std::size_t>(seconds > 0 ? seconds : 0);
    std::vector<double> stall(n, 0);
    PushSimRandom q(99);
    for (int t = 0; t < seconds;) {
        stall[static_cast<std::size_t>(t)] = 20 + q.below(61);
        t += 2 + static_cast<int>(q.below(7));
    }
    std::vector<char> episode(n, 0);
    PushSimRandom e(1313);
    for (int t = 100, j = 0; t < seconds; ++j) {
        const int length = j % 2 ? 5 : 3;   // a 3 s episode ends before a step can come: it ends on its own
        for (int k = 0; k < length && t + k < seconds; ++k) episode[static_cast<std::size_t>(t + k)] = 1;
        t += 150 + static_cast<int>(e.below(251));
    }
    std::vector<PushSimUserAction> actions;
    if (kind == 11)
        for (int t = 6 * 3600; t < seconds; t += 6 * 3600) actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::NewStream, {}});
    if (kind == 12)
        for (int t = 300 + 420; t < seconds; t += 7200) actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::NewStream, {}});
    return pushRunGovernorSim(pushSimReview5TraceName(kind), seconds, [&stall, &episode, kind](int t, int lastChange) {
        PushSimWorld w;
        const std::size_t i = static_cast<std::size_t>(t);
        if (kind <= 5) {
            const double scales[3] = {2.0, 2.2, 2.6};
            w.jitterMs = 2.5;
            w.decodeStallMs = stall[i];
            if (kind <= 2) w.arrivalStallMs = stall[i];
            if ((t / 1800) % 2) w.gpuScale = scales[kind % 3];
        } else if (kind == 6 || kind == 11) {
            w.gpuScale = 2.0;
        } else if (kind <= 9) {
            const int back = (kind - 7 + 9) * 60;
            w.gpuScale = 1.5;
            w.decodeStallMs = lastChange >= 0 && t - lastChange < back ? 0.0 : 17.0;
        } else if (kind == 12) {
            const int p = t % 7200;
            if (p >= 300 && p < 900) w.gpuScale = 2.6;
        } else if (kind == 13) {
            if (episode[i]) w.latencyStallMs = 11;
        } else if (kind == 10) {
            if (t % 1800 >= 600 && t % 1800 < 660) {
                w.decodeStallMs = 9 + 0.13 * (t % 1800 - 600);   // rising: each second's stall is a new peak
                w.arrivalStallMs = w.decodeStallMs;
            }
        }
        return w;
    }, 7u, config, PushFgRequest{PushFgMethod::PushFlow, 3, 2}, actions);
}

// Facts of a trace the summary does not hold: time reduced (below the user's request), the longest reduced run,
// the most graph rebuilds in any 60 s, and the reduced share of the last hour.
struct PushSimTraceFacts {
    double reducedPercent = 0, lastHourReducedPercent = 0;
    unsigned longestReducedS = 0, maxRebuildsPerMinute = 0;
};
inline PushSimTraceFacts pushSimTraceFacts(const PushSimSummary& s) {
    PushSimTraceFacts f;
    unsigned reduced = 0, run = 0, lastHour = 0;
    std::vector<std::int64_t> rebuilds;
    const std::size_t hourFrom = s.trace.size() > 3600 ? s.trace.size() - 3600 : 0;
    for (std::size_t i = 0; i < s.trace.size(); ++i) {
        const auto& r = s.trace[i];
        const bool below = !(r.after.multiplier == r.userRequest.multiplier && (r.after.multiplier < 2 || r.after.tier == r.userRequest.tier));
        if (below) { ++reduced; ++run; f.longestReducedS = std::max(f.longestReducedS, run); if (i >= hourFrom) ++lastHour; }
        else run = 0;
        if (r.changed && ((r.after.multiplier >= 2) != (r.before.multiplier >= 2) || (r.after.multiplier >= 2 && r.before.multiplier >= 2 && r.after.tier != r.before.tier)))
            rebuilds.push_back(r.nowMs);
    }
    for (std::size_t i = 0; i < rebuilds.size(); ++i) {
        unsigned k = 0;
        for (std::size_t j = i; j < rebuilds.size() && rebuilds[j] - rebuilds[i] < 60000; ++j) ++k;
        f.maxRebuildsPerMinute = std::max(f.maxRebuildsPerMinute, k);
    }
    const double n = s.trace.empty() ? 1.0 : static_cast<double>(s.trace.size());
    f.reducedPercent = 100.0 * reduced / n;
    f.lastHourReducedPercent = 100.0 * lastHour / static_cast<double>(s.trace.size() - hourFrom > 0 ? s.trace.size() - hourFrom : 1);
    return f;
}

// A random world and random user actions (fixed seeds in the tests).
struct PushSimRandomTrace {
    std::vector<PushSimWorld> worlds;
    std::vector<PushSimUserAction> actions;
    PushFgRequest request{};
};
inline PushSimRandomTrace pushSimRandomTrace(std::uint64_t seed, int seconds) {
    PushSimRandom r(seed);
    PushSimRandomTrace out;
    out.request = PushFgRequest{PushFgMethod::PushFlow, 2 + r.below(3), r.below(3)};
    PushSimWorld w;
    for (int t = 0; t < seconds; ++t) {
        if (r.below(20) == 0) {   // a new segment
            w = PushSimWorld{};
            w.sourceFps = r.below(3) == 0 ? 59.94 : 29.97;
            w.gpuScale = 0.8 + 2.4 * r.uniform();
            w.jitterMs = r.below(4) == 0 ? 20 * r.uniform() : 0.5 * r.uniform();
            w.lossPercent = r.below(6) == 0 ? 10 * r.uniform() : 0;
            w.decodeStallMs = r.below(8) == 0 ? 30 * r.uniform() : 0;
            w.latencyKnown = r.below(6) != 0;
            w.decodeKnown = r.below(8) != 0;
            w.rateKnown = r.below(15) != 0;
        }
        PushSimWorld now = w;
        if (r.below(10) == 0) now.latencyKnown = !now.latencyKnown;   // flapping knowledge
        now.gpuScale *= 0.95 + 0.1 * r.uniform();
        out.worlds.push_back(now);
        const unsigned act = r.below(100);
        if (act < 2) out.actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::SetRequest, PushFgRequest{PushFgMethod::PushFlow, 1 + r.below(4), r.below(3)}});
        else if (act < 3) out.actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::NewStream, {}});
        else if (act < 6) out.actions.push_back(PushSimUserAction{t, PushSimUserAction::Kind::RefuseNext, {}});
    }
    return out;
}
} // namespace veyra::engine
