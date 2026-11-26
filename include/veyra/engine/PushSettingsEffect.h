// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "s12-settings": requested / applied / executing for every frame-generation setting (SETTINGS_AUDIT.md).
// The engine fills PushEffectFacts on its own thread from values it already has (the per-pair multiplier at each
// rule, the duplicate and pacing counters, the HUD lock state, the presenter's sync facts) and publishes them in the
// snapshot; pushSettingsEffect() turns them into one row per setting with the reason why what runs differs from what
// was asked. The same rows feed the log line [settings-effect] (pushSettingsEffectLine), the interface's grey status
// lines (PushEffectNote.qml) and the "running now" block, so the interface can never say more than the engine saw.
// Pure: no Windows, no Qt, no clocks; evidence only (nothing here changes how frames are made, timed or shown).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace veyra::engine {

// Why a setting's executing state is what it is. Names are stable log / interface ids (pushEffectWhyName).
enum class PushEffectWhy : std::uint8_t {
    Running = 0,          // asked and running
    Off,                  // switched off
    NotPlaying,           // nothing plays: nothing can run yet
    OwnMotion,            // the frame generator estimates motion itself (NVIDIA VFG, AMD FRC, PushFlow): flow settings do nothing
    NoFlowConsumer,       // no stage of the running pipeline reads optical flow
    OtherMethod,          // belongs to another frame-generation method
    FilePlaybackOnly,     // only file playback reads it; live sources never do
    NothingToRepair,      // pacing repair ran and re-timed nothing (frames arrive evenly)
    ContentClock,         // duplicate removal ran, found no repeated picture, and still times frames by the game's clock
    NothingToChoose,      // adaptive multiplier: the ceiling is already the closest to the target
    LimitedBySmoothness,  // Smoothness Mode chose a lower multiplier
    LimitedByCap,         // the output cap chose a lower multiplier
    LimitedByAdaptive,    // the adaptive multiplier chose a lower multiplier
    AlreadyByVsync,       // display sync Vsync keeps a one-frame queue anyway
    NeedsAutomaticSync,   // the variable refresh switch is read only with display sync Automatic
    DriverReportsOff,     // NVIDIA low latency: asked, the driver reports it off for this window
    Needs2X,              // VFG auto regulation: live 2X only
    NeedsSingleModel,     // VFG auto regulation: mixed quality levels never load the faster model
    NoFasterModel,        // VFG auto regulation: Low has no faster model
    NeedsDedupOff,        // VFG auto regulation: not with duplicate removal on
    NeedsDedupOn,         // pacing repair: works on the frames duplicate removal keeps
    NotLoaded,            // VFG auto regulation: the faster model did not load (video memory)
    Unavailable,          // asked, could not start on this PC (HUD lock pass failed)
    WorksAtLowerSize,     // AI enhancement: its model works at a smaller size inside
    Unknown,              // asked; nothing in the last window measured whether it ran (never shown as running)
    PairsSkipped,         // frame generation runs but fewer generated frames per real frame were shown than the multiplier gives
    NeedsAbove2X,         // adaptive multiplier: a ceiling of 2X leaves nothing to choose
    DriverVrrOff,         // variable refresh: asked; the driver last reported VRR not active on this display
    NotMeasurable,        // on; PushFPS has no counter for it (strict cadence, the low queue without Vsync): never "running"
    Count
};

