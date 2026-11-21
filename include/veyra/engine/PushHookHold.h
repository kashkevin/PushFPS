// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <atomic>

namespace veyra::engine {
// True while an autorun hook step `sr`, `fg` or `quality` runs (src/ui/bridge/PushCompare.cpp sets it before the
// step's first hold and clears it before the user's settings are put back). While it is set the bridge does not
// write the session file (QmlPlayerBridge_p.h, persistSession), so a crash or a close in the middle of a hold
// leaves the user's own settings there, not the hold's; the restore at the end is saved as usual. Only the autorun
// runner (VEYRA_TEST_RP_AUTORUN) ever sets it: without it this is one relaxed load per session save. Portable;
// tests in tests/remoteplay/SrPlanTests.inc.
inline std::atomic<bool>& pushHookHoldActive() {
    static std::atomic<bool> active{false};
    return active;
}
// The session save's question: may it write now?
inline bool pushSessionSaveAllowed() { return !pushHookHoldActive().load(std::memory_order_relaxed); }
} // namespace veyra::engine
