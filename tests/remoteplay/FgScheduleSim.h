// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "lagfix": a SCHEDULING SIMULATOR for live frame generation (30 FPS content at 3X, 4K-like GPU costs). It is
// evidence about the program's LOGIC, never about a GPU, a capture card, a PS5 or a display: every cost, jitter and
// delay below is made up (a model), and only the decisions are the program's own.
//
// What is the program's own code, called as the engine calls it (src/engine/EngineController.cpp):
//   LivePairLatency (the learned pair phase), LiveDeadlineMargin (0-1 ms on a learned phase), FgRecoveryBudget
//   (processingAllowance, phaseAllowance, admit, complete), FgCostSettling (VFG / PushFlow probe holds),
//   LiveFgRejectionHold, PushFgFallbackHold, LiveArrivalSmoother (a card's presentation anchor), liveGeneratedGrace100ns,
//   pushUllGeneratedGrace, PresentationScheduler (deadlines), PushSmoothnessMode + PushSmoothCostModel +
//   pushUllHeadroomTarget (part "headroom"), pushSmoothCapMultiplier (cap-fit), and since stream "lagfix" the rules
//   pushUllPartsInForce and pushStreamPairDelay (PushFgLatency.h) that decide what a switch may still do.
// What is MODELLED here (the engine's glue, written again; each line names the engine line it stands for):
//   - one GPU queue, serial: a pair's base work, then its generated frames one after another (fg1 = the first);
//     nothing else on the GPU; no graph-queue priority (GRAPH_QUEUE_PRIORITY cannot be simulated: it changes how the
//     GPU interleaves two queues, which only the driver decides);
//   - the engine thread picks a frame up 0.3 ms after its callback / decode and decides admission 0.3 ms later;
//     readiness is seen 0.2 ms after the GPU finishes (CPU observation);
//   - presentation: frames of a pair in order, each at max(deadline, seen ready, previous Present return); a Present
//     call takes 0.3 ms and never blocks (variable refresh or tearing: NO refresh grid, no capacity wait, no DXGI
//     queue); a generated frame not presented by deadline + grace is dropped (PreviewFrameReadiness: expiry first);
//   - VFG's slower first evaluations after a second without one: x1.7 for two groups (FgCostSettling's own note);
//   - a card's callbacks jitter (sd 0.3 ms) around the device clock; a PS5 stream's decode times jitter (sd 1.5 ms,
//     cut at 4 ms) around the console's 29.97 FPS clock;
//   - Smoothness Mode (part "headroom") on a 180 Hz panel with variable refresh confirmed; the backend offers 2X-4X.
// Not modelled: content duplicates, history resets after start, the 2X-only VFG fast model, provider (present-sink)
// frame generation, the recorder, the network, the display. Deterministic: costs come from a fixed seed through an
// integer generator (the same numbers under MSVC and g++), drawn per pair before any decision, so every configuration
// sees exactly the same costs.
#include "veyra/engine/FgRecoveryBudget.h"
#include "veyra/engine/LivePairLatency.h"
#include "veyra/engine/LivePresentationTiming.h"
#include "veyra/engine/PresentationScheduler.h"
#include "veyra/engine/PushFgLatency.h"
#include "veyra/engine/PushFgSchedule.h"
#include "veyra/engine/PushSmoothness.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fgsim {
using namespace veyra::engine;
constexpr std::int64_t kMs = 10000;   // 100 ns ticks per millisecond

