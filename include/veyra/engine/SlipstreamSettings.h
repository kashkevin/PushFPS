// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushPerformanceMode.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {
// slipstream.ini: the settings file of the low-latency PS5 Remote Play build ("Slipstream" is the
// temporary internal name of that work). It sits next to the executable and is read once at start-up
// (Windows part: veyra/ui/SlipstreamSettingsFile.h). This header is the portable parser; tests are in
// tests/remoteplay/PerformanceModeTests.inc. User documentation: docs/SETUP.md.
//
//   # comment (a '#' at the start of a line; there are no comments after a value)
//   mode = balanced                      ultra-low-latency | competitive | balanced | maximum-smoothness | automatic
//   ceiling = balanced                   for mode=automatic: the highest-latency mode it may use
//   lan = 1                              the LAN profile, with any mode (1/0, on/off, yes/no, true/false)
//   smartlink = observe                  SmartLink: observe | apply (the same as VEYRA_EXP_RP_SMARTLINK=<value>)
//   VEYRA_EXP_RP_PAD_POLL_MS = 1         any experiment switch, by its own name
//   VEYRA_EXP_RP_KBM_BINDS = "F=Square"  quotes are optional and removed
//
// Rules
//   - Only keys that begin with VEYRA_EXP_RP_ or VEYRA_TEST_RP_ become environment variables. No other
//     variable can be set from this file; any other key is ignored and reported once. Three of those can only come
//     from the real environment (slipstream_detail::fileDenied): VEYRA_TEST_RP_AUTORUN,
//     VEYRA_TEST_RP_AUTOCONNECT and VEYRA_TEST_RP_CAPTURE, so a line left in the file never connects, starts a
//     capture or presses buttons at every start;
//     the file reports them as unknown keys.
//   - A key written in the file wins over what `mode` and `lan` would set. The process environment wins
//     over the file (that part is the Windows glue: a variable that is already set is left alone).
//   - An empty value means "leave this switch unset", also when the mode asks for it.
//   - Several switches are on/off switches (slipstreamPresenceSwitch): the VEYRA_TEST_RP_LEGACY_* ones are read
//     as "set or not set" whatever their value, VEYRA_EXP_RP_ADAPTIVE_PHASE and VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS
//     only as "1". For those the file
//     reads 0, off, false and no as "unset" and writes every other value as 1. Switches that carry a value
//     (a poll interval, a sensitivity) keep what was written, a 0 included.
//   - The last of two equal keys counts.
//   - One malformed line makes the whole file void: nothing is applied, and the error names the line
//     number but never repeats its text.
inline constexpr std::size_t kSlipstreamMaxBytes = 65536;
inline constexpr std::size_t kSlipstreamMaxValue = 512;
inline constexpr std::size_t kSlipstreamMaxSwitches = 64;
inline constexpr std::size_t kSlipstreamMaxUnknownKeys = 16;

enum class SlipstreamOrigin : std::uint8_t { File, Mode, Lan };
struct SlipstreamSwitch {
    std::string name;
    std::string value;
    SlipstreamOrigin origin = SlipstreamOrigin::File;
};
struct SlipstreamSettings {
    bool valid = true;
    std::string error;                           // "line N: what is wrong", or "file too large"
    std::optional<PushPerformanceMode> mode;
    std::optional<PushPerformanceMode> ceiling;
    bool lan = false;
    std::vector<SlipstreamSwitch> switches;      // to set, unless the environment already has them
    std::vector<std::string> heldOff;            // written with an empty (or "off") value: not set by the mode either
    std::vector<std::string> unknownKeys;        // each once, at most kSlipstreamMaxUnknownKeys
};

