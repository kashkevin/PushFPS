// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushSettingsEffect.h (stream "s12-settings"): requested / applied / executing per setting.
// The facts below are HIS configuration from his Package 16 log (SETTINGS_AUDIT.md); nothing here is a measurement.
#include "veyra/engine/PushSettingsEffect.h"
#include <string>

namespace settings_effect_tests {
using namespace veyra::engine;
// Capture card 4K30, NVIDIA VFG quality 5, 3X ceiling, Smoothness Mode 90, dedup on, Vsync pacing, playing; the measured
// fields as one 10 s window of his session b (300 real / 600 generated presented, Fg1 1.87 / Fg2 4.16 ms, driver LL off).
inline PushEffectFacts his() {
    PushEffectFacts f;
    f.known = f.playing = f.liveSource = f.fgOn = f.vfg = f.ownMotion = true;
    f.requestedMultiplier = f.afterAdaptive = f.afterCap = f.executingMultiplier = 3;
    f.smoothnessOn = true; f.smoothTargetFps = 90; f.contentFps = 30;
    f.dedupOn = true; f.dedupObserved = 300; f.dedupRepeats = 0;
    f.vfgQuality = 5; f.vsyncPaced = true; f.nvidiaLowLatencyOn = true; f.driverLowLatency = 0; f.lowQueueOn = true;
    f.vrrOn = true; f.displayAutomatic = false; f.driverVrr = 0;
    f.realPresented = 300; f.generatedPresented = 600;
    f.vfgSubframeMs = {1.87, 4.16, 0, 0}; f.vfgSubframeSamples = {300, 300, 0, 0}; f.expectedGenerated = 600;
    return f;
}
inline PushEffectWhy why(const PushEffectRows& r, PushEffectSetting s) { return r[std::size_t(s)].why; }
inline bool runs(const PushEffectRows& r, PushEffectSetting s) { return r[std::size_t(s)].executing; }
inline bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}

TEST(settings_effect_his_session_b_3x_rows){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    const auto r = pushSettingsEffect(T::his());
    CHECK(T::why(r, PushEffectSetting::Multiplier) == PushEffectWhy::Running && T::runs(r, PushEffectSetting::Multiplier));
    CHECK(T::why(r, PushEffectSetting::FlowGroup) == PushEffectWhy::OwnMotion && !T::runs(r, PushEffectSetting::FlowGroup));
    // Review 1 MUST 3: no repeat to remove, still the game's clock: effective and measured (frames observed).
    CHECK(T::why(r, PushEffectSetting::Dedup) == PushEffectWhy::ContentClock && T::runs(r, PushEffectSetting::Dedup));
    CHECK(T::why(r, PushEffectSetting::NvidiaLowLatency) == PushEffectWhy::DriverReportsOff && !T::runs(r, PushEffectSetting::NvidiaLowLatency));
    CHECK(T::why(r, PushEffectSetting::LowQueue) == PushEffectWhy::AlreadyByVsync);
    CHECK(T::why(r, PushEffectSetting::Vrr) == PushEffectWhy::NeedsAutomaticSync);
    CHECK(T::why(r, PushEffectSetting::VfgQuality) == PushEffectWhy::Running && T::runs(r, PushEffectSetting::VfgQuality));
    CHECK(T::why(r, PushEffectSetting::HudLock) == PushEffectWhy::Off);
    CHECK(pushGeneratedPerReal(T::his()) == 2.0);
}

