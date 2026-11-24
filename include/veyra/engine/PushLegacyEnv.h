// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Windows half of PushLegacy.h: the process environment as a PushEnvLookup (slipstream.ini's switches are already in it:
// veyra/ui/SlipstreamSettingsFile.h runs before Qt, the bridge and the engine). Windows sources only.
#include "veyra/engine/PushLegacy.h"
#include <windows.h>
#include <optional>
#include <string>
#include <string_view>

namespace veyra::engine {
inline std::optional<std::string> pushReadEnvironment(std::string_view name) {
    const std::string key(name);
    char value[256]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetEnvironmentVariableA(key.c_str(), value, sizeof value);
    if (n == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string();
    }
    if (n >= sizeof value) return std::string("?");   // too long for any value these switches accept
    return std::string(value, n);
}
inline const PushEnvLookup& pushEnvironment() {
    static const PushEnvLookup lookup = [](std::string_view name) { return pushReadEnvironment(name); };
    return lookup;
}
}  // namespace veyra::engine
