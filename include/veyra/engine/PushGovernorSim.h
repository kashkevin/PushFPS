// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushGovernorControl.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
        out.latencyP95Ms = pushPlannedDelayMs(m, w.sourceFps) + base + 0.5 * perGenerated + backlogMs_ + 0.4 * w.jitterMs + w.decodeStallMs + noise;
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
    PushControlTick tick{};
    double latencyMs = 0, outputFps = 0;
    double sourceFps = -1;                // the rate the governor was given (-1 unknown)
    double jitterMs = -1;                 // the network jitter it was given (-1 unknown)
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
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const auto& r = trace[i];
        const PushGpuLevel request{r.userRequest.tier, r.userRequest.multiplier};
        if (!pushLevelWithin(r.after, request)) return at(r, "above the user's request");
        if (r.tick.userOverride || i == 0) { lastDirection = 0; }
        if (!r.changed) continue;
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
                if (r.after.multiplier >= r.before.multiplier) return at(r, "a latency step down that keeps the multiplier");
            }
            haveDown = true; lastDownFrom = r.before; lastDownTo = r.after;
            downCause.emplace_back(r.after, r.tick.cause);
        } else if (rollback) {
            if (!haveDown || !(r.before.multiplier == lastDownTo.multiplier && r.before.tier == lastDownTo.tier) ||
                !(r.after.multiplier == lastDownFrom.multiplier && r.after.tier == lastDownFrom.tier)) return at(r, "a rollback that is not the reverse of the last step down");
            if (r.tick.protectionTrouble) return at(r, "a rollback (more work) during protection trouble");
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
            if (lastChange != INT64_MIN && r.nowMs - lastChange < c.healthyHoldMs) return at(r, "a step up before the healthy hold");
            // ... and every tick of the healthy hold before it was healthy and measured.
            for (std::size_t j = i; j-- > 0 && trace[j].nowMs > r.nowMs - c.healthyHoldMs;) {
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
    for (int t = 0; t < seconds; ++t) {
        const PushSimWorld w = worldAt(t);
        for (const auto& a : actions) {
            if (a.second != t) continue;
            if (a.kind == PushSimUserAction::Kind::SetRequest) {
                const PushFgRequest r = pushNormalizedFgRequest(a.request);
                // Choosing what is already in force changes nothing anyone can observe: not a new request.
                if (r == current) continue;
                // Before the stream's first tick the settings in force are simply the request.
                const bool changedM = r.multiplier != current.multiplier, changedT = r.tier != current.tier;
                if (control.started() && changedM) frozenM = true;
                if (control.started() && changedT) frozenT = true;
                // Only the setting the user changed becomes their request (the other keeps its own value).
                if (control.started()) user = PushFgRequest{r.method, changedM ? r.multiplier : user.multiplier, changedT ? r.tier : user.tier};
                else user = r;
                current = r;
            } else if (a.kind == PushSimUserAction::Kind::NewStream) {
                if (const auto back = control.pendingRestore(current)) { current = *back; control.restored(); }
                control.newStream();
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
        PushSimRecord rec;
        rec.nowMs = sample.nowMs;
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
inline PushSimSummary pushRunBurstSim(int seconds, const PushControlConfig& config = {}) {
    std::vector<char> burst(static_cast<std::size_t>(seconds > 0 ? seconds : 0), 0);
    PushSimRandom r(4242);
    for (int t = 30; t < seconds;) {
        for (int k = 0; k < 6 && t + k < seconds; ++k) burst[static_cast<std::size_t>(t + k)] = 1;
        t += 30 + static_cast<int>(r.below(121));
    }
    return pushRunGovernorSim("periodic-short-bursts", seconds, [&burst](int t) {
        PushSimWorld w;
        if (burst[static_cast<std::size_t>(t)]) w.jitterMs = 18;
        return w;
    }, 99u, config);
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
