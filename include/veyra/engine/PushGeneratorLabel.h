// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg", step 3: the generator label of the evidence lines (EngineController, a hook marked "stream ownfg").
#include "veyra/engine/EnhancementSettings.h"
#include <string>

namespace veyra::engine {

// Step 3 (the owner's comparison at identical settings): " generator=<nvidia-vfg|pushwarp|pushflow|dlss|...|off>" on every
// [capture-timing] / [fg-health] line, so one log with a minute of each generator is read with the same field names.
inline std::string pushGeneratorLabel(FrameGenerationBackend backend, unsigned multiplier) {
    if (multiplier < 2) return " generator=off";
    std::string name(frameGenerationBackendName(backend));
    for (auto& c : name) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c == ' ' ? '-' : c;
    return " generator=" + name;
}

}  // namespace veyra::engine
