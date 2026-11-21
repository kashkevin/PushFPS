// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace veyra::engine {
// Latency governor (experimental; recommendations only). Portable and header-only, no engine or UI
// types; tests: tests/remoteplay/AdaptiveTransportTests.inc (governor_*). Nothing in the engine or
// the interface calls it yet: a caller would feed it one measurement about once a second and decide
// itself whether to apply what it recommends.
//
// It looks at the network (jitter, loss, frames lost), decode time, GPU time and headroom, how late
// the owner thread is scheduled, the ingress queue depth, how late frames are presented and the source
// cadence, and recommends at most one change at a time to knobs that exist today:
//   - Backlog off           (VEYRA_EXP_RP_BACKLOG) when the ingress queue stands at 2.5 frames or more;
//   - frame generation down one step, then quality tier down one step, when the GPU has under 10 %
//     headroom or frames are presented 2 ms or more late;
//   - controller poll interval one step longer (1/2/4/8 ms) only when the owner thread is already
//     scheduled at least that late (the shorter interval is not happening anyway), one step shorter
//     after a long calm stretch;
//   - reorder profile from the library's long wait to the short one after a long clean stretch.
// Each needs 3 periods in a row (10 for the calm rules); between "bad" and "clear" thresholds the count
// holds (hysteresis). After any change the next waits kDwellMs. kEvaluateMs after a change the target
// metric is compared with the periods before it: worse by over 10 % plus a small margin and the change
// is rolled back, and that knob is left alone for kBlockMs (doubling, up to 16x). A recommendation the
// caller has not applied within kApplyTimeoutMs is dropped and the knob blocked the same way.
//
// Invariant: no recommendation raises latency to show more frames. It never turns the backlog on,
// never raises frame generation or the quality tier, never lengthens the reorder wait and lengthens
// the poll interval only up to the scheduling delay already paid (LatencyEffect::Neutral). The one
// move back is a rollback, and only of its own change whose target (a latency, lower is better) got
// worse. Restoring settings after a step-down is the user's decision. Thresholds are provisional and
// not calibrated on any hardware.
enum class GovernorKnob : std::uint8_t { Reorder = 0, Backlog = 1, ControllerPoll = 2, FrameGeneration = 3, QualityTier = 4 };
enum class GovernorMetric : std::uint8_t { QueueDepth, GpuMs, CpuSchedulingDelayMs, FramesLostPerSecond };
enum class LatencyEffect : std::uint8_t { Lower, Neutral, Higher };
enum class ReorderProfile : std::uint8_t { Short = 0, Adaptive = 1, Long = 2 };

struct GovernorMeasurement {
    double networkJitterMs = 0, lossPercent = 0, framesLostPerSecond = 0;
    double decodeMs = 0, gpuMs = 0, cpuSchedulingDelayMs = 0;
    double queueDepth = 0;   // mean ingress queue depth, frames
    double presentLateMs = 0; // how late frames were presented against their target
    double sourceFps = 0;     // source cadence; must be above 0
    double gpuHeadroomPercent = 0; // 0..100
};
struct GovernorKnobs {
    ReorderProfile reorder = ReorderProfile::Short;
    bool backlog = false;
    int controllerPollMs = 1;
    int fgStep = 0;      // 0 = no frame generation; higher = more generated frames
    int qualityTier = 0; // 0 = cheapest
    bool operator==(const GovernorKnobs&) const = default;
};
struct GovernorRecommendation {
    GovernorKnob knob = GovernorKnob::Backlog;
    int from = 0, to = 0; // the knob's value as an int (ReorderProfile and bool as their numbers)
    GovernorMetric target = GovernorMetric::QueueDepth;
    LatencyEffect latencyEffect = LatencyEffect::Lower;
    bool rollback = false;
    std::int64_t at = 0;
    const char* reason = "";
};
inline int governorKnobValue(const GovernorKnobs& k, GovernorKnob knob) noexcept {
    switch (knob) {
    case GovernorKnob::Reorder: return static_cast<int>(k.reorder);
    case GovernorKnob::Backlog: return k.backlog ? 1 : 0;
    case GovernorKnob::ControllerPoll: return k.controllerPollMs;
    case GovernorKnob::FrameGeneration: return k.fgStep;
    case GovernorKnob::QualityTier: return k.qualityTier;
    }
    return 0;
}
inline void applyRecommendation(GovernorKnobs& k, const GovernorRecommendation& r) noexcept {
    switch (r.knob) {
    case GovernorKnob::Reorder: k.reorder = static_cast<ReorderProfile>(r.to); break;
    case GovernorKnob::Backlog: k.backlog = r.to != 0; break;
    case GovernorKnob::ControllerPoll: k.controllerPollMs = r.to; break;
    case GovernorKnob::FrameGeneration: k.fgStep = r.to; break;
    case GovernorKnob::QualityTier: k.qualityTier = r.to; break;
    }
}

