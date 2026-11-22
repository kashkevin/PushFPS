// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushGpuBudget.h"
#include "veyra/engine/PushHardwareCapabilities.h"
#include "veyra/engine/PushLatencyGovernor.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace veyra::engine {
// Glue between the player snapshot and the two recommendation models (PushLatencyGovernor.h, PushGpuBudget.h).
// Portable and header-only; tests: tests/remoteplay/GovernorFeedTests.inc. The bridge (PushSlipstream.cpp,
// pushSlipstreamWatch) copies numbers the snapshot already holds into PushFeedInput once a second; this file
// turns cumulative counters into per-second shares and marks what is not measured. Switches:
//   VEYRA_EXP_RP_GOVERNOR=1          observe only: the governor's recommendation is logged, never applied
//   VEYRA_EXP_RP_GPU_BUDGET=1        observe only: the GPU budget's level is logged
//   VEYRA_EXP_RP_GPU_BUDGET=apply    the GPU budget's reductions (and its recovery, never above the request) are
//                                    applied through the bridge's own setters
// Unverified on Windows; no number here was measured on any GPU.
//
// Inputs that the snapshot does not have (19 November 2026) and how each model receives them:
//   - present lateness and the PS5 owner thread's scheduling delay: no source. The governor gets a neutral
//     value between its "bad" and "clear" thresholds, so neither can start or clear a rule.
//   - thermal throttling: no vendor-neutral source; Unknown.
//   - late real frames: not counted; missed deadlines are generated frames that expired after evaluation.
//   - decode-queue delay: PS5 / stream sources only (CpuStage::DecodedQueue); unknown for capture cards, so the
//     budget never sees a capture card as healthy and never steps back up there.
// Approximations: the per-frame GPU time is the enhancement P95 minus the FG batch P95 (the enhancement span
// contains the batch; P95s do not subtract exactly), and the queue depth is the ingress queue's current depth,
// not a mean.

enum class PushExperimentMode : std::uint8_t { Off = 0, Observe = 1, Apply = 2 };
// value: the switch's text, nullopt when unset. Empty or "0" is off. "apply" (any case) is Apply when allowed;
// anything else is Observe (a switch that only observes can never do harm).
inline PushExperimentMode pushExperimentMode(std::optional<std::string_view> value, bool applyAllowed) noexcept {
    if (!value || value->empty() || *value == "0") return PushExperimentMode::Off;
    if (applyAllowed && value->size() == 5) {
        constexpr std::string_view apply = "apply";
        bool same = true;
        for (std::size_t i = 0; i < 5; ++i) {
            const char c = (*value)[i];
            const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            same = same && lower == apply[i];
        }
        if (same) return PushExperimentMode::Apply;
    }
    return PushExperimentMode::Observe;
}
inline const char* pushExperimentModeName(PushExperimentMode mode) noexcept {
    return mode == PushExperimentMode::Apply ? "apply" : mode == PushExperimentMode::Observe ? "observe" : "off";
}

// What the bridge copies from the snapshot. -1 (or any negative / non-finite value) = not measured.
struct PushFeedInput {
    std::int64_t now100ns = 0;          // monotonic
    std::uint64_t sessionId = 0;
    double sourceFps = -1;              // unique source pictures per second
    double outputFps = -1;              // presents per second
    double enhancementP95Ms = -1;       // GPU time of one source frame's graph work, FG batch included
    double fgBatchP95Ms = -1;           // GPU time of one frame-generation batch
    double decodeQueueP95Ms = -1;       // decoded picture waiting for the engine (stream sources)
    double decodeP95Ms = -1;
    double jitterMs = -1;
    double lossPercent = -1;
    double queueDepth = -1;             // ingress queue, frames
    double gpuUtilizationPercent = -1;
    double schedulingDelayMs = -1;      // the PS5 owner thread's lateness: no source today
    double presentLateMs = -1;          // no source today
    bool countersKnown = false;         // the four counters below are this session's cumulative values
    std::uint64_t presented = 0, expired = 0, pairsCandidate = 0, pairsSkipped = 0;
    bool framesLostKnown = false;
    std::uint64_t framesLost = 0;       // cumulative frames the stream could not complete
    unsigned multiplier = 1;            // in force: 1 = frame generation off
    PushThermalState thermal = PushThermalState::Unknown;
    PushPowerSource power = PushPowerSource::Unknown;
};