constexpr std::string_view pushEffectWhyName(PushEffectWhy why) {
    switch (why) {
    case PushEffectWhy::Running: return "running";
    case PushEffectWhy::Off: return "off";
    case PushEffectWhy::NotPlaying: return "not-playing";
    case PushEffectWhy::OwnMotion: return "own-motion";
    case PushEffectWhy::NoFlowConsumer: return "no-flow-consumer";
    case PushEffectWhy::OtherMethod: return "other-method";
    case PushEffectWhy::FilePlaybackOnly: return "file-playback-only";
    case PushEffectWhy::NothingToRepair: return "nothing-to-repair";
    case PushEffectWhy::ContentClock: return "content-clock";
    case PushEffectWhy::NothingToChoose: return "nothing-to-choose";
    case PushEffectWhy::LimitedBySmoothness: return "limited-by-smoothness";
    case PushEffectWhy::LimitedByCap: return "limited-by-cap";
    case PushEffectWhy::LimitedByAdaptive: return "limited-by-adaptive";
    case PushEffectWhy::AlreadyByVsync: return "already-by-vsync";
    case PushEffectWhy::NeedsAutomaticSync: return "needs-automatic-sync";
    case PushEffectWhy::DriverReportsOff: return "driver-reports-off";
    case PushEffectWhy::Needs2X: return "needs-2x";
    case PushEffectWhy::NeedsSingleModel: return "needs-single-model";
    case PushEffectWhy::NoFasterModel: return "no-faster-model";
    case PushEffectWhy::NeedsDedupOff: return "needs-dedup-off";
    case PushEffectWhy::NeedsDedupOn: return "needs-dedup-on";
    case PushEffectWhy::NotLoaded: return "not-loaded";
    case PushEffectWhy::Unavailable: return "unavailable";
    case PushEffectWhy::WorksAtLowerSize: return "works-at-lower-size";
    case PushEffectWhy::Unknown: return "unknown";
    case PushEffectWhy::PairsSkipped: return "pairs-skipped";
    case PushEffectWhy::NeedsAbove2X: return "needs-above-2x";
    case PushEffectWhy::DriverVrrOff: return "driver-vrr-off";
    case PushEffectWhy::NotMeasurable: return "not-measurable";
    case PushEffectWhy::Count: break;
    }
    return "?";
}

// Whether the setting is doing what it was asked to do (for the interface's colour: grey when false).
constexpr bool pushEffectWhyEffective(PushEffectWhy why) {
    return why == PushEffectWhy::Running || why == PushEffectWhy::WorksAtLowerSize || why == PushEffectWhy::ContentClock;
}

enum class PushEffectSetting : std::uint8_t {
    Multiplier = 0, Adaptive, PacingRepair, Dedup, HudLock, VfgQuality, VfgAutoRegulate, StrictCadence, FlowGroup,
    EdgeAegis, NvidiaLowLatency, LowQueue, Vrr, Enhancement, Count
};
inline constexpr std::size_t kPushEffectSettings = std::size_t(PushEffectSetting::Count);

constexpr std::string_view pushEffectSettingName(PushEffectSetting s) {
    switch (s) {
    case PushEffectSetting::Multiplier: return "multiplier";
    case PushEffectSetting::Adaptive: return "adaptive";
    case PushEffectSetting::PacingRepair: return "pacingRepair";
    case PushEffectSetting::Dedup: return "dedup";
    case PushEffectSetting::HudLock: return "hudLock";
    case PushEffectSetting::VfgQuality: return "vfgQuality";
    case PushEffectSetting::VfgAutoRegulate: return "vfgAutoRegulate";
    case PushEffectSetting::StrictCadence: return "strictCadence";
    case PushEffectSetting::FlowGroup: return "flow";
    case PushEffectSetting::EdgeAegis: return "edgeAegis";
    case PushEffectSetting::NvidiaLowLatency: return "nvidiaLowLatency";
    case PushEffectSetting::LowQueue: return "lowQueue";
    case PushEffectSetting::Vrr: return "vrr";
    case PushEffectSetting::Enhancement: return "enhancement";
    case PushEffectSetting::Count: break;
    }
    return "?";
}

// The VFG model per generated-frame position (EnhancementSettings.h vfgPrimaryMode / vfgSecondaryMode /
// vfgPrimarySubframe, repeated here so this header stays free of the settings header): 0 Low, 1 Medium, 2 High.
constexpr unsigned pushVfgModelAt(unsigned quality, unsigned multiplier, unsigned subframe) {
    const unsigned primary = quality < 3 ? quality : quality == 5 ? 1u : 2u;
    const unsigned secondary = quality == 3 ? 1u : (quality == 4 || quality == 5) ? 0u : primary;
    return subframe == (multiplier + 1) / 2 ? primary : secondary;
}
constexpr char pushVfgModelLetter(unsigned mode) { return mode == 0 ? 'L' : mode == 1 ? 'M' : mode == 2 ? 'H' : '?'; }
// Package 15/16 "Lower delay" at 3X (review 2 MUST 1): ONE VFG frame per pair, interpolated at FrameIndex 2 with the primary
// model (Fg1 times it), then the predicted frame P, which is no VFG model (Fg2 times it). 'P' marks that position.
constexpr char pushVfgPositionLetter(unsigned quality, unsigned multiplier, unsigned subframe, bool lowDelay) {
    if (lowDelay && multiplier == 3) return subframe == 1 ? pushVfgModelLetter(pushVfgModelAt(quality, 3, 2)) : 'P';
    return pushVfgModelLetter(pushVfgModelAt(quality, multiplier, subframe));
}

