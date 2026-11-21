// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace veyra::engine {
// Performance modes for PS5 Remote Play (docs/ROADMAP_AND_PROJECTIONS_2026-11-18.md, section 7).
//
// Portable and header-only: tables and arithmetic, no Windows, no engine types. Tests:
// tests/remoteplay/PerformanceModeTests.inc. This header is the description and the list of experiment
// switches each mode asks for (applied at start-up from slipstream.ini, see SlipstreamSettings.h).
// The quick setup's mode row (qml/Veyra/PushModes.js, a copy of pushModePlan() held to this one by the
// same table of rows in both test suites) turns a plan into the settings the interface already has:
// frame generation, its multiplier, duplicate removal, the present queue, display sync and the output
// cap. The engine itself still reads no mode. Unverified on Windows.
//
//   Mode                 30 fps game        60 fps game        presentation
//   Ultra Low Latency    no generation      no generation      newest picture at once, low queue, VRR or tearing
//   Competitive          no generation      2X  -> 120         low queue, VRR
//   Balanced             3X  -> 90          2X  -> 120         VRR (120 on a 180 Hz panel is uneven without it)
//   Maximum Smoothness   4X  -> 120         3X  -> 180         VRR; only while the generator holds its deadline
//   Automatic            a controller (PushAutomaticMode) that moves between the four above
//
// Not offered, and reported as such by pushModeTarget(): 30 -> 180 (6X, above the generator's 4X) and
// 60 -> 90 (1.5X, not a whole multiplier).
//
// "LAN optimized" is not a mode: it is a profile flag (network settings) that combines with any of them.
//
// Interpolation cannot show an in-between picture before the next real one has arrived, so a multiplier M
// delays every real picture by at least (M-1)/M of a source interval: pushInterpolationDelayMs(). That
// figure is arithmetic, not a measurement, and the generator's own GPU time comes on top of it.
enum class PushPerformanceMode : std::uint8_t { UltraLowLatency = 0, Competitive = 1, Balanced = 2, MaximumSmoothness = 3, Automatic = 4 };

// PushFlow, the project's own generator, offers 2X to 4X (displayFgMultiplierSelectable).
inline constexpr unsigned kPushModeMaxMultiplier = 4;
// Same fit rule as the display hint (DisplayFgBudget.h): source x M may exceed the refresh rate by 2 %, so
// 60.00 x 3 still counts as fitting a 179.82 Hz mode.
inline constexpr double kPushModeDisplayFit = 1.02;
inline constexpr unsigned kPushModeConcreteCount = 4;

constexpr bool pushModeIsConcrete(PushPerformanceMode mode) { return static_cast<unsigned>(mode) < kPushModeConcreteCount; }

constexpr std::string_view pushPerformanceModeName(PushPerformanceMode mode) {
    switch (mode) {
    case PushPerformanceMode::UltraLowLatency: return "ultra-low-latency";
    case PushPerformanceMode::Competitive: return "competitive";
    case PushPerformanceMode::Balanced: return "balanced";
    case PushPerformanceMode::MaximumSmoothness: return "maximum-smoothness";
    case PushPerformanceMode::Automatic: return "automatic";
    }
    return "invalid";
}

