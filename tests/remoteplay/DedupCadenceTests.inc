// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "lagfix" (the owner's capture-card scope): the content cadence of a 30 FPS game seen through a 60 Hz capture
// card (veyra/engine/ContentDedup.h, setLegacyCadence). Made-up picture sequences; nothing here is a measurement.
#include "veyra/engine/ContentDedup.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace dedup_cadence_tests {
using Pictures = std::function<std::uint64_t(int)>;
// Content times of the kept samples: n samples, transport period tr (100 ns).
inline std::vector<std::int64_t> content(bool legacy, int n, std::int64_t tr, const Pictures& picture) {
    veyra::engine::ContentDedup d;
    d.setLegacyCadence(legacy);
    std::vector<std::int64_t> out;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t p = picture(i);
        const auto decision = d.observe(&p, std::int64_t(i) * tr + 1000000, tr, i == 0);
        if (decision.keep) out.push_back(decision.contentPts);
    }
    return out;
}
// Steps where a content interval differs from the one before by more than 1 ms: what frame generation turns into
// uneven pair spans.
inline int unevenSteps(const std::vector<std::int64_t>& c) {
    int n = 0;
    for (std::size_t k = 2; k < c.size(); ++k)
        if (std::llabs((c[k] - c[k - 1]) - (c[k - 1] - c[k - 2])) > 10000) ++n;
    return n;
}
// A game frame every gameMs, sampled every trMs: the newest frame at each sample.
inline Pictures game(double gameMs, double trMs) {
    return [=](int i) { return std::uint64_t(std::floor(double(i) * trMs / gameMs + 1e-9)) + 1; };
}
constexpr double kT60 = 16.6833;
constexpr std::int64_t kTr60 = 166833;
}  // namespace dedup_cadence_tests

TEST(dedup_cadence_steady_rates_are_unchanged) {
    using namespace dedup_cadence_tests;
    // Every steady rate: the repairs never fire, the content times are the same to the 100 ns.
    for (const double g : {33.3667, 25.0, 22.22, 41.67, 50.0, 16.6833, 33.3}) CHECK(content(true, 1200, kTr60, game(g, kT60)) == content(false, 1200, kTr60, game(g, kT60)));
    CHECK(content(true, 1200, 83417, game(33.3667, 8.3417)) == content(false, 1200, 83417, game(33.3667, 8.3417)));   // 30 in 120 Hz
    // A 2,2,2,1,3 paced 30 FPS game and a game frame dropped every 2 s.
    const Pictures paced = [](int i) { static const int pattern[] = {2, 2, 2, 1, 3}; int k = 0, c = 0; while (c + pattern[k % 5] <= i) c += pattern[k++ % 5]; return std::uint64_t(k + 1); };
    CHECK(content(true, 1200, kTr60, paced) == content(false, 1200, kTr60, paced));
    const Pictures dropped = [](int i) { const int f = i / 2; return std::uint64_t(f - f / 60 + 1); };
    CHECK(content(true, 1200, kTr60, dropped) == content(false, 1200, kTr60, dropped));
}

TEST(dedup_cadence_a_falling_rate_relearns_in_three_pictures) {
    using namespace dedup_cadence_tests;
    // A 60 FPS menu, then the 30 FPS game: the old cadence placed every second picture one sample early for ~1.5 s.
    const Pictures menuThenGame = [](int i) { return std::uint64_t(i < 300 ? i + 1 : 300 + (i - 300) / 2 + 1); };
    CHECK(unevenSteps(content(true, 1200, kTr60, menuThenGame)) > 20);    // the defect (legacy)
    CHECK(unevenSteps(content(false, 1200, kTr60, menuThenGame)) <= 2);   // one switch: two steps
    const Pictures thirtyThenTwenty = [](int i) { return i < 300 ? std::uint64_t(i / 2 + 1) : std::uint64_t(150 + int(std::floor(double(i - 300) * kT60 / 50.0)) + 1); };
    CHECK(unevenSteps(content(true, 1200, kTr60, thirtyThenTwenty)) > 20);
    CHECK(unevenSteps(content(false, 1200, kTr60, thirtyThenTwenty)) <= 2);
}

TEST(dedup_cadence_restarts_cleanly_after_a_still_picture) {
    using namespace dedup_cadence_tests;
    // 30 FPS, 3 s of a still picture (held on the cadence), 30 FPS again: the fit restarted from the held sample and
    // drifted content times earlier; now the intervals after the restart are exactly the game's.
    const Pictures pause = [](int i) { return std::uint64_t(i < 200 ? i / 2 + 1 : i < 380 ? 100 : 100 + (i - 380) / 2 + 1); };
    const auto legacy = content(true, 1200, kTr60, pause), now = content(false, 1200, kTr60, pause);
    CHECK(unevenSteps(now) < unevenSteps(legacy));
    for (std::size_t k = now.size() - 200; k < now.size(); ++k) CHECK(std::llabs((now[k] - now[k - 1]) - 333667) <= 2);
}