// --- costs ---------------------------------------------------------------------------------------------------------
struct Costs {
    const char* name;
    double baseLo, baseHi;          // base work per pair, uniform (ms)
    double genMs, genSd;            // one generated frame (ms), normal
    double spikeShare, spikeMs;     // share of generated frames with a spike, and its size
    double baseSpikeShare, baseSpikeMs;
    double driftMs, driftPeriodS;   // generated-frame cost rises by up to driftMs and back over the period
    double spikeMaxMs = 0;          // > spikeMs: a spike's size is uniform in [spikeMs, spikeMaxMs]
    double hiccupEveryS = 0, hiccupMs = 0;   // a GPU hiccup: hiccupMs more base work every hiccupEveryS seconds
};
inline std::vector<Costs> costSets() {
    return {
        {"comfortable", 1.0, 3.0, 6.6, 0.4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"tight", 3.0, 5.0, 10.0, 1.2, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {"spikes", 1.0, 3.0, 6.6, 0.6, 0.02, 15.0, 0.01, 10.0, 0.0, 0.0},
        {"drift", 1.0, 5.0, 6.6, 0.6, 0.0, 0.0, 0.0, 0.0, 6.0, 60.0},
        {"near-limit", 3.0, 5.0, 11.5, 1.2, 0.02, 10.0, 0.0, 0.0, 0.0, 0.0},
        {"overload", 3.0, 5.0, 13.5, 1.5, 0.02, 10.0, 0.0, 0.0, 0.0, 0.0},
        // Capture-card sets bracketing a 4K VFG pair (the owner's scope, 23 Nov 2026 22:51 ET): typical, heavier scene,
        // slow thermal rise over two minutes, a long session with a GPU hiccup (+20 ms base) every 3 s.
        {"vfg-4k", 2.0, 4.0, 7.0, 0.5, 0.01, 5.0, 0.0, 0.0, 0.0, 0.0, 15.0},
        {"heavy", 3.0, 5.0, 9.5, 0.8, 0.01, 5.0, 0.0, 0.0, 0.0, 0.0, 15.0},
        {"thermal", 2.0, 4.0, 7.0, 0.5, 0.0, 0.0, 0.0, 0.0, 3.0, 240.0},
        {"hiccup", 2.0, 4.0, 7.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, 20.0},
    };
}
// Fixed-sequence random numbers: mt19937's output is defined by the standard, the conversions below are ours.
class Rng {
    std::mt19937 g_;
public:
    explicit Rng(std::uint32_t seed) : g_(seed) {}
    double uniform() { return (double(g_() >> 8) + 0.5) / 16777216.0; }
    double normal() {   // Box-Muller, one value per call
        const double u = uniform(), v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
    }
};
struct PairCosts { double base; double gen[4]; double jitterMs; };
inline std::vector<PairCosts> drawCosts(const Costs& c, std::uint32_t seed, std::size_t pairs, double jitterSd, double interval100ns) {
    Rng r(seed);
    std::vector<PairCosts> out(pairs);
    for (std::size_t k = 0; k < pairs; ++k) {
        PairCosts& p = out[k];
        p.base = c.baseLo + (c.baseHi - c.baseLo) * r.uniform();
        if (r.uniform() < c.baseSpikeShare) p.base += c.baseSpikeMs;
        const double t = double(k) * interval100ns / 1e7;
        const double drift = c.driftPeriodS > 0 ? c.driftMs * 0.5 * (1.0 - std::cos(6.283185307179586 * t / c.driftPeriodS)) : 0.0;
        for (double& g : p.gen) {
            g = std::max(0.5, c.genMs + drift + c.genSd * r.normal());
            if (r.uniform() < c.spikeShare) g += c.spikeMaxMs > c.spikeMs ? c.spikeMs + (c.spikeMaxMs - c.spikeMs) * r.uniform() : c.spikeMs;
        }
        p.jitterMs = std::clamp(jitterSd * r.normal(), -4.0, 4.0);
        if (c.hiccupEveryS > 0) {
            const auto every = static_cast<std::size_t>(std::llround(c.hiccupEveryS * 1e7 / interval100ns));
            if (every && k % every == every / 2) p.base += c.hiccupMs;
        }
    }
    return out;
}

// --- configurations --------------------------------------------------------------------------------------------------
enum class Source { Card, Stream };
// Candidate scheduling changes (stream "lagfix", Directive 25). 0 / false: today's behaviour.
struct Fixes {
    int admit = 0;                  // admission: 0 today; 1 robustAdmit (PushFgSchedule.h); designs compared and not taken:
                                    // 2 min(P95, max of the last 8 groups), 3 admit after 3 skipped pairs
    bool phaseBehindWarmup = false; // no phase sample from a pair queued behind a warm-up group (or behind one so excluded)
    bool streamSmooth = false;      // a PS5 stream's present anchor through LiveArrivalSmoother, as a card's
    bool holdCardsOnly = false;     // LiveFgRejectionHold only on capture cards
    bool gridEarly = false;         // a skipped, not held pair's real frame on the earliest grid slot its base P95 reaches
    double graceMs = -1;            // explored: a generated frame later than this past its deadline is dropped (-1: today's)
};
struct Config {
    std::string name;
    PushUllParts parts;          // VEYRA_EXP_RP_FG_ULL as written
    bool adaptivePhase = false;  // VEYRA_EXP_RP_ADAPTIVE_PHASE=1 (also what the interface's modes set on a stream)
    bool package13 = true;       // true: the parts as Package 13's program runs them; false: through this branch's rules
    Fixes fixes{};
};
inline PushUllParts parts(bool hold, bool phase, bool headroom, bool grace) {
    PushUllParts p;
    p.hold = hold; p.phase = phase; p.headroom = headroom; p.grace = grace;
    return p;
}

struct Result {
    double realMeanMs = 0, realP95Ms = 0;       // callback / decode -> Present return of real frames
    double realGenMeanMs = -1, realGenP95Ms = -1;
    PushGridWindow::Stats grid;                 // the engine's steadiness counter over the whole run   // the same, only real frames of pairs that showed a generated frame
    double genShownPerS = 0, genDroppedPerS = 0; // generated frames presented / made and dropped (late)
    double skippedPerS = 0;                      // pairs admission skipped (no generated frames made)
    double offShare = 0, longestMs = 0;          // present intervals more than 1 ms off content/M; the longest
    double holdsPerMin = 0, probeHoldsPerMin = 0;// FG-off holds (rejection or ULL) started; FgCostSettling holds started
    double phaseMeanMs = 0;                      // pairDelay (the scheduled phase) on admitted, not held pairs
    double twoXShare = 0;                        // share of pairs at 2X (Smoothness Mode under part "headroom")
    std::size_t pairs = 0;
};

// --- the simulation ----------------------------------------------------------------------------------------------------
inline Result simulate(const Costs& costs, Source source, const Config& cfg, std::uint32_t seed, double seconds) {
    const bool stream = source == Source::Stream;
    const std::int64_t T = 333667;   // 29.97 FPS (100 ns)
    const unsigned ceiling = 3;
    const std::size_t pairs = std::size_t(seconds * 1e7 / double(T));
    const auto pc = drawCosts(costs, seed, pairs + 1, stream ? 1.5 : 0.3, double(T));

    // What the program does with the switch (EngineController: ullParts, remoteAdaptivePhase, adaptiveCapturePhase).
    const PushUllParts ull = cfg.package13 ? cfg.parts : pushUllPartsInForce(cfg.parts);
    const bool remoteAdaptivePhase = stream && (cfg.adaptivePhase || ull.phase);
    const bool adaptiveCapturePhase = !stream || remoteAdaptivePhase;
    const bool marginPair = adaptiveCapturePhase;   // options.fg && !presentSink
    const bool smoothAnchor = !stream || cfg.fixes.streamSmooth;   // smoothTransportAnchor (no dedup)

    FgRecoveryBudget budget;
    FgCostSettling settling;
    LivePairLatency pairLatency;
    LiveDeadlineMargin margin;
    LiveFgRejectionHold rejectionHold;
    PushFgFallbackHold ullHold;
    LiveArrivalSmoother arrival;
    PushSmoothnessMode smooth;
    PushSmoothCostModel smoothCosts;
    const int headroomTarget = ull.headroom ? pushUllHeadroomTarget(180.0, 1) : 0;
    const PushFgMask offered = (1u << 2) | (1u << 3) | (1u << 4);

    struct Pending { std::int64_t done, submit; bool admitted; double totalMs, groupMs; std::int64_t anchor; std::vector<std::int64_t> ready; std::vector<std::int64_t> pts; std::int64_t b; };
    std::deque<Pending> pending;
    std::int64_t gpuFree = 0, lastPresentEnd = 0, lastPresentBegin = 0, lastEvaluated = 0;
    unsigned coldGroups = 0;
    std::uint64_t admittedPairs = 0, rejectedPairs = 0;
    std::vector<double> realDelay, realGenDelay, intervals;
    PushGridWindow gridWindow;
    std::vector<unsigned> intervalMultiplier;
    double genShown = 0, genDropped = 0, skipped = 0, holds = 0, probeHolds = 0, phaseSum = 0, phaseCount = 0, twoX = 0;
    bool wasHeld = false;
    unsigned multiplier = ceiling;
    const std::int64_t start = 100 * kMs;

    std::deque<std::pair<std::int64_t, double>> fgSamples;   // the budget's FG samples, for the candidate estimates
    PushFgCostWindow costWindow;
    PushPhaseQueueGuard queueGuard;
    unsigned costSkips = 0;
    const auto fgEstimate = [&](std::int64_t now, int policy) -> double {
        std::vector<double> v;
        for (const auto& [t, ms] : fgSamples) if (t >= now - 20000000) v.push_back(ms);
        if (v.empty()) return 0.0;
        std::vector<double> sorted = v;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](double q) { return sorted[std::min(sorted.size() - 1, std::size_t(std::ceil(q * double(sorted.size()))) - 1)]; };
        const double p95 = at(0.95);
        if (policy == 1) return std::min(p95, 1.2 * at(0.75));
        if (policy == 2) { double m = 0; for (std::size_t i = v.size() > 8 ? v.size() - 8 : 0; i < v.size(); ++i) m = std::max(m, v[i]); return std::min(p95, m); }
        return p95;
    };
    const auto completeUpTo = [&](std::int64_t now) {
        while (!pending.empty() && pending.front().done <= now) {
            const Pending p = pending.front();
            pending.pop_front();
            const std::int64_t holdBefore = settling.holdUntil();
            // costSettling: VFG / PushFlow only (the modelled backend is VFG).
            const bool costSettling = settling.observe(!p.admitted, p.admitted, p.done);
            if (settling.holdUntil() != holdBefore) ++probeHolds;
            budget.complete(p.totalMs, p.admitted, costSettling, p.done, p.admitted ? std::optional<double>(p.groupMs) : std::nullopt, true);
            if (p.admitted && !costSettling) smoothCosts.record(unsigned(p.ready.size() - 1), p.groupMs);
            if (p.admitted && !costSettling) {
                while (!fgSamples.empty() && (fgSamples.size() >= 64 || fgSamples.front().first < p.done - 20000000)) fgSamples.pop_front();
                fgSamples.emplace_back(p.done, p.groupMs);
            }
            if (p.admitted && !costSettling) costWindow.add(p.done, p.groupMs);   // PushFgCostWindow, fed as the engine feeds it
            const bool behind = cfg.fixes.phaseBehindWarmup && queueGuard.excluded(p.submit, p.done, costSettling, p.admitted);
            if (adaptiveCapturePhase && p.admitted && !costSettling && !behind) {
                std::int64_t required = 0;
                for (std::size_t i = 0; i < p.ready.size(); ++i) required = std::max(required, p.ready[i] - p.anchor + p.b - p.pts[i]);
                pairLatency.observe(p.done, required);
            }
        }
    };

    for (std::size_t k = 1; k <= pairs; ++k) {
        const PairCosts& c = pc[k];
        const std::int64_t b = std::int64_t(k) * T, a = b - T;
        const std::int64_t callback = start + b + std::int64_t(c.jitterMs * double(kMs));
        const std::int64_t pickup = callback + 3000, now = pickup + 3000;
        completeUpTo(now);

        // Smoothness Mode (part "headroom" only): PushSmoothHook::choose.
        if (headroomTarget) {
            PushSmoothInput in;
            in.targetFps = headroomTarget; in.contentFps = 1e7 / double(T); in.capFps = 180.0; in.refreshHz = 180.0;
            in.ceiling = ceiling; in.offered = offered; in.baseMs = budget.baseCost(now).value_or(-1.0); in.costs = &smoothCosts;
            in.blitMs = 0.3; in.admitted = admittedPairs; in.rejected = rejectedPairs; in.now100ns = now;
            const auto d = smooth.update(in);
            if (d.multiplier >= 2) multiplier = d.multiplier;
        }
        // Cap-fit (VEYRA_EXP_RP_SMOOTH_CAP_FIT) and the existing ceil rule agree here: 29.97 x 3 = 90 under a 180 cap.
        twoX += multiplier == 2;
        const unsigned M = multiplier;

        // Phase (EngineController: processingAllowance, legacyPairDelay, pairExtra, fgPhaseAllowance, pairDelay).
        const std::int64_t legacy = T + budget.processingAllowance(now, T);
        const std::int64_t extra = marginPair ? margin.extra() : 0;
        const std::int64_t allowance = marginPair ? budget.phaseAllowance(now, T) : 0;
        std::int64_t pairDelay = marginPair ? pairLatency.select(now, legacy, T, M, allowance) + extra : legacy;
        if (!cfg.package13) pairDelay = pushStreamPairDelay(pairDelay, legacy, remoteAdaptivePhase);

        // Admission (admitFg): the callback anchor, the last generated frame's deadline.
        PresentationScheduler admitTimeline;
        admitTimeline.resetPair(1, b, callback, pairDelay);
        const std::int64_t lastGenerated = a + (b - a) * std::int64_t(M - 1) / std::int64_t(M);
        const std::int64_t lastDeadline = admitTimeline.deadline(lastGenerated);
        bool admitted = !settling.holding(now) && budget.admit(now, lastDeadline, 0.3, 0.3, false);
        if (!admitted && !settling.holding(now) && cfg.fixes.admit) {
            const double base = budget.baseCost(now).value_or(0.0), progress = std::clamp(0.3, 0.0, base);
            if (cfg.fixes.admit == 1) admitted = pushRobustAdmitFits(now, lastDeadline, 0.3, 0.3, budget.baseCost(now), costWindow.robust(now));
            else if (cfg.fixes.admit == 3) admitted = costSkips >= 3;
            else admitted = double(lastDeadline - now) / 1e4 + 10.0 >= base + fgEstimate(now, cfg.fixes.admit) - progress + 0.3;
        }
        costSkips = admitted || settling.holding(now) ? 0 : costSkips + 1;
        (admitted ? admittedPairs : rejectedPairs) += 1;
        if (!admitted) ++skipped;

        // GPU (modelled).
        const std::int64_t gpuStart = std::max(pickup + 2000, gpuFree);
        const bool cold = admitted && lastEvaluated && now - lastEvaluated >= 10000000;
        if (cold) coldGroups = 2;
        const double factor = admitted && coldGroups ? 1.7 : 1.0;
        if (admitted && coldGroups) --coldGroups;
        if (admitted) lastEvaluated = now;
        Pending job;
        job.admitted = admitted; job.anchor = callback; job.b = b; job.submit = pickup;
        std::int64_t t = gpuStart + std::int64_t(c.base * double(kMs));
        const std::int64_t realReady = t + 2000;
        double group = 0;
        std::vector<std::int64_t> genReady;
        if (admitted)
            for (unsigned i = 1; i < M; ++i) {
                const double g = c.gen[i - 1] * factor;
                group += g;
                t += std::int64_t(g * double(kMs));
                genReady.push_back(t + 2000);
            }
        gpuFree = t;
        job.done = t + 2000;
        job.totalMs = c.base + group;
        job.groupMs = group;
        for (unsigned i = 1; i < M && admitted; ++i) { job.ready.push_back(genReady[i - 1]); job.pts.push_back(a + (b - a) * std::int64_t(i) / std::int64_t(M)); }
        job.ready.push_back(realReady); job.pts.push_back(b);
        pending.push_back(job);

        // Holds (EngineController: ullParts.hold -> PushFgFallbackHold, else marginPair -> LiveFgRejectionHold).
        bool held = false;
        if (ull.hold) held = ullHold.observe(now, admitted, settling.holding(now));
        else if (marginPair && !(cfg.fixes.holdCardsOnly && stream)) held = rejectionHold.observe(now, admitted);
        if (held && !wasHeld) ++holds;
        wasHeld = held;

        // Presentation anchor, grace, timeline.
        const std::int64_t smoothed = smoothAnchor ? arrival.anchor(b, callback, T) : callback;
        const std::int64_t presentAnchor = held ? callback : smoothed;
        std::int64_t grace = liveGeneratedGrace100ns(callback, presentAnchor);
        if (ull.grace) grace = pushUllGeneratedGrace(grace, kLiveGeneratedGrace100ns, T, M);
        if (cfg.fixes.graceMs >= 0) grace = std::min<std::int64_t>(grace, std::int64_t(cfg.fixes.graceMs * double(kMs)) + std::max<std::int64_t>(0, callback - presentAnchor));
        PresentationScheduler timeline;
        std::int64_t lookahead = held ? 0 : pairDelay;
        if (cfg.fixes.gridEarly && !held && !admitted)
            lookahead = pushGridEarlyDelay(pairDelay, T, M, budget.baseCost(now), callback - presentAnchor);
        timeline.resetPair(1, b, presentAnchor, lookahead);
        if (admitted && !held) { phaseSum += double(pairDelay) / double(kMs); ++phaseCount; }

        const auto present = [&](std::int64_t at, unsigned m) {
            gridWindow.observe(at, T / std::int64_t(m));
            if (lastPresentBegin) { intervals.push_back(double(at - lastPresentBegin) / double(kMs)); intervalMultiplier.push_back(m); }
            lastPresentBegin = at;
            lastPresentEnd = at + 3000;
        };
        unsigned shownInPair = 0;
        for (unsigned i = 1; i < M && admitted; ++i) {
            const std::int64_t pts = a + (b - a) * std::int64_t(i) / std::int64_t(M);
            const std::int64_t deadline = timeline.deadline(pts), ready = genReady[i - 1];
            const std::int64_t at = std::max({deadline, ready, lastPresentEnd});
            if (at > deadline + grace) { ++genDropped; lastPresentEnd = std::max(lastPresentEnd, std::min(at, deadline + grace)); continue; }
            ++genShown;
            ++shownInPair;
            if (marginPair && !held) margin.observe(at, deadline, ready, extra);
            present(at, M);
        }
        const std::int64_t at = std::max({timeline.deadline(b), realReady, lastPresentEnd});
        present(at, M);
        realDelay.push_back(double(at + 3000 - callback) / double(kMs));
        if (shownInPair) realGenDelay.push_back(realDelay.back());
    }

    Result r;
    r.pairs = pairs;
    const double duration = double(pairs) * double(T) / 1e7;
    std::vector<double> sorted = realDelay;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double d : realDelay) sum += d;
    r.realMeanMs = sum / double(realDelay.size());
    r.realP95Ms = sorted[std::min(sorted.size() - 1, std::size_t(std::ceil(0.95 * double(sorted.size()))) - 1)];
    if (!realGenDelay.empty()) {
        std::vector<double> g = realGenDelay;
        std::sort(g.begin(), g.end());
        double gs = 0;
        for (const double d : g) gs += d;
        r.realGenMeanMs = gs / double(g.size());
        r.realGenP95Ms = g[std::min(g.size() - 1, std::size_t(std::ceil(0.95 * double(g.size()))) - 1)];
    }
    r.grid = gridWindow.take();
    r.genShownPerS = genShown / duration;
    r.genDroppedPerS = genDropped / duration;
    r.skippedPerS = skipped / duration;
    std::size_t off = 0;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        const double ideal = double(T) / double(kMs) / double(intervalMultiplier[i]);
        if (std::abs(intervals[i] - ideal) > 1.0) ++off;
        r.longestMs = std::max(r.longestMs, intervals[i]);
    }
    r.offShare = intervals.empty() ? 0.0 : double(off) / double(intervals.size());
    r.holdsPerMin = holds / duration * 60.0;
    r.probeHoldsPerMin = probeHolds / duration * 60.0;
    r.phaseMeanMs = phaseCount ? phaseSum / phaseCount : -1.0;
    r.twoXShare = twoX / double(pairs);
    return r;
}