// What the engine saw. Window counters are deltas over the last evidence window (about 10 s).
struct PushEffectFacts {
    // Configuration and applied state (what was asked and what the engine accepted).
    bool known = false;               // filled by a running session at least once
    std::uint64_t revision = 0;       // the settings revision these facts were measured with
    bool playing = false;             // a source plays and the engine processes pairs
    bool liveSource = false;          // capture card, stream or screen (not a file)
    bool fgOn = false;                // frame generation in the running settings
    bool presentSinkFg = false;       // XeSS: the provider owns generation and pacing
    bool vfg = false;                 // NVIDIA VFG is the running method
    bool ownMotion = false;           // the running method estimates motion itself (VFG, AMD FRC, PushFlow)
    bool fgUsesFlow = false;          // the running method reads optical flow (motionUsesFlow)
    bool lowDelayActive = false;      // the Lower delay schedule ran pairs in the window (its wait is one slot less)
    bool otherFlowConsumer = false;   // not playing: AI enhancement or an upscaler is on, so a flow session may run
    unsigned requestedMultiplier = 1; // the user's multiplier (ceiling)
    unsigned afterAdaptive = 1;       // after the adaptive rule (the engine's per-pair choice: measured)
    unsigned afterCap = 1;            // after the output-cap rule
    unsigned executingMultiplier = 1; // what the last pair was asked to use (after Smoothness Mode)
    bool adaptiveOn = false;
    bool smoothnessOn = false;        // a Smoothness Mode target is set
    double smoothTargetFps = 0;
    double capFps = 0;                // the output cap in force, 0 = none
    double contentFps = 0;
    bool dedupOn = false;
    bool pacingRepairOn = false;
    int hudLockState = 0;             // EnhanceGraph::hudLockState: 0 off, 1 pass exists, 2 failed, 3 not applicable
    unsigned vfgQuality = 1;
    bool vfgAutoRegulateOn = false, vfgFastLoaded = false;
    bool strictOn = false;
    bool edgeAegisOn = false;
    bool nvidiaLowLatencyOn = false;
    bool lowQueueOn = false;
    bool vsyncPaced = false;          // display sync Vsync keeps the one-frame queue (displayQueuePaced)
    bool vrrOn = false;               // the variable refresh switch
    bool displayAutomatic = false;    // display sync Automatic
    bool vrrSync = false;             // the presenter treats Automatic as VRR + Vsync (its own flag, not the driver's)
    bool enhanceOn = false;           // AI enhancement layers in the running settings
    unsigned enhanceWidth = 0, enhanceHeight = 0, outputWidth = 0, outputHeight = 0;
    // Measured in the last window (deltas of session counters, GPU timestamps, the driver's answers).
    std::uint64_t realPresented = 0, generatedPresented = 0;
    std::uint64_t dedupObserved = 0, dedupRepeats = 0;
    std::uint64_t pacingFrames = 0, pacingRepaired = 0;
    std::uint64_t hudLockComposes = 0;       // HUD lock compose dispatches recorded (CPU counter, no readback)
    std::uint64_t vfgFastPairs = 0;          // pairs that ran VFG's faster model (auto regulation)
    std::array<double, 4> vfgSubframeMs{};   // GPU time per generated-frame position (Fg1..Fg4 stages, P95 of the stage's recent samples, ms)
    std::array<std::uint64_t, 4> vfgSubframeSamples{};   // GPU samples of those stages IN THIS WINDOW (review 2 SHOULD 5: the evidence)
    double flowGpuMs = 0;                    // optical-flow stage GPU P95 of recent samples
    double enhanceGpuMs = 0;                 // AI enhancement (NR) stage GPU P95 of recent samples
    std::uint64_t flowSamples = 0, enhanceSamples = 0;   // their GPU samples in this window
    std::uint64_t expectedGenerated = 0;     // sum of (multiplier - 1) over the pairs admitted in this window
    int driverLowLatency = -1;               // NvAPI_D3D_GetSleepStatus: 1 on, 0 off, -1 not read
    int driverVrr = -1;                      // the driver's VRR state for the window's display: 1 active, 0 not, -1 unknown
};

