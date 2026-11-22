// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <cstddef>
#include <string_view>

namespace veyra::ui {
// Whether this copy may look for, offer and install releases of the official project (UpdateChecker).
//
// A Slipstream development build reports a version such as 2.1.5-pushfps.<run>, and the official project
// publishes 3.x releases, so the unguarded updater would offer to replace the development build with the
// official one, and install it in one click. Two things turn the updater off, and nothing else changes:
//
//   SLIPSTREAM_BUILD (CMake option, OFF by default; the build workflow sets it for development builds)
//       the build never checks, offers or installs. No switch can turn it back on.
//   VEYRA_EXP_RP_NO_UPDATE (environment, or a line in slipstream.ini)
//       any build stops checking while the switch is set to something other than "", 0, off, no, false.
//
// With neither, the policy is Enabled: exactly the behaviour of an official build. Pure and portable so
// that the decision is tested without Qt (tests/remoteplay/UpdatePolicyTests.inc).
enum class UpdatePolicy { Enabled, SlipstreamBuild, SwitchedOff };

inline constexpr std::string_view kUpdateSwitchName = "VEYRA_EXP_RP_NO_UPDATE";

namespace update_policy_detail {
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr std::string_view trimmed(std::string_view text) {
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    return text;
}
constexpr bool sameWord(std::string_view text, std::string_view word) {
    if (text.size() != word.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (lower(text[i]) != word[i]) return false;
    return true;
}
// The words that mean "this switch asks for nothing".
constexpr bool offWord(std::string_view value) {
    const std::string_view text = trimmed(value);
    return text.empty() || sameWord(text, "0") || sameWord(text, "off") || sameWord(text, "no") || sameWord(text, "false");
}
} // namespace update_policy_detail

// slipstreamBuild: the build was compiled as a Slipstream development build.
// switchSet / switchValue: VEYRA_EXP_RP_NO_UPDATE as found in the environment of the process.
constexpr UpdatePolicy updatePolicy(bool slipstreamBuild, bool switchSet, std::string_view switchValue) {
    if (slipstreamBuild) return UpdatePolicy::SlipstreamBuild;
    if (switchSet && !update_policy_detail::offWord(switchValue)) return UpdatePolicy::SwitchedOff;
    return UpdatePolicy::Enabled;
}

constexpr bool updatesAllowed(UpdatePolicy policy) { return policy == UpdatePolicy::Enabled; }

// The text after "[update-check] disabled: " in the log; empty when updates are allowed.
constexpr std::string_view updatePolicyReason(UpdatePolicy policy) {
    switch (policy) {
    case UpdatePolicy::Enabled: return {};
    case UpdatePolicy::SlipstreamBuild: return "Slipstream development build";
    case UpdatePolicy::SwitchedOff: return "VEYRA_EXP_RP_NO_UPDATE is set";
    }
    return "unknown policy";
}
} // namespace veyra::ui