// Review 1 MUST 2 (the reviewer's test, kept): with nothing measured, nothing may say "run".
TEST(settings_effect_nothing_measured_nothing_runs){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his();
    f.realPresented = f.generatedPresented = 0; f.vfgSubframeSamples = {0, 0, 0, 0}; f.expectedGenerated = 0; f.driverLowLatency = -1; f.dedupObserved = 0;
    f.hudLockState = 1; f.hudLockComposes = 0; f.displayAutomatic = true; f.vrrSync = true; f.driverVrr = -1;
    f.enhanceOn = true; f.enhanceGpuMs = 0; f.pacingRepairOn = true; f.pacingFrames = 0; f.vfgAutoRegulateOn = true;
    f.strictOn = true; f.liveSource = false; f.edgeAegisOn = true;
    const auto r = pushSettingsEffect(f);
    for (std::size_t i = 0; i < r.size(); ++i) CHECK(!r[i].executing);
    CHECK(T::has(pushSettingsEffectLine(f, "window"), "multiplier=on/idle:unknown"));
    CHECK(!T::has(pushSettingsEffectLine(f, "window"), "/run:"));
    CHECK(T::why(r, PushEffectSetting::NvidiaLowLatency) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::Vrr) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::Dedup) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::HudLock) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::VfgQuality) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::Enhancement) == PushEffectWhy::Unknown);
    CHECK(T::why(r, PushEffectSetting::StrictCadence) == PushEffectWhy::NotMeasurable);   // review 2 SHOULD 7: grey, not a fault
    // Measured sources make them run.
    f.realPresented = 300; f.generatedPresented = 600; f.vfgSubframeSamples = {300, 300, 0, 0}; f.hudLockComposes = 600;
    f.driverLowLatency = 1; f.driverVrr = 1; f.dedupObserved = 300; f.enhanceSamples = 300;
    const auto m = pushSettingsEffect(f);
    for (const auto s : {PushEffectSetting::Multiplier, PushEffectSetting::VfgQuality, PushEffectSetting::HudLock, PushEffectSetting::NvidiaLowLatency,
                         PushEffectSetting::Vrr, PushEffectSetting::Dedup, PushEffectSetting::Enhancement})
        CHECK(T::runs(m, s));
}

// Revision 26-53 of his log: 4X asked, 3X ran (Smoothness Mode 90). Revision 40: 6X asked, the 120 cap made 4X, then 3X.
TEST(settings_effect_multiplier_reason_follows_the_rule_order){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his(); f.requestedMultiplier = 4; f.afterAdaptive = 4; f.afterCap = 4; f.executingMultiplier = 3;
    auto r = pushSettingsEffect(f);
    CHECK(T::why(r, PushEffectSetting::Multiplier) == PushEffectWhy::LimitedBySmoothness);
    CHECK(T::runs(r, PushEffectSetting::Multiplier));   // frames are generated and shown, at 3X
    f.requestedMultiplier = 6; f.afterAdaptive = 6; f.afterCap = 4; f.executingMultiplier = 4;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::LimitedByCap);
    f.executingMultiplier = 3;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::LimitedBySmoothness);
    f.adaptiveOn = true; f.afterAdaptive = 4; f.afterCap = 4; f.executingMultiplier = 4;
    r = pushSettingsEffect(f);
    CHECK(T::why(r, PushEffectSetting::Multiplier) == PushEffectWhy::LimitedByAdaptive);
    CHECK(T::why(r, PushEffectSetting::Adaptive) == PushEffectWhy::Running && T::runs(r, PushEffectSetting::Adaptive));
    f.executingMultiplier = 3;   // Smoothness Mode after adaptive: adaptive's choice is overridden
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Adaptive) == PushEffectWhy::LimitedBySmoothness);
    f = T::his(); f.adaptiveOn = true;   // a 3X ceiling at 180 Hz: adaptive keeps 3X
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Adaptive) == PushEffectWhy::NothingToChoose);
    f.requestedMultiplier = 2; f.afterAdaptive = f.afterCap = f.executingMultiplier = 2;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Adaptive) == PushEffectWhy::NeedsAbove2X);   // review 1 SHOULD 8
    // Review 1 SHOULD 8: Smoothness Mode back at the requested 3X over a 2X cap fit is not "limited".
    f = T::his(); f.afterCap = 2;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::Running);
    // Fewer generated frames shown than the admitted pairs' multipliers give: pairs skipped.
    f = T::his(); f.generatedPresented = 450;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::PairsSkipped);
    // Review 2 SHOULD 4: half the window at 2X (Smoothness Mode / adaptive), nothing skipped: 150 x 1 + 150 x 2 = 450 expected.
    f.expectedGenerated = 450;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::Running);
    f.generatedPresented = 440;   // within 3 % (0.97 x 450 = 436.5)
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::Running);
    // Review 2 SHOULD 5: a stopped VFG stage (no samples in the window) is not "running" even with an old P95.
    f = T::his(); f.vfgSubframeSamples = {300, 0, 0, 0};
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgQuality) == PushEffectWhy::Unknown);
    f = T::his(); f.fgOn = false;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::Off);
    f = T::his(); f.playing = false;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Multiplier) == PushEffectWhy::NotPlaying);
}

