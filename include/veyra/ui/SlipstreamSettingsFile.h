// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/Log.h"
#include "veyra/engine/SlipstreamSettings.h"

#include <QByteArray>
#include <QtGlobal>

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace veyra::ui {
// The Windows half of slipstream.ini (parser and rules: veyra/engine/SlipstreamSettings.h).
//
// Reads slipstream.ini from the folder of the executable and puts the experiment switches it asks for
// into the process environment, each only when the environment does not have it already. Call it once,
// on the main thread, after the log file is open and before Qt, the bridge or the engine start: the
// switches are read when a PS5 session starts, and nothing else may be reading the environment yet.
//
//   no file                         nothing happens and nothing is logged: the behaviour of a build without this
//   malformed file                  nothing is applied; one warning with the line number
//   VEYRA_TEST_RP_NO_SETTINGS_FILE  set in the environment: the file is not read (the hardware test kit
//                                   sets it, so that every test arm has exactly the switches the kit gave it)
//
// A switch is written twice: with SetEnvironmentVariableW for the code that reads it with
// GetEnvironmentVariableW, and with qputenv because the Qt DLLs keep their own C runtime copy of the
// environment (the executable links the static runtime) and qEnvironmentVariable reads that copy.
// Unverified on Windows.
namespace slipstream_file_detail {
inline std::wstring widen(std::string_view ascii) {
    std::wstring wide;
    wide.reserve(ascii.size());
    for (const char c : ascii) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return wide;
}
inline bool inEnvironment(const std::wstring& name, const std::string& narrowName) {
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(name.c_str(), nullptr, 0) > 0) return true;
    if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) return true;   // set, with an empty value
    return qEnvironmentVariableIsSet(narrowName.c_str());
}
// The whole file, or nothing. found: the file exists.
inline std::string readSmallFile(const std::wstring& path, bool& found) {
    found = false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        found = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        return {};
    }
    found = true;
    std::string text(engine::kSlipstreamMaxBytes + 1, '\0');   // one more than allowed: the parser then says "file too large"
    std::size_t have = 0;
    while (have < text.size()) {
        DWORD got = 0;
        if (!ReadFile(file, text.data() + have, static_cast<DWORD>(text.size() - have), &got, nullptr) || got == 0) break;
        have += got;
    }
    CloseHandle(file);
    text.resize(have);
    return text;
}
} // namespace slipstream_file_detail

inline void applySlipstreamSettingsFile() {
    namespace d = slipstream_file_detail;
    if (GetEnvironmentVariableW(L"VEYRA_TEST_RP_NO_SETTINGS_FILE", nullptr, 0) > 0) {
        log::info("remoteplay-settings", "slipstream.ini not read (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)");
        return;
    }
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return;
    path.resize(length);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.resize(slash + 1);
    path += L"slipstream.ini";

    bool found = false;
    const std::string text = d::readSmallFile(path, found);
    if (!found) return;
    const engine::SlipstreamSettings settings = engine::parseSlipstreamSettings(text);
    if (!settings.valid) {
        log::warn("remoteplay-settings", "slipstream.ini ignored, nothing applied: " + settings.error);
        return;
    }
    std::size_t applied = 0;
    std::string kept;   // names the environment already had: the environment wins
    for (const engine::SlipstreamSwitch& item : settings.switches) {
        const std::wstring name = d::widen(item.name);
        if (d::inEnvironment(name, item.name)) {
            if (!kept.empty()) kept += ' ';
            kept += item.name;
            continue;
        }
        const std::wstring value = d::widen(item.value);
        const bool win32 = SetEnvironmentVariableW(name.c_str(), value.c_str()) != FALSE;
        const bool qt = qputenv(item.name.c_str(), QByteArray(item.value.data(), static_cast<qsizetype>(item.value.size())));
        if (win32 && qt) ++applied;
        else log::warn("remoteplay-settings", "slipstream.ini: could not set " + item.name);
    }
    log::info("remoteplay-settings", "slipstream.ini " + engine::describeSlipstreamSettings(settings) + " applied=" + std::to_string(applied) +
                                         " alreadyInEnvironment=[" + kept + "]");
}
} // namespace veyra::ui