constexpr double pushGeneratedPerRealOf(std::uint64_t real, std::uint64_t generated) {
    return real ? double(generated) / double(real) : -1.0;
}

struct PushEffectRow {
    PushEffectWhy why = PushEffectWhy::Off;
    bool requested = false;   // switched on / chosen by the user
    bool executing = false;   // doing its work in the running pipeline in the last window
};
using PushEffectRows = std::array<PushEffectRow, kPushEffectSettings>;

inline PushEffectRows pushSettingsEffect(const PushEffectFacts& f) {
    // Review 1 MUST 2: "executing" is MEASURED in the last window (frames presented, GPU time, a dispatch counter, the
    // driver's answer). Where nothing measures a setting the reason is Unknown, never Running.
    PushEffectRows r{};
    const auto set = [&r](PushEffectSetting s, bool requested, PushEffectWhy why, bool measured) {
        r[std::size_t(s)] = PushEffectRow{why, requested, requested && measured};
    };
    const bool fg = f.fgOn && f.requestedMultiplier > 1;
    const bool generating = fg && f.playing && f.generatedPresented > 0;
    {
        // The rules run in this order per pair (EngineController.cpp): adaptive OR output cap, then Smoothness Mode.
        PushEffectWhy why = PushEffectWhy::Running;
        const bool lower = f.executingMultiplier < f.requestedMultiplier;
        const double expected = double(f.executingMultiplier) - 1.0;
        if (!fg) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (lower && f.executingMultiplier < f.afterCap && f.executingMultiplier < f.afterAdaptive) why = PushEffectWhy::LimitedBySmoothness;
        else if (lower && f.afterCap < f.afterAdaptive) why = PushEffectWhy::LimitedByCap;
        else if (lower && f.afterAdaptive < f.requestedMultiplier) why = PushEffectWhy::LimitedByAdaptive;
        else if (f.generatedPresented == 0) why = PushEffectWhy::Unknown;
        else if (f.realPresented >= 30 && (f.expectedGenerated > 0 ? double(f.generatedPresented) < 0.97 * double(f.expectedGenerated)
                                                                   : pushGeneratedPerRealOf(f.realPresented, f.generatedPresented) < expected - 0.1))
            why = PushEffectWhy::PairsSkipped;
        set(PushEffectSetting::Multiplier, fg, why, generating);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.adaptiveOn || !fg) why = PushEffectWhy::Off;
        else if (f.presentSinkFg) why = PushEffectWhy::OtherMethod;
        else if (f.requestedMultiplier <= 2) why = PushEffectWhy::NeedsAbove2X;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.afterAdaptive >= f.requestedMultiplier) why = PushEffectWhy::NothingToChoose;
        else if (f.executingMultiplier < f.afterAdaptive) why = PushEffectWhy::LimitedBySmoothness;
        // Measured: the engine's own per-pair choice lowered the multiplier.
        set(PushEffectSetting::Adaptive, f.adaptiveOn, why, fg && f.playing && f.afterAdaptive < f.requestedMultiplier);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.pacingRepairOn || !fg) why = PushEffectWhy::Off;
        else if (f.presentSinkFg) why = PushEffectWhy::OtherMethod;
        else if (!f.dedupOn) why = PushEffectWhy::NeedsDedupOn;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.pacingFrames == 0 || !(f.contentFps > 0)) why = PushEffectWhy::Unknown;
        else if (f.pacingRepaired == 0) why = PushEffectWhy::NothingToRepair;
        set(PushEffectSetting::PacingRepair, f.pacingRepairOn, why, f.pacingRepaired > 0);
    }
    {
        // Review 1 MUST 3: with no repeat to remove it still runs the schedule on the game's clock (LOG: on-grid 99.9 %).
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.dedupOn) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.dedupObserved == 0) why = PushEffectWhy::Unknown;
        else if (f.dedupRepeats == 0) why = PushEffectWhy::ContentClock;
        set(PushEffectSetting::Dedup, f.dedupOn, why, f.playing && f.dedupObserved > 0);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        const bool asked = f.hudLockState != 0;
        if (!asked || !fg) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.hudLockState == 2) why = PushEffectWhy::Unavailable;
        else if (f.hudLockState == 3) why = f.presentSinkFg ? PushEffectWhy::OtherMethod : PushEffectWhy::Off;
        else if (f.hudLockComposes == 0) why = PushEffectWhy::Unknown;
        set(PushEffectSetting::HudLock, asked, why, fg && f.playing && f.hudLockComposes > 0);
    }
    {
        bool timed = f.executingMultiplier >= 2;
        for (unsigned sub = 1; sub < f.executingMultiplier && sub <= f.vfgSubframeSamples.size(); ++sub) timed = timed && f.vfgSubframeSamples[sub - 1] > 0;
        PushEffectWhy why = PushEffectWhy::Running;
        if (!fg) why = PushEffectWhy::Off;
        else if (!f.vfg) why = PushEffectWhy::OtherMethod;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (!timed || f.generatedPresented == 0) why = PushEffectWhy::Unknown;
        set(PushEffectSetting::VfgQuality, fg && f.vfg, why, generating && f.vfg && timed);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.vfgAutoRegulateOn || !fg) why = PushEffectWhy::Off;
        else if (!f.vfg) why = PushEffectWhy::OtherMethod;
        else if (f.requestedMultiplier != 2) why = PushEffectWhy::Needs2X;
        else if (f.vfgQuality >= 3) why = PushEffectWhy::NeedsSingleModel;
        else if (f.vfgQuality == 0) why = PushEffectWhy::NoFasterModel;
        else if (f.dedupOn) why = PushEffectWhy::NeedsDedupOff;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (!f.vfgFastLoaded) why = PushEffectWhy::NotLoaded;
        else if (f.vfgFastPairs == 0) why = PushEffectWhy::Unknown;
        set(PushEffectSetting::VfgAutoRegulate, f.vfgAutoRegulateOn, why, f.vfgFastPairs > 0);
    }
    {
        PushEffectWhy why = PushEffectWhy::NotMeasurable;   // file playback: no counter says how many groups it refused
        if (!f.strictOn || !fg) why = PushEffectWhy::Off;
        else if (f.liveSource) why = PushEffectWhy::FilePlaybackOnly;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        set(PushEffectSetting::StrictCadence, f.strictOn, why, false);
    }
    {
        // The flow group has no switch: "requested" = the stored choices exist; executing = the flow stage took GPU time.
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.playing) why = fg && f.ownMotion && !f.otherFlowConsumer ? PushEffectWhy::OwnMotion : PushEffectWhy::NotPlaying;
        else if (f.flowSamples > 0) why = PushEffectWhy::Running;
        else if (fg && f.ownMotion) why = PushEffectWhy::OwnMotion;
        else why = PushEffectWhy::NoFlowConsumer;
        set(PushEffectSetting::FlowGroup, true, why, f.playing && f.flowSamples > 0);
    }
    {
        // Edge Aegis refines the flow frame generation reads: never VFG / AMD FRC / PushFlow, even when flow runs for NR.
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.edgeAegisOn || !fg) why = PushEffectWhy::Off;
        else if (f.ownMotion) why = PushEffectWhy::OwnMotion;
        else if (!f.fgUsesFlow) why = PushEffectWhy::NoFlowConsumer;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.flowSamples == 0) why = PushEffectWhy::Unknown;
        set(PushEffectSetting::EdgeAegis, f.edgeAegisOn, why, fg && f.playing && f.fgUsesFlow && !f.ownMotion && f.flowSamples > 0);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.nvidiaLowLatencyOn) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.driverLowLatency < 0) why = PushEffectWhy::Unknown;
        else if (f.driverLowLatency == 0) why = PushEffectWhy::DriverReportsOff;
        set(PushEffectSetting::NvidiaLowLatency, f.nvidiaLowLatencyOn, why, f.playing && f.driverLowLatency == 1);
    }
    {
        // The queue is "in force" under Vsync pacing whether the switch is on or not; nothing else measures the queue.
        PushEffectWhy why = PushEffectWhy::NotMeasurable;
        if (f.vsyncPaced && f.playing) why = PushEffectWhy::AlreadyByVsync;
        else if (!f.lowQueueOn) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        set(PushEffectSetting::LowQueue, f.lowQueueOn, why, false);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        if (!f.vrrOn) why = PushEffectWhy::Off;
        else if (!f.displayAutomatic) why = PushEffectWhy::NeedsAutomaticSync;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (f.driverVrr < 0) why = PushEffectWhy::Unknown;
        else if (f.driverVrr == 0) why = PushEffectWhy::DriverVrrOff;
        set(PushEffectSetting::Vrr, f.vrrOn, why, f.playing && f.driverVrr == 1);
    }
    {
        PushEffectWhy why = PushEffectWhy::Running;
        const bool ran = f.playing && f.enhanceSamples > 0;
        if (!f.enhanceOn) why = PushEffectWhy::Off;
        else if (!f.playing) why = PushEffectWhy::NotPlaying;
        else if (!ran) why = PushEffectWhy::Unknown;
        else if (f.enhanceWidth > 0 && f.outputWidth > 0 &&
                 (std::uint64_t(f.enhanceWidth) * f.enhanceHeight < std::uint64_t(f.outputWidth) * f.outputHeight))
            why = PushEffectWhy::WorksAtLowerSize;
        set(PushEffectSetting::Enhancement, f.enhanceOn, why, ran);
    }
    return r;
}