// On/off switches: read by the application as present or absent (LEGACY_*) or as "1" (ADAPTIVE_PHASE,
// RESUME_AFTER_DEVICE_LOSS).
constexpr bool slipstreamPresenceSwitch(std::string_view name) {
    return name == "VEYRA_EXP_RP_ADAPTIVE_PHASE" || name == "VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS" || name.starts_with("VEYRA_TEST_RP_LEGACY_") || name == "VEYRA_TEST_RP_NO_SETTINGS_FILE";
}

namespace slipstream_detail {
constexpr std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool sameWord(std::string_view text, std::string_view lowerCaseWord) {
    if (text.size() != lowerCaseWord.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (lower(text[i]) != lowerCaseWord[i]) return false;
    return true;
}
constexpr bool offWord(std::string_view value) { return value == "0" || sameWord(value, "off") || sameWord(value, "false") || sameWord(value, "no"); }
constexpr bool onWord(std::string_view value) { return value == "1" || sameWord(value, "on") || sameWord(value, "true") || sameWord(value, "yes"); }
constexpr bool switchKey(std::string_view key) {
    std::string_view rest;
    if (key.starts_with("VEYRA_EXP_RP_")) rest = key.substr(13);
    else if (key.starts_with("VEYRA_TEST_RP_")) rest = key.substr(14);
    if (rest.empty()) return false;
    for (const char c : rest)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
// Test automation that connects and presses buttons: never from the file (see Rules above).
constexpr bool fileDenied(std::string_view key) {
    return key == "VEYRA_TEST_RP_AUTORUN" || key == "VEYRA_TEST_RP_AUTOCONNECT" || key == "VEYRA_TEST_RP_CAPTURE";   // CAPTURE: stream capture
}
constexpr bool keyCharacter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}
inline bool contains(const std::vector<std::string>& list, std::string_view name) {
    for (const std::string& item : list)
        if (item == name) return true;
    return false;
}
} // namespace slipstream_detail

inline SlipstreamSettings parseSlipstreamSettings(std::string_view text) {
    namespace d = slipstream_detail;
    const auto fail = [](std::size_t line, std::string_view what) {
        SlipstreamSettings bad;
        bad.valid = false;
        bad.error = line ? "line " + std::to_string(line) + ": " + std::string(what) : std::string(what);
        return bad;
    };
    if (text.size() > kSlipstreamMaxBytes) return fail(0, "file too large");
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

    SlipstreamSettings out;
    std::vector<SlipstreamSwitch> written;   // value empty: held off
    std::size_t lineNumber = 0;
    while (!text.empty()) {
        ++lineNumber;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        line = d::trim(line);
        if (line.empty() || line.front() == '#') continue;
        for (const char c : line)
            if (c != '\t' && (c < 0x20 || c > 0x7E)) return fail(lineNumber, "a character outside printable ASCII");
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) return fail(lineNumber, "no '=' (expected key=value; sections are not supported)");
        std::string_view key = d::trim(line.substr(0, equals));
        // `smartlink = observe|apply` is VEYRA_EXP_RP_SMARTLINK with its value passed through (not an on/off switch).
        if (d::sameWord(key, "smartlink")) key = "VEYRA_EXP_RP_SMARTLINK";
        std::string_view value = d::trim(line.substr(equals + 1));
        if (key.empty()) return fail(lineNumber, "no key before '='");
        for (const char c : key)
            if (!d::keyCharacter(c)) return fail(lineNumber, "a key may hold letters, digits, '_', '.' and '-' only");
        if (!value.empty() && value.front() == '"') {
            if (value.size() < 2 || value.back() != '"') return fail(lineNumber, "a quote that is not closed");
            value = value.substr(1, value.size() - 2);
        }
        if (value.size() > kSlipstreamMaxValue) return fail(lineNumber, "value too long");

        if (d::sameWord(key, "mode")) {
            out.mode = parsePushPerformanceMode(value);
            if (!out.mode) return fail(lineNumber, "unknown mode (ultra-low-latency, competitive, balanced, maximum-smoothness, automatic)");
        } else if (d::sameWord(key, "ceiling")) {
            out.ceiling = parsePushPerformanceMode(value);
            if (!out.ceiling || !pushModeIsConcrete(*out.ceiling)) return fail(lineNumber, "ceiling must be ultra-low-latency, competitive, balanced or maximum-smoothness");
        } else if (d::sameWord(key, "lan")) {
            if (d::onWord(value)) out.lan = true;
            else if (d::offWord(value)) out.lan = false;
            else return fail(lineNumber, "lan must be 1 or 0");
        } else if (d::switchKey(key) && !d::fileDenied(key)) {
            const bool off = value.empty() || (slipstreamPresenceSwitch(key) && d::offWord(value));
            SlipstreamSwitch* existing = nullptr;
            for (SlipstreamSwitch& item : written)
                if (item.name == key) existing = &item;
            if (!existing) {
                if (written.size() == kSlipstreamMaxSwitches) return fail(lineNumber, "too many switches");
                written.push_back(SlipstreamSwitch{std::string(key), {}, SlipstreamOrigin::File});
                existing = &written.back();
            }
            // A presence switch that is on is always written as 1: ADAPTIVE_PHASE is read as "1 enables".
            existing->value = off ? std::string() : slipstreamPresenceSwitch(key) ? std::string("1") : std::string(value);
        } else if (out.unknownKeys.size() < kSlipstreamMaxUnknownKeys && !d::contains(out.unknownKeys, key.substr(0, 48))) {
            out.unknownKeys.emplace_back(key.substr(0, 48));
        }
    }