class PushLatencyGovernor {
public:
    static constexpr std::int64_t kDwellMs = 10000, kEvaluateMs = 5000, kApplyTimeoutMs = 5000, kBlockMs = 60000;
    static constexpr unsigned kSustainPeriods = 3, kCalmPeriods = 10, kMaxBlockShift = 4;
    static constexpr std::array<int, 4> kPollStepsMs{1, 2, 4, 8};
    // Thresholds: trigger, clear (between them a count holds).
    static constexpr double kQueueBad = 2.5, kQueueClear = 1.5;
    static constexpr double kHeadroomBad = 10, kHeadroomClear = 20, kPresentLateBad = 2, kPresentLateClear = 1;
    static constexpr double kCalmJitterMs = 1, kCalmLossPercent = 0.05, kCalmSchedulingShare = 0.5;
    static constexpr double kWorseShare = 1.10;

    // now: monotonic milliseconds. Returns at most one recommendation.
    std::optional<GovernorRecommendation> update(std::int64_t now, const GovernorMeasurement& m, const GovernorKnobs& k) noexcept {
        if (!valid(m) || !valid(k) || (started_ && now < last_)) { ++invalid_; return {}; }
        const std::int64_t previous = started_ ? last_ : now;
        started_ = true; last_ = now;
        // A recommendation waiting to be applied.
        if (pending_) {
            if (governorKnobValue(k, pending_->knob) == pending_->to) {
                const auto applied = *pending_;
                pending_.reset();
                if (!applied.rollback) { evaluating_ = Evaluating{applied, pendingBefore_}; evalStart_ = previous; evalSum_ = 0; evalCount_ = 0; }
            } else if (now - pending_->at >= kApplyTimeoutMs) {
                block(pending_->knob, now);
                pending_.reset();
                ++declined_;
                lastChange_ = now;
            }
        }
        // The knob under evaluation changed by someone else: no verdict.
        if (evaluating_ && governorKnobValue(k, evaluating_->rec.knob) != evaluating_->rec.to) evaluating_.reset();
        std::optional<GovernorRecommendation> out;
        if (evaluating_) {
            evalSum_ += metric(m, evaluating_->rec.target); ++evalCount_;
            if (now - evalStart_ >= kEvaluateMs) {
                const double after = evalSum_ / static_cast<double>(evalCount_);
                const auto done = *evaluating_;
                evaluating_.reset();
                if (after > done.beforeValue * kWorseShare + margin(done.rec.target)) {
                    block(done.rec.knob, now);
                    ++rollbacks_;
                    GovernorRecommendation r = done.rec;
                    r.from = done.rec.to; r.to = done.rec.from; r.rollback = true; r.at = now;
                    r.latencyEffect = LatencyEffect::Lower; r.reason = "the change made its target worse";
                    out = r;
                } else {
                    unblockPenalty(done.rec.knob);
                }
            }
        }
        observe(m, k);
        remember(m);
        if (out) return issue(*out, now);
        if (pending_ || evaluating_ || now - lastChange_ < kDwellMs) return {};
        // One rule, in this order.
        if (k.backlog && queueBad_ >= kSustainPeriods && open(GovernorKnob::Backlog, now))
            return issue(make(GovernorKnob::Backlog, 1, 0, GovernorMetric::QueueDepth, LatencyEffect::Lower, "the ingress queue stands"), now);
        if (gpuBad_ >= kSustainPeriods) {
            if (k.fgStep > 0 && open(GovernorKnob::FrameGeneration, now))
                return issue(make(GovernorKnob::FrameGeneration, k.fgStep, k.fgStep - 1, GovernorMetric::GpuMs, LatencyEffect::Lower, "the GPU has no headroom"), now);
            if (k.qualityTier > 0 && open(GovernorKnob::QualityTier, now))
                return issue(make(GovernorKnob::QualityTier, k.qualityTier, k.qualityTier - 1, GovernorMetric::GpuMs, LatencyEffect::Lower, "the GPU has no headroom"), now);
        }
        if (const auto longer = pollStep(k.controllerPollMs, +1); longer && pollLate_ >= kSustainPeriods && open(GovernorKnob::ControllerPoll, now))
            return issue(make(GovernorKnob::ControllerPoll, k.controllerPollMs, *longer, GovernorMetric::CpuSchedulingDelayMs, LatencyEffect::Neutral, "the owner thread is scheduled later than the poll interval"), now);
        if (const auto shorter = pollStep(k.controllerPollMs, -1); shorter && pollCalm_ >= kCalmPeriods && open(GovernorKnob::ControllerPoll, now))
            return issue(make(GovernorKnob::ControllerPoll, k.controllerPollMs, *shorter, GovernorMetric::CpuSchedulingDelayMs, LatencyEffect::Lower, "the owner thread is scheduled on time"), now);
        if (k.reorder == ReorderProfile::Long && networkCalm_ >= kCalmPeriods && open(GovernorKnob::Reorder, now))
            return issue(make(GovernorKnob::Reorder, static_cast<int>(ReorderProfile::Long), static_cast<int>(ReorderProfile::Short), GovernorMetric::FramesLostPerSecond, LatencyEffect::Lower, "the link is clean"), now);
        return {};
    }
    unsigned rollbacks() const noexcept { return rollbacks_; }
    unsigned declined() const noexcept { return declined_; }
    unsigned invalidMeasurements() const noexcept { return invalid_; }

private:
    static bool finite(double v) noexcept { return std::isfinite(v) && v >= 0; }
    static bool valid(const GovernorMeasurement& m) noexcept {
        return finite(m.networkJitterMs) && finite(m.lossPercent) && finite(m.framesLostPerSecond) && finite(m.decodeMs) &&
            finite(m.gpuMs) && finite(m.cpuSchedulingDelayMs) && finite(m.queueDepth) && finite(m.presentLateMs) &&
            finite(m.sourceFps) && m.sourceFps > 0 && finite(m.gpuHeadroomPercent) && m.gpuHeadroomPercent <= 100 && m.lossPercent <= 100;
    }
    static bool valid(const GovernorKnobs& k) noexcept {
        return k.fgStep >= 0 && k.qualityTier >= 0 && k.controllerPollMs > 0 && static_cast<unsigned>(k.reorder) <= 2u;
    }
    static double metric(const GovernorMeasurement& m, GovernorMetric t) noexcept {
        switch (t) {
        case GovernorMetric::QueueDepth: return m.queueDepth;
        case GovernorMetric::GpuMs: return m.gpuMs;
        case GovernorMetric::CpuSchedulingDelayMs: return m.cpuSchedulingDelayMs;
        case GovernorMetric::FramesLostPerSecond: return m.framesLostPerSecond;
        }
        return 0;
    }
    static double margin(GovernorMetric t) noexcept {
        switch (t) {
        case GovernorMetric::QueueDepth: return 0.5;
        case GovernorMetric::GpuMs: return 0.5;
        case GovernorMetric::CpuSchedulingDelayMs: return 0.5;
        case GovernorMetric::FramesLostPerSecond: return 0.2;
        }
        return 0;
    }
    // The next poll step up (+1) or down (-1); none at the ends or for a value off the table.
    static std::optional<int> pollStep(int current, int direction) noexcept {
        for (std::size_t i = 0; i < kPollStepsMs.size(); ++i) {
            if (kPollStepsMs[i] != current) continue;
            if (direction > 0 && i + 1 < kPollStepsMs.size()) return kPollStepsMs[i + 1];
            if (direction < 0 && i > 0) return kPollStepsMs[i - 1];
            return std::nullopt;
        }
        return std::nullopt;
    }
    static void count(unsigned& counter, bool bad, bool clear) noexcept {
        if (bad) { if (counter < 1000u) ++counter; }
        else if (clear) counter = 0;
    }
    void observe(const GovernorMeasurement& m, const GovernorKnobs& k) noexcept {
        count(queueBad_, m.queueDepth >= kQueueBad, m.queueDepth < kQueueClear);
        count(gpuBad_, m.gpuHeadroomPercent < kHeadroomBad || m.presentLateMs >= kPresentLateBad,
              m.gpuHeadroomPercent > kHeadroomClear && m.presentLateMs < kPresentLateClear);
        const auto longer = pollStep(k.controllerPollMs, +1);
        const auto shorter = pollStep(k.controllerPollMs, -1);
        // Late by at least the next interval: that interval costs nothing more than what is paid already.
        if (longer && m.cpuSchedulingDelayMs >= static_cast<double>(*longer)) { if (pollLate_ < 1000u) ++pollLate_; }
        else pollLate_ = 0;
        if (shorter && m.cpuSchedulingDelayMs < kCalmSchedulingShare * static_cast<double>(*shorter)) { if (pollCalm_ < 1000u) ++pollCalm_; }
        else pollCalm_ = 0;
        if (m.networkJitterMs < kCalmJitterMs && m.lossPercent < kCalmLossPercent && m.framesLostPerSecond == 0) { if (networkCalm_ < 1000u) ++networkCalm_; }
        else networkCalm_ = 0;
    }
    void remember(const GovernorMeasurement& m) noexcept {
        history_[historyNext_] = m;
        historyNext_ = (historyNext_ + 1) % history_.size();
        if (historyCount_ < history_.size()) ++historyCount_;
    }
    double before(GovernorMetric t) const noexcept {
        if (historyCount_ == 0) return 0;
        double sum = 0;
        for (std::size_t i = 0; i < historyCount_; ++i) sum += metric(history_[i], t);
        return sum / static_cast<double>(historyCount_);
    }
    static std::size_t index(GovernorKnob knob) noexcept { return static_cast<std::size_t>(knob); }
    bool open(GovernorKnob knob, std::int64_t now) const noexcept { return now >= blockedUntil_[index(knob)]; }
    void block(GovernorKnob knob, std::int64_t now) noexcept {
        auto& shift = blockShift_[index(knob)];
        blockedUntil_[index(knob)] = now + (kBlockMs << shift);
        if (shift < kMaxBlockShift) ++shift;
    }
    void unblockPenalty(GovernorKnob knob) noexcept { blockShift_[index(knob)] = 0; }
    GovernorRecommendation make(GovernorKnob knob, int from, int to, GovernorMetric target, LatencyEffect effect, const char* reason) const noexcept {
        GovernorRecommendation r;
        r.knob = knob; r.from = from; r.to = to; r.target = target; r.latencyEffect = effect; r.reason = reason;
        return r;
    }
    GovernorRecommendation issue(GovernorRecommendation r, std::int64_t now) noexcept {
        r.at = now;
        pending_ = r;
        lastChange_ = now;
        pendingBefore_ = r.rollback ? 0.0 : before(r.target);
        return r;
    }
    // A change applied and being judged: its target's mean over the periods before it.
    struct Evaluating {
        GovernorRecommendation rec;
        double beforeValue = 0;
    };
    std::optional<GovernorRecommendation> pending_;
    std::optional<Evaluating> evaluating_;
    std::array<GovernorMeasurement, 5> history_{};
    std::size_t historyNext_ = 0, historyCount_ = 0;
    std::array<std::int64_t, 5> blockedUntil_{};
    std::array<unsigned, 5> blockShift_{};
    std::int64_t last_ = 0, lastChange_ = -kDwellMs, evalStart_ = 0;
    double evalSum_ = 0, pendingBefore_ = 0;
    unsigned evalCount_ = 0, queueBad_ = 0, gpuBad_ = 0, pollLate_ = 0, pollCalm_ = 0, networkCalm_ = 0;
    unsigned rollbacks_ = 0, declined_ = 0, invalid_ = 0;
    bool started_ = false;
};
} // namespace veyra::engine
