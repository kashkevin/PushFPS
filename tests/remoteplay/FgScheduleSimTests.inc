// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "lagfix": the scheduling simulator (FgScheduleSim.h). Evidence about the program's LOGIC only: every cost and
// jitter is made up; nothing here is a measurement of a GPU, a capture card, a PS5 or a display.
#include "FgScheduleSim.h"
#include <iostream>
// Under MSVC FgScheduleSim.h brings in <d3d12.h> and with it <windows.h>, whose rpcndr.h / windef.h define `small`,
// `near` and `far` as macros; earlier .inc files use those words as names. Include this file last, and drop them here
// in case one is added after it.
#ifdef small
#undef small
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

namespace fgsim_tests {
// The table of every configuration, printed once (the CI log keeps it).
inline void printTable(bool package13) {
    namespace F = fgsim;
    std::cout << "\n[fg-schedule-sim] 29.97 FPS content at 3X, 4K-like costs, 120 s per row, seed 7. A MODEL of the program's "
                 "decisions, not a measurement. "
              << (package13 ? "Parts as Package 13 runs them." : "Parts as this branch runs them.") << "\n";
    for (const F::Source source : {F::Source::Card, F::Source::Stream})
        for (const F::Costs& costs : F::costSets()) {
            std::cout << "== " << (source == F::Source::Card ? "capture card" : "PS5 stream") << ", costs " << costs.name << "\n" << F::header() << "\n";
            for (const F::Config& config : F::reportConfigs(package13)) std::cout << F::row(config.name, F::simulate(costs, source, config, 7, 120)) << "\n";
        }
}
inline const fgsim::Costs& costsNamed(const char* name) {
    static const auto sets = fgsim::costSets();
    for (const auto& c : sets)
        if (std::string(c.name) == name) return c;
    throw std::runtime_error("no cost set");
}
}  // namespace fgsim_tests

TEST(fgsim_prints_the_package13_table) {
    fgsim_tests::printTable(true);
}

TEST(fgsim_is_deterministic_and_the_same_costs_reach_every_configuration) {
    namespace F = fgsim;
    const F::Costs& c = fgsim_tests::costsNamed("spikes");
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    const F::Result a = F::simulate(c, F::Source::Card, d, 11, 30), b = F::simulate(c, F::Source::Card, d, 11, 30);
    CHECK(a.realMeanMs == b.realMeanMs && a.genShownPerS == b.genShownPerS && a.offShare == b.offShare);
    // The costs are drawn before any decision: the same seed gives the same numbers whatever the configuration.
    const auto x = F::drawCosts(c, 11, 50, 1.5, 333667.0), y = F::drawCosts(c, 11, 50, 1.5, 333667.0);
    for (std::size_t i = 0; i < x.size(); ++i) CHECK(x[i].base == y[i].base && x[i].gen[0] == y[i].gen[0] && x[i].jitterMs == y[i].jitterMs);
}

TEST(fgsim_default_shows_every_generated_frame_when_the_gpu_has_room) {
    namespace F = fgsim;
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    for (const F::Source s : {F::Source::Card, F::Source::Stream}) {
        const F::Result r = F::simulate(fgsim_tests::costsNamed("comfortable"), s, d, 7, 60);
        CHECK(r.genShownPerS > 59.5 && r.genDroppedPerS == 0.0 && r.skippedPerS == 0.0);
        // The real frame waits for the interpolation: about (M-1)/M x 33.4 = 22.2 ms plus base and fg1.
        CHECK(r.realMeanMs > 28.0 && r.realMeanMs < 40.0);
    }
}

// Package 13's traps, as the simulator finds them (the program parts these describe change in the commits after this
// one; the checks below run the parts exactly as Package 13 does).
TEST(fgsim_package13_phase_on_a_stream_can_hold_real_frames_longer_than_the_fixed_phase) {
    namespace F = fgsim;
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    const F::Config phase{"phase", F::parts(false, true, false, false), false, true};
    // When the first generated frame takes more than a third of the interval (tight), or with spikes, the learned
    // phase goes past interval + base cost (LivePairLatency's allowance plus LiveDeadlineMargin's extra).
    for (const char* costs : {"tight", "spikes"}) {
        const F::Result base = F::simulate(fgsim_tests::costsNamed(costs), F::Source::Stream, d, 7, 120);
        const F::Result p = F::simulate(fgsim_tests::costsNamed(costs), F::Source::Stream, phase, 7, 120);
        CHECK(p.realP95Ms > base.realP95Ms + 1.0);
    }
}

