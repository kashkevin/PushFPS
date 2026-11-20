// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: the performance-mode work ("Slipstream" is its temporary internal name).
//   - the LAN profile preference -> the experiment switch a PS5 session reads when it starts;
//   - Automatic mode (only with VEYRA_EXP_AUTO_MODE=1): engine::PushAutomaticMode fed with numbers the
//     snapshot already holds;
//   - what this PC offers, for the first-launch card;
//   - the hooks VEYRA_EXP_RP_GOVERNOR (=1 observe, =apply active control) and VEYRA_EXP_RP_GPU_BUDGET
//     (pushSlipstreamWatch).
// The modes themselves are applied by the interface (qml/Veyra/PushModes.js) through the settings that
// already exist; nothing here changes a setting. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushBenchmarkProbe.h"
#include "veyra/engine/PushGovernorControl.h"
#include "veyra/engine/PushCalibrationPlan.h"
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
// change mode halfway. 1 (or any value but 0 / apply) observes; "apply" lets PushGovernorControl change the
// frame generation multiplier and PushFlow tier (experimental).
engine::PushExperimentMode governorSwitch() {
    static const engine::PushExperimentMode mode = [] {
        const auto text = switchText(L"VEYRA_EXP_RP_GOVERNOR");
        return engine::pushExperimentMode(text ? std::optional<std::string_view>(*text) : std::nullopt, true);
    }();
    return mode;
}
// VEYRA_EXP_RP_GOVERNOR_BUDGET_MS: the latency budget (allowed unplanned delay, P95) in whole ms, 1..200;
// unset or anything else keeps the default (PushControlConfig).
engine::PushControlConfig governorConfig() {
    static const engine::PushControlConfig config = [] {
        engine::PushControlConfig c;
        bool ok = false;
        // Decimal only (qEnvironmentVariableIntValue would read "012" as octal).
        const int asked = qEnvironmentVariable("VEYRA_EXP_RP_GOVERNOR_BUDGET_MS").trimmed().toInt(&ok, 10);
        if (ok && asked >= 1 && asked <= 200) {
            c.latencyBudgetMs = double(asked);
            c.hysteresisMs = c.latencyBudgetMs * 0.3;
        }
        return engine::pushSanitizedControlConfig(c);
    }();
    return config;
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
    // VEYRA_EXP_RP_GOVERNOR=apply: the arbiter. Created once and kept across streams (it keeps the user's
    // request for the restore, and its rate limits and lock-out hold for the whole run).
    std::optional<engine::PushGovernorControl> control;
    engine::PushFeedLogGate controlLog;
    engine::PushControlTick lastTick{};
    uint64_t sessionId = 0;
    bool active = false;
    bool budgetApplying = false;     // =apply is in force for this stream (not refused for missing inputs)
    bool applyRefusedLogged = false;
    bool controlApplying = false;    // the governor applies for this stream (not refused for missing inputs)
    bool controlRefusedLogged = false;
    bool supersededLogged = false;
    bool quitHooked = false;
    void reset() {
        feed.reset();
        governor.reset();
        governorLog.reset();
        budget.newSource();   // keeps the user's request: apply mode puts it back for the next stream
        budgetLog.reset();
        if (control) control->newStream();
        controlLog.reset();
        lastTick = {};
        active = false;
        budgetApplying = false;
        applyRefusedLogged = false;
        controlApplying = false;
        controlRefusedLogged = false;
    }
};
SlipstreamWatchState& slipstreamWatchState() {
    static SlipstreamWatchState state;
    return state;
}
std::string feedNumber(double value) { return std::isfinite(value) && value >= 0 ? std::format("{:.2f}", value) : std::string("unavailable"); }
// Preference pushGovernorRestore: the user's frame generation choice while the governor (or the GPU budget's
// apply mode) has a reduced level in force, written before the first reduced level is set and cleared once
// the choice is back; "{}" or absent = nothing to restore. The setters save the session 300 ms later, so
// after a crash the reduced level is what is on disk: the first watch tick of the next run puts this back.
// Format 2: {"format":2,"method","multiplier","tier" (the user's choice),"atMultiplier","atTier" (the reduced level
// set)}. A record of another format, or whose reduced level is not what is in force, is dropped, never applied.
constexpr int kRestoreFormat = 2;
QString restoreText(const engine::PushFgRequest& r, const engine::PushFgRequest& at) {
    return QStringLiteral("{\"format\":%1,\"method\":%2,\"multiplier\":%3,\"tier\":%4,\"atMultiplier\":%5,\"atTier\":%6}")
        .arg(kRestoreFormat).arg(int(r.method)).arg(r.multiplier).arg(r.tier).arg(at.multiplier).arg(at.tier);
}
struct RestoreRecord { engine::PushFgRequest request, at; };
std::optional<RestoreRecord> restoreParse(const QString& text) {
    const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
    if (o.value(QStringLiteral("format")).toInt(-1) != kRestoreFormat) return std::nullopt;
    const int method = o.value(QStringLiteral("method")).toInt(-1), multiplier = o.value(QStringLiteral("multiplier")).toInt(-1),
              tier = o.value(QStringLiteral("tier")).toInt(-1), atMultiplier = o.value(QStringLiteral("atMultiplier")).toInt(-1),
              atTier = o.value(QStringLiteral("atTier")).toInt(-1);
    const int largest = int(engine::kPushFgLargestMultiplier);
    if (method < 0 || method >= int(engine::kPushFgMethodCount) || multiplier < 1 || multiplier > largest || tier < 0 || tier > 2 || atMultiplier < 1 ||
        atMultiplier > largest || atTier < 0 || atTier > 2)
        return std::nullopt;
    const auto m = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(method));
    return RestoreRecord{engine::pushNormalizedFgRequest(engine::PushFgRequest{m, unsigned(multiplier), unsigned(tier)}),
                         engine::pushNormalizedFgRequest(engine::PushFgRequest{m, unsigned(atMultiplier), unsigned(atTier)})};
}
// A measurement owns the settings: the optimiser (pushOptimizeRestore) or the first-launch calibration
// (pushCalibrationRestore) while it runs or awaits its own crash restore.
bool measurementPref(const QVariantMap& prefs) {
    for (const auto* key : {"pushOptimizeRestore", "pushCalibrationRestore"}) {
        const QString v = prefs.value(QString::fromLatin1(key)).toString();
        if (!v.isEmpty() && v != QLatin1String("{}")) return true;
    }
    return false;
}
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
    if (d::inEnvironment(L"VEYRA_TEST_RP_NO_SETTINGS_FILE", "VEYRA_TEST_RP_NO_SETTINGS_FILE")) {   // also when set empty
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
// First-launch recommendation and its calibration (qml/Veyra/PushCalibrationCard.qml; the rules are
// engine/PushCalibrationPlan.h). request:
//   "sample"  the numbers one calibration phase reads at its start and end: running, settled, sessionId,
//             fgBackend, fgMultiplier, gpuMs (P95 GPU time of one source frame's graph work, generation batch
//             included; -1 unmeasured), fgBatchMs (-1), and the counters presented, expired, pairsCandidate,
//             pairsSkipped. Reads the snapshot only.
//   "plan"    args: game (30 | 60 | anything else = unknown), method (the frame generation id in use) and,
//             after a calibration, measured { baseGpuMs, totalGpuMs, fgBatchGpuMs, multiplier, method,
//             start {counters}, end {counters} }. Asks pushAdapterCapabilities (Hardware.cpp) for the
//             capability report, so it is only called on the user's way to the card, never on a timer.
//             Returns valid (false when the report says nothing about the GPU: nothing is offered then), the
//             plan's fields, its reasons and unknowns, and a hardware class (vendor, dedicated or integrated,
//             memory in GB; no device name). Changes no setting.
//   "switches" puts the experiment switches of the chosen interface mode in the environment for the next
//             connection (below). Unverified on Windows.
namespace {
constexpr std::array<const char*, engine::kPushFgMethodCount> kCalibrationMethodIds{"dlss", "xess", "fsr3", "fsr4", "vfg", "anr", "amdfrc", "pushflow"};
std::optional<engine::PushFgMethod> calibrationMethod(const QString& id) {
    for (std::size_t i = 0; i < kCalibrationMethodIds.size(); ++i)
        if (id == QLatin1String(kCalibrationMethodIds[i])) return static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(i));
    return std::nullopt;
}
QString calibrationMethodId(engine::PushFgMethod method) {
    const auto index = static_cast<std::size_t>(method);
    return index < kCalibrationMethodIds.size() ? QString::fromLatin1(kCalibrationMethodIds[index]) : QString();
}
engine::PushCapability calibrationCapability(const QVariant& value) {
    const QString text = value.toString();
    return text == QLatin1String("yes") ? engine::PushCapability::Yes : text == QLatin1String("no") ? engine::PushCapability::No : engine::PushCapability::Unknown;
}
engine::PushCalibrationCounters calibrationCounters(const QVariantMap& map) {
    engine::PushCalibrationCounters c;
    c.presented = map.value("presented").toULongLong();
    c.expired = map.value("expired").toULongLong();
    c.pairsCandidate = map.value("pairsCandidate").toULongLong();
    c.pairsSkipped = map.value("pairsSkipped").toULongLong();
    return c;
}
double calibrationNumber(const QVariantMap& map, const char* key) {
    bool ok = false;
    const double value = map.value(QLatin1String(key)).toDouble(&ok);
    return ok && std::isfinite(value) ? value : -1.0;
}
QString calibrationText(std::string_view text) { return QString::fromLatin1(text.data(), qsizetype(text.size())); }
} // namespace