// One bit per model input that was not measured in a sample.
namespace feed_input {
inline constexpr std::uint32_t kSourceFps = 1u << 0, kOutputFps = 1u << 1, kGpuFrame = 1u << 2, kFgCost = 1u << 3,
    kDecodeQueue = 1u << 4, kMissedDeadlines = 1u << 5, kSkippedPairs = 1u << 6, kThermal = 1u << 7, kPower = 1u << 8,
    kJitter = 1u << 9, kLoss = 1u << 10, kFramesLost = 1u << 11, kDecode = 1u << 12, kGpuHeadroom = 1u << 13,
    kQueueDepth = 1u << 14, kScheduling = 1u << 15, kPresentLate = 1u << 16;
inline constexpr std::size_t kCount = 17;
inline constexpr std::string_view kNames[kCount] = {"sourceFps", "outputFps", "gpuFrame", "fgCost", "decodeQueue",
    "missedDeadlines", "skippedPairs", "thermal", "power", "jitter", "loss", "framesLost", "decode", "gpuHeadroom",
    "queueDepth", "scheduling", "presentLate"};
} // namespace feed_input
// "a,b,c" for the bits set, "none" when none is.
inline std::string pushFeedUnavailable(std::uint32_t mask) {
    std::string out;
    for (std::size_t i = 0; i < feed_input::kCount; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        if (!out.empty()) out += ',';
        out += feed_input::kNames[i];
    }
    return out.empty() ? std::string("none") : out;
}

// What the governor's controller can change today. The bridge reads the switches once per stream, when its
// watch starts for that stream (a PS5 session reads them when it connects).
struct PushGovernorSwitches {
    bool lanProfile = false;        // VEYRA_EXP_RP_LAN_PROFILE: the short reorder wait
    bool reorderAdaptive = false;   // VEYRA_EXP_RP_REORDER_ADAPTIVE
    bool backlog = false;           // VEYRA_EXP_RP_BACKLOG
    int padPollMs = 4;              // VEYRA_EXP_RP_PAD_POLL_MS (the bridge's ps5PadPollMs)
};
inline GovernorKnobs pushGovernorKnobs(const PushGovernorSwitches& s, unsigned multiplier, unsigned tier) noexcept {
    GovernorKnobs k;
    k.reorder = s.reorderAdaptive ? ReorderProfile::Adaptive : s.lanProfile ? ReorderProfile::Short : ReorderProfile::Long;
    k.backlog = s.backlog;
    k.controllerPollMs = s.padPollMs > 0 ? s.padPollMs : 4;
    k.fgStep = multiplier >= 2 && multiplier <= 64 ? static_cast<int>(multiplier) - 1 : 0;
    k.qualityTier = tier <= 64 ? static_cast<int>(tier) : 0;
    return k;
}

struct PushFeedOutput {
    PushGpuBudgetSample budget{};
    std::uint32_t budgetUnknown = 0;
    GovernorMeasurement governor{};
    bool governorValid = false;          // false without a source rate: the governor is not fed then
    std::uint32_t governorUnknown = 0;
    std::int64_t nowMs = 0;              // the governor's clock
};

class PushGovernorFeed {
public:
    void reset() noexcept { have_ = false; }

