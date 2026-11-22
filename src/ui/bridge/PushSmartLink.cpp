// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge for SmartLink (the adaptive networking experiment, VEYRA_EXP_RP_SMARTLINK; engine types in
// veyra/remoteplay/SmartLinkTypes.h, the running stream's latest decision in SmartLinkShared.h):
//   - pushSmartLink(): what the interface's "Connection" row shows (PushSmartLinkRow.qml), numbers only;
//   - preference pushSmartLink (0 off, the default / 1 advise / 2 automatic) -> VEYRA_EXP_RP_SMARTLINK
//     ("1" / "apply") for the next PS5 connection, only in a Slipstream build with PS5 Remote Play;
//   - the smartlink= key of the [slipstream-config] line (PushAutorun.cpp).
// Nothing here changes a Windows, adapter or router setting, and nothing here is logged or returned that
// names a network, a console, an address or an account. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/remoteplay/SmartLinkShared.h"
#include "veyra/ui/SlipstreamSettingsFile.h"
#include "veyra/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace veyra::ui {
namespace {
namespace sl = remoteplay::smartlink;

// The interface row and the environment switch exist only in a Slipstream build with PS5 Remote Play.
#if defined(VEYRA_SLIPSTREAM_BUILD) && defined(VEYRA_ENABLE_REMOTEPLAY)
constexpr bool kSmartLinkUi = true;
#else
constexpr bool kSmartLinkUi = false;
#endif

constexpr const wchar_t* kSmartLinkSwitchW = L"VEYRA_EXP_RP_SMARTLINK";
constexpr const char* kSmartLinkSwitch = "VEYRA_EXP_RP_SMARTLINK";

// Host monotonic milliseconds: the clock SmartLinkShared.h's publisher stamps updatedMs with (steady_clock).
std::int64_t smartLinkNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The switch as the PS5 session reads it (GetEnvironmentVariableW), ASCII; nullopt when unset.
std::optional<std::string> smartLinkSwitchText() {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(kSmartLinkSwitchW, value, 16);
    if (length == 0) return std::nullopt;
    if (length >= 16) return std::string("?");   // too long for any known value: SmartLinkTypes.h reads it as observe
    std::string text;
    for (DWORD i = 0; i < length; ++i) text.push_back(value[i] > 0 && value[i] < 128 ? static_cast<char>(value[i]) : '?');
    return text;
}
} // namespace

// Called by a QML timer once a second, only while a PS5 stream runs and the row is visible. One short lock
// in latestShared() (a copy of a few hundred bytes). active = the decision is fresh (isFresh); everything
// else is meaningful only then. advice: the Advice enumerator of the decision as a number (0 = none).
QVariantMap QmlPlayerBridge::pushSmartLink() const {
    const sl::SharedState state = sl::latestShared();
    const bool active = kSmartLinkUi && sl::isFresh(state, smartLinkNowMs());
    const sl::Decision decision = active ? state.decision : sl::Decision{};
    const auto number = [](double v) { return std::isfinite(v) && v >= 0 ? v : -1.0; };
    return {{"available", kSmartLinkUi},
            {"active", active},
            {"mode", QString::fromLatin1(sl::modeName(active ? state.mode : sl::Mode::Off))},
            {"profile", QString::fromLatin1(sl::profileName(decision.profile))},
            {"regime", QString::fromLatin1(sl::regimeName(decision.regime))},
            {"risk", std::isfinite(decision.stallRisk) ? std::clamp(decision.stallRisk, 0.0, 1.0) : 0.0},
            {"rttMs", number(decision.rttMs)},
            {"rttMaxMs", number(decision.rttMaxMs)},
            {"targetKbps", number(decision.targetKbps)},
            {"measuredKbps", number(decision.measuredKbps)},
            {"signalPercent", number(decision.signalPercent)},
            {"reorderMs", static_cast<int>(decision.reorderWindowMs)},
            {"audioTargetMs", static_cast<int>(decision.audioTargetMs)},
            {"netLimited", decision.networkLimited},
            {"headroom", decision.linkHeadroom},
            {"advice", static_cast<int>(decision.advice)}};
}

// Preference pushSmartLink (quick setup, row pushfps-smartlink): VEYRA_EXP_RP_SMARTLINK for the next PS5
// connection, as applyPushLanProfile (PushSlipstream.cpp) does for the LAN profile. The session reads the
// switch when it starts, so a running stream keeps what it started with. A value already in the environment
// (set by the user, or by slipstream.ini at start-up) is never touched; only a value this function set is
// changed or removed again. Not applied under VEYRA_TEST_RP_NO_SETTINGS_FILE (the hardware test kit gives
// every arm exactly its own switches). 0 (the default) sets nothing: exactly the behaviour without SmartLink.
void QmlPlayerBridge::applyPushSmartLink() {
    if constexpr (kSmartLinkUi) {
        namespace d = slipstream_file_detail;
        static bool ours = false;
        const int choice = impl_->prefInt("pushSmartLink", 0);
        const std::string_view want = choice == 1 ? std::string_view("1") : choice == 2 ? std::string_view("apply") : std::string_view();
        if (d::inEnvironment(L"VEYRA_TEST_RP_NO_SETTINGS_FILE", "VEYRA_TEST_RP_NO_SETTINGS_FILE")) {   // also when set empty
            if (!want.empty()) veyra::log::info("remoteplay-settings", "SmartLink preference not applied (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)");
            return;
        }
        const std::wstring name(kSmartLinkSwitchW);
        if (!want.empty()) {
            if (!ours && d::inEnvironment(name, kSmartLinkSwitch)) {
                veyra::log::info("remoteplay-settings", std::string("SmartLink preference=") + (choice == 2 ? "automatic" : "advise") +
                                                            " not applied: VEYRA_EXP_RP_SMARTLINK is already in the environment");
                return;
            }
            const std::wstring value = d::widen(want);
            const bool win32 = SetEnvironmentVariableW(name.c_str(), value.c_str()) != FALSE;
            const bool qt = qputenv(kSmartLinkSwitch, QByteArray(want.data(), static_cast<qsizetype>(want.size())));
            if (win32 && qt) {
                ours = true;
                veyra::log::info("remoteplay-settings", "SmartLink preference=" + std::string(choice == 2 ? "automatic" : "advise") + " set VEYRA_EXP_RP_SMARTLINK=" +
                                                            std::string(want) + " (read when the next PS5 connection starts)");
            } else {
                veyra::log::warn("remoteplay-settings", "SmartLink: could not set VEYRA_EXP_RP_SMARTLINK");
            }
        } else if (ours) {
            SetEnvironmentVariableW(name.c_str(), nullptr);
            qunsetenv(kSmartLinkSwitch);
            ours = false;
            veyra::log::info("remoteplay-settings", "SmartLink preference=off removed VEYRA_EXP_RP_SMARTLINK (read when the next PS5 connection starts)");
        }
    }
}

// [slipstream-config] smartlink=: the mode the running stream published when it published anything, else what
// the switch says now (the session reads it at its start). off | observe | apply.
const char* QmlPlayerBridge::pushSmartLinkConfigMode() const {
    const sl::SharedState state = sl::latestShared();
    if (state.sequence != 0) return sl::modeName(state.mode);
    const auto text = smartLinkSwitchText();
    return sl::modeName(sl::parseMode(text ? std::optional<std::string_view>(*text) : std::nullopt));
}

} // namespace veyra::ui