// --- Directive 25 B: other schedules, compared on equal terms (SIMULATED) -------------------------------------------------
// Not the engine's code: a generic pair schedule with the same costs, jitter and presentation model as simulate(), and a
// phase learned the way LivePairLatency learns it (the P95 of each pair's required phase over the last second plus
// 0.5 ms, never stepping down more than 0.5 ms a pair). No admission: every pair generates; a generated or predicted
// frame later than its deadline + 10 ms is dropped. Each schedule lists, per pair, its frames in display order with the
// GPU work each needs and its display slot relative to the real frame B (slots T/3 apart unless compressed).
enum class Schedule { Interpolate3X, RealThenPredict, PredictTwo, Compressed, Interpolate2X };
inline const char* scheduleName(Schedule s) {
    switch (s) {
    case Schedule::Interpolate3X: return "interpolate 3X (today)";
    case Schedule::RealThenPredict: return "interp + B + 1 predicted";
    case Schedule::PredictTwo: return "B + 2 predicted";
    case Schedule::Compressed: return "interpolate, 8 ms slots";
    case Schedule::Interpolate2X: return "interpolate 2X (60 FPS)";
    }
    return "?";
}
struct ScheduleResult {
    double realMeanMs = 0, realP95Ms = 0, shownPerS = 0, droppedPerS = 0, predictedShare = 0, offShare = 0, longestMs = 0;
};
// warpMs: the first predicted frame's GPU time (optical flow A->B and a forward warp), warp2Ms: the second's (warp only).
inline ScheduleResult simulateSchedule(const Costs& costs, Source source, Schedule kind, std::uint32_t seed, double seconds, double warpMs = 3.0, double warp2Ms = 1.0) {
    const bool stream = source == Source::Stream;
    const std::int64_t T = 333667;
    const std::size_t pairs = std::size_t(seconds * 1e7 / double(T));
    const auto pc = drawCosts(costs, seed, pairs + 1, stream ? 1.5 : 0.3, double(T));
    struct Frame { int kind; double ms; double slot; };   // kind 0 interpolated, 1 real, 2 predicted; slot in ms from B's slot
    const double s = double(T) / 30000.0;                  // T/3 in ms
    LiveArrivalSmoother arrival;
    std::deque<std::pair<std::int64_t, std::int64_t>> required;   // (time, required phase)
    std::int64_t phase = 0, gpuFree = 0, lastBegin = 0, lastEnd = 0;
    std::vector<double> real, gaps;
    std::vector<double> ideal;
    double shown = 0, dropped = 0, predicted = 0;
    std::deque<std::pair<std::int64_t, std::int64_t>> pending;   // (done, required) to learn when observed
    for (std::size_t k = 1; k <= pairs; ++k) {
        const PairCosts& c = pc[k];
        std::vector<Frame> f;
        switch (kind) {
        case Schedule::Interpolate3X: f = {{0, c.gen[0], -2 * s}, {0, c.gen[1], -s}, {1, 0, 0}}; break;
        case Schedule::RealThenPredict: f = {{0, c.gen[0], -s}, {1, 0, 0}, {2, warpMs, s}}; break;
        case Schedule::PredictTwo: f = {{1, 0, 0}, {2, warpMs, s}, {2, warp2Ms, 2 * s}}; break;
        case Schedule::Compressed: f = {{0, c.gen[0], -16.0}, {0, c.gen[1], -8.0}, {1, 0, 0}}; break;
        case Schedule::Interpolate2X: f = {{0, c.gen[0], -1.5 * s}, {1, 0, 0}}; break;
        }
        const std::int64_t b = std::int64_t(k) * T;
        const std::int64_t callback = 100 * kMs + b + std::int64_t(c.jitterMs * double(kMs));
        const std::int64_t pickup = callback + 3000;
        while (!pending.empty() && pending.front().first <= pickup) {
            while (!required.empty() && required.front().first < pending.front().first - 10000000) required.pop_front();
            required.push_back(pending.front());
            pending.pop_front();
        }
        if (required.size() >= 8) {
            std::vector<std::int64_t> v;
            for (const auto& r : required) v.push_back(r.second);
            std::sort(v.begin(), v.end());
            const std::int64_t target = v[(v.size() * 95 + 99) / 100 - 1] + 5000;
            phase = phase ? std::max(target, phase - 5000) : target;
        } else if (!phase) phase = std::int64_t((c.base + 30.0) * double(kMs));
        const std::int64_t anchor = stream ? callback : arrival.anchor(b, callback, T);
        // GPU: the real frame's base work first, then the generated / predicted frames in display order.
        std::int64_t t = std::max(pickup + 2000, gpuFree) + std::int64_t(c.base * double(kMs));
        const std::int64_t realReady = t + 2000;
        std::vector<std::int64_t> ready(f.size(), realReady);
        for (std::size_t i = 0; i < f.size(); ++i)
            if (f[i].kind != 1) { t += std::int64_t(f[i].ms * double(kMs)); ready[i] = t + 2000; }
        gpuFree = t;
        std::int64_t need = 0;
        for (std::size_t i = 0; i < f.size(); ++i) need = std::max(need, ready[i] - callback - std::int64_t(f[i].slot * double(kMs)));
        pending.emplace_back(t + 2000, need);
        for (std::size_t i = 0; i < f.size(); ++i) {
            const std::int64_t deadline = anchor + phase + std::int64_t(f[i].slot * double(kMs));
            const std::int64_t at = std::max({deadline, ready[i], lastEnd});
            if (f[i].kind != 1 && at > deadline + kLiveGeneratedGrace100ns) { ++dropped; continue; }
            if (lastBegin) { gaps.push_back(double(at - lastBegin) / double(kMs)); }
            lastBegin = at;
            lastEnd = at + 3000;
            if (f[i].kind == 1) real.push_back(double(at + 3000 - callback) / double(kMs));
            else { ++shown; predicted += f[i].kind == 2; }
        }
    }
    ScheduleResult r;
    const double duration = double(pairs) * double(T) / 1e7;
    std::vector<double> sorted = real;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double d : real) sum += d;
    r.realMeanMs = sum / double(real.size());
    r.realP95Ms = sorted[std::size_t(std::ceil(0.95 * double(sorted.size()))) - 1];
    r.shownPerS = shown / duration;
    r.droppedPerS = dropped / duration;
    r.predictedShare = (shown + double(real.size())) > 0 ? predicted / (shown + double(real.size())) : 0.0;
    const double slotMs = kind == Schedule::Interpolate2X ? 1.5 * s : s;
    std::size_t off = 0;
    for (const double g : gaps) { off += std::abs(g - slotMs) > 1.0; r.longestMs = std::max(r.longestMs, g); }
    r.offShare = gaps.empty() ? 0.0 : double(off) / double(gaps.size());
    return r;
}
inline std::string scheduleRow(Schedule kind, const ScheduleResult& r) {
    char line[256];
    std::snprintf(line, sizeof line, "%-28s %7.2f %7.2f %9.2f %6.2f %8.1f%% %6.1f%% %7.1f", scheduleName(kind), r.realMeanMs, r.realP95Ms, r.shownPerS,
                  r.droppedPerS, r.predictedShare * 100.0, r.offShare * 100.0, r.longestMs);
    return line;
}
inline const char* scheduleHeader() {
    return "schedule                      realMs realP95 genShown/s drop/s predicted   off%  longest";
}