    PushFeedOutput update(const PushFeedInput& in, const GovernorKnobs& knobs) noexcept {
        namespace b = feed_input;
        PushFeedOutput out;
        out.nowMs = in.now100ns / 10000;
        // Counter deltas against the previous sample of the same session; none across a restart or a step back.
        const bool continuous = have_ && in.sessionId == previous_.sessionId && in.now100ns > previous_.now100ns;
        const double seconds = continuous ? static_cast<double>(in.now100ns - previous_.now100ns) / 1e7 : 0.0;
        const bool counters = continuous && in.countersKnown && previous_.countersKnown && in.presented >= previous_.presented &&
            in.expired >= previous_.expired && in.pairsCandidate >= previous_.pairsCandidate && in.pairsSkipped >= previous_.pairsSkipped;
        const bool lost = continuous && in.framesLostKnown && previous_.framesLostKnown && in.framesLost >= previous_.framesLost;
        const std::uint64_t presented = counters ? in.presented - previous_.presented : 0;
        const std::uint64_t expired = counters ? in.expired - previous_.expired : 0;
        const std::uint64_t candidates = counters ? in.pairsCandidate - previous_.pairsCandidate : 0;
        const std::uint64_t skipped = counters ? in.pairsSkipped - previous_.pairsSkipped : 0;
        const double lostPerSecond = lost ? static_cast<double>(in.framesLost - previous_.framesLost) / seconds : -1;
        previous_ = in;
        have_ = true;

        // GPU budget sample.
        auto& s = out.budget;
        std::uint32_t unknown = 0;
        s.now100ns = in.now100ns;
        if (known(in.sourceFps) && in.sourceFps > 0) s.sourceIntervalMs = 1000.0 / in.sourceFps; else unknown |= b::kSourceFps;
        if (known(in.outputFps) && in.outputFps > 0) s.outputIntervalMs = 1000.0 / in.outputFps; else unknown |= b::kOutputFps;
        const bool generating = in.multiplier >= 2;
        if (!generating) s.fgMs = 0;
        else if (known(in.fgBatchP95Ms)) s.fgMs = in.fgBatchP95Ms;
        else unknown |= b::kFgCost;
        if (known(in.enhancementP95Ms)) {
            const double rest = in.enhancementP95Ms - (generating && known(in.fgBatchP95Ms) ? in.fgBatchP95Ms : 0.0);
            s.gpuFrameMs = rest > 0 ? rest : 0.0;
        } else unknown |= b::kGpuFrame;
        if (known(in.decodeQueueP95Ms)) s.decodeQueueDelayMs = in.decodeQueueP95Ms; else unknown |= b::kDecodeQueue;
        if (counters && presented + expired > 0 && presented <= 0xFFFFFFFFull && expired <= 0xFFFFFFFFull) {
            s.presented = static_cast<unsigned>(presented);
            s.missedDeadlines = static_cast<unsigned>(expired);
        } else unknown |= b::kMissedDeadlines;
        if (counters && candidates > 0 && skipped <= candidates) s.skippedPairShare = static_cast<double>(skipped) / static_cast<double>(candidates);
        else unknown |= b::kSkippedPairs;
        s.thermal = in.thermal;
        if (in.thermal == PushThermalState::Unknown) unknown |= b::kThermal;
        s.power = in.power;
        if (in.power == PushPowerSource::Unknown) unknown |= b::kPower;
        out.budgetUnknown = unknown;

        // Governor measurement: unknown values become neutral ones (see the top of this file).
        using G = PushLatencyGovernor;
        auto& g = out.governor;
        unknown = 0;
        const auto take = [&unknown](double value, double neutral, std::uint32_t bit) {
            if (known(value)) return value;
            unknown |= bit;
            return neutral;
        };
        g.networkJitterMs = take(in.jitterMs, G::kCalmJitterMs, b::kJitter);
        g.lossPercent = take(in.lossPercent > 100 ? -1.0 : in.lossPercent, G::kCalmLossPercent, b::kLoss);
        g.framesLostPerSecond = take(lostPerSecond, 1.0, b::kFramesLost);
        g.decodeMs = take(in.decodeP95Ms, 0.0, b::kDecode);
        g.gpuMs = take(in.enhancementP95Ms, 0.0, b::kGpuFrame);
        g.cpuSchedulingDelayMs = take(in.schedulingDelayMs, static_cast<double>(knobs.controllerPollMs > 0 ? knobs.controllerPollMs : 4), b::kScheduling);
        g.queueDepth = take(in.queueDepth, (G::kQueueBad + G::kQueueClear) / 2, b::kQueueDepth);
        g.presentLateMs = take(in.presentLateMs, (G::kPresentLateBad + G::kPresentLateClear) / 2, b::kPresentLate);
        // A utilization over 100 % gives a negative headroom, which take() treats as not measured.
        const double headroom = known(in.gpuUtilizationPercent) ? 100.0 - in.gpuUtilizationPercent : -1.0;
        g.gpuHeadroomPercent = take(headroom, (G::kHeadroomBad + G::kHeadroomClear) / 2, b::kGpuHeadroom);
        if (known(in.sourceFps) && in.sourceFps > 0) { g.sourceFps = in.sourceFps; out.governorValid = true; }
        else unknown |= b::kSourceFps;
        out.governorUnknown = unknown;
        return out;
    }

private:
    static bool known(double value) noexcept { return std::isfinite(value) && value >= 0; }
    PushFeedInput previous_{};
    bool have_ = false;
};

