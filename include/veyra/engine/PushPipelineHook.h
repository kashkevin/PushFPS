// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Slipstream test hook VEYRA_TEST_RP_PIPELINE=<key=value;key=value> (test-only, environment only; contract item 1):
// the pure part. Which keys exist, which values each takes, the order they are applied in (the profile first, then
// an extension's early key, the others as written) and the one log line
//   [slipstream-pipeline] applied=<n> refused=<n> detail=<key:ok|key:refused:reason,...>
// The image keys are here: sr, srModel, srTarget, clean, fg, fgMultiplier, hdr, profile, cap (the output cap: display =
// Follow display, off, or 30..360 FPS; stream "fps"). Another part of the app
// may add keys through a PipeHookExtension (the neural layer adds its own); without it such a key is refused as
// `unknown-key` (for example `nr:refused:unknown-key` in a build without the neural layer).
// The bridge (src/ui/bridge/PushPipelineHook.cpp) reads the environment, applies each key through the setters the
// interface uses and names a key it cannot apply. Tests: tests/remoteplay/PipelineHookTests.inc.
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veyra::engine {

struct PipeHookItem {
    std::string key, value;
    std::string refusal;      // empty = accepted so far; else unknown-key, unknown-model, bad-value, duplicate, or the bridge's reason
    bool applied = false;
};
inline constexpr std::size_t kPipeHookMaxBytes = 512, kPipeHookMaxItems = 16;

constexpr char pipeHookLower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
constexpr bool pipeHookOneOf(std::string_view value, std::initializer_list<std::string_view> set) {
    for (const auto candidate : set)
        if (value == candidate) return true;
    return false;
}
// The number in [lowest, highest], or -1 (digits only, at most four).
constexpr int pipeHookNumber(std::string_view value, int lowest, int highest) {
    if (value.empty() || value.size() > 4) return -1;
    int number = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return -1;
        number = number * 10 + (c - '0');
    }
    return number >= lowest && number <= highest ? number : -1;
}
// srModel: the letters and ids of the app's DLSS SR model choice (dlssSrModelChoices: 0 K, 1 M, 2 L); -1 otherwise.
constexpr int pipeHookSrModel(std::string_view value) {
    if (value == "k" || value == "0") return 0;
    if (value == "m" || value == "1") return 1;
    if (value == "l" || value == "2") return 2;
    return -1;
}
constexpr bool pipeHookBaseKey(std::string_view key) {
    return pipeHookOneOf(key, {"sr", "srModel", "srTarget", "clean", "fg", "fgMultiplier", "hdr", "profile", "cap"});
}
// value is already lower case.
constexpr bool pipeHookBaseValueOk(std::string_view key, std::string_view value) {
    if (key == "sr") return pipeHookOneOf(value, {"off", "dlss", "vsr-stream", "nis"});
    if (key == "srModel") return pipeHookSrModel(value) >= 0;
    if (key == "srTarget") return pipeHookOneOf(value, {"qhd", "4k"});
    if (key == "clean") return pipeHookNumber(value, 0, 100) >= 0;
    if (key == "hdr") return pipeHookOneOf(value, {"0", "1"});
    if (key == "fg") return pipeHookOneOf(value, {"off", "vfg", "dlss", "pushflow", "fsr"});
    if (key == "fgMultiplier") return pipeHookNumber(value, 2, 8) >= 0;
    if (key == "profile") return pipeHookOneOf(value, {"original", "clean-1440", "enhanced-1440", "max-fidelity", "ultra-low-latency"});
    if (key == "cap") return pipeHookOneOf(value, {"display", "off"}) || pipeHookNumber(value, 30, 360) >= 0;
    return false;
}

// Keys another part of the app adds. known / valueOk must not accept a base key; early (optional) is applied right
// after the profile wherever it is written.
struct PipeHookExtension {
    bool (*known)(std::string_view key) = nullptr;
    bool (*valueOk)(std::string_view key, std::string_view value) = nullptr;
    std::string_view early{};
};

// Every item of the text, the profile first, then the extension's early key. A key that is not known is logged as
// written when it has only letters, digits and '-', else as "invalid".
inline std::vector<PipeHookItem> pipeHookParse(std::string_view text, const PipeHookExtension* extension = nullptr) {
    std::vector<PipeHookItem> items;
    if (text.size() > kPipeHookMaxBytes) { items.push_back({"plan", "", "too-long", false}); return items; }
    const auto trim = [](std::string_view part) {
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
        return part;
    };
    const auto plainWord = [](std::string_view word) {
        if (word.empty() || word.size() > 24) return false;
        for (const char c : word)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
        return true;
    };
    const auto extended = [&](std::string_view key) { return extension && extension->known && extension->known(key); };
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find(';', begin);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view part = trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (part.empty()) continue;
        if (items.size() >= kPipeHookMaxItems) { items.push_back({"more", "", "too-many", false}); break; }
        const std::size_t equals = part.find('=');
        const std::string_view key = trim(part.substr(0, equals));
        PipeHookItem item;
        item.key = plainWord(key) ? std::string(key) : std::string("invalid");
        const bool base = pipeHookBaseKey(key);
        if (!base && !extended(key)) { item.refusal = "unknown-key"; items.push_back(std::move(item)); continue; }
        std::string value(equals == std::string_view::npos ? std::string_view{} : trim(part.substr(equals + 1)));
        for (char& c : value) c = pipeHookLower(c);
        const bool valueOk = base ? pipeHookBaseValueOk(key, value) : (extension->valueOk && extension->valueOk(key, value));
        if (!valueOk) { item.refusal = key == "srModel" ? "unknown-model" : "bad-value"; items.push_back(std::move(item)); continue; }
        item.value = std::move(value);
        for (const auto& earlier : items)
            if (earlier.key == item.key && earlier.refusal.empty()) item.refusal = "duplicate";
        items.push_back(std::move(item));
    }
    // The profile first, then the extension's early key; the other keys keep their order and override the profile.
    std::size_t front = 0;
    for (const std::string_view first : {std::string_view("profile"), extension ? extension->early : std::string_view{}}) {
        if (first.empty()) continue;
        for (std::size_t i = front; i < items.size(); ++i)
            if (items[i].key == first && items[i].refusal.empty()) {
                PipeHookItem moved = std::move(items[i]);
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(front), std::move(moved));
                ++front;
                break;
            }
    }
    return items;
}

// applied=<n> refused=<n> detail=<key:ok|key:refused:reason,...> (the bridge appends streamHdr= and videoHdr=).
inline std::string pipeHookLine(const std::vector<PipeHookItem>& items) {
    std::size_t applied = 0, refused = 0;
    std::string detail;
    for (const auto& item : items) {
        if (!detail.empty()) detail += ',';
        detail += item.key;
        if (item.applied && item.refusal.empty()) { ++applied; detail += ":ok"; }
        else { ++refused; detail += ":refused:" + (item.refusal.empty() ? std::string("not-applied") : item.refusal); }
    }
    return "applied=" + std::to_string(applied) + " refused=" + std::to_string(refused) + " detail=" + (detail.empty() ? std::string("none") : detail);
}

}  // namespace veyra::engine
