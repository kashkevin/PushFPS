// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg", step 2: PushWarp's I(2/3) as a SECOND SOURCE for the first frame of a "Lower delay" predicted pair
// (I(2/3), B, P: PushPredict.h). The schedule, its admission, exits and evidence lines stay PushLowDelay.h's; this header
// only decides WHO makes I for a pair and keeps that source's own cost:
//   (a) HELD (review 1, MUST 3, decided): when NVIDIA VFG refuses its whole FrameIndex-2-only ladder the mode is
//       unavailable with that reason, exactly as before; PushWarp never takes over by itself. The row may only OFFER the
//       PushFPS source in its text;
//   (b) the configured generator is not VFG: PushFlow (stateless per pair, so a pair it does not run costs it nothing) is
//       served while the row is on, its only source. Not served, with the reason: the DLSS path / ANR (their providers
//       keep a history a skipped evaluation would break: not tested), FidelityFX frame generation and AMD FRC (2X only;
//       the schedule is defined for 3X). Experimental: not tested end to end (no PushFlow runtime in the e2e workflow);
//   (c) the user chose it: the row's choice "Frame source: NVIDIA / PushFPS" (preference pushLowDelaySource, default
//       "auto" = NVIDIA where VFG is the generator). The source is the USER'S choice and nothing else.
// With VFG PushWarp is created and self-checked only when the PushFPS source (or the causal schedule) is chosen
// (EnhancementSettings::lowDelayOwnWarp, a graph rebuild: review 1, SHOULD 9).
// Nothing here runs or is created unless the Lower delay row (or the kit's switch) created the predicted-frame pass; with
// the NVIDIA source and VFG accepting the request every pair is exactly the other engineer's (EnhanceGraph hooks
// marked "stream ownfg"). Pure; unit tests: tests/remoteplay/OwnFrameTests.inc.
//
// Picture (OFFLINE, synthetic 4K scenes, CPU: research OWNFG_RESULTS.md): PushWarp's I is about level with a learned
// interpolator on average, worse at thin objects in front of a moving background; not measured on game footage; not
// measured on any graphics card. NVIDIA's own I could not be measured there.
#include "veyra/pipeline/PushPredict.h"
#include "veyra/pipeline/PushWarp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::pipeline {

// The row's choice (preference pushLowDelaySource: "auto", "nvidia", "pushfps"; anything else: auto).
enum class PushOwnFrameSource : std::uint8_t { Auto = 0, Nvidia = 1, PushFps = 2 };
template <class Char>
inline PushOwnFrameSource pushOwnFrameSourceFrom(std::basic_string_view<Char> v) {
    const auto is = [&](const char* w) {
        std::size_t n = 0;
        while (w[n]) ++n;
        if (v.size() != n) return false;
        for (std::size_t i = 0; i < n; ++i) if (static_cast<std::uint32_t>(v[i]) != static_cast<std::uint32_t>(w[i])) return false;
        return true;
    };
    return is("nvidia") ? PushOwnFrameSource::Nvidia : is("pushfps") ? PushOwnFrameSource::PushFps : PushOwnFrameSource::Auto;
}
inline const char* pushOwnFrameSourceName(PushOwnFrameSource s) {
    return s == PushOwnFrameSource::Nvidia ? "nvidia" : s == PushOwnFrameSource::PushFps ? "pushfps" : "auto";
}

// Why a pair's I is PushWarp's (None: NVIDIA's, or no predicted pair at all).
enum class PushOwnFrameUse : std::uint8_t { None = 0, Chosen, NotVfg, VfgRefused, Generator };
inline const char* pushOwnFrameUseName(PushOwnFrameUse u) {
    switch (u) {
    case PushOwnFrameUse::Chosen: return "chosen";
    case PushOwnFrameUse::NotVfg: return "not-vfg";
    case PushOwnFrameUse::VfgRefused: return "vfg-refused";
    case PushOwnFrameUse::Generator: return "generator";   // step 3: PushWarp is the configured frame generator
    default: return "none";
    }
}

