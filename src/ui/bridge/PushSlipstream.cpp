// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: the performance-mode work ("Slipstream" is its temporary internal name).
//   - the LAN profile preference -> the experiment switch a PS5 session reads when it starts;
//   - Automatic mode (only with VEYRA_EXP_AUTO_MODE=1): engine::PushAutomaticMode fed with numbers the
//     snapshot already holds;
//   - what this PC offers, for the first-launch card;
//   - the observation hooks VEYRA_EXP_RP_GOVERNOR and VEYRA_EXP_RP_GPU_BUDGET (pushSlipstreamWatch).
// The modes themselves are applied by the interface (qml/Veyra/PushModes.js) through the settings that
// already exist; nothing here changes a setting. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushBenchmarkProbe.h"
#include "veyra/engine/PushGovernorFeed.h"
#include "veyra/engine/PushPerformanceMode.h"
#include "veyra/ui/SlipstreamSettingsFile.h"
#include <array>
#include <chrono>
#include <optional>
#include <string>

namespace veyra::ui {
namespace {
// Set to exactly "1" in the environment when PushFPS started. Read once: the row it shows must not come
// and go while the program runs.
bool autoModeSwitch() {
    static const bool on = [] {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_AUTO_MODE", value, 4);
        return length == 1 && value[0] == L'1';
    }();
    return on;
}
struct AutoModeState {
    std::optional<engine::PushAutomaticMode> controller;
    uint64_t sessionId = 0;
    std::string reason = "idle";
};
// One bridge exists per process, and only its thread (the interface's) calls these.
AutoModeState& autoModeState() {
    static AutoModeState state;
    return state;
}
std::int64_t steady100ns() {
    return std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// A switch's value as ASCII; nullopt when unset or empty (as RemotePlaySessionSource.cpp reads them).
std::optional<std::string> switchText(const wchar_t* name) {
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(name, value, 32);
    if (length == 0) return std::nullopt;
    if (length >= 32) return std::string("?");
    std::string text;
    for (DWORD i = 0; i < length; ++i) text.push_back(value[i] > 0 && value[i] < 128 ? static_cast<char>(value[i]) : '?');
    return text;
}
bool switchOn(const wchar_t* name) {
    const auto text = switchText(name);
    return text && !text->empty() && *text != "0";
}
// Read once when first asked (after slipstream.ini was applied at start-up): a running test must not
// change mode halfway.
engine::PushExperimentMode governorSwitch() {
    static const engine::PushExperimentMode mode = [] {
        const auto text = switchText(L"VEYRA_EXP_RP_GOVERNOR");
        return engine::pushExperimentMode(text ? std::optional<std::string_view>(*text) : std::nullopt, false);
    }();
    return mode;
}
// VEYRA_EXP_RP_GPU_BUDGET: 1 (or any value but 0 / apply) observes, "apply" applies the reductions.
engine::PushExperimentMode gpuBudgetSwitch() {
    static const engine::PushExperimentMode mode = [] {
        const auto text = switchText(L"VEYRA_EXP_RP_GPU_BUDGET");
        return engine::pushExperimentMode(text ? std::optional<std::string_view>(*text) : std::nullopt, true);
    }();
    return mode;
}
// The pad poll interval the PS5 session uses (RemotePlay.cpp ps5PadPollMs).
int padPollMs() {
    if (qEnvironmentVariableIsSet("VEYRA_TEST_RP_LEGACY_TIMING") || qEnvironmentVariableIsSet("VEYRA_TEST_RP_LEGACY_INPUT")) return 8;
    bool ok = false;
    const int asked = qEnvironmentVariableIntValue("VEYRA_EXP_RP_PAD_POLL_MS", &ok);
    return ok && asked >= 1 && asked <= 8 ? asked : 4;
}
struct SlipstreamWatchState {
    engine::PushGovernorFeed feed;
    std::optional<engine::PushLatencyGovernor> governor;
    engine::PushFeedLogGate governorLog;
    engine::PushGpuBudgetDriver budget;
    engine::PushFeedLogGate budgetLog;
    engine::PushGovernorSwitches switches;   // read once per stream, when the watch starts for it
    uint64_t sessionId = 0;
    bool active = false;
    bool budgetApplying = false;     // =apply is in force for this stream (not refused for missing inputs)
    bool applyRefusedLogged = false;
    bool quitHooked = false;
    void reset() {
        feed.reset();
        governor.reset();
        governorLog.reset();
        budget.newSource();   // keeps the user's request: apply mode puts it back for the next stream
        budgetLog.reset();
        active = false;
        budgetApplying = false;
        applyRefusedLogged = false;
    }
};
SlipstreamWatchState& slipstreamWatchState() {
    static SlipstreamWatchState state;
    return state;
}
std::string feedNumber(double value) { return std::isfinite(value) && value >= 0 ? std::format("{:.2f}", value) : std::string("unavailable"); }
QString modeName(engine::PushPerformanceMode mode) {
    const auto name = engine::pushPerformanceModeName(mode);
    return QString::fromLatin1(name.data(), qsizetype(name.size()));
}
} // namespace

// Preference pushLanProfile (quick setup, row pushfps-mode): VEYRA_EXP_RP_LAN_PROFILE for the next PS5
// connection. A PS5 session reads its experiment switches with GetEnvironmentVariableW when it starts
// (RemotePlaySessionSource.cpp), so a running stream keeps what it started with. A switch that is in the
// environment already (set by the user, or by slipstream.ini at start-up) is never touched, and only a
// switch this function set is ever removed again. Not applied at all under VEYRA_TEST_RP_NO_SETTINGS_FILE:
// the hardware test kit sets that so every test arm has exactly the switches the kit gave it.
void QmlPlayerBridge::applyPushLanProfile() {
    namespace d = slipstream_file_detail;
    static std::array<bool, 16> ours{};
    const bool want = impl_->prefBool("pushLanProfile", false);
    if (GetEnvironmentVariableW(L"VEYRA_TEST_RP_NO_SETTINGS_FILE", nullptr, 0) > 0) {
        if (want) veyra::log::info("remoteplay-settings", "LAN profile preference not applied (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)");
        return;
    }
    std::string set, kept, removed;
    std::size_t index = 0;
    for (const engine::PushModeSwitch& item : engine::pushLanProfileSwitches()) {
        if (index >= ours.size()) break;
        bool& mine = ours[index++];
        const std::string narrow(item.name);
        const std::wstring name = d::widen(item.name);
        if (want) {
            if (mine) continue;
            if (d::inEnvironment(name, narrow)) { kept += (kept.empty() ? "" : " ") + narrow; continue; }
            const std::wstring value = d::widen(item.value);
            if (SetEnvironmentVariableW(name.c_str(), value.c_str()) != FALSE) { mine = true; set += (set.empty() ? "" : " ") + narrow; }
            else veyra::log::warn("remoteplay-settings", "LAN profile: could not set " + narrow);
        } else if (mine) {
            SetEnvironmentVariableW(name.c_str(), nullptr);
            mine = false;
            removed += (removed.empty() ? "" : " ") + narrow;
        }
    }
    if (want || !removed.empty())
        veyra::log::info("remoteplay-settings", "LAN profile preference=" + std::string(want ? "on" : "off") + " set=[" + set + "] removed=[" + removed +
                                                    "] alreadyInEnvironment=[" + kept + "] (read when the next PS5 connection starts)");
}

bool QmlPlayerBridge::pushAutoModeAvailable() const { return autoModeSwitch(); }

QVariantMap QmlPlayerBridge::pushAutoModeState() const {
    const auto& state = autoModeState();
    QVariantMap out{{"available", autoModeSwitch()}, {"active", state.controller.has_value()}};
    if (state.controller) {
        out["mode"] = modeName(state.controller->mode());
        out["ceiling"] = modeName(state.controller->ceiling());
        out["reason"] = QString::fromStdString(state.reason);
    }
    return out;
}

QVariantMap QmlPlayerBridge::pushAutoModeStep(const QString& ceilingName) {
    QVariantMap out{{"available", false}, {"changed", false}};
    if (!autoModeSwitch()) return out;
    const auto ceiling = engine::parsePushPerformanceMode(ceilingName.toStdString());
    auto& state = autoModeState();
    if (!ceiling || !engine::pushModeIsConcrete(*ceiling)) {
        state.controller.reset();   // no ceiling, no controller: the next one starts fresh
        return out;
    }
    const auto& s = impl_->snapshot;
    bool started = false;
    // A new source starts at the ceiling, as a new controller does: what troubled the last stream says
    // nothing about this one.
    if (!state.controller || state.sessionId != s.sessionId) {
        state.controller.emplace(*ceiling);
        state.sessionId = s.sessionId;
        started = true;
    } else if (state.controller->ceiling() != *ceiling) {
        state.controller->setCeiling(*ceiling);
    }
    engine::PushAutoSignals seen;
    seen.sourceFps = s.streamCadence.active ? s.streamCadence.sourceFps : double(s.contentFps);
    seen.displayHz = impl_->displayRefreshHz;
    seen.gpuUtilizationKnown = gpuUtilizationKnown();
    seen.gpuUtilizationPercent = gpuUtilization();
    seen.generationOverBudget = s.fgBudgetLimited;
    if (s.remotePlay) {
        seen.queueDepth = unsigned(std::min<std::size_t>(s.remoteStream.video.depth, 1000));
        const auto& health = s.remoteHealth;
        seen.network = !health.valid ? engine::PushAutoNetwork::Unknown
                        : health.grade == remoteplay::HealthGrade::Stable ? engine::PushAutoNetwork::Stable
                        : health.grade == remoteplay::HealthGrade::Unsteady ? engine::PushAutoNetwork::Unsteady
                        : health.grade == remoteplay::HealthGrade::Unstable ? engine::PushAutoNetwork::Unstable
                        : engine::PushAutoNetwork::Unknown;
    }
#ifdef VEYRA_ENABLE_MOONLIGHT
    else if (s.moonlightActive) seen.network = engine::PushAutoNetwork::Unknown;
#endif
#ifdef VEYRA_ENABLE_XBOX
    else if (s.xboxActive) seen.network = engine::PushAutoNetwork::Unknown;
#endif
    const engine::PushPerformanceMode before = state.controller->mode();
    const engine::PushAutoDecision decision = state.controller->update(engine::pushAutoInputs(seen, steady100ns()));
    state.reason.assign(decision.reason.data(), decision.reason.size());
    if (decision.changed || started)
        veyra::log::info("push-mode", std::format("automatic mode={} from={} ceiling={} reason={} sourceFps={:.2f} displayHz={:.2f} gpuKnown={} gpuPercent={:.0f} "
            "overBudget={} queueDepth={} network={} session={}", engine::pushPerformanceModeName(decision.mode), engine::pushPerformanceModeName(before),
            engine::pushPerformanceModeName(*ceiling), decision.reason, seen.sourceFps, seen.displayHz, seen.gpuUtilizationKnown,
            seen.gpuUtilizationPercent, seen.generationOverBudget, seen.queueDepth, unsigned(seen.network), s.sessionId));
    out["available"] = true;
    out["mode"] = modeName(decision.mode);
    // The first decision for a source is reported as a change too: the interface then puts the ceiling's
    // settings in place, which a mode lowered for the previous source may have left behind.
    out["changed"] = decision.changed || started;
    out["started"] = started;
    out["reason"] = QString::fromStdString(state.reason);
    return out;
}

QVariantMap QmlPlayerBridge::pushHardwareSummary() {
    QVariantMap out;
    const auto* adapter = engineAdapterDesc();
    out["gpuName"] = adapter ? QString::fromWCharArray(adapter->Description).trimmed() : QString();
    const uint32_t vendor = adapter ? adapter->VendorId : 0;
    out["gpuVendor"] = vendor == 0x10DE ? QStringLiteral("nvidia") : vendor == 0x1002 ? QStringLiteral("amd") : vendor == 0x8086 ? QStringLiteral("intel") : QString();
    const QVariantMap decode = pushDecodeCapabilities();
    out["decodeKnown"] = decode.value("known");
    out["decodeH264"] = decode.value("h264");
    out["decodeHevc"] = decode.value("hevc");
    out["decodeHevc10"] = decode.value("hevc10");
    QStringList backends;
    for (const auto& choice : fgBackendChoices()) {
        const auto row = choice.toMap();
        if (!row.value("disabled").toBool()) backends << row.value("label").toString();
    }
    out["fgBackends"] = backends;
    out["fgMaxMultiplier"] = backends.isEmpty() ? 1 : fgMaxMultiplier();
    out["displayHz"] = impl_->displayRefreshHz;
    out["vrrState"] = impl_->vrrState;
    out["lowLatencyNvidia"] = nvidiaLowLatencyAvailable();
    bool networkKnown = false, ethernet = false, wifi = false, routeKnown = false, routeDirectLan = false;
    QString routeLink = QStringLiteral("unknown");
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const auto interfaces = remoteplay::localInterfaces();
    networkKnown = !interfaces.empty();
    for (const auto& local : interfaces) {
        ethernet = ethernet || local.kind == remoteplay::LinkKind::Ethernet;
        wifi = wifi || local.kind == remoteplay::LinkKind::WiFi;
    }
    if (networkKnown && !impl_->remotePlayHost.trimmed().isEmpty()) {
        const auto route = remoteplay::assessRoute(impl_->remotePlayHost.trimmed().toStdString(), interfaces);
        routeKnown = route.addressKnown;
        routeDirectLan = route.directLan();
        routeLink = QString::fromLatin1(route.linkName());
    }
#endif
    out["networkKnown"] = networkKnown;
    out["ethernet"] = ethernet;
    out["wifi"] = wifi;
    out["routeKnown"] = routeKnown;
    out["routeDirectLan"] = routeDirectLan;
    out["routeLink"] = routeLink;
    // VEYRA_EXP_RP_GOVERNOR / VEYRA_EXP_RP_GPU_BUDGET only: the capability model's view (pushAdapterCapabilities,
    // Hardware.cpp; it logs [push-gpu-caps]): every key
    // that is not already above is added as it is; one whose name is taken (decodeH264, decodeHevc, decodeHevc10,
    // vrrState) is added with a "caps" prefix (capsDecodeH264 ...), so no existing key changes meaning.
    // Only while one of the hardware-test switches is set, and asked once per process: it creates short-lived
    // D3D12 devices and enumerates the outputs. Without the switches the summary is exactly as before.
    static const bool wanted = governorSwitch() != engine::PushExperimentMode::Off || gpuBudgetSwitch() != engine::PushExperimentMode::Off;
    static std::optional<QVariantMap> cachedCaps;
    if (wanted && !cachedCaps) cachedCaps = pushAdapterCapabilities(impl_->displayRefreshHz, impl_->vrrState);
    const QVariantMap caps = wanted ? *cachedCaps : QVariantMap();
    for (auto it = caps.cbegin(); it != caps.cend(); ++it) {
        if (!out.contains(it.key())) out.insert(it.key(), it.value());
        else out.insert(QStringLiteral("caps") + it.key().left(1).toUpper() + it.key().mid(1), it.value());
    }
    veyra::log::info("push-hardware", std::format("vendor={:#06x} decodeKnown={} h264={} hevc={} hevc10={} fgBackends={} displayHz={:.2f} vrrState={} "
        "networkKnown={} ethernet={} wifi={} routeKnown={} directLan={} link={}", vendor, decode.value("known").toBool(), decode.value("h264").toBool(),
        decode.value("hevc").toBool(), decode.value("hevc10").toBool(), backends.size(), impl_->displayRefreshHz, impl_->vrrState, networkKnown, ethernet,
        wifi, routeKnown, routeDirectLan, routeLink.toStdString()));
    return out;
}
// VEYRA_EXP_RP_GOVERNOR=1: once a second (from pushFlowBudgetWatch's tick) the latency governor is fed with
// what the snapshot already holds and its recommendation is logged ([slipstream-governor], at most every 10 s
// or at once when it changes). It is never applied: the governor then drops it after 5 s and leaves that knob
// alone for a while, which the log shows as "declined". Not fed while the optimiser or the quality test runs,
// while settings are being applied, or without a measured source rate.
// VEYRA_EXP_RP_GPU_BUDGET=1: the same tick feeds PushGpuBudget and logs [slipstream-gpu-budget] the same way.
// =apply: its level is put in place through setFgMultiplier / setVfgQuality (the setters pushFlowBudgetWatch
// uses): a lower tier, a lower multiplier or generation off, and later the slow way back, never above what
// the user chose (a change the user makes becomes the new ceiling). While it applies, pushFlowBudgetWatch
// does not lower PushFlow's tier itself (the return value). Unverified on Windows.
bool QmlPlayerBridge::pushSlipstreamWatch() {
    const engine::PushExperimentMode governorMode = governorSwitch();
    const engine::PushExperimentMode budgetMode = gpuBudgetSwitch();
    if (governorMode == engine::PushExperimentMode::Off && budgetMode == engine::PushExperimentMode::Off) return false;
    const bool budgetApplies = budgetMode == engine::PushExperimentMode::Apply;
    auto& state = slipstreamWatchState();
    const auto& s = impl_->snapshot;
    const QString optimizing = impl_->prefs.value(QStringLiteral("pushOptimizeRestore")).toString();
    const bool measuring = engine::pushBenchmarkProbe().active() || impl_->engine.qualityTest().running ||
                           (!optimizing.isEmpty() && optimizing != QLatin1String("{}"));
    if (budgetApplies && !state.quitHooked) {
        // A reduced level must never be what the final save writes: put the user's choice back first. The
        // destructor's persistSession runs after aboutToQuit.
        state.quitHooked = true;
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { pushGpuBudgetRestore("quit"); });
    }
    if (!s.running || measuring) {
        if (state.active) veyra::log::info("slipstream-watch", measuring ? "paused: the optimiser or the quality test is running" : "stopped: nothing is playing");
        // The user's choice back before the optimiser or the quality test measures, or before the stopped
        // stream's settings are saved; then stand down (the optimiser's own changes are not the user's).
        if (budgetApplies) pushGpuBudgetRestore(measuring ? "measurement" : "stopped");
        state.reset();
        if (measuring) state.budget.reset();
        return false;
    }
    if (impl_->facade.applying() || s.applying || s.desired.revision != s.applied.revision) return state.budgetApplying;
    if (!state.active || state.sessionId != s.sessionId) {
        state.reset();
        state.active = true;
        state.sessionId = s.sessionId;
        state.switches.lanProfile = switchOn(L"VEYRA_EXP_RP_LAN_PROFILE");
        state.switches.reorderAdaptive = switchOn(L"VEYRA_EXP_RP_REORDER_ADAPTIVE");
        state.switches.backlog = switchOn(L"VEYRA_EXP_RP_BACKLOG");
        state.switches.padPollMs = padPollMs();
    }
    const auto& flow = s.metrics.flow;
    engine::PushFeedInput in;
    in.now100ns = steady100ns();
    in.sessionId = s.sessionId;
    in.sourceFps = s.streamCadence.active ? s.streamCadence.sourceFps : double(s.contentFps);
    in.outputFps = s.submissionFps.value_or(-1.0);
    in.enhancementP95Ms = flow.enhancementProcessing.p95.value_or(-1.0);
    in.fgBatchP95Ms = flow.gpuTiming[size_t(diagnostics::GpuStage::FgBatch)].p95.value_or(-1.0);
    in.decodeQueueP95Ms = flow.cpuTiming[size_t(diagnostics::CpuStage::DecodedQueue)].p95.value_or(-1.0);
    if (s.remotePlay) {
        in.decodeP95Ms = s.remoteStream.decodeP95Ms.value_or(-1.0);
        in.queueDepth = double(s.remoteStream.video.depth);
        in.framesLostKnown = true;
        in.framesLost = s.remoteStream.framesLost;
        if (s.remoteHealth.valid) {
            in.jitterMs = s.remoteHealth.frameJitterMs;
            in.lossPercent = s.remoteHealth.lossPercent;
        } else if (s.remoteStream.ratesReady) {
            in.jitterMs = s.remoteStream.frameJitterMs;
        }
    }
    in.gpuUtilizationPercent = gpuUtilizationKnown() ? gpuUtilization() : -1.0;
    const auto& counters = flow.counters;
    in.countersKnown = true;
    in.presented = counters.realPresented + counters.generatedPresented;
    in.expired = counters.generatedExpiredAfterEval;
    in.pairsCandidate = counters.fgCandidate;
    in.pairsSkipped = counters.fgSkippedBeforeEval;
    in.multiplier = unsigned(std::max(1, fgMultiplier()));
    const bool pushFlow = settings().frameGenerationBackend == engine::FrameGenerationBackend::PushFlow;
    const unsigned tier = pushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u;
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power))
        in.power = power.BatteryFlag == 128 || power.ACLineStatus == 1 ? engine::PushPowerSource::Mains
                   : power.ACLineStatus == 0 ? engine::PushPowerSource::Battery : engine::PushPowerSource::Unknown;
    const engine::GovernorKnobs knobs = engine::pushGovernorKnobs(state.switches, in.multiplier, tier);
    const engine::PushFeedOutput fed = state.feed.update(in, knobs);

    if (budgetMode != engine::PushExperimentMode::Off) {
        const auto method = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(settings().frameGenerationBackend));
        const engine::PushFgRequest current{method, in.multiplier, tier};
        // Capture cards have no decode-queue delay: the budget could never see a healthy period there and
        // would only ever step down. It observes instead (said once per stream).
        const bool canApply = engine::pushGpuBudgetCanApply(fed.budgetUnknown);
        if (budgetApplies && !canApply && !state.applyRefusedLogged) {
            state.applyRefusedLogged = true;
            veyra::log::info("slipstream-gpu-budget", "apply refused: the decode-queue delay is not measured for this source (a capture card); observing only");
        }
        state.budgetApplying = budgetApplies && canApply;
        const auto step = state.budget.update(current, fed.budget, state.budgetApplying);
        bool applied = false;
        if (step.apply) {
            // One change at a time; the multiplier first (1 turns generation off), then the tier.
            if (step.target.multiplier != in.multiplier) setFgMultiplier(int(step.target.multiplier));
            if (engine::pushFgTierCount(method) > 1 && step.target.tier != tier) setVfgQuality(int(step.target.tier));
            applied = true;
        }
        const std::string key = std::format("{}{}{}", step.decision.index, applied ? "+applied" : "", step.refused ? "+refused" : "");
        if (state.budgetLog.due(in.now100ns, key) || step.restarted) {
            const auto& sample = fed.budget;
            const auto& level = step.decision.level;
            veyra::log::info("slipstream-gpu-budget", std::format("mode={} level={} multiplier={} tier={} reason={} request multiplier={} tier={} method={} "
                "restarted={} refused={} held={} applied={}{} sourceIntervalMs={} outputIntervalMs={} gpuFrameMs={} fgMs={} decodeQueueMs={} presented={} missed={} "
                "skippedPairShare={} power={} thermal=unavailable unavailable=[{}] session={}", engine::pushExperimentModeName(budgetMode), step.decision.index,
                level.multiplier, level.tier, step.decision.reason, step.request.multiplier, step.request.tier, int(method), step.restarted ? 1 : 0,
                step.refused ? 1 : 0, step.held ? 1 : 0, applied ? 1 : 0, applied ? std::format(" to multiplier={} tier={}", step.target.multiplier, step.target.tier) : std::string(),
                feedNumber(sample.sourceIntervalMs), feedNumber(sample.outputIntervalMs), feedNumber(sample.gpuFrameMs), feedNumber(sample.fgMs),
                feedNumber(sample.decodeQueueDelayMs), sample.presented, sample.missedDeadlines, feedNumber(sample.skippedPairShare),
                int(sample.power), engine::pushFeedUnavailable(fed.budgetUnknown), s.sessionId));
        }
    }
    if (governorMode == engine::PushExperimentMode::Off) return state.budgetApplying;

    if (!state.governor) state.governor.emplace();
    std::optional<engine::GovernorRecommendation> recommendation;
    if (fed.governorValid) recommendation = state.governor->update(fed.nowMs, fed.governor, knobs);
    const std::string text = fed.governorValid ? engine::pushGovernorRecommendationText(recommendation) : std::string("not-fed (no source rate)");
    if (state.governorLog.due(in.now100ns, text)) {
        const auto& g = fed.governor;
        const auto known = [&fed](std::uint32_t bit, double value) { return (fed.governorUnknown & bit) != 0 ? std::string("unavailable") : feedNumber(value); };
        namespace b = engine::feed_input;
        veyra::log::info("slipstream-governor", std::format("mode={} recommend={} applied=0 sourceFps={} jitterMs={} lossPercent={} framesLostPerSecond={} "
            "decodeMs={} gpuMs={} gpuHeadroomPercent={} queueDepth={} schedulingMs={} presentLateMs={} knobs reorder={} backlog={} pollMs={} fgStep={} tier={} "
            "unavailable=[{}] rollbacks={} declined={} invalid={} session={}", engine::pushExperimentModeName(governorMode), text,
            known(b::kSourceFps, g.sourceFps), known(b::kJitter, g.networkJitterMs), known(b::kLoss, g.lossPercent), known(b::kFramesLost, g.framesLostPerSecond),
            known(b::kDecode, g.decodeMs), known(b::kGpuFrame, g.gpuMs), known(b::kGpuHeadroom, g.gpuHeadroomPercent), known(b::kQueueDepth, g.queueDepth),
            known(b::kScheduling, g.cpuSchedulingDelayMs), known(b::kPresentLate, g.presentLateMs), int(knobs.reorder), knobs.backlog ? 1 : 0,
            knobs.controllerPollMs, knobs.fgStep, knobs.qualityTier, engine::pushFeedUnavailable(fed.governorUnknown), state.governor->rollbacks(),
            state.governor->declined(), state.governor->invalidMeasurements(), s.sessionId));
    }
    return state.budgetApplying;
}

// VEYRA_EXP_RP_GPU_BUDGET=apply: the user's frame generation choice back, when the settings in force are still
// a level the budget put in place (PushGpuBudgetDriver::pendingRestore). Before a stop, a measurement and the
// final save at quit. Unverified on Windows.
void QmlPlayerBridge::pushGpuBudgetRestore(const char* why) {
    auto& state = slipstreamWatchState();
    const auto method = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(settings().frameGenerationBackend));
    const unsigned multiplier = unsigned(std::max(1, fgMultiplier()));
    const unsigned tier = method == engine::PushFgMethod::PushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u;
    const auto back = state.budget.pendingRestore(engine::PushFgRequest{method, multiplier, tier});
    if (!back) return;
    if (back->multiplier != multiplier) setFgMultiplier(int(back->multiplier));
    if (engine::pushFgTierCount(method) > 1 && back->tier != tier) setVfgQuality(int(back->tier));
    state.budget.restored();
    veyra::log::info("slipstream-gpu-budget", std::format("restored the user's choice ({}): multiplier {} -> {} tier {} -> {}", why, multiplier,
        back->multiplier, tier, back->tier));
}
} // namespace veyra::ui