    for (SlipstreamSwitch& item : written) {
        if (item.value.empty()) out.heldOff.push_back(std::move(item.name));
        else out.switches.push_back(std::move(item));
    }
    // A switch the file names, set or held off, is never touched by the mode or the LAN flag.
    const auto taken = [&out](std::string_view name) {
        if (slipstream_detail::contains(out.heldOff, name)) return true;
        for (const SlipstreamSwitch& item : out.switches)
            if (item.name == name) return true;
        return false;
    };
    if (out.mode)
        for (const PushModeSwitch& item : pushModeSwitches(*out.mode, false))
            if (!taken(item.name)) out.switches.push_back(SlipstreamSwitch{std::string(item.name), std::string(item.value), SlipstreamOrigin::Mode});
    if (out.lan)
        for (const PushModeSwitch& item : pushLanProfileSwitches())
            if (!taken(item.name)) out.switches.push_back(SlipstreamSwitch{std::string(item.name), std::string(item.value), SlipstreamOrigin::Lan});
    return out;
}

// One line for the log: what the file asked for. Values are cut at 48 characters.
inline std::string describeSlipstreamSettings(const SlipstreamSettings& settings) {
    if (!settings.valid) return "invalid: " + settings.error;
    std::string text = "mode=" + std::string(settings.mode ? pushPerformanceModeName(*settings.mode) : std::string_view("none"));
    if (settings.ceiling) text += " ceiling=" + std::string(pushPerformanceModeName(*settings.ceiling));
    text += settings.lan ? " lan=1" : " lan=0";
    text += " switches=[";
    for (std::size_t i = 0; i < settings.switches.size(); ++i) {
        const SlipstreamSwitch& item = settings.switches[i];
        if (i) text += ' ';
        text += item.name + '=' + item.value.substr(0, 48);
        text += item.origin == SlipstreamOrigin::File ? "" : item.origin == SlipstreamOrigin::Mode ? "(mode)" : "(lan)";
    }
    text += "] off=[";
    for (std::size_t i = 0; i < settings.heldOff.size(); ++i) text += (i ? " " : "") + settings.heldOff[i];
    text += "] unknown=[";
    for (std::size_t i = 0; i < settings.unknownKeys.size(); ++i) text += (i ? " " : "") + settings.unknownKeys[i];
    text += ']';
    return text;
}
} // namespace veyra::engine