// Generated frames shown per real frame in the window, -1 when nothing real was shown.
constexpr double pushGeneratedPerReal(const PushEffectFacts& f) { return pushGeneratedPerRealOf(f.realPresented, f.generatedPresented); }

// The models at each generated-frame position for the running multiplier, e.g. "L,M" (3X, quality 5); "-" without VFG.
inline std::string pushVfgModelsText(const PushEffectFacts& f) {
    if (!f.vfg || f.executingMultiplier < 2) return "-";
    std::string s;
    for (unsigned sub = 1; sub < f.executingMultiplier; ++sub) {
        if (!s.empty()) s += ',';
        s += pushVfgPositionLetter(f.vfgQuality, f.executingMultiplier, sub, f.lowDelayActive);
    }
    return s;
}

// The [settings-effect] line: one field per setting as name=asked/running:why, then the executing facts.
// ASCII, fixed order, no paths or names: safe for the kit's allow-list.
inline std::string pushSettingsEffectLine(const PushEffectFacts& f, std::string_view trigger) {
    const auto rows = pushSettingsEffect(f);
    std::string s = "trigger=";
    s += trigger;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        s += ' ';
        s += pushEffectSettingName(PushEffectSetting(i));
        s += '=';
        s += rows[i].requested ? "on" : "off";
        s += '/';
        s += rows[i].executing ? "run" : "idle";
        s += ':';
        s += pushEffectWhyName(rows[i].why);
    }
    const auto num = [&s](std::string_view key, double v, int decimals) {
        char buffer[48];
        const int n = decimals == 0 ? std::snprintf(buffer, sizeof buffer, " %.*s=%.0f", int(key.size()), key.data(), v)
                                    : std::snprintf(buffer, sizeof buffer, " %.*s=%.*f", int(key.size()), key.data(), decimals, v);
        if (n > 0) s.append(buffer, std::size_t(n) < sizeof buffer ? std::size_t(n) : sizeof buffer - 1);
    };
    num("requestedX", f.requestedMultiplier, 0);
    num("adaptiveX", f.afterAdaptive, 0);
    num("capX", f.afterCap, 0);
    num("runningX", f.executingMultiplier, 0);
    num("generatedPerReal", pushGeneratedPerReal(f), 2);
    num("smoothTargetFps", f.smoothnessOn ? f.smoothTargetFps : 0.0, 0);
    num("capFps", f.capFps, 1);
    num("contentFps", f.contentFps, 2);
    s += " vfgQuality=" + std::to_string(f.vfg ? int(f.vfgQuality) : -1) + " vfgModelsAsSet=" + pushVfgModelsText(f);
    for (unsigned i = 0; i < f.vfgSubframeMs.size(); ++i) {
        char key[16];
        std::snprintf(key, sizeof key, "vfgSub%uP95Ms", i + 1);
        num(key, f.vfgSubframeMs[i], 3);
    }
    s += " vfgSubSamples=" + std::to_string(f.vfgSubframeSamples[0]) + "/" + std::to_string(f.vfgSubframeSamples[1]) + "/" +
         std::to_string(f.vfgSubframeSamples[2]) + "/" + std::to_string(f.vfgSubframeSamples[3]);
    s += " expectedGenerated=" + std::to_string(f.expectedGenerated) + " generatedPresented=" + std::to_string(f.generatedPresented) +
         " realPresented=" + std::to_string(f.realPresented);
    s += " hudLockState=" + std::to_string(f.hudLockState);
    s += " dedupRepeats=" + std::to_string(f.dedupRepeats) + "/" + std::to_string(f.dedupObserved);
    s += " pacingRepaired=" + std::to_string(f.pacingRepaired) + "/" + std::to_string(f.pacingFrames);
    s += " driverLowLatency=" + std::to_string(f.driverLowLatency);
    s += std::string(" vsyncPaced=") + (f.vsyncPaced ? "1" : "0") + " vrrSync=" + (f.vrrSync ? "1" : "0");
    num("flowGpuP95Ms", f.flowGpuMs, 3);
    num("enhanceGpuP95Ms", f.enhanceGpuMs, 3);
    s += " flowSamples=" + std::to_string(f.flowSamples) + " enhanceSamples=" + std::to_string(f.enhanceSamples);
    s += " hudLockComposes=" + std::to_string(f.hudLockComposes) + " vfgFastPairs=" + std::to_string(f.vfgFastPairs);
    s += " driverVrr=" + std::to_string(f.driverVrr) + " revision=" + std::to_string(f.revision);
    s += std::string(" lowDelayActive=") + (f.lowDelayActive ? "1" : "0");
    s += " enhanceSize=" + std::to_string(f.enhanceWidth) + "x" + std::to_string(f.enhanceHeight) +
         " outputSize=" + std::to_string(f.outputWidth) + "x" + std::to_string(f.outputHeight);
    s += " (asked/measured per setting with the reason: run = measured in the window (frames presented, GPU time, dispatch counter or the driver's answer), unknown = asked, not measured; window about 10 s; vfgModelsAsSet from the quality level; evidence only)";
    return s;
}

// When to write the line: at once after a settings or presentation change (once the next pair ran with it), then
// every periodMs while playing. Pure: the caller passes its clock.
class PushSettingsEffectGate {
public:
    explicit PushSettingsEffectGate(std::int64_t periodMs = 10000) : periodMs_(periodMs) {}
    // Returns the trigger name, or an empty view when nothing is due.
    std::string_view due(std::int64_t nowMs, std::uint64_t revision, std::uint64_t presentationSerial) {
        if (!started_ || revision != revision_ || presentationSerial != presentation_) {
            const bool first = !started_;
            started_ = true; revision_ = revision; presentation_ = presentationSerial; next_ = nowMs + periodMs_;
            return first ? "start" : "change";
        }
        if (nowMs >= next_) { next_ = nowMs + periodMs_; return "window"; }
        return {};
    }
    void reset() { started_ = false; }
private:
    std::int64_t periodMs_ = 10000, next_ = 0;
    std::uint64_t revision_ = 0, presentation_ = 0;
    bool started_ = false;
};

}  // namespace veyra::engine
