// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: the performance-mode work ("Slipstream" is its temporary internal name).
//   - the LAN profile preference -> the experiment switch a PS5 session reads when it starts;
//   - Automatic mode (only with VEYRA_EXP_AUTO_MODE=1): engine::PushAutomaticMode fed with numbers the
//     snapshot already holds;
//   - what this PC offers, for the first-launch card.
// The modes themselves are applied by the interface (qml/Veyra/PushModes.js) through the settings that
// already exist; nothing here changes a setting. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushPerformanceMode.h"
#include "veyra/ui/SlipstreamSettingsFile.h"
#include <array>
#include <chrono>
#include <optional>

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
    veyra::log::info("push-hardware", std::format("vendor={:#06x} decodeKnown={} h264={} hevc={} hevc10={} fgBackends={} displayHz={:.2f} vrrState={} "
        "networkKnown={} ethernet={} wifi={} routeKnown={} directLan={} link={}", vendor, decode.value("known").toBool(), decode.value("h264").toBool(),
        decode.value("hevc").toBool(), decode.value("hevc10").toBool(), backends.size(), impl_->displayRefreshHz, impl_->vrrState, networkKnown, ethernet,
        wifi, routeKnown, routeDirectLan, routeLink.toStdString()));
    return out;
}
} // namespace veyra::ui