// The generator the graph runs, as far as the own source cares.
// Step 3: PushWarp is also a frame generation choice of its own (FrameGenerationBackend::PushWarp): then every
// generated frame of every schedule is PushWarp's.
enum class PushOwnFrameGenerator : std::uint8_t { Vfg = 0, PushFlow, Other, PushWarp };
// Whether the predicted pass is created for this generator when the PushFPS source is available (pushPredictCreate's
// `vfg` argument): VFG as before; PushFlow because PushWarp makes I; nothing else.
inline bool pushOwnFrameServes(PushOwnFrameGenerator g) { return g == PushOwnFrameGenerator::PushFlow || g == PushOwnFrameGenerator::PushWarp; }

// The decision for one pair. warpReady: PushWarp passed its start-up self-check on this device; inputs: both real
// frames are held as pitch-linear buffers (VFG's input buffers, or the own copies for PushFlow). vfgRefused is not an
// input any more (review 1, MUST 3): a refusal never brings PushWarp in; VfgRefused is kept only as a name in old logs.
inline PushOwnFrameUse pushOwnFrameUse(PushOwnFrameSource source, PushOwnFrameGenerator generator, bool warpReady, bool inputs) {
    if (!warpReady || !inputs) return PushOwnFrameUse::None;
    if (generator == PushOwnFrameGenerator::Other) return PushOwnFrameUse::None;
    if (generator == PushOwnFrameGenerator::PushWarp) return PushOwnFrameUse::Generator;   // the source choice does not apply
    if (generator == PushOwnFrameGenerator::PushFlow) return source == PushOwnFrameSource::Nvidia ? PushOwnFrameUse::None : PushOwnFrameUse::NotVfg;
    return source == PushOwnFrameSource::PushFps ? PushOwnFrameUse::Chosen : PushOwnFrameUse::None;   // VFG: NVIDIA's unless chosen
}
// Whether the graph creates PushWarp at all (review 1, SHOULD 9): with PushFlow while the row shapes the graph; with VFG
// only when the user chose the PushFPS source or the causal schedule (EnhancementSettings::lowDelayOwnWarp).
inline bool pushOwnFrameCreate(PushOwnFrameGenerator generator, bool ownWarpChosen) {
    return generator == PushOwnFrameGenerator::PushFlow || generator == PushOwnFrameGenerator::PushWarp ||
           (generator == PushOwnFrameGenerator::Vfg && ownWarpChosen);
}

// The eligibility a pair is judged with when its I is PushWarp's: the generator counts as served (pushPredictPairRefusal's
// NotVfg) and VFG's refusal does not apply (no VFG request is made). Every other reason stays as it was.
inline void pushOwnFrameEligibility(PushPredictEligibility& e, PushOwnFrameUse use) {
    if (use == PushOwnFrameUse::None) return;
    e.vfg = true;
    e.vfgRefused = false;
}

// Shared with the bridge (the row) and the engine thread: relaxed atomics, one writer each.
struct PushOwnFrameShared {
    std::atomic<std::uint8_t> source{0};        // PushOwnFrameSource, written by the bridge from the preference
    std::atomic<std::uint8_t> inUse{0};         // PushOwnFrameUse of the last predicted pair (graph)
    std::atomic<std::uint8_t> warp{0};          // 0 not created, 1 ready, 2 its self-check failed, 3 too slow (review 1, SHOULD 4)
    std::atomic<std::uint64_t> ownPairs{0};     // predicted pairs whose I was PushWarp's (graph)
};
inline PushOwnFrameShared& pushOwnFrameShared() { static PushOwnFrameShared s; return s; }

