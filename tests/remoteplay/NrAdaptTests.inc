// SPDX-License-Identifier: GPL-3.0-or-later
// Adaptive neural layer under frame generation (veyra/engine/PushNrAdapt.h, VEYRA_EXP_RP_NR_ADAPT). Included by
// CoreTests.cpp directly above main().
#include "veyra/engine/PushNrAdapt.h"
#include "veyra/engine/AutoNrReuseController.h"
#include <cstring>
namespace {
namespace pna = veyra::engine;
pna::PushNrAdaptInput nrAdaptEligible() {
    pna::PushNrAdaptInput in;
    in.switchOn = true; in.live = true; in.image = false; in.neuralLayer = true; in.frameGeneration = true;
    in.presentSinkFg = false; in.ngxRuntime = true; in.userReuseOff = true;
    return in;
}
}  // namespace

TEST(nr_adapt_sets_auto_only_for_live_layer_with_graph_frame_generation){
    const auto in = nrAdaptEligible();
    CHECK(pna::pushNrAdaptForcesAuto(in));
    CHECK(pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::PairBudget);
    CHECK(std::strcmp(pna::pushNrAdaptReasonName(pna::pushNrAdaptReason(in)), "pair-budget") == 0);
}
TEST(nr_adapt_each_condition_keeps_the_user_setting_with_its_reason){
    // The switch unset: nothing changes, whatever else holds.
    auto in = nrAdaptEligible(); in.switchOn = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::SwitchOff);
    in = nrAdaptEligible(); in.live = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NotLive);
    in = nrAdaptEligible(); in.image = true;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NotLive);
    in = nrAdaptEligible(); in.neuralLayer = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NoNeuralLayer);
    // Frame generation off: no pair budget to protect, the layer runs every frame as before.
    in = nrAdaptEligible(); in.frameGeneration = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NoFrameGeneration);
    in = nrAdaptEligible(); in.presentSinkFg = true;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::PresentSinkFg);
    in = nrAdaptEligible(); in.ngxRuntime = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NotNgx);
    // A reuse mode the user chose (Auto or a fixed share) is never replaced.
    in = nrAdaptEligible(); in.userReuseOff = false;
    CHECK(!pna::pushNrAdaptForcesAuto(in) && pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::UserChoice);
    // The first rule that applies is the one reported.
    in = pna::PushNrAdaptInput{};
    CHECK(pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::SwitchOff);
    in.switchOn = true;
    CHECK(pna::pushNrAdaptReason(in) == pna::PushNrAdaptReason::NotLive);
    for (unsigned r = 0; r <= unsigned(pna::PushNrAdaptReason::PairBudget); ++r)
        CHECK(std::strcmp(pna::pushNrAdaptReasonName(pna::PushNrAdaptReason(r)), "unknown") != 0);
}
TEST(nr_adapt_pair_rate_prefers_a_measured_content_rate){
    // 30 fps game in the 59.94 stream with duplicates removed: the pairs are 33 ms apart.
    CHECK(pna::pushNrAdaptPairFps(59.94, 29.97) == 29.97);
    // Still measuring (0), unusable or above what arrives: the transport rate.
    CHECK(pna::pushNrAdaptPairFps(59.94, 0.0) == 59.94);
    CHECK(pna::pushNrAdaptPairFps(59.94, std::nan("")) == 59.94);
    CHECK(pna::pushNrAdaptPairFps(59.94, 120.0) == 59.94);
    CHECK(pna::pushNrAdaptPairFps(59.94, 0.5) == 59.94);
    // A content rate a hair above the transport (rounding of two measurements) still counts.
    CHECK(pna::pushNrAdaptPairFps(59.94, 60.0) == 60.0);
    // Nothing usable: 0.
    CHECK(pna::pushNrAdaptPairFps(0.0, 0.0) == 0.0);
    CHECK(pna::pushNrAdaptPairFps(std::nan(""), -3.0) == 0.0);
    CHECK(pna::pushNrAdaptPairFps(-1.0, 30.0) == 30.0);
}
TEST(nr_adapt_budget_is_eighty_percent_of_the_pair_interval){
    const double b60 = pna::pushNrAdaptBudgetMs(59.94, 0.0);
    CHECK(b60 > 13.34 && b60 < 13.35);  // 0.8 x 16.683 ms
    const double b30 = pna::pushNrAdaptBudgetMs(59.94, 29.97);
    CHECK(b30 > 26.68 && b30 < 26.70);  // 0.8 x 33.367 ms
    CHECK(pna::pushNrAdaptBudgetMs(0.0, 0.0) == 0.0);
    // The same share as the upstream controller's transport budget (EngineController: 800 / transportFps).
    CHECK(std::fabs(pna::pushNrAdaptBudgetMs(60.0, 0.0) - 800.0 / 60.0) < 1e-9);
}
TEST(nr_adapt_budget_drives_the_existing_controller_with_hysteresis){
    // AutoNrReuseController is upstream's (unchanged); this checks what the pair budget makes it do.
    // 100 ns units; the controller looks every 0.25 s after a 1.25 s settle.
    const std::int64_t s = 10000000;
    // (a) 60 fps stream, mean processing 15 ms per source frame (layer 5.5 ms of it) against 13.35 ms: over for
    // 0.5 s -> one level up (reuse 1 frame in 4), the reason being the measured overrun.
    {
        pna::AutoNrReuseController c; c.reset(0);
        const double budget = pna::pushNrAdaptBudgetMs(59.94, 0.0);
        std::optional<unsigned> changed;
        std::int64_t t = 0;
        for (; t <= 3 * s && !changed; t += s / 4) changed = c.observe(t, 15.0, 60, 5.5, budget);
        CHECK(changed && *changed == 1u && c.level() == 1u);
        CHECK(t > s);  // never before the settle time plus the 0.5 s over
    }
    // (b) the same 15 ms for a 30 fps game whose pairs are 33 ms apart: under the pair budget, nothing changes,
    // where the transport budget (13.35 ms) would have stepped up.
    {
        pna::AutoNrReuseController pair; pair.reset(0);
        pna::AutoNrReuseController transport; transport.reset(0);
        const double pairBudget = pna::pushNrAdaptBudgetMs(59.94, 29.97), transportBudget = 800.0 / 59.94;
        bool pairChanged = false, transportChanged = false;
        for (std::int64_t t = 0; t <= 5 * s; t += s / 4) {
            pairChanged = pair.observe(t, 15.0, 30, 5.5, pairBudget).has_value() || pairChanged;
            transportChanged = transport.observe(t, 15.0, 60, 5.5, transportBudget).has_value() || transportChanged;
        }
        CHECK(!pairChanged && pair.level() == 0u);
        CHECK(transportChanged && transport.level() >= 1u);
    }
    // (c) hysteresis: at level 1 with 12 ms mean, going back to level 0 would add 0.25 x 5.5 = 1.4 ms -> 13.4 ms,
    // above 0.85 x budget (11.3 ms): it stays at 1, no bouncing. At 8 ms it steps down after 3 s.
    {
        pna::AutoNrReuseController c; c.reset(0, 1);
        const double budget = pna::pushNrAdaptBudgetMs(59.94, 0.0);
        bool changed = false;
        for (std::int64_t t = 0; t <= 8 * s; t += s / 4) changed = c.observe(t, 12.0, 60, 5.5, budget).has_value() || changed;
        CHECK(!changed && c.level() == 1u);
        std::optional<unsigned> down;
        std::int64_t t = 9 * s;
        const std::int64_t start = t;
        for (; t <= 20 * s && !down; t += s / 4) down = c.observe(t, 8.0, 60, 5.5, budget);
        CHECK(down && *down == 0u);
        CHECK(t - start >= 3 * s);
    }
    // (d) no rate known: budget 0, the controller does nothing at any load.
    {
        pna::AutoNrReuseController c; c.reset(0);
        bool changed = false;
        for (std::int64_t t = 0; t <= 5 * s; t += s / 4) changed = c.observe(t, 40.0, 60, 5.5, pna::pushNrAdaptBudgetMs(0.0, 0.0)).has_value() || changed;
        CHECK(!changed && c.level() == 0u);
    }
}