TEST(fgsim_package13_grace_shows_fewer_generated_frames) {
    namespace F = fgsim;
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    const F::Config grace{"grace", F::parts(false, false, false, true), false, true};
    for (const F::Source s : {F::Source::Card, F::Source::Stream}) {
        const F::Result base = F::simulate(fgsim_tests::costsNamed("spikes"), s, d, 7, 120);
        const F::Result g = F::simulate(fgsim_tests::costsNamed("spikes"), s, grace, 7, 120);
        CHECK(g.genShownPerS < base.genShownPerS);
    }
}

TEST(fgsim_package13_hold_makes_a_stream_less_even) {
    namespace F = fgsim;
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    const F::Config hold{"hold", F::parts(true, false, false, false), false, true};
    const F::Result base = F::simulate(fgsim_tests::costsNamed("near-limit"), F::Source::Stream, d, 7, 120);
    const F::Result h = F::simulate(fgsim_tests::costsNamed("near-limit"), F::Source::Stream, hold, 7, 120);
    // Fewer generated frames (admitted pairs stay held until 8 in a row) and longer gaps.
    CHECK(h.genShownPerS < base.genShownPerS);
    CHECK(h.longestMs > base.longestMs + 5.0);
}

TEST(fgsim_package13_headroom_can_delay_real_frames) {
    namespace F = fgsim;
    const F::Config d{"default", F::parts(false, false, false, false), false, true};
    const F::Config headroom{"headroom", F::parts(false, false, true, false), false, true};
    // Smoothness Mode steps 3X down to 2X after rejected pairs: the card's rejection hold no longer shows real frames
    // early, so they wait for the 2X phase.
    const F::Result base = F::simulate(fgsim_tests::costsNamed("near-limit"), F::Source::Card, d, 7, 120);
    const F::Result h = F::simulate(fgsim_tests::costsNamed("near-limit"), F::Source::Card, headroom, 7, 120);
    CHECK(h.twoXShare > 0.5);
    CHECK(h.realMeanMs > base.realMeanMs);
}

// --- this branch's rules (stream "lagfix") ------------------------------------------------------------------------------
TEST(fgsim_prints_the_lagfix_table) {
    fgsim_tests::printTable(false);
}

namespace fgsim_tests {
inline bool same(const fgsim::Result& a, const fgsim::Result& b) {
    return a.realMeanMs == b.realMeanMs && a.realP95Ms == b.realP95Ms && a.genShownPerS == b.genShownPerS && a.genDroppedPerS == b.genDroppedPerS &&
           a.offShare == b.offShare && a.longestMs == b.longestMs && a.holdsPerMin == b.holdsPerMin && a.twoXShare == b.twoXShare;
}
}  // namespace fgsim_tests

// No part of VEYRA_EXP_RP_FG_ULL may show fewer generated frames, make intervals less even or delay a real frame: each
// is inert now (pushUllPartsInForce), so every configuration of the switch plays exactly as the default at every cost
// set, on a card and on a stream. The same check on the parts as Package 13 ran them fails (second half).
TEST(fgsim_lagfix_every_fg_ull_configuration_plays_as_the_default) {
    namespace F = fgsim;
    for (const F::Source s : {F::Source::Card, F::Source::Stream})
        for (const F::Costs& c : F::costSets()) {
            const F::Result base = F::simulate(c, s, F::Config{"default", F::parts(false, false, false, false), false, false}, 7, 60);
            for (const F::Config& config : F::reportConfigs(false)) {
                if (config.adaptivePhase) continue;
                CHECK(fgsim_tests::same(F::simulate(c, s, config, 7, 60), base));
            }
        }
    // Package 13: each part changes playback somewhere (the check above would fail for it).
    for (const F::Config& config : F::reportConfigs(true)) {
        if (config.adaptivePhase || !config.parts.any()) continue;
        bool differs = false;
        for (const F::Source s : {F::Source::Card, F::Source::Stream})
            for (const F::Costs& c : F::costSets()) {
                const F::Result base = F::simulate(c, s, F::Config{"default", F::parts(false, false, false, false), false, true}, 7, 60);
                differs = differs || !fgsim_tests::same(F::simulate(c, s, config, 7, 60), base);
            }
        CHECK(differs);
    }
}

