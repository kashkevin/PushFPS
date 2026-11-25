// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Release review REL16 SHOULD 1: stopping a helper thread must not hang its caller (the capture-stall witness's sampler is
// stopped from CaptureCardSource::close(), the engine thread, and can sit inside PDH, which loads Windows' and third-party
// performance-counter providers). pushJoinWithin waits at most `limit` for the thread to say it finished, then joins it;
// past the limit it detaches it and returns false: the caller then leaks what the thread still uses (never frees it) and
// switches the helper off for the process. Portable (tests/remoteplay/DisplayPathTests.inc).
#include <atomic>
#include <chrono>
#include <thread>

namespace veyra::engine {

// `finished` is set by the thread as its last action (store(true, release)).
inline bool pushJoinWithin(std::thread& t, const std::atomic<bool>& finished, std::chrono::milliseconds limit) {
    if (!t.joinable()) return true;
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!finished.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= until) { t.detach(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    t.join();
    return true;
}

}  // namespace veyra::engine