QVariantMap QmlPlayerBridge::pushCalibration(const QString& request, const QVariantMap& args) {
    const auto& s = impl_->snapshot;
    if (request == QLatin1String("sample")) {
        const auto& flow = s.metrics.flow;
        const auto& counters = flow.counters;
        const auto gpu = flow.enhancementProcessing.p95;
        const auto batch = flow.gpuTiming[size_t(diagnostics::GpuStage::FgBatch)].p95;
        return {{"running", s.running}, {"settled", s.running && !impl_->facade.applying() && !s.applying && s.desired.revision == s.applied.revision},
                {"sessionId", QVariant::fromValue(qulonglong(s.sessionId))}, {"fgBackend", fgBackendName()}, {"fgMultiplier", fgMultiplier()},
                {"gpuMs", gpu ? *gpu : -1.0}, {"fgBatchMs", batch ? *batch : -1.0},
                {"presented", QVariant::fromValue(qulonglong(counters.realPresented + counters.generatedPresented))},
                {"expired", QVariant::fromValue(qulonglong(counters.generatedExpiredAfterEval))},
                {"pairsCandidate", QVariant::fromValue(qulonglong(counters.fgCandidate))},
                {"pairsSkipped", QVariant::fromValue(qulonglong(counters.fgSkippedBeforeEval))}};
    }
    if (request == QLatin1String("switches")) {
        // The experiment switches the interface's mode asks for (engine::pushInterfaceSwitches), from the next
        // connection. Written like slipstream.ini writes them (SetEnvironmentVariableW and qputenv); only ones
        // this function set are ever removed. Not applied under VEYRA_TEST_RP_NO_SETTINGS_FILE (the hardware test
        // kit gives every arm exactly its own switches). Returns { applied, set, unset, kept } (switch names).
        namespace d = slipstream_file_detail;
        static std::array<bool, engine::kPushInterfaceSwitchNames.size()> ours{};
        QStringList set, unset, kept;
        const bool kit = d::inEnvironment(L"VEYRA_TEST_RP_NO_SETTINGS_FILE", "VEYRA_TEST_RP_NO_SETTINGS_FILE");   // also when set empty
        // Only a mode chosen with this version (pref pushModeSwitches, written by PushModes.js select()) asks for
        // switches: a mode chosen before keeps running without them until the user chooses it again.
        const auto chosen = impl_->prefBool("pushModeSwitches", false)
                                ? engine::parsePushPerformanceMode(impl_->prefs.value(QStringLiteral("pushMode")).toString().toStdString())
                                : std::nullopt;
        const auto& mode = chosen;
        const bool automatic = autoModeSwitch() && impl_->prefBool("pushModeAuto", false);
        const engine::PushModeSwitches wanted = engine::pushInterfaceSwitches(mode && engine::pushModeIsConcrete(*mode) ? mode : std::nullopt, automatic);
        std::array<bool, engine::kPushInterfaceSwitchNames.size()> present{};
        for (std::size_t i = 0; i < present.size(); ++i) {
            const std::string narrow(engine::kPushInterfaceSwitchNames[i]);
            present[i] = d::inEnvironment(d::widen(engine::kPushInterfaceSwitchNames[i]), narrow);
        }
        if (!kit) {
            const auto steps = engine::pushInterfaceSwitchSteps(wanted, ours, present);
            for (std::size_t i = 0; i < steps.size(); ++i) {
                const auto& step = steps[i];
                const std::string narrow(step.name);
                const std::wstring name = d::widen(step.name);
                if (step.action == engine::PushSwitchAction::Set) {
                    const std::wstring value = d::widen(step.value);
                    const bool win32 = SetEnvironmentVariableW(name.c_str(), value.c_str()) != FALSE;
                    const bool qt = qputenv(narrow.c_str(), QByteArray(step.value.data(), static_cast<qsizetype>(step.value.size())));
                    if (win32 && qt) { ours[i] = true; set << QString::fromStdString(narrow); }
                    else veyra::log::warn("push-mode", "switches: could not set " + narrow);
                } else if (step.action == engine::PushSwitchAction::Unset) {
                    SetEnvironmentVariableW(name.c_str(), nullptr);
                    qunsetenv(narrow.c_str());
                    ours[i] = false;
                    unset << QString::fromStdString(narrow);
                } else if (step.action == engine::PushSwitchAction::UserOwned) {
                    kept << QString::fromStdString(narrow);
                }
            }
        }
        if (!set.isEmpty() || !unset.isEmpty() || !kept.isEmpty() || kit)
            veyra::log::info("push-mode", std::format("switches mode={} automatic={} set=[{}] removed=[{}] alreadyInEnvironment=[{}]{} (read when the next PS5 connection starts)",
                mode ? std::string(engine::pushPerformanceModeName(*mode)) : std::string("off"), automatic, set.join(' ').toStdString(),
                unset.join(' ').toStdString(), kept.join(' ').toStdString(), kit ? " not applied: VEYRA_TEST_RP_NO_SETTINGS_FILE" : ""));
        return {{"applied", !kit}, {"set", set}, {"unset", unset}, {"kept", kept}};
    }
    if (request != QLatin1String("plan")) return {};
    const QVariantMap report = pushAdapterCapabilities(impl_->displayRefreshHz, impl_->vrrState);
    engine::PushCalibrationInput in;
    auto& caps = in.caps;
    const QString vendor = report.value("vendor").toString();
    caps.vendor = vendor == QLatin1String("nvidia") ? engine::PushGpuVendor::Nvidia : vendor == QLatin1String("amd") ? engine::PushGpuVendor::Amd
                  : vendor == QLatin1String("intel") ? engine::PushGpuVendor::Intel : vendor == QLatin1String("other") ? engine::PushGpuVendor::Other
                  : engine::PushGpuVendor::Unknown;
    caps.dedicatedVramBytes = std::uint64_t(report.value("vramMB").toULongLong()) << 20;
    bool levelOk = false;
    const uint level = report.value("featureLevel").toString().toUInt(&levelOk, 16);
    caps.featureLevel = levelOk ? std::uint32_t(level) : 0u;
    caps.uma = calibrationCapability(report.value("uma"));
    caps.decodeH264 = calibrationCapability(report.value("decodeH264"));
    caps.decodeHevc = calibrationCapability(report.value("decodeHevc"));
    caps.decodeHevc10 = calibrationCapability(report.value("decodeHevc10"));
    caps.fgMethods = std::uint32_t(report.value("fgMethods").toUInt());
    caps.opticalFlows = std::uint32_t(report.value("opticalFlows").toUInt());
    caps.refreshHz = report.value("refreshHz").toDouble();
    caps.vrr = engine::pushVrrState(report.value("vrrState", 0).toInt());
    caps.hdrDisplay = calibrationCapability(report.value("hdrDisplay"));
    caps.hybridGpu = calibrationCapability(report.value("hybridGpu"));
    const QString power = report.value("power").toString();
    caps.power = power == QLatin1String("mains") ? engine::PushPowerSource::Mains : power == QLatin1String("battery") ? engine::PushPowerSource::Battery
                 : engine::PushPowerSource::Unknown;
    const int game = args.value("game").toInt();
    in.game = game == 30 ? engine::PushGameRate::Thirty : game == 60 ? engine::PushGameRate::Sixty : engine::PushGameRate::Unknown;
    const auto preferred = calibrationMethod(args.value("method").toString());
    in.preferredMethod = preferred ? *preferred : engine::PushFgMethod::PushFlow;
    const QVariantMap measured = args.value("measured").toMap();
    if (!measured.isEmpty()) {
        const auto measuredMethod = calibrationMethod(measured.value("method").toString());
        const int multiplier = measured.value("multiplier").toInt();
        in.measured = engine::pushCalibrationMeasurement(calibrationNumber(measured, "baseGpuMs"), calibrationNumber(measured, "totalGpuMs"),
            calibrationNumber(measured, "fgBatchGpuMs"), calibrationCounters(measured.value("start").toMap()), calibrationCounters(measured.value("end").toMap()),
            multiplier > 0 ? unsigned(multiplier) : 0u, measuredMethod ? *measuredMethod : engine::PushFgMethod::PushFlow, measuredMethod.has_value());
    }
    const engine::PushCalibrationPlan plan = engine::pushCalibrationPlan(in);
    QStringList unknown;
    for (const auto item : plan.unknown) unknown << calibrationText(item);
    const bool valid = caps.vendor != engine::PushGpuVendor::Unknown;
    const QString gpuClass = caps.uma == engine::PushCapability::Yes ? QStringLiteral("integrated") : caps.uma == engine::PushCapability::No ? QStringLiteral("dedicated")
                             : QStringLiteral("unknown");
    const QVariantMap out{{"valid", valid}, {"mode", modeName(plan.mode)}, {"frameGeneration", plan.frameGeneration},
        {"fgMethod", plan.fgMethodKnown ? calibrationMethodId(plan.fgMethod) : QString()}, {"fgMethodKnown", plan.fgMethodKnown},
        {"multiplier", plan.multiplier}, {"tier", plan.tier}, {"outputFps", plan.outputFps}, {"minAddedDelayMs", plan.minAddedDelayMs},
        {"enhancement", plan.enhancement}, {"codec", plan.codec == engine::PushStreamCodec::Hevc ? QStringLiteral("hevc") : QStringLiteral("h264")}, {"hdr", plan.hdr},
        {"modeReason", calibrationText(plan.modeReason)}, {"fgReason", calibrationText(plan.fgReason)}, {"codecReason", calibrationText(plan.codecReason)},
        {"hdrReason", calibrationText(plan.hdrReason)}, {"enhancementReason", calibrationText(plan.enhancementReason)}, {"unknown", unknown},
        {"measured", in.measured.valid}, {"smootherOffered", plan.smootherOffered}, {"smootherMultiplier", plan.smootherMultiplier},
        {"smootherOutputFps", plan.smootherOutputFps}, {"smootherDelayMs", plan.smootherDelayMs},
        {"vendor", vendor}, {"gpuClass", gpuClass}, {"vramGB", caps.vramKnown() ? int((caps.dedicatedVramBytes + (std::uint64_t(1) << 29)) >> 30) : 0},
        {"refreshHz", caps.refreshHz}, {"vrrState", int(caps.vrr)}};
    veyra::log::info("push-calibration", std::format("plan valid={} game={} measured={} mode={} fg={} method={} multiplier={} codec={} hdr={} fgReason={} "
        "codecReason={} hdrReason={} unknown=[{}]", valid, game, in.measured.valid, engine::pushPerformanceModeName(plan.mode), plan.frameGeneration,
        plan.fgMethodKnown ? calibrationMethodId(plan.fgMethod).toStdString() : std::string("none"), plan.multiplier,
        plan.codec == engine::PushStreamCodec::Hevc ? "hevc" : "h264", plan.hdr, plan.fgReason, plan.codecReason, plan.hdrReason, unknown.join(',').toStdString()));
    return out;
}
// VEYRA_EXP_RP_GOVERNOR=1: once a second (from pushFlowBudgetWatch's tick) the latency governor is fed with
// what the snapshot already holds and its recommendation is logged ([slipstream-governor], at most every 10 s
// or at once when it changes). It is never applied: the governor then drops it after 5 s and leaves that knob
// alone for a while, which the log shows as "declined". Not fed while the optimiser or the quality test runs,
// while settings are being applied, or without a measured source rate.
// VEYRA_EXP_RP_GOVERNOR=apply: instead, PushGovernorControl (the arbiter: decode and presentation protection,
// then latency) changes the multiplier and PushFlow's tier through the same setters, logs every action and
// rollback ([slipstream-governor] mode=apply), puts the user's choice back before a stop, a measurement and the
// final save (pushGpuBudgetRestore), and keeps pushFlowBudgetWatch's tier step and the GPU budget's own apply
// mode aside. Refused, with one line, when the decode-queue delay is not measured (capture cards).
// VEYRA_EXP_RP_GPU_BUDGET=1: the same tick feeds PushGpuBudget and logs [slipstream-gpu-budget] the same way.
// =apply: its level is put in place through setFgMultiplier / setVfgQuality (the setters pushFlowBudgetWatch
// uses): a lower tier, a lower multiplier or generation off, and later the slow way back, never above what
// the user chose (a change the user makes becomes the new ceiling). While it applies, pushFlowBudgetWatch
// does not lower PushFlow's tier itself (the return value). Unverified on Windows.
bool QmlPlayerBridge::pushSlipstreamWatch() {
    // pushGovernorRestore: written only when it changes (see restoreText above).
    const auto noteRestore = [this](std::optional<engine::PushFgRequest> request, engine::PushFgRequest at = {}) {
        const QString want = request ? restoreText(*request, at) : QStringLiteral("{}");
        const QString have = impl_->prefs.value(QStringLiteral("pushGovernorRestore")).toString();
        if (have == want || (!request && have.isEmpty())) return;
        impl_->prefs[QStringLiteral("pushGovernorRestore")] = want;
        if (!impl_->savePrefs()) veyra::log::warn("slipstream-governor", "restore record not saved");
    };
    // A reduced level left on disk by a run that ended without its restore (a crash): the user's choice back
    // once, at the first tick, whatever the switches say now. Nothing happens unless the preference was written.
    {
        static bool checked = false;
        if (!checked) {
            checked = true;
            const QString text = impl_->prefs.value(QStringLiteral("pushGovernorRestore")).toString();
            if (!text.isEmpty() && text != QLatin1String("{}")) {
                const auto record = restoreParse(text);
                const auto method = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(settings().frameGenerationBackend));
                const auto inForce = [this, method] {
                    return engine::pushNormalizedFgRequest(engine::PushFgRequest{method, unsigned(std::max(1, fgMultiplier())),
                        method == engine::PushFgMethod::PushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u});
                };
                const engine::PushFgRequest before = inForce();
                // Only over exactly the reduced level it recorded: anything else is a choice made since.
                if (record && record->request.method == method && before == record->at) {
                    const auto& back = record->request;
                    if (back.multiplier != before.multiplier) setFgMultiplier(int(back.multiplier));
                    if (back.multiplier >= 2 && engine::pushFgTierCount(method) > 1 && back.tier != before.tier) setVfgQuality(int(back.tier));
                    const engine::PushFgRequest after = inForce();
                    veyra::log::info("slipstream-governor", std::format("{} the user's choice after an unfinished run: multiplier {} -> {} tier {} -> {}",
                        after == back ? "restored" : "could not restore (a setter refused)", before.multiplier, after.multiplier, before.tier, after.tier));
                } else {
                    veyra::log::info("slipstream-governor", "restore record dropped: unreadable, another format, another method, or the settings changed since");
                }
                impl_->prefs[QStringLiteral("pushGovernorRestore")] = QStringLiteral("{}");
                impl_->savePrefs();
            }
        }
    }
    const engine::PushExperimentMode governorMode = governorSwitch();
    const engine::PushExperimentMode budgetMode = gpuBudgetSwitch();
    if (governorMode == engine::PushExperimentMode::Off && budgetMode == engine::PushExperimentMode::Off) return false;
    const bool budgetApplies = budgetMode == engine::PushExperimentMode::Apply;
    const bool governorApplies = governorMode == engine::PushExperimentMode::Apply;
    auto& state = slipstreamWatchState();
    const auto& s = impl_->snapshot;
    // The optimiser and the calibration snapshot the settings in force when they start: the user's choice is put
    // back on the tick their preference appears (up to a second after it was written: that snapshot can still
    // hold a reduced level; the restore record below covers a crash in that window only).
    const bool measuring = engine::pushBenchmarkProbe().active() || impl_->engine.qualityTest().running || measurementPref(impl_->prefs);
    if ((budgetApplies || governorApplies) && !state.quitHooked) {
        // A reduced level must never be what the final save writes: put the user's choice back first. The
        // destructor's persistSession runs after aboutToQuit.
        state.quitHooked = true;
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { pushGpuBudgetRestore("quit"); });
    }
    if (!s.running || measuring) {
        if (state.active) veyra::log::info("slipstream-watch", measuring ? "paused: the optimiser or the quality test is running" : "stopped: nothing is playing");
        // The user's choice back before the optimiser or the quality test measures, or before the stopped
        // stream's settings are saved; then stand down (the optimiser's own changes are not the user's).
        if (budgetApplies || governorApplies) pushGpuBudgetRestore(measuring ? "measurement" : "stopped");
        state.reset();
        if (measuring) state.budget.reset();
        return false;
    }
    if (impl_->facade.applying() || s.applying || s.desired.revision != s.applied.revision) return state.budgetApplying || state.controlApplying;
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
        // With VEYRA_EXP_RP_GOVERNOR=apply the governor's arbiter is the only one that changes settings (it
        // runs its own copy of the budget for decode and presentation): this one only observes.
        if (budgetApplies && governorApplies && !state.supersededLogged) {
            state.supersededLogged = true;
            veyra::log::info("slipstream-gpu-budget", "apply superseded: VEYRA_EXP_RP_GOVERNOR=apply arbitrates; the budget observes");
        }
        state.budgetApplying = budgetApplies && canApply && !governorApplies;
        const auto step = state.budget.update(current, fed.budget, state.budgetApplying);
        bool applied = false;
        if (step.apply) {
            // The user's choice on disk before a reduced level can be saved; cleared when the request is back.
            noteRestore(step.target == step.request ? std::nullopt : std::optional<engine::PushFgRequest>(step.request), step.target);
            // One change at a time; the multiplier first (1 turns generation off), then the tier.
            if (step.target.multiplier != in.multiplier) setFgMultiplier(int(step.target.multiplier));
            if (engine::pushFgTierCount(method) > 1 && step.target.tier != tier) setVfgQuality(int(step.target.tier));
            applied = true;
        }
        if (state.budgetApplying && !step.apply && !state.budget.pendingRestore(current)) noteRestore(std::nullopt);
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

    if (governorApplies) {
        const auto method = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(settings().frameGenerationBackend));
        const engine::PushFgRequest current{method, in.multiplier, tier};
        // As for the budget: without the decode-queue delay (capture cards) protection can never see a healthy
        // period. Nothing is changed there; said once per stream.
        if (!engine::pushGpuBudgetCanApply(fed.budgetUnknown)) {
            state.controlApplying = false;
            if (!state.controlRefusedLogged) {
                state.controlRefusedLogged = true;
                veyra::log::info("slipstream-governor", "apply refused: the decode-queue delay is not measured for this source (a capture card); nothing is changed");
            }
            return state.budgetApplying;
        }
        if (!state.control) state.control.emplace(governorConfig());
        state.controlApplying = true;
        engine::PushControlSample sample;
        sample.nowMs = in.now100ns / 10000;
        sample.sourceFps = in.sourceFps;
        sample.latencyP95Ms = flow.softwareLatencyP95Ms.value_or(-1.0);
        sample.lossPercent = in.lossPercent;
        sample.networkJitterMs = in.jitterMs;   // -1 when not measured: then no latency step (its cause is unknown)
        sample.budget = fed.budget;
        const engine::PushControlTick tick = state.control->update(current, sample);
        bool refused = false;
        if (tick.apply) {
            // The user's choice on disk before any reduced level can be saved.
            if (!(tick.target == state.control->request())) noteRestore(state.control->request(), tick.target);
            // The setters the GPU budget uses. Generation off leaves the tier as it is.
            if (tick.target.multiplier != in.multiplier) setFgMultiplier(int(tick.target.multiplier));
            if (tick.target.multiplier >= 2 && engine::pushFgTierCount(method) > 1 && tick.target.tier != tier) setVfgQuality(int(tick.target.tier));
            const unsigned multiplierNow = unsigned(std::max(1, fgMultiplier()));
            const unsigned tierNow = method == engine::PushFgMethod::PushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u;
            if (!(engine::pushNormalizedFgRequest(engine::PushFgRequest{method, multiplierNow, tierNow}) == tick.target)) {
                state.control->applyRefused();
                refused = true;
            }
        }
        state.lastTick = tick;
        if (state.control->index() == 0 || !state.control->pendingRestore(engine::PushFgRequest{method, unsigned(std::max(1, fgMultiplier())),
                method == engine::PushFgMethod::PushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u}))
            noteRestore(std::nullopt);   // back at the request, or the user's own choice is in force
        // Every action and rollback at once (the control's minimum interval limits them); otherwise at most every
        // 10 s or when the reason changes.
        if (tick.apply || state.controlLog.due(in.now100ns, tick.reason)) {
            const auto& c = *state.control;
            const auto level = [](unsigned m, unsigned t) { return m < 2 ? std::string("off") : std::format("{}x/t{}", m, t); };
            const auto request = c.request();
            veyra::log::info("slipstream-governor", std::format("mode=apply action={} from={} to={} cause={} reason={} refused={} unplannedMs={} latencyP95Ms={} "
                "budgetMs={:.1f} level={} protectLevel={} budgetReason={} request={} frozen={}{} lockedOut={} rollbacks={} lockouts={} changes={} unavailable=[{}] "
                "session={}", tick.apply ? (tick.cause == engine::PushControlCause::Rollback ? "rollback" : "step") : "none",
                level(in.multiplier, tier), tick.apply ? level(tick.target.multiplier, tick.target.tier) : std::string("-"),
                engine::pushControlCauseName(tick.cause), tick.reason, refused ? 1 : 0, feedNumber(tick.unplannedMs), feedNumber(sample.latencyP95Ms),
                c.config().latencyBudgetMs, tick.index, tick.protectIndex, tick.budgetReason, level(request.multiplier, request.tier),
                c.frozenMultiplier() ? "multiplier" : "", c.frozenTier() ? (c.frozenMultiplier() ? ",tier" : "tier") : "",
                c.lockedOut(sample.nowMs) ? 1 : 0, c.rollbacks(), c.lockouts(), c.changes(), engine::pushFeedUnavailable(fed.budgetUnknown), s.sessionId));
        }
        return true;
    }

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
    // pushGovernorRestore: written only when it changes (see restoreText above).
    const auto noteRestore = [this](std::optional<engine::PushFgRequest> request, engine::PushFgRequest at = {}) {
        const QString want = request ? restoreText(*request, at) : QStringLiteral("{}");
        const QString have = impl_->prefs.value(QStringLiteral("pushGovernorRestore")).toString();
        if (have == want || (!request && have.isEmpty())) return;
        impl_->prefs[QStringLiteral("pushGovernorRestore")] = want;
        if (!impl_->savePrefs()) veyra::log::warn("slipstream-governor", "restore record not saved");
    };
    auto& state = slipstreamWatchState();
    const auto method = static_cast<engine::PushFgMethod>(static_cast<std::uint8_t>(settings().frameGenerationBackend));
    const unsigned multiplier = unsigned(std::max(1, fgMultiplier()));
    const unsigned tier = method == engine::PushFgMethod::PushFlow ? unsigned(std::clamp(vfgQuality(), 0, 2)) : 0u;
    // VEYRA_EXP_RP_GOVERNOR=apply: the same for a level the governor put in place (only one of the two applies).
    if (state.control) {
        if (const auto request = state.control->pendingRestore(engine::PushFgRequest{method, multiplier, tier})) {
            if (request->multiplier != multiplier) setFgMultiplier(int(request->multiplier));
            if (request->multiplier >= 2 && engine::pushFgTierCount(method) > 1 && request->tier != tier) setVfgQuality(int(request->tier));
            state.control->restored();
            noteRestore(std::nullopt);
            veyra::log::info("slipstream-governor", std::format("restored the user's choice ({}): multiplier {} -> {} tier {} -> {}", why, multiplier,
                request->multiplier, tier, request->tier));
            return;
        }
    }
    const auto back = state.budget.pendingRestore(engine::PushFgRequest{method, multiplier, tier});
    if (!back) return;
    if (back->multiplier != multiplier) setFgMultiplier(int(back->multiplier));
    if (engine::pushFgTierCount(method) > 1 && back->tier != tier) setVfgQuality(int(back->tier));
    state.budget.restored();
    noteRestore(std::nullopt);
    veyra::log::info("slipstream-gpu-budget", std::format("restored the user's choice ({}): multiplier {} -> {} tier {} -> {}", why, multiplier,
        back->multiplier, tier, back->tier));
}