// The settings frame generation runs with (or the user asked for). multiplier 1 = off.
struct PushFgRequest {
    PushFgMethod method = PushFgMethod::PushFlow;
    unsigned multiplier = 1;
    unsigned tier = 0;
    bool operator==(const PushFgRequest&) const = default;
};
inline PushFgRequest pushNormalizedFgRequest(PushFgRequest r) noexcept {
    if (r.multiplier < 2) { r.multiplier = 1; r.tier = 0; }
    const unsigned tiers = pushFgTierCount(r.method);
    if (r.tier >= tiers) r.tier = tiers - 1;
    return r;
}

// Runs PushGpuBudget against the settings in force. The ladder is built from the user's request; whenever the
// settings in force differ from what this driver expects (the user changed something, or a setter refused a
// level) it starts again with those settings as the new request, so it never climbs above what the user chose.
// In observe mode it never asks for a change. In apply mode it asks for the decision's level; level 0 is the
// request itself, so a ladder whose top is not exactly the request never "corrects" the user's choice.
// newSource(): a new stream (or a pause for the optimiser) starts the budget again at the request, as
// PushGpuBudget::reset asks; when the settings in force are still a level this driver put in place, apply mode
// then asks for the request again, so a reduction never outlives the stream it was made for.
//
// A setter that refuses a level (the settings stay where they were) is not a user choice: the request stays,
// and nothing is applied for kRefusedHold100ns (so a refused setter's notice does not come back every tick).
// pendingRestore(): the user's request, when the settings in force are still a level this driver put in
// place; the caller puts it back before a stop, a quit or a measurement (optimiser, quality test), so a
// reduced setting is never saved as the user's.
class PushGpuBudgetDriver {
public:
    static constexpr std::int64_t kRefusedHold100ns = 600000000;   // 60 s
    struct Step {
        PushGpuBudgetDecision decision{};
        PushFgRequest request{};   // the ceiling
        bool restarted = false;
        bool refused = false;      // the last level asked for was not taken
        bool held = false;         // a change is due but waits after a refusal
        bool apply = false;        // set target now
        PushFgRequest target{};
    };
    void reset() noexcept { started_ = false; fresh_ = false; applyPending_ = false; holdUntil_ = kNoHold; }
    void newSource() noexcept { fresh_ = started_; }
    bool started() const noexcept { return started_; }

    std::optional<PushFgRequest> pendingRestore(PushFgRequest current) const noexcept {
        if (!started_ || expected_ == request_ || !(pushNormalizedFgRequest(current) == expected_)) return std::nullopt;
        return request_;
    }
    // The caller put pendingRestore()'s request back.
    void restored() noexcept { expected_ = request_; applyPending_ = false; }