TEST(settings_effect_pacing_strict_and_auto_regulation){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his(); f.pacingRepairOn = true; f.pacingFrames = 300; f.pacingRepaired = 0;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::PacingRepair) == PushEffectWhy::NothingToRepair);
    f.contentFps = 0;   // review 1 SHOULD 8: no rate measured, no "even 0 FPS"
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::PacingRepair) == PushEffectWhy::Unknown);
    f.contentFps = 30; f.pacingRepaired = 4;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::PacingRepair) == PushEffectWhy::Running);
    f.dedupOn = false;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::PacingRepair) == PushEffectWhy::NeedsDedupOn);
    f = T::his(); f.strictOn = true;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::StrictCadence) == PushEffectWhy::FilePlaybackOnly);
    f = T::his(); f.vfgAutoRegulateOn = true;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::Needs2X);
    f.requestedMultiplier = 2;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::NeedsSingleModel);
    f.vfgQuality = 0;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::NoFasterModel);
    f.vfgQuality = 2;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::NeedsDedupOff);
    f.dedupOn = false;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::NotLoaded);
    f.vfgFastLoaded = true;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::Unknown);   // loaded, no fast pair yet
    f.vfgFastPairs = 12;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::VfgAutoRegulate) == PushEffectWhy::Running);
}

TEST(settings_effect_hud_lock_flow_enhancement_and_vrr){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his(); f.hudLockState = 1; f.hudLockComposes = 600;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::HudLock) == PushEffectWhy::Running);
    f.hudLockState = 2;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::HudLock) == PushEffectWhy::Unavailable);
    // Review 1 MUST 5: with AI enhancement the flow stage runs even under VFG; Edge Aegis still never acts with VFG.
    f = T::his(); f.flowSamples = 300; f.edgeAegisOn = true;
    auto r = pushSettingsEffect(f);
    CHECK(T::why(r, PushEffectSetting::FlowGroup) == PushEffectWhy::Running && T::runs(r, PushEffectSetting::FlowGroup));
    CHECK(T::why(r, PushEffectSetting::EdgeAegis) == PushEffectWhy::OwnMotion && !T::runs(r, PushEffectSetting::EdgeAegis));
    f = T::his(); f.vfg = false; f.ownMotion = false; f.fgUsesFlow = true; f.flowSamples = 300; f.edgeAegisOn = true;
    r = pushSettingsEffect(f);
    CHECK(T::why(r, PushEffectSetting::EdgeAegis) == PushEffectWhy::Running);
    CHECK(T::why(r, PushEffectSetting::VfgQuality) == PushEffectWhy::OtherMethod && !r[std::size_t(PushEffectSetting::VfgQuality)].requested);
    f.flowSamples = 0; f.flowGpuMs = 0.4;   // review 2 SHOULD 5: an old P95 without samples in this window is not running
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::FlowGroup) == PushEffectWhy::NoFlowConsumer);
    f = T::his(); f.enhanceOn = true; f.enhanceSamples = 300; f.enhanceWidth = 1920; f.enhanceHeight = 1080; f.outputWidth = 3840; f.outputHeight = 2160;
    r = pushSettingsEffect(f);
    CHECK(T::why(r, PushEffectSetting::Enhancement) == PushEffectWhy::WorksAtLowerSize && T::runs(r, PushEffectSetting::Enhancement));
    f.enhanceWidth = 3840; f.enhanceHeight = 2160;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Enhancement) == PushEffectWhy::Running);
    f = T::his(); f.displayAutomatic = true; f.vrrSync = true;   // the presenter's flag alone is not the driver's VRR
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Vrr) == PushEffectWhy::DriverVrrOff);
    f.driverVrr = 1;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::Vrr) == PushEffectWhy::Running);
    f.vsyncPaced = false; f.lowQueueOn = false;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::LowQueue) == PushEffectWhy::Off);
    f.driverLowLatency = 1;
    CHECK(T::why(pushSettingsEffect(f), PushEffectSetting::NvidiaLowLatency) == PushEffectWhy::Running);
}