// Accepts the names above in any letter case, with spaces, '-' or '_' between the words or none, and a
// few short forms. Anything else is no mode.
inline std::optional<PushPerformanceMode> parsePushPerformanceMode(std::string_view text) {
    std::array<char, 24> folded{};
    std::size_t length = 0;
    for (const char c : text) {
        if (c == ' ' || c == '-' || c == '_' || c == '\t') continue;
        if (length == folded.size()) return std::nullopt;
        folded[length++] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view name(folded.data(), length);
    if (name == "ultralowlatency" || name == "ultralow" || name == "ull") return PushPerformanceMode::UltraLowLatency;
    if (name == "competitive") return PushPerformanceMode::Competitive;
    if (name == "balanced") return PushPerformanceMode::Balanced;
    if (name == "maximumsmoothness" || name == "maxsmoothness" || name == "smooth") return PushPerformanceMode::MaximumSmoothness;
    if (name == "automatic" || name == "auto") return PushPerformanceMode::Automatic;
    return std::nullopt;
}

// The table has rows for a 30 and a 60 fps game only (29.97 and 59.94 on the stream). Anything else,
// for example a 40 fps mode, has no row.
enum class PushSourceCadence : std::uint8_t { Unknown, Thirty, Sixty };
inline PushSourceCadence pushSourceCadence(double sourceFps) {
    if (!std::isfinite(sourceFps)) return PushSourceCadence::Unknown;
    if (sourceFps >= 28.5 && sourceFps <= 31.5) return PushSourceCadence::Thirty;
    if (sourceFps >= 57.0 && sourceFps <= 63.0) return PushSourceCadence::Sixty;
    return PushSourceCadence::Unknown;
}

// Minimum delay interpolation adds to every real picture, in milliseconds: (M-1)/M x source interval.
// 0 without generation or for unusable input.
inline double pushInterpolationDelayMs(unsigned multiplier, double sourceFps) {
    if (multiplier < 2 || !std::isfinite(sourceFps) || sourceFps <= 0) return 0;
    return (static_cast<double>(multiplier) - 1.0) / static_cast<double>(multiplier) * 1000.0 / sourceFps;
}

// Can this output rate be made from this source rate at all?
//   ok                                 whole multiplier from 2 to kPushModeMaxMultiplier
//   no-generation                      output equals the source
//   fractional-multiplier              60 -> 90 would be 1.5X: only whole multipliers exist
//   multiplier-above-generator-limit   30 -> 180 would be 6X (multiplier and the delay it would add are reported)
//   below-source-rate, bad-input
struct PushModeTarget {
    bool available = false;
    unsigned multiplier = 0;
    double minAddedDelayMs = 0;
    std::string_view reason = "bad-input";
};
inline PushModeTarget pushModeTarget(double sourceFps, double outputFps) {
    PushModeTarget target;
    if (!std::isfinite(sourceFps) || !std::isfinite(outputFps) || sourceFps <= 0 || outputFps <= 0) return target;
    const double ratio = outputFps / sourceFps;
    if (ratio < 1.0 / kPushModeDisplayFit) { target.reason = "below-source-rate"; return target; }
    const double rounded = std::round(ratio);
    if (rounded < 1.0 || rounded > 64.0 || std::abs(ratio / rounded - 1.0) > kPushModeDisplayFit - 1.0) { target.reason = "fractional-multiplier"; return target; }
    target.multiplier = static_cast<unsigned>(rounded);
    target.minAddedDelayMs = pushInterpolationDelayMs(target.multiplier, sourceFps);
    if (target.multiplier == 1) { target.available = true; target.reason = "no-generation"; return target; }
    if (target.multiplier > kPushModeMaxMultiplier) { target.reason = "multiplier-above-generator-limit"; return target; }
    target.available = true;
    target.reason = "ok";
    return target;
}

// What one concrete mode means for a source rate and a display.
// reason: ok | display-limit (the multiplier was lowered so the output fits the refresh rate) |
//         bad-source-rate | bad-display-rate | mode-not-concrete. Every reason but the first two comes with
//         valid == false and no generation: unknown input never adds delay.
struct PushModePlan {
    bool valid = false;
    PushPerformanceMode mode = PushPerformanceMode::UltraLowLatency;
    bool frameGeneration = false;
    unsigned multiplier = 1;
    double outputFps = 0;
    double minAddedDelayMs = 0;        // (M-1)/M x source interval: the delay to disclose
    bool lowQueue = false;             // at most one frame queued for presentation
    bool presentNewestAtOnce = false;  // a newly decoded picture is shown at once, never held for pacing
    bool wantsVrr = true;              // the stream is 59.94 fps: every rate slips against a fixed 180.00 Hz
    bool tearingAcceptable = false;    // without VRR, tearing rather than waiting for the refresh
    bool unevenWithoutVrr = false;     // refresh rate / output rate is not a whole number (120 on 180 Hz)
    bool onlyWhileStable = false;      // meant to be left as soon as the generator misses its deadline
    std::string_view reason = "mode-not-concrete";
};
inline PushModePlan pushModePlan(PushPerformanceMode mode, double sourceFps, double displayHz) {
    PushModePlan plan;
    if (!pushModeIsConcrete(mode)) return plan;
    plan.mode = mode;
    plan.lowQueue = mode == PushPerformanceMode::UltraLowLatency || mode == PushPerformanceMode::Competitive;
    plan.presentNewestAtOnce = mode == PushPerformanceMode::UltraLowLatency;
    plan.tearingAcceptable = mode == PushPerformanceMode::UltraLowLatency;
    plan.onlyWhileStable = mode == PushPerformanceMode::MaximumSmoothness;
    const PushSourceCadence cadence = pushSourceCadence(sourceFps);
    if (cadence == PushSourceCadence::Unknown) { plan.reason = "bad-source-rate"; return plan; }
    if (!std::isfinite(displayHz) || displayHz <= 0) { plan.reason = "bad-display-rate"; return plan; }
    const bool thirty = cadence == PushSourceCadence::Thirty;
    unsigned multiplier = 1;
    switch (mode) {
    case PushPerformanceMode::UltraLowLatency: multiplier = 1; break;
    case PushPerformanceMode::Competitive: multiplier = thirty ? 1u : 2u; break;
    case PushPerformanceMode::Balanced: multiplier = thirty ? 3u : 2u; break;
    case PushPerformanceMode::MaximumSmoothness: multiplier = thirty ? 4u : 3u; break;
    case PushPerformanceMode::Automatic: break;
    }
    plan.reason = "ok";
    while (multiplier > 1 && sourceFps * static_cast<double>(multiplier) > displayHz * kPushModeDisplayFit) {
        --multiplier;
        plan.reason = "display-limit";
    }
    plan.valid = true;
    plan.multiplier = multiplier;
    plan.frameGeneration = multiplier > 1;
    plan.outputFps = sourceFps * static_cast<double>(multiplier);
    plan.minAddedDelayMs = pushInterpolationDelayMs(multiplier, sourceFps);
    const double refreshesPerFrame = displayHz / plan.outputFps;
    plan.unevenWithoutVrr = refreshesPerFrame >= 1.0 && std::abs(refreshesPerFrame / std::round(refreshesPerFrame) - 1.0) > kPushModeDisplayFit - 1.0;
    return plan;
}

// --- experiment switches a mode asks for ------------------------------------------------------------
//
// Environment switches are read when a session starts, so this list depends on the mode and the LAN flag
// only, never on the source rate. A switch that only matters with frame generation is harmless without it.
//
//   VEYRA_EXP_RP_PAD_POLL_MS=2      Ultra Low Latency, Competitive. Not 1: a moving stick or mouse then sends
//                                   about 1000 state packets a second and the console's reaction to that is
//                                   untested. Write the key yourself in slipstream.ini to try 1.
//   VEYRA_EXP_RP_ADAPTIVE_PHASE=1   every mode that can generate frames (and Automatic, which can enter one).
//
// The LAN profile belongs to the network work (branch exp/lan-profile: reorder wait, receive buffer). Its
// master switch there is VEYRA_EXP_RP_LAN_PROFILE=1, which is all `lan=1` asks for. NOTHING ON THIS BRANCH
// READS THAT SWITCH: until the network work is merged, `lan=1` sets a variable nobody looks at. Its
// companions (VEYRA_EXP_RP_THREAD_PRIORITY, _BACKLOG, _IDR_RETRY_MS, _HEALTH) are separate experiments
// and stay opt-in by their own key. If the name changes before the merge, change it in
// pushLanProfileSwitches() and nowhere else; the tests only require that the flag adds its own switches
// and leaves the mode's alone.
struct PushModeSwitch {
    std::string_view name;
    std::string_view value;
};
class PushModeSwitches {
public:
    constexpr bool add(std::string_view name, std::string_view value) {
        if (count_ == items_.size()) return false;
        items_[count_++] = PushModeSwitch{name, value};
        return true;
    }
    constexpr std::size_t size() const { return count_; }
    constexpr const PushModeSwitch& operator[](std::size_t index) const { return items_[index]; }
    constexpr const PushModeSwitch* begin() const { return items_.data(); }
    constexpr const PushModeSwitch* end() const { return items_.data() + count_; }

private:
    std::array<PushModeSwitch, 16> items_{};
    std::size_t count_ = 0;
};

constexpr PushModeSwitches pushLanProfileSwitches() {
    PushModeSwitches list;
    list.add("VEYRA_EXP_RP_LAN_PROFILE", "1");
    return list;
}

constexpr PushModeSwitches pushModeSwitches(PushPerformanceMode mode, bool lanOptimized) {
    PushModeSwitches list;
    if (static_cast<unsigned>(mode) > static_cast<unsigned>(PushPerformanceMode::Automatic)) return list;
    if (mode == PushPerformanceMode::UltraLowLatency || mode == PushPerformanceMode::Competitive) list.add("VEYRA_EXP_RP_PAD_POLL_MS", "2");
    if (mode != PushPerformanceMode::UltraLowLatency) list.add("VEYRA_EXP_RP_ADAPTIVE_PHASE", "1");
    if (lanOptimized)
        for (const PushModeSwitch& item : pushLanProfileSwitches()) list.add(item.name, item.value);
    return list;
}

// --- Automatic ----------------------------------------------------------------------------------------
//
// The user gives a ceiling: the highest-latency mode they accept. Automatic starts there and
//   - steps DOWN one level (towards lower latency) when trouble lasted troubleHold: GPU busy share at or
//     above gpuBusyHigh, queueDepthHigh or more frames waiting, or a poor network;
//   - steps back UP one level only after a healthy period of healthyHold without a single sample that was
//     not healthy, never above the ceiling, and never because of anything but that healthy period. A return
//     that did not last (trouble again soon after) doubles the healthy period asked for next time, up to 8x;
//   - never switches twice within minDwell;
//   - rests on the lowest-latency mode of its level: when two modes give the same multiplier for this source
//     and display (Balanced and Competitive for a 60 fps game; everything on a panel the source already
//     fills) it uses the lower one, because the higher one would add delay and no frames.
// A "level" is a distinct multiplier, so for a 30 fps game the levels are none, 3X and 4X.
// It never moves to a higher-latency mode than the ceiling, and raising the ceiling (setCeiling) does not
// raise the mode by itself. Input it cannot use is never counted as healthy.
//
// Every decision carries a short machine-readable reason:
//   start | at-ceiling | hold | healthy-waiting | dwell | at-floor | source-unknown | bad-input |
//   trouble-gpu-busy | trouble-queue-depth | trouble-network |
//   step-down-gpu-busy | step-down-queue-depth | step-down-network | step-down-no-benefit |
//   step-up-sustained-healthy
// Times are in 100 ns units, like the rest of the engine. Not thread-safe: one owner calls update().
// Called by the interface only while VEYRA_EXP_AUTO_MODE=1 (src/ui/bridge/PushSlipstream.cpp, fed through
// pushAutoInputs() below); the thresholds are starting points and have never run on the target PC.
enum class PushNetworkHealth : std::uint8_t { Healthy = 0, Degraded = 1, Poor = 2 };

struct PushAutoConfig {
    std::int64_t minDwell100ns = 50000000;      // 5 s
    std::int64_t troubleHold100ns = 30000000;   // 3 s
    std::int64_t healthyHold100ns = 300000000;  // 30 s
    double gpuBusyHigh = 0.90;                  // trouble at or above
    double gpuBusyLow = 0.70;                   // healthy at or below
    unsigned queueDepthHigh = 2;                // trouble at or above (frames waiting to be shown)
    unsigned queueDepthLow = 1;                 // healthy at or below
};

struct PushAutoInputs {
    std::int64_t now100ns = 0;
    double sourceFps = 0;       // from the cadence lock; anything that is not a 30 or 60 fps source counts as unknown
    double displayHz = 0;
    double gpuBusyShare = 0;    // 0..1
    bool gpuKnown = true;       // false: no GPU reading. Then the GPU is neither trouble nor healthy
    unsigned queueDepth = 0;
    PushNetworkHealth network = PushNetworkHealth::Healthy;
};

struct PushAutoDecision {
    PushPerformanceMode mode = PushPerformanceMode::UltraLowLatency;
    bool changed = false;
    std::string_view reason = "start";
};

class PushAutomaticMode {
public:
    explicit PushAutomaticMode(PushPerformanceMode chosenCeiling, PushAutoConfig settings = {})
        : config_(sanitized(settings)), ceiling_(pushModeIsConcrete(chosenCeiling) ? chosenCeiling : PushPerformanceMode::UltraLowLatency), mode_(ceiling_) {}

    PushPerformanceMode mode() const { return mode_; }
    PushPerformanceMode ceiling() const { return ceiling_; }
    const PushAutoConfig& config() const { return config_; }

    // The user's choice. Lowering applies at once; raising only allows a later step up.
    bool setCeiling(PushPerformanceMode chosenCeiling) {
        if (!pushModeIsConcrete(chosenCeiling)) return false;
        ceiling_ = chosenCeiling;
        if (rank(mode_) > rank(ceiling_)) mode_ = ceiling_;
        healthySince_ = kNever;
        return true;
    }

    PushAutoDecision update(const PushAutoInputs& in) {
        if (!started_) {
            started_ = true;
            lastNow_ = lastSwitch_ = in.now100ns;
            if (!usable(in)) return decide(false, "bad-input");
            observe(in, limit(in));
            return decide(false, "start");
        }
        if (in.now100ns < lastNow_ || !usable(in)) {
            // Also for a clock that ran backwards: forget the running periods, keep the dwell from here.
            if (in.now100ns < lastNow_) lastSwitch_ = in.now100ns;
            lastNow_ = in.now100ns;
            healthySince_ = troubleSince_ = kNever;
            return decide(false, "bad-input");
        }
        lastNow_ = in.now100ns;
        const bool sourceKnown = pushSourceCadence(in.sourceFps) != PushSourceCadence::Unknown && std::isfinite(in.displayHz) && in.displayHz > 0;
        const PushPerformanceMode cap = sourceKnown ? limit(in) : mode_;
        const Sample sample = observe(in, cap);
        const bool dwellOver = in.now100ns - lastSwitch_ >= config_.minDwell100ns;

        if (sample != Sample::Healthy && sample != Sample::Neutral) {
            if (mode_ == PushPerformanceMode::UltraLowLatency) return decide(false, "at-floor");
            if (in.now100ns - troubleSince_ >= config_.troubleHold100ns) {
                if (!dwellOver) return decide(false, "dwell");
                const PushPerformanceMode lower = sourceKnown ? levelBelow(mode_, in) : static_cast<PushPerformanceMode>(rank(mode_) - 1);
                const bool sameLevel = sourceKnown && multiplier(lower, in) == multiplier(mode_, in);
                if (!sameLevel && haveStepUp_) failedReturns_ = in.now100ns - lastStepUp_ < 8 * config_.healthyHold100ns ? (failedReturns_ < 3 ? failedReturns_ + 1 : 3u) : 0u;
                haveStepUp_ = false;
                switchTo(lower, in.now100ns);
                if (sameLevel) return decide(true, "step-down-no-benefit");
                return decide(true, sample == Sample::GpuBusy ? "step-down-gpu-busy" : sample == Sample::QueueDepth ? "step-down-queue-depth" : "step-down-network");
            }
            return decide(false, sample == Sample::GpuBusy ? "trouble-gpu-busy" : sample == Sample::QueueDepth ? "trouble-queue-depth" : "trouble-network");
        }
        if (!sourceKnown) return decide(false, "source-unknown");
        const PushPerformanceMode resting = lowestOfLevel(rank(mode_) > rank(cap) ? cap : mode_, in);
        if (resting != mode_) {
            if (!dwellOver) return decide(false, "dwell");
            switchTo(resting, in.now100ns);
            return decide(true, "step-down-no-benefit");
        }
        if (sample == Sample::Neutral) return decide(false, "hold");
        if (mode_ == cap) return decide(false, "at-ceiling");
        if (in.now100ns - healthySince_ < (config_.healthyHold100ns << failedReturns_)) return decide(false, "healthy-waiting");
        if (!dwellOver) return decide(false, "dwell");
        const PushPerformanceMode higher = levelAbove(mode_, cap, in);
        if (higher == mode_) return decide(false, "at-ceiling");
        switchTo(higher, in.now100ns);
        haveStepUp_ = true;
        lastStepUp_ = in.now100ns;
        healthySince_ = in.now100ns;
        return decide(true, "step-up-sustained-healthy");
    }

private:
    enum class Sample : std::uint8_t { Healthy, Neutral, GpuBusy, QueueDepth, Network };
    static constexpr std::int64_t kNever = INT64_MIN;
    static constexpr unsigned rank(PushPerformanceMode of) { return static_cast<unsigned>(of); }

    static PushAutoConfig sanitized(PushAutoConfig c) {
        const PushAutoConfig standard;
        const std::int64_t longest = INT64_C(864000000000);   // a day: keeps the back-off shift and the sums far from overflow
        c.minDwell100ns = c.minDwell100ns < 0 ? 0 : c.minDwell100ns > longest ? longest : c.minDwell100ns;
        c.troubleHold100ns = c.troubleHold100ns < 0 ? 0 : c.troubleHold100ns > longest ? longest : c.troubleHold100ns;
        c.healthyHold100ns = c.healthyHold100ns < 0 ? 0 : c.healthyHold100ns > longest ? longest : c.healthyHold100ns;
        if (!std::isfinite(c.gpuBusyHigh) || c.gpuBusyHigh <= 0 || c.gpuBusyHigh > 1) c.gpuBusyHigh = standard.gpuBusyHigh;
        if (!std::isfinite(c.gpuBusyLow) || c.gpuBusyLow < 0) c.gpuBusyLow = 0;
        if (c.gpuBusyLow > c.gpuBusyHigh) c.gpuBusyLow = c.gpuBusyHigh;
        if (c.queueDepthHigh < 1) c.queueDepthHigh = 1;
        if (c.queueDepthLow >= c.queueDepthHigh) c.queueDepthLow = c.queueDepthHigh - 1;
        return c;
    }
    static bool usable(const PushAutoInputs& in) {
        return (!in.gpuKnown || (std::isfinite(in.gpuBusyShare) && in.gpuBusyShare >= 0 && in.gpuBusyShare <= 1)) &&
               static_cast<unsigned>(in.network) <= static_cast<unsigned>(PushNetworkHealth::Poor);
    }
    static unsigned multiplier(PushPerformanceMode of, const PushAutoInputs& in) { return pushModePlan(of, in.sourceFps, in.displayHz).multiplier; }
    // The lowest-latency mode that gives the same multiplier as this one.
    static PushPerformanceMode lowestOfLevel(PushPerformanceMode of, const PushAutoInputs& in) {
        unsigned r = rank(of);
        while (r > 0 && multiplier(static_cast<PushPerformanceMode>(r - 1), in) == multiplier(of, in)) --r;
        return static_cast<PushPerformanceMode>(r);
    }
    static PushPerformanceMode levelBelow(PushPerformanceMode of, const PushAutoInputs& in) {
        const PushPerformanceMode resting = lowestOfLevel(of, in);
        if (rank(resting) == 0) return resting;
        return lowestOfLevel(static_cast<PushPerformanceMode>(rank(resting) - 1), in);
    }
    static PushPerformanceMode levelAbove(PushPerformanceMode of, PushPerformanceMode cap, const PushAutoInputs& in) {
        for (unsigned r = rank(of) + 1; r <= rank(cap); ++r)
            if (multiplier(static_cast<PushPerformanceMode>(r), in) > multiplier(of, in)) return static_cast<PushPerformanceMode>(r);
        return of;
    }
    // The highest mode worth being in for this source and display: the lowest-latency mode of the ceiling's level.
    PushPerformanceMode limit(const PushAutoInputs& in) const {
        if (pushSourceCadence(in.sourceFps) == PushSourceCadence::Unknown || !std::isfinite(in.displayHz) || in.displayHz <= 0) return mode_;
        return lowestOfLevel(ceiling_, in);
    }
    // Classifies the sample and keeps the two running periods. A limit that rose (the game went from 60 to
    // 30 fps, a faster display mode) restarts the healthy period: the step it allows is a step up like any other.
    Sample observe(const PushAutoInputs& in, PushPerformanceMode cap) {
        Sample sample = Sample::Neutral;
        if (in.network == PushNetworkHealth::Poor) sample = Sample::Network;
        else if (in.queueDepth >= config_.queueDepthHigh) sample = Sample::QueueDepth;
        else if (in.gpuKnown && in.gpuBusyShare >= config_.gpuBusyHigh) sample = Sample::GpuBusy;
        else if (in.gpuKnown && in.network == PushNetworkHealth::Healthy && in.queueDepth <= config_.queueDepthLow && in.gpuBusyShare <= config_.gpuBusyLow) sample = Sample::Healthy;
        const bool trouble = sample != Sample::Healthy && sample != Sample::Neutral;
        if (trouble) { if (troubleSince_ == kNever) troubleSince_ = in.now100ns; }
        else troubleSince_ = kNever;
        // Only a healthy sample counts towards the healthy period; a neutral one (between the thresholds, or a
        // degraded network) ends it like trouble does, without being trouble.
        if (sample != Sample::Healthy) healthySince_ = kNever;
        else if (healthySince_ == kNever || rank(cap) > rank(lastCap_)) healthySince_ = in.now100ns;
        lastCap_ = cap;
        return sample;
    }
    void switchTo(PushPerformanceMode next, std::int64_t now) {
        mode_ = next;
        lastSwitch_ = now;
        troubleSince_ = kNever;
    }
    PushAutoDecision decide(bool changed, std::string_view reason) const { return PushAutoDecision{mode_, changed, reason}; }

    PushAutoConfig config_;
    PushPerformanceMode ceiling_;
    PushPerformanceMode mode_;
    PushPerformanceMode lastCap_ = PushPerformanceMode::UltraLowLatency;
    bool started_ = false;
    bool haveStepUp_ = false;
    unsigned failedReturns_ = 0;
    std::int64_t lastNow_ = 0;
    std::int64_t lastSwitch_ = 0;
    std::int64_t lastStepUp_ = 0;
    std::int64_t healthySince_ = kNever;
    std::int64_t troubleSince_ = kNever;
};

// --- what the interface has, as controller input ------------------------------------------------------
//
// The interface reads these from numbers the engine already computes (pushLiveStats); nothing is measured
// for the controller. What is not known stays unknown:
//   - no GPU reading: the GPU is neither trouble nor healthy, so nothing steps up on it;
//   - the generator skipped pairs in the last second (fgBudgetLimited): counted as a busy GPU, whatever the
//     utilisation figure says, because that is the deadline Maximum Smoothness is only kept while it holds;
//   - a network source without a connection grade (VEYRA_EXP_RP_HEALTH is off, or its first five seconds
//     have not passed): Degraded, which is never trouble and never healthy. So Automatic can step down on a
//     PS5 stream for the GPU or the queue, but steps back up only when the health experiment reports Stable;
//   - a local source (capture card, file) has no network to be unhealthy: Healthy.
enum class PushAutoNetwork : std::uint8_t { Local = 0, Unknown = 1, Stable = 2, Unsteady = 3, Unstable = 4 };
struct PushAutoSignals {
    double sourceFps = 0;                   // unique pictures per second as measured; 0 = not measured
    double displayHz = 0;
    bool gpuUtilizationKnown = false;
    double gpuUtilizationPercent = 0;       // 0..100
    bool generationOverBudget = false;
    unsigned queueDepth = 0;                // pictures waiting (the stream's ingress queue; 0 for local sources)
    PushAutoNetwork network = PushAutoNetwork::Local;
};
inline PushAutoInputs pushAutoInputs(const PushAutoSignals& seen, std::int64_t now100ns) {
    PushAutoInputs in;
    in.now100ns = now100ns;
    in.sourceFps = seen.sourceFps;
    in.displayHz = seen.displayHz;
    in.queueDepth = seen.queueDepth;
    if (seen.generationOverBudget) {
        in.gpuKnown = true;
        in.gpuBusyShare = 1.0;
    } else if (seen.gpuUtilizationKnown && std::isfinite(seen.gpuUtilizationPercent)) {
        const double share = seen.gpuUtilizationPercent / 100.0;
        in.gpuKnown = true;
        in.gpuBusyShare = share < 0 ? 0 : share > 1 ? 1 : share;
    } else {
        in.gpuKnown = false;
        in.gpuBusyShare = 0;
    }
    switch (seen.network) {
    case PushAutoNetwork::Local:
    case PushAutoNetwork::Stable: in.network = PushNetworkHealth::Healthy; break;
    case PushAutoNetwork::Unstable: in.network = PushNetworkHealth::Poor; break;
    case PushAutoNetwork::Unknown:
    case PushAutoNetwork::Unsteady: in.network = PushNetworkHealth::Degraded; break;
    }
    return in;
}
} // namespace veyra::engine