    Step update(PushFgRequest current, const PushGpuBudgetSample& sample, bool applyMode) {
        current = pushNormalizedFgRequest(current);
        Step out;
        if (applyPending_) {
            applyPending_ = false;
            if (!(current == expected_) && current == applyFrom_) {
                expected_ = current;
                holdUntil_ = sample.now100ns + kRefusedHold100ns;
                out.refused = true;
            }
        }
        if (!started_ || !(current == expected_)) {
            request_ = current;
            budget_.reset(pushGpuBudgetLadder(current.method, current.multiplier, current.tier));
            expected_ = current;
            started_ = true;
            holdUntil_ = kNoHold;
            out.restarted = true;
        } else if (fresh_) {
            budget_.reset(pushGpuBudgetLadder(request_.method, request_.multiplier, request_.tier));
            out.restarted = true;
        }
        fresh_ = false;
        out.decision = budget_.update(sample);
        out.request = request_;
        if (applyMode) {
            PushFgRequest want = request_;
            if (out.decision.index > 0) {
                want.multiplier = out.decision.level.multiplier;
                want.tier = out.decision.level.tier;
                want = pushNormalizedFgRequest(want);
            }
            if (!(want == current)) {
                if (sample.now100ns < holdUntil_) {
                    out.held = true;
                } else {
                    out.apply = true;
                    out.target = want;
                    applyFrom_ = current;
                    expected_ = want;
                    applyPending_ = true;
                }
            }
        }
        return out;
    }

private:
    static constexpr std::int64_t kNoHold = INT64_MIN;
    PushGpuBudget budget_{};
    PushFgRequest request_{}, expected_{}, applyFrom_{};
    std::int64_t holdUntil_ = kNoHold;
    bool started_ = false;
    bool fresh_ = false;
    bool applyPending_ = false;
};

// "apply" needs every core input to be measurable: without the decode-queue delay (capture cards) the budget
// can never see a healthy period, so it could only ever step down. The caller observes instead.
inline bool pushGpuBudgetCanApply(std::uint32_t budgetUnknown) noexcept { return (budgetUnknown & feed_input::kDecodeQueue) == 0; }

// One log line at most every 10 s, or at once when the key (the recommendation) changes.
class PushFeedLogGate {
public:
    static constexpr std::int64_t kInterval100ns = 100000000;
    bool due(std::int64_t now100ns, std::string_view key) {
        if (have_ && key == key_ && now100ns >= last_ && now100ns - last_ < kInterval100ns) return false;
        have_ = true;
        key_.assign(key.data(), key.size());
        last_ = now100ns;
        return true;
    }
    void reset() noexcept { have_ = false; }

private:
    std::string key_;
    std::int64_t last_ = 0;
    bool have_ = false;
};

inline const char* pushGovernorKnobName(GovernorKnob knob) noexcept {
    switch (knob) {
    case GovernorKnob::Reorder: return "reorder";
    case GovernorKnob::Backlog: return "backlog";
    case GovernorKnob::ControllerPoll: return "controller-poll-ms";
    case GovernorKnob::FrameGeneration: return "frame-generation-step";
    case GovernorKnob::QualityTier: return "quality-tier";
    }
    return "unknown";
}
// "knob from->to effect=... rollback=0|1 reason=\"...\"", or "none".
inline std::string pushGovernorRecommendationText(const std::optional<GovernorRecommendation>& r) {
    if (!r) return "none";
    std::string out = pushGovernorKnobName(r->knob);
    out += ' ';
    out += std::to_string(r->from);
    out += "->";
    out += std::to_string(r->to);
    out += r->latencyEffect == LatencyEffect::Lower ? " effect=lower" : r->latencyEffect == LatencyEffect::Neutral ? " effect=neutral" : " effect=higher";
    out += r->rollback ? " rollback=1" : " rollback=0";
    out += " reason=\"";
    out += r->reason ? r->reason : "";
    out += '"';
    return out;
}
} // namespace veyra::engine