// A stream's learned phase (VEYRA_EXP_RP_ADAPTIVE_PHASE=1, also set by the interface's modes) never holds a real frame
// longer than the fixed phase: mean and P95 at every cost set at most the default's. Package 13 failed this (tight,
// spikes, drift).
TEST(fgsim_lagfix_a_learned_stream_phase_never_delays_real_frames_past_the_fixed_phase) {
    namespace F = fgsim;
    unsigned package13Worse = 0;
    for (const F::Costs& c : F::costSets()) {
        const F::Result base = F::simulate(c, F::Source::Stream, F::Config{"default", F::parts(false, false, false, false), false, false}, 7, 120);
        const F::Result learned = F::simulate(c, F::Source::Stream, F::Config{"adaptive", F::parts(false, false, false, false), true, false}, 7, 120);
        CHECK(learned.realMeanMs <= base.realMeanMs + 0.01);
        CHECK(learned.realP95Ms <= base.realP95Ms + 0.01);
        CHECK(learned.phaseMeanMs <= base.phaseMeanMs + 0.01);
        const F::Result old = F::simulate(c, F::Source::Stream, F::Config{"adaptive", F::parts(false, false, false, false), true, true}, 7, 120);
        package13Worse += old.realP95Ms > base.realP95Ms + 0.01;
    }
    CHECK(package13Worse >= 3);
}

TEST(fglat_lagfix_parts_in_force_and_the_stream_phase_limit) {
    using veyra::engine::pushUllParse;
    for (const wchar_t* value : {L"1", L"all", L"hold", L"phase", L"headroom", L"grace", L"hold,phase,grace"}) {
        const auto asked = pushUllParse(value);
        CHECK(asked.any());
        const auto inForce = veyra::engine::pushUllPartsInForce(asked);
        CHECK(!inForce.any() && inForce.valid);
        CHECK(std::string(veyra::engine::pushUllPartsDisabledName(asked)) == veyra::engine::pushUllPartsName(asked));
    }
    CHECK(!veyra::engine::pushUllPartsInForce(pushUllParse(L"hodl")).valid);
    using veyra::engine::pushStreamPairDelay;
    static_assert(pushStreamPairDelay(400000, 360000, true) == 360000);    // a stream: never above the fixed phase
    static_assert(pushStreamPairDelay(300000, 360000, true) == 300000);    // below it: as learned
    static_assert(pushStreamPairDelay(400000, 360000, false) == 400000);   // a capture card: unchanged
}

// Cap-fit (VEYRA_EXP_RP_SMOOTH_CAP_FIT) at the simulated case: 29.97 FPS at 3X under a 180 Hz cap. It and the existing
// rule (ceil(cap / content) clamped to [2, ceiling]) both give 3X, so the simulator runs 3X for both; at 59.94 under a
// 179.95 cap they differ (3X against 4X, which the 4X ceiling would allow), outside this report.
TEST(fgsim_cap_fit_and_the_existing_rule_agree_at_30_to_90) {
    const veyra::engine::PushFgMask offered = (1u << 2) | (1u << 3) | (1u << 4);
    const auto existing = [](unsigned ceiling, double content, double cap) {
        return std::clamp(unsigned(std::ceil(cap / content)), 2u, ceiling);
    };
    CHECK(veyra::engine::pushSmoothCapMultiplier(3, 29.97, 180.0, offered) == 3);
    CHECK(existing(3, 29.97, 180.0) == 3);
    CHECK(veyra::engine::pushSmoothCapMultiplier(4, 59.94, 179.95, offered) == 3);
    CHECK(existing(4, 59.94, 179.95) == 4);
}

// Existing behaviour of the DEFAULT path, the same in Package 12A and 13 (reported, not changed): near the GPU's limit
// with occasional slow generated frames, a capture card skips many pairs (the FG cost P95 of the last 2 s includes the
// slow groups and no new sample arrives while pairs are skipped) and its learned phase jumps after each skip run
// (the queue behind a probe's slow groups raises the readiness it learns), so real frames wait up to ~60 ms at P95.
TEST(fgsim_default_card_near_the_limit_is_fragile_in_both_builds) {
    namespace F = fgsim;
    const F::Result r = F::simulate(fgsim_tests::costsNamed("near-limit"), F::Source::Card, F::Config{"default", F::parts(false, false, false, false), false, false}, 7, 120);
    CHECK(r.skippedPerS > 5.0);
    CHECK(r.genShownPerS < 45.0);
    CHECK(r.realP95Ms > 50.0);
}