// The configurations of the report: the default, each part of VEYRA_EXP_RP_FG_ULL alone, the likely combinations.
inline std::vector<Config> reportConfigs(bool package13) {
    const auto c = [package13](const char* name, PushUllParts p, bool adaptive = false) { return Config{name, p, adaptive, package13}; };
    return {c("default (12A = 13)", parts(false, false, false, false)),
            c("hold", parts(true, false, false, false)),
            c("phase", parts(false, true, false, false)),
            c("headroom", parts(false, false, true, false)),
            c("grace", parts(false, false, false, true)),
            c("hold,phase,grace", parts(true, true, false, true)),
            c("1 (all)", parts(true, true, true, true)),
            c("ADAPTIVE_PHASE=1", parts(false, false, false, false), true)};
}
inline std::string row(const std::string& label, const Result& r) {
    char line[320];
    std::snprintf(line, sizeof line, "%-30s %7.2f %7.2f %7.2f %6.2f %6.2f %6.1f%% %6.1f %6.2f %6.2f %6.2f %5.1f%%", label.c_str(), r.realMeanMs, r.realP95Ms,
                  r.genShownPerS, r.genDroppedPerS, r.skippedPerS, r.offShare * 100.0, r.longestMs, r.holdsPerMin, r.probeHoldsPerMin, r.phaseMeanMs,
                  r.twoXShare * 100.0);
    return line;
}
inline const char* header() {
    return "configuration                  realMs  realP95 genShown/s drop/s skip/s   off%  longest holds/m probe/m phaseMs  at2X";
}
}  // namespace fgsim