// VEYRA_EXP_RP_GOVERNOR only: the governor's state for the statistics overlay, as new keys in pushLiveStats()
// (PushFps.cpp). Without the switch nothing is added. Unverified on Windows.
void pushSlipstreamLiveStats(QVariantMap& m) {
    const engine::PushExperimentMode mode = governorSwitch();
    if (mode == engine::PushExperimentMode::Off) return;
    const auto& state = slipstreamWatchState();
    m["governorMode"] = QString::fromLatin1(engine::pushExperimentModeName(mode));
    m["governorApplying"] = state.controlApplying;
    if (!state.control || !state.controlApplying) return;
    const auto& c = *state.control;
    const auto& t = state.lastTick;
    const auto level = c.ladder()[c.index() < c.ladder().count ? c.index() : 0];
    const auto request = c.request();
    m["governorMultiplier"] = int(level.multiplier);
    m["governorTier"] = int(level.tier);
    m["governorRequestMultiplier"] = int(request.multiplier);
    m["governorRequestTier"] = int(request.tier);
    m["governorReduced"] = c.index() > 0;
    m["governorCause"] = QString::fromLatin1(engine::pushControlCauseName(t.cause));
    m["governorReason"] = QString::fromLatin1(t.reason.data(), qsizetype(t.reason.size()));
    m["governorUnplannedMs"] = t.unplannedMs >= 0 ? QVariant(t.unplannedMs) : QVariant();
    m["governorBudgetMs"] = c.config().latencyBudgetMs;
    m["governorRollbacks"] = int(c.rollbacks());
    m["governorLockouts"] = int(c.lockouts());
    m["governorChanges"] = int(c.changes());
    m["governorFrozenMultiplier"] = c.frozenMultiplier();
    m["governorFrozenTier"] = c.frozenTier();
}
} // namespace veyra::ui