// The model per position is the one EnhancementSettings.h gives (vfgPrimaryMode / vfgSecondaryMode / vfgPrimarySubframe).
TEST(settings_effect_vfg_models_per_position){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his();
    CHECK(pushVfgModelsText(f) == "L,M");          // his quality 5 at 3X: t=1/3 Low, t=2/3 Medium
    f.vfgQuality = 1; CHECK(pushVfgModelsText(f) == "M,M");
    f.vfgQuality = 2; CHECK(pushVfgModelsText(f) == "H,H");
    f.vfgQuality = 3; CHECK(pushVfgModelsText(f) == "M,H");
    f.vfgQuality = 4; CHECK(pushVfgModelsText(f) == "L,H");
    f.vfgQuality = 5; f.executingMultiplier = 2; CHECK(pushVfgModelsText(f) == "M");
    f.executingMultiplier = 4; CHECK(pushVfgModelsText(f) == "L,M,L");
    f.executingMultiplier = 6; CHECK(pushVfgModelsText(f) == "L,L,M,L,L");
    f.vfgQuality = 0; f.executingMultiplier = 4; CHECK(pushVfgModelsText(f) == "L,L,L");
    // Review 2 MUST 1: Lower delay at 3X: one VFG frame (FrameIndex 2, the primary model), then the predicted frame.
    f.vfgQuality = 5; f.executingMultiplier = 3; f.lowDelayActive = true; CHECK(pushVfgModelsText(f) == "M,P");
    f.vfgQuality = 4; CHECK(pushVfgModelsText(f) == "H,P");
    CHECK(pushVfgPositionLetter(5, 3, 1, true) == 'M' && pushVfgPositionLetter(5, 3, 2, true) == 'P' && pushVfgPositionLetter(5, 3, 1, false) == 'L');
    f.lowDelayActive = false;
    f.vfg = false; CHECK(pushVfgModelsText(f) == "-");
}

TEST(settings_effect_line_and_gate){
    namespace T = settings_effect_tests; using namespace veyra::engine;
    auto f = T::his(); f.requestedMultiplier = 4; f.afterAdaptive = 4; f.afterCap = 4;
    const auto line = pushSettingsEffectLine(f, "window");
    CHECK(T::has(line, "trigger=window multiplier=on/run:limited-by-smoothness "));
    CHECK(T::has(line, " dedup=on/run:content-clock "));
    CHECK(T::has(line, " flow=on/idle:own-motion "));
    CHECK(T::has(line, " nvidiaLowLatency=on/idle:driver-reports-off "));
    CHECK(T::has(line, " requestedX=4 adaptiveX=4 capX=4 runningX=3 generatedPerReal=2.00 smoothTargetFps=90"));
    CHECK(T::has(line, " vfgQuality=5 vfgModelsAsSet=L,M vfgSub1P95Ms=1.870 vfgSub2P95Ms=4.160"));
    for (const char c : line) CHECK(c >= 0x20 && c < 0x7f);   // ASCII, one line
    for (unsigned i = 0; i < unsigned(PushEffectWhy::Count); ++i)
        for (unsigned j = i + 1; j < unsigned(PushEffectWhy::Count); ++j)
            CHECK(pushEffectWhyName(PushEffectWhy(i)) != pushEffectWhyName(PushEffectWhy(j)));
    PushSettingsEffectGate g(10000);
    CHECK(g.due(0, 5, 1) == "start");
    CHECK(g.due(5000, 5, 1).empty());
    CHECK(g.due(6000, 6, 1) == "change");
    CHECK(g.due(7000, 6, 2) == "change");
    CHECK(g.due(16999, 6, 2).empty());
    CHECK(g.due(17000, 6, 2) == "window");
    g.reset();
    CHECK(g.due(17001, 6, 2) == "start");
}

// [capture-driver-age] (source/PushDriverAge.h): his Package 16 numbers, p50 138 -> 155 -> 174 ms in 40 s, are a drift of
// about 0.9 ms a second with a small spread; a queue shows as spread, not as slope.
#include "veyra/source/PushDriverAge.h"
TEST(driver_age_drift_is_not_a_queue){
    using veyra::source::pushDriverAgeFit;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 600; ++i) points.emplace_back(i / 30.0, 138.0 + 0.9 * (i / 30.0) + ((i % 3) - 1) * 0.5);
    const auto fit = pushDriverAgeFit(points);
    CHECK(fit.valid && std::fabs(fit.driftMsPerS - 0.9) < 0.01 && fit.residualP95Ms < 0.6);
    // A driver that queues one more 33 ms frame every other frame: no drift, a 33 ms spread.
    points.clear();
    for (int i = 0; i < 600; ++i) points.emplace_back(i / 30.0, 40.0 + (i % 2) * 33.3);
    const auto queued = pushDriverAgeFit(points);
    CHECK(queued.valid && std::fabs(queued.driftMsPerS) < 0.2 && queued.residualP95Ms > 15.0);
    CHECK(!pushDriverAgeFit({{0.0, 1.0}, {1.0, 2.0}}).valid);
}