// PushWarp's GPU time by stage over the last 10 s (as PushPredictCostWindow for P), with nearest-rank percentiles.
class PushWarpCostWindow {
public:
    static constexpr std::int64_t kWindow100ns = 100000000;   // 10 s
    static constexpr std::size_t kMaxSamples = 512;
    void clear() { samples_.clear(); }
    void add(std::int64_t now100ns, const PushWarpStageMs& ms) {
        samples_.push_back({now100ns, ms});
        while (samples_.size() > kMaxSamples || (!samples_.empty() && samples_.front().first < now100ns - kWindow100ns)) samples_.pop_front();
    }
    std::size_t size() const { return samples_.size(); }
    double percentile(PushWarpStage stage, double q) const {
        std::vector<double> v;
        v.reserve(samples_.size());
        for (const auto& s : samples_) { const double x = s.second[static_cast<unsigned>(stage)]; if (x >= 0) v.push_back(x); }
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        const auto rank = static_cast<std::size_t>(std::ceil(std::clamp(q, 0.0, 1.0) * double(v.size())));
        return v[rank == 0 ? 0 : rank - 1];
    }

private:
    std::deque<std::pair<std::int64_t, PushWarpStageMs>> samples_;
};
inline std::string pushWarpCostText(const PushWarpCostWindow& w) {
    std::string t = "samples:" + std::to_string(w.size());
    for (unsigned i = 0; i < static_cast<unsigned>(PushWarpStage::Count); ++i) {
        const auto stage = static_cast<PushWarpStage>(i);
        char part[96];
        const int n = std::snprintf(part, sizeof part, ",%s:%.2f/%.2f/%.2f", pushWarpStageName(stage), w.percentile(stage, 0.5), w.percentile(stage, 0.95), w.percentile(stage, 0.99));
        if (n > 0) t.append(part, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof part) - 1)));
    }
    return t;
}

// What the graph keeps about the own source for the evidence lines (EnhanceGraph::ownFrameTimingText): totals and the
// last 10 s. Labelled by source so a log says which generator made I.
struct PushOwnFrameEvidence {
    PushOwnFrameSource source = PushOwnFrameSource::Auto;
    PushOwnFrameUse lastUse = PushOwnFrameUse::None;
    std::uint64_t ownPairs = 0, vfgPairs = 0;          // predicted pairs by the source of I (totals)
    std::uint64_t wholeFrameNearest = 0, measured = 0;  // own I whose verdict showed the nearer real frame; own I read back
    double holeShareSum = 0;
    int warp = 0;                                       // PushOwnFrameShared::warp
};
// " fgSource=<pushwarp|nvidia> ownI=<use> ownIPairs=<n> vfgIPairs=<n> ownIWholeHeld=<n> ownIHoleShare=<mean> ownIGpuMs=samples:..."
// (ms P50/P95/P99 over 10 s, measured by timestamps in I's own list; -1 unknown). Empty when PushWarp was never created.
inline std::string pushOwnFrameTimingText(const PushOwnFrameEvidence& e, const PushWarpCostWindow& w) {
    if (e.warp == 0) return std::string();
    char head[256];
    const int n = std::snprintf(head, sizeof head, " fgSource=%s ownI=%s ownIChoice=%s ownIPairs=%llu vfgIPairs=%llu ownIWholeHeld=%llu ownIHoleShare=%.3f ownIWarp=%s ownIGpuMs=",
                                e.lastUse == PushOwnFrameUse::None ? "nvidia" : "pushwarp", pushOwnFrameUseName(e.lastUse), pushOwnFrameSourceName(e.source),
                                static_cast<unsigned long long>(e.ownPairs), static_cast<unsigned long long>(e.vfgPairs),
                                static_cast<unsigned long long>(e.wholeFrameNearest), e.measured ? e.holeShareSum / double(e.measured) : -1.0,
                                e.warp == 1 ? "ready" : e.warp == 3 ? "too-slow" : "check-failed");
    std::string t = n > 0 ? std::string(head, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof head) - 1))) : std::string();
    return t + pushWarpCostText(w);
}

}  // namespace veyra::pipeline
