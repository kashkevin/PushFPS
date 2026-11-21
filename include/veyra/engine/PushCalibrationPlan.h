// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushGpuBudget.h"
#include "veyra/engine/PushHardwareCapabilities.h"
#include "veyra/engine/PushPerformanceMode.h"
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace veyra::engine {
// First-launch recommendation ("Recommended settings", qml/Veyra/PushCalibrationCard.qml): from what the PC
// reports (PushHardwareCapabilities), the display, the source (PS5 Remote Play: a 60 fps stream carrying a
// 30 or 60 fps game, or a game rate nobody told us) and, when the user ran the short calibration, measured GPU
// times, one configuration with a reason per choice and the list of what could not be determined.
//
// Portable and header-only. Tests: tests/remoteplay/CalibrationPlanTests.inc. Nothing here changes a setting:
// the interface shows the result and applies it only when the user presses Apply. Unverified on Windows; the
// thresholds are PushGpuBudgetConfig's starting points (estimates), never measured on a real GPU.
//
// Rules:
//   - Responsiveness first. The recommended mode is Ultra Low Latency (no frame generation) unless every
//     condition for generation is known and met; then Competitive (2X of a 60 fps game, the least delay any
//     generation adds: half a source interval). A 30 fps game is never given generation by this
//     recommendation: every multiplier there holds each real picture at least 16.7 ms. The smoother choice is
//     reported separately (smoother*) for the details view and is never what Apply sets.
//   - Unknown is never treated as present: an unknown game rate, refresh rate, GPU, power source or an
//     unmeasured GPU cost turns generation off; an unknown decoder means H.264; unknown HDR means SDR.
//   - A slower or unknown GPU gets less, never more: a larger measured cost can only turn generation off.
//   - Output must fit the refresh rate and, without variable refresh the driver reports as possible or
//     active (state 4; 3 = possible is not enough), divide it evenly; otherwise generation stays off (off always fits).
//   - Enhancement (AI picture enhancement, super resolution) is off: it costs GPU time on every real frame.

enum class PushGameRate : std::uint8_t { Unknown = 0, Thirty = 30, Sixty = 60 };

// Counters the bridge reads twice (start and end of a calibration phase); the same meanings as PushFeedInput.
struct PushCalibrationCounters {
    std::uint64_t presented = 0, expired = 0, pairsCandidate = 0, pairsSkipped = 0;
};

// What the calibration measured. Negative = not measured.
struct PushCalibrationMeasurement {
    bool valid = false;
    double baseGpuMs = -1;          // P95 GPU time per source frame, frame generation off (presentation alone)
    double totalGpuMs = -1;         // P95 GPU time per source frame with generation on (batch included)
    double fgBatchGpuMs = -1;       // P95 GPU time of one generation batch
    double missedShare = -1;        // generated outputs that expired after evaluation / all outputs
    double skippedPairShare = -1;   // pairs the per-pair admission skipped / pairs seen
    unsigned multiplier = 0;        // the multiplier the generation phase ran at
    PushFgMethod method = PushFgMethod::PushFlow;
    bool methodKnown = false;
};

// A generation phase's counters into shares. Counters that went backwards (another source started) or
// saw nothing leave the share unmeasured.
inline PushCalibrationMeasurement pushCalibrationMeasurement(double baseGpuMs, double totalGpuMs, double fgBatchGpuMs,
                                                             const PushCalibrationCounters& start, const PushCalibrationCounters& end,
                                                             unsigned multiplier, PushFgMethod method, bool methodKnown) {
    PushCalibrationMeasurement m;
    const auto known = [](double v) { return std::isfinite(v) && v >= 0; };
    m.baseGpuMs = known(baseGpuMs) ? baseGpuMs : -1;
    m.totalGpuMs = known(totalGpuMs) ? totalGpuMs : -1;
    m.fgBatchGpuMs = known(fgBatchGpuMs) ? fgBatchGpuMs : -1;
    m.multiplier = multiplier;
    m.method = method;
    m.methodKnown = methodKnown;
    const bool forward = end.presented >= start.presented && end.expired >= start.expired && end.pairsCandidate >= start.pairsCandidate &&
                         end.pairsSkipped >= start.pairsSkipped;
    if (forward) {
        const std::uint64_t presented = end.presented - start.presented, expired = end.expired - start.expired;
        const std::uint64_t candidates = end.pairsCandidate - start.pairsCandidate, skipped = end.pairsSkipped - start.pairsSkipped;
        if (presented + expired > 0) m.missedShare = static_cast<double>(expired) / static_cast<double>(presented + expired);
        if (candidates > 0 && skipped <= candidates) m.skippedPairShare = static_cast<double>(skipped) / static_cast<double>(candidates);
    }
    // Zero is not "fast": a reading of 0 means the stage was not timed (the enhancement time excludes the
    // colour and blit passes, so with everything off it can read 0). Unverified on Windows which paths time it.
    m.valid = m.baseGpuMs > 0 && m.totalGpuMs > 0 && m.fgBatchGpuMs > 0 && m.missedShare >= 0 && m.skippedPairShare >= 0 && multiplier >= 2 && methodKnown;
    return m;
}

struct PushCalibrationInput {
    PushHardwareCapabilities caps;
    double streamFps = 60;                         // the PS5 Remote Play stream
    PushGameRate game = PushGameRate::Unknown;     // the game's own rate inside it
    PushFgMethod preferredMethod = PushFgMethod::PushFlow;   // the method set now; the fallback chain applies
    PushCalibrationMeasurement measured;
};

// Machine-readable reasons (stable; the interface turns them into text, the log keeps them):
//   modeReason: lowest-latency | generation-fits
//   fgReason:   generation-fits | game-rate-unknown | game-thirty | display-unknown | source-fills-display |
//               output-above-refresh | uneven-without-vrr | gpu-unknown | power-unknown | on-battery |
//               integrated-gpu | gpu-memory-unknown | <a pushHardwareDefaults fg-* reason> | not-measured | measured-other-method |
//               gpu-load-high | generated-frame-cost-high | missed-deadlines | skipped-pairs
//   codecReason: hevc-decode | hevc-decode-missing | hevc-decode-unknown | stream-h264
//   hdrReason:   hdr-on | hdr-display-off | hdr-display-unknown | hdr-needs-hevc | hdr-needs-hevc10-decode
//   enhancementReason: enhancement-costs-gpu-time
// unknown (what could not be determined): game-rate, display-refresh, variable-refresh, gpu, gpu-memory,
//   video-decode, hdr-display, power-source, gpu-cost.
struct PushCalibrationPlan {
    PushPerformanceMode mode = PushPerformanceMode::UltraLowLatency;
    bool frameGeneration = false;
    PushFgMethod fgMethod = PushFgMethod::PushFlow;
    bool fgMethodKnown = false;      // a method exists on this PC (also when generation is off)
    unsigned multiplier = 1;
    unsigned tier = 0;               // always the lowest: the cheapest generation
    double outputFps = 0;            // game rate x multiplier; 0 when the game rate is unknown
    double minAddedDelayMs = 0;      // (M-1)/M of a game frame interval, arithmetic
    bool enhancement = false;
    PushStreamCodec codec = PushStreamCodec::H264;
    bool hdr = false;
    std::string_view modeReason = "lowest-latency";
    std::string_view fgReason = "game-rate-unknown";
    std::string_view codecReason = "hevc-decode-unknown";
    std::string_view hdrReason = "hdr-display-unknown";
    std::string_view enhancementReason = "enhancement-costs-gpu-time";
    PushReasons unknown;
    // The smoother choice for the details view (Balanced for this game and display), never applied by Apply.
    bool smootherOffered = false;
    unsigned smootherMultiplier = 1;
    double smootherOutputFps = 0;
    double smootherDelayMs = 0;
};

inline PushCalibrationPlan pushCalibrationPlan(const PushCalibrationInput& in) {
    PushCalibrationPlan out;
    const PushHardwareCapabilities& caps = in.caps;
    const bool refreshKnown = std::isfinite(caps.refreshHz) && caps.refreshHz > 0;
    const bool gameKnown = in.game == PushGameRate::Thirty || in.game == PushGameRate::Sixty;
    const double gameFps = gameKnown ? static_cast<double>(static_cast<unsigned>(in.game)) : 0.0;

    if (!gameKnown) out.unknown.add("game-rate");
    if (!refreshKnown) out.unknown.add("display-refresh");
    if (caps.vrr == PushVrrState::Unknown) out.unknown.add("variable-refresh");
    if (caps.vendor == PushGpuVendor::Unknown) out.unknown.add("gpu");
    if (!caps.vramKnown() && caps.uma != PushCapability::Yes) out.unknown.add("gpu-memory");
    if (caps.decodeHevc == PushCapability::Unknown) out.unknown.add("video-decode");
    if (caps.hdrDisplay == PushCapability::Unknown) out.unknown.add("hdr-display");
    if (caps.power == PushPowerSource::Unknown) out.unknown.add("power-source");
    if (!in.measured.valid) out.unknown.add("gpu-cost");

    // Codec and HDR: the capability model's own rules (HEVC only with a listed hardware decoder; HDR only with
    // the 10-bit decoder and a display already in HDR mode).
    PushHardwareRequest stream;
    stream.codec = PushStreamCodec::Hevc;
    stream.hdr = true;
    stream.frameGeneration = false;
    const PushHardwareDefaults streamDefaults = pushHardwareDefaults(caps, stream);
    out.codec = streamDefaults.codec;
    out.codecReason = out.codec == PushStreamCodec::Hevc ? std::string_view("hevc-decode")
                      : streamDefaults.reasons.has("hevc-decode-missing") ? std::string_view("hevc-decode-missing")
                                                                           : std::string_view("hevc-decode-unknown");
    out.hdr = streamDefaults.hdr;
    if (out.hdr) out.hdrReason = "hdr-on";
    else if (caps.hdrDisplay == PushCapability::No) out.hdrReason = "hdr-display-off";
    else if (caps.hdrDisplay == PushCapability::Unknown) out.hdrReason = "hdr-display-unknown";
    else if (streamDefaults.reasons.has("hdr-needs-hevc")) out.hdrReason = "hdr-needs-hevc";
    else out.hdrReason = "hdr-needs-hevc10-decode";

    // Which method generation would use (also reported when it stays off).
    PushFgMethod method = in.preferredMethod;
    out.fgMethodKnown = pushNextFgMethod(caps, in.preferredMethod, method);
    out.fgMethod = method;

    // The smoother choice (details only): Balanced for this game rate on this display.
    if (gameKnown && refreshKnown && out.fgMethodKnown) {
        const PushModePlan balanced = pushModePlan(PushPerformanceMode::Balanced, gameFps, caps.refreshHz);
        unsigned m = 0;
        for (unsigned k = balanced.valid ? balanced.multiplier : 0u; k >= 2; --k)
            if (pushFgMultiplierSelectable(method, k)) { m = k; break; }
        if (m >= 2) {
            out.smootherOffered = true;
            out.smootherMultiplier = m;
            out.smootherOutputFps = gameFps * static_cast<double>(m);
            out.smootherDelayMs = pushInterpolationDelayMs(m, gameFps);
        }
    }

    if (gameKnown) out.outputFps = gameFps;
    const auto off = [&out](std::string_view reason) {
        out.fgReason = reason;
        return out;
    };
    // Generation: every condition known and met, in the order a user can act on.
    if (!gameKnown) return off("game-rate-unknown");
    if (!refreshKnown) return off("display-unknown");
    // Competitive's row: no generation for a 30 fps game.
    const PushModePlan competitive = pushModePlan(PushPerformanceMode::Competitive, gameFps, caps.refreshHz);
    if (in.game == PushGameRate::Thirty) return off("game-thirty");
    if (!competitive.valid || competitive.multiplier < 2) {
        const PushOutputTargets any = pushValidOutputTargets(method, gameFps, caps.refreshHz);
        return off(any.sourceFillsDisplay ? "source-fills-display" : "output-above-refresh");
    }
    const unsigned wanted = competitive.multiplier;
    const double outputFps = gameFps * static_cast<double>(wanted);
    // Only variable refresh the driver reports as active counts; "possible" (not confirmed for this window)
    // gets evenly dividing targets only.
    const bool vrrUsable = caps.vrr == PushVrrState::Active;
    if (!vrrUsable && !pushOutputEvenWithoutVrr(outputFps, caps.refreshHz)) return off("uneven-without-vrr");
    if (caps.vendor == PushGpuVendor::Unknown) return off("gpu-unknown");
    if (caps.power == PushPowerSource::Unknown) return off("power-unknown");
    if (caps.power == PushPowerSource::Battery) return off("on-battery");
    if (caps.uma == PushCapability::Yes) return off("integrated-gpu");
    // A dedicated GPU whose memory was reported: anything less (a software adapter, an adapter that did not
    // answer) is not counted on.
    if (caps.uma != PushCapability::No || !caps.vramKnown()) return off("gpu-memory-unknown");
    PushHardwareRequest fg;
    fg.codec = out.codec;
    fg.frameGeneration = true;
    fg.fgMethod = in.preferredMethod;
    fg.multiplier = wanted;
    fg.tier = 0;
    fg.sourceFps = gameFps;
    const PushHardwareDefaults fgDefaults = pushHardwareDefaults(caps, fg);
    if (!fgDefaults.frameGeneration || fgDefaults.multiplier != wanted) {
        for (const std::string_view reason : {std::string_view("fg-gpu-unknown"), std::string_view("fg-feature-level"), std::string_view("fg-vram-low"),
                                              std::string_view("fg-no-method"), std::string_view("fg-source-fills-display"), std::string_view("fg-display-limit"),
                                              std::string_view("fg-multiplier-unsupported")})
            if (fgDefaults.reasons.has(reason)) return off(reason);
        return off("fg-no-method");
    }
    // Measured on this PC, with this method, at this multiplier; the GPU budget's thresholds.
    const PushCalibrationMeasurement& m = in.measured;
    if (!m.valid || m.multiplier != wanted) return off("not-measured");
    if (m.method != fgDefaults.fgMethod) return off("measured-other-method");
    const PushGpuBudgetConfig budget;
    const double sourceIntervalMs = 1000.0 / gameFps;
    const double outputIntervalMs = 1000.0 / outputFps;
    // The larger of the two readings: a slower number anywhere can only raise the load.
    const double withBatch = m.baseGpuMs + m.fgBatchGpuMs;
    const double load = (m.totalGpuMs > withBatch ? m.totalGpuMs : withBatch) / sourceIntervalMs;
    if (!(load <= budget.loadLow)) return off("gpu-load-high");
    if (!(m.fgBatchGpuMs / static_cast<double>(wanted - 1) <= outputIntervalMs * budget.loadLow)) return off("generated-frame-cost-high");
    if (!(m.missedShare < budget.missedHigh)) return off("missed-deadlines");
    if (!(m.skippedPairShare < budget.skippedHigh)) return off("skipped-pairs");

    out.mode = PushPerformanceMode::Competitive;
    out.modeReason = "generation-fits";
    out.frameGeneration = true;
    out.fgMethod = fgDefaults.fgMethod;
    out.multiplier = wanted;
    out.tier = 0;
    out.outputFps = outputFps;
    out.minAddedDelayMs = pushInterpolationDelayMs(wanted, gameFps);
    out.fgReason = "generation-fits";
    return out;
}
// --- the interface's mode -> the experiment switches the settings-file modes set ---------------------------
//
// A mode chosen in slipstream.ini sets pushModeSwitches() in the environment at start-up. A mode chosen in the
// interface (PushModes.js select(), Automatic in PushModeRow.qml) asks for the same ones, and Automatic also for
// connection health (VEYRA_EXP_RP_HEALTH=1: without it the controller sees no network grade on a PS5 stream and
// never steps back up). Every reader looks them up when a stream starts (pad poll when the controller timer
// starts, the pair phase when the engine starts a source, health when a PS5 session starts), so a change counts
// from the next connection. The bridge (pushCalibration("switches"), PushSlipstream.cpp) owns only what it set
// itself: a switch already in the environment (the user's, or slipstream.ini's) is never touched, and a switch it
// set is removed again when no mode wants it. The LAN profile has its own preference and is not managed here.
inline constexpr std::array<std::string_view, 3> kPushInterfaceSwitchNames{"VEYRA_EXP_RP_PAD_POLL_MS", "VEYRA_EXP_RP_ADAPTIVE_PHASE", "VEYRA_EXP_RP_HEALTH"};

// What the interface asks for: no mode = nothing; Automatic (only meaningful while VEYRA_EXP_AUTO_MODE=1) = the
// Automatic switches plus health; else the mode's own.
inline PushModeSwitches pushInterfaceSwitches(std::optional<PushPerformanceMode> mode, bool automatic) {
    PushModeSwitches list;
    if (!mode || !pushModeIsConcrete(*mode)) return list;
    const PushModeSwitches own = pushModeSwitches(automatic ? PushPerformanceMode::Automatic : *mode, false);
    for (const PushModeSwitch& item : own) list.add(item.name, item.value);
    if (automatic) list.add("VEYRA_EXP_RP_HEALTH", "1");
    return list;
}

enum class PushSwitchAction : std::uint8_t { None = 0, Set = 1, Unset = 2, UserOwned = 3 };
struct PushSwitchStep {
    std::string_view name;
    std::string_view value;          // for Set
    PushSwitchAction action = PushSwitchAction::None;
};
// ours[i] / present[i]: for kPushInterfaceSwitchNames[i], whether the bridge set it itself, and whether it is
// in the environment now. One step per managed name, in that order.
inline std::array<PushSwitchStep, kPushInterfaceSwitchNames.size()> pushInterfaceSwitchSteps(const PushModeSwitches& wanted,
    const std::array<bool, kPushInterfaceSwitchNames.size()>& ours, const std::array<bool, kPushInterfaceSwitchNames.size()>& present) {
    std::array<PushSwitchStep, kPushInterfaceSwitchNames.size()> steps{};
    for (std::size_t i = 0; i < kPushInterfaceSwitchNames.size(); ++i) {
        PushSwitchStep& step = steps[i];
        step.name = kPushInterfaceSwitchNames[i];
        const PushModeSwitch* want = nullptr;
        for (const PushModeSwitch& item : wanted)
            if (item.name == step.name) want = &item;
        if (!ours[i] && present[i]) { step.action = want ? PushSwitchAction::UserOwned : PushSwitchAction::None; continue; }
        if (want) {
            step.value = want->value;
            step.action = ours[i] && present[i] ? PushSwitchAction::None : PushSwitchAction::Set;
            continue;
        }
        step.action = ours[i] ? PushSwitchAction::Unset : PushSwitchAction::None;
    }
    return steps;
}
} // namespace veyra::engine
