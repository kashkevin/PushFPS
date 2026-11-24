// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Package 15, TASK 3: the delay readout split into picture + first generated frame + schedule wait + margin
// (include/veyra/engine/PushDelaySplit.h). The parts of every frame add up to its total, so the window's means add up to
// the readout.
#include "veyra/engine/PushDelaySplit.h"
#include <cmath>
#include <cstdint>

namespace delaysplit_tests {
using namespace veyra::engine;
inline double sumOf(const PushDelayParts& p) { return p.pictureMs + p.firstGeneratedMs + p.waitMs + p.marginMs; }

TEST(delaysplit_interpolating_3x_at_30_fps) {
    // 31 ms readout: picture ready 2 ms after arrival, first generated frame 7 ms later, B after both generated frames.
    const double T = 33.3667;
    const auto p = pushDelayParts(31.0, 2.0, 9.0, 2, 3, T);
    CHECK(p.split);
    CHECK(std::abs(p.pictureMs - 2.0) < 1e-9 && std::abs(p.firstGeneratedMs - 7.0) < 1e-9);
    CHECK(std::abs(p.waitMs - std::min(2.0 / 3.0 * T, 31.0 - 9.0)) < 1e-9);   // 2T/3 = 22.2 ms
    CHECK(std::abs(sumOf(p) - 31.0) < 1e-9 && p.marginMs >= 0.0);
}

TEST(delaysplit_lower_delay_waits_one_slot) {
    const double T = 33.3667;
    const auto p = pushDelayParts(21.0, 2.0, 9.0, 1, 3, T);   // [I(2/3), B, P]: one generated frame before B
    CHECK(std::abs(p.waitMs - T / 3.0) < 1e-9);
    CHECK(std::abs(sumOf(p) - 21.0) < 1e-9);
}

TEST(delaysplit_parts_never_exceed_the_total) {
    // A phase too short for the formula: the wait is cut, margin is 0, the sum is the total.
    auto p = pushDelayParts(20.0, 2.0, 9.0, 2, 3, 33.3667);
    CHECK(std::abs(p.waitMs - 11.0) < 1e-9 && p.marginMs == 0.0 && std::abs(sumOf(p) - 20.0) < 1e-9);
    // Readiness seen after the Present returned (a late look): picture takes the whole total.
    p = pushDelayParts(5.0, 6.0, 7.0, 2, 3, 33.3667);
    CHECK(p.pictureMs == 5.0 && p.firstGeneratedMs == 0.0 && p.waitMs == 0.0 && p.marginMs == 0.0);
    // A pair without generated frames: no first generated frame, no wait.
    p = pushDelayParts(12.0, 3.0, -1.0, 0, 1, 33.3667);
    CHECK(p.firstGeneratedMs == 0.0 && p.waitMs == 0.0 && std::abs(p.marginMs - 9.0) < 1e-9);
    // Readiness never seen: unsplit, the whole total as margin.
    p = pushDelayParts(30.0, -1.0, -1.0, 2, 3, 33.3667);
    CHECK(!p.split && p.marginMs == 30.0);
    // Nonsense total: nothing.
    p = pushDelayParts(std::nan(""), 1.0, 2.0, 2, 3, 33.0);
    CHECK(!p.split && sumOf(p) == 0.0);
}

TEST(delaysplit_window_means_add_up_to_the_readout) {
    PushDelaySplitSums w;
    CHECK(w.mean(w.total) == -1.0 && w.splitShare() == 0.0);
    std::uint32_t seed = 99;
    double totals = 0;
    for (int i = 0; i < 90; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double total = 20.0 + double(seed % 2000) / 100.0, ready = double(seed % 300) / 100.0;
        const bool unseen = i % 17 == 0;
        const auto p = pushDelayParts(total, unseen ? -1.0 : ready, ready + 6.5, i % 5 == 0 ? 1u : 2u, 3, 33.3667);
        w.add(total, p);
        totals += total;
    }
    const double readout = totals / 90.0;
    CHECK(std::abs(w.mean(w.total) - readout) < 1e-9);
    const double parts = w.mean(w.picture) + w.mean(w.firstGenerated) + w.mean(w.wait) + w.mean(w.margin);
    CHECK(std::abs(parts - readout) < 1e-9);
    CHECK(w.frames == 90 && w.split == 84 && std::abs(w.splitShare() - 84.0 / 90.0) < 1e-12);
}

}  // namespace delaysplit_tests
