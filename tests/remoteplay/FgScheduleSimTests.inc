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

// --- Directive 25: the scheduling changes for the default path's fragile spots (PushFgSchedule.h) ----------------------
namespace fgsim_tests {
inline fgsim::Config sched(const char* name, fgsim::Fixes f, bool adaptive = false) {
    return fgsim::Config{name, fgsim::parts(false, false, false, false), adaptive, false, f};
}
inline fgsim::Fixes fixes(int admit, bool behind, bool smooth, bool cards) {
    fgsim::Fixes f;
    f.admit = admit; f.phaseBehindWarmup = behind; f.streamSmooth = smooth; f.holdCardsOnly = cards;
    return f;
}
}  // namespace fgsim_tests

TEST(fgsim_prints_the_scheduling_candidates) {
    namespace F = fgsim;
    using fgsim_tests::fixes;
    using fgsim_tests::sched;
    std::cout << "\n[fg-schedule-sim] Directive 25 candidates (SIMULATED). admit 2/3: designs compared and not taken.\n";
    const std::vector<F::Config> list = {sched("today", fixes(0, false, false, false)), sched("robustAdmit", fixes(1, false, false, false)),
                                         sched("admit: max of last 8", fixes(2, false, false, false)), sched("admit: probe after 3", fixes(3, false, false, false)),
                                         sched("phaseBehindWarmup", fixes(0, true, false, false)), sched("streamSmooth", fixes(0, false, true, false)),
                                         sched("all four", fixes(1, true, true, true)), sched("ADAPTIVE_PHASE today", fixes(0, false, false, false), true),
                                         sched("ADAPTIVE_PHASE holdCardsOnly", fixes(0, false, false, true), true),
                                         sched("ADAPTIVE_PHASE all four", fixes(1, true, true, true), true)};
    for (const F::Source source : {F::Source::Card, F::Source::Stream})
        for (const F::Costs& costs : F::costSets()) {
            std::cout << "== " << (source == F::Source::Card ? "capture card" : "PS5 stream") << ", costs " << costs.name << "\n" << F::header() << "\n";
            for (const F::Config& c : list) std::cout << F::row(c.name, F::simulate(costs, source, c, 7, 120)) << "\n";
        }
}

// Each test: the defect with the part off (today), and what the part changes, in the cost set that shows it; and that
// with every cost set where the defect does not occur the part changes nothing.
TEST(fgsched_robust_admit_ends_the_lockout) {
    namespace F = fgsim;
    const F::Costs& c = fgsim_tests::costsNamed("near-limit");
    for (const F::Source s : {F::Source::Card, F::Source::Stream}) {
        const F::Result today = F::simulate(c, s, fgsim_tests::sched("today", fgsim_tests::fixes(0, false, false, false)), 7, 120);
        const F::Result fixed = F::simulate(c, s, fgsim_tests::sched("robust", fgsim_tests::fixes(1, false, false, false)), 7, 120);
        CHECK(today.skippedPerS > 5.0);                       // fails today's way: the lock-out
        CHECK(fixed.skippedPerS < 0.5);
        CHECK(fixed.genShownPerS > today.genShownPerS + 20.0);
        CHECK(fixed.offShare < today.offShare && fixed.longestMs <= today.longestMs);
        CHECK(fixed.realP95Ms <= today.realP95Ms + 0.01);
    }
    for (const char* name : {"comfortable", "tight", "spikes", "drift"})
        for (const F::Source s : {F::Source::Card, F::Source::Stream})
            CHECK(fgsim_tests::same(F::simulate(fgsim_tests::costsNamed(name), s, fgsim_tests::sched("t", fgsim_tests::fixes(0, false, false, false)), 7, 60),
                                    F::simulate(fgsim_tests::costsNamed(name), s, fgsim_tests::sched("r", fgsim_tests::fixes(1, false, false, false)), 7, 60)));
}

TEST(fgsched_phase_behind_warmup_stops_the_jump) {
    namespace F = fgsim;
    const F::Costs& c = fgsim_tests::costsNamed("near-limit");
    const F::Result today = F::simulate(c, F::Source::Card, fgsim_tests::sched("today", fgsim_tests::fixes(0, false, false, false)), 7, 120);
    const F::Result fixed = F::simulate(c, F::Source::Card, fgsim_tests::sched("behind", fgsim_tests::fixes(0, true, false, false)), 7, 120);
    CHECK(today.realP95Ms > 55.0);                            // the jump to ~63 ms
    CHECK(fixed.realP95Ms < today.realP95Ms - 10.0);
    CHECK(fixed.phaseMeanMs < today.phaseMeanMs);
}

TEST(fgsched_stream_smoothing_takes_decode_jitter_off_the_panel) {
    namespace F = fgsim;
    for (const char* name : {"comfortable", "tight", "spikes", "drift", "near-limit"}) {
        const F::Costs& c = fgsim_tests::costsNamed(name);
        const F::Result today = F::simulate(c, F::Source::Stream, fgsim_tests::sched("today", fgsim_tests::fixes(0, false, false, false)), 7, 120);
        const F::Result fixed = F::simulate(c, F::Source::Stream, fgsim_tests::sched("smooth", fgsim_tests::fixes(0, false, true, false)), 7, 120);
        CHECK(fixed.offShare < today.offShare);
        CHECK(fixed.realMeanMs <= today.realMeanMs + 0.01);          // no added AVERAGE delay
        CHECK(fixed.genShownPerS >= today.genShownPerS - 0.01);
        CHECK(fixed.realP95Ms <= today.realP95Ms + 3.0 * 1.5);       // the P95 from decode rises by at most the jitter it removes
    }
    // Capture cards: unchanged.
    const F::Result card = F::simulate(fgsim_tests::costsNamed("comfortable"), F::Source::Card, fgsim_tests::sched("smooth", fgsim_tests::fixes(0, false, true, false)), 7, 60);
    CHECK(fgsim_tests::same(card, F::simulate(fgsim_tests::costsNamed("comfortable"), F::Source::Card, fgsim_tests::sched("t", fgsim_tests::fixes(0, false, false, false)), 7, 60)));
}

TEST(fgsched_hold_cards_only_keeps_a_learned_stream_phase_generating) {
    namespace F = fgsim;
    const F::Costs& c = fgsim_tests::costsNamed("near-limit");
    const F::Result today = F::simulate(c, F::Source::Stream, fgsim_tests::sched("today", fgsim_tests::fixes(0, false, false, false), true), 7, 120);
    const F::Result fixed = F::simulate(c, F::Source::Stream, fgsim_tests::sched("cards", fgsim_tests::fixes(0, false, false, true), true), 7, 120);
    CHECK(today.holdsPerMin > 5.0);                           // the card's hold on a stream
    CHECK(fixed.holdsPerMin == 0.0);
    CHECK(fixed.genShownPerS > today.genShownPerS && fixed.longestMs < today.longestMs);
}

TEST(fgsched_switches_parse_and_default_off) {
    using veyra::engine::pushFgSchedResolve;
    const auto none = pushFgSchedResolve(L"", L"");
    CHECK(!none.robustAdmit && !none.phaseBehindWarmup && !none.streamSmooth && !none.holdCardsOnly && none.understood);
    const auto all = pushFgSchedResolve(L"all", L"");
    CHECK(all.robustAdmit && all.phaseBehindWarmup && all.streamSmooth && all.holdCardsOnly);
    const auto two = pushFgSchedResolve(L"robustAdmit,streamSmooth", L"");
    CHECK(two.robustAdmit && two.streamSmooth && !two.phaseBehindWarmup && !two.holdCardsOnly);
    const auto legacy = pushFgSchedResolve(L"all", L"streamSmooth");
    CHECK(legacy.robustAdmit && !legacy.streamSmooth);
    for (const wchar_t* bad : {L"1", L"ALL", L"robustAdmit,", L"robust", L"robustAdmit, streamSmooth"}) {
        const auto p = pushFgSchedResolve(bad, L"");
        CHECK(!p.understood && !p.robustAdmit && !p.streamSmooth);
    }
    veyra::engine::PushFgSched on;
    on.robustAdmit = true;
    CHECK(!pushFgSchedResolve(L"", L"robustAdmit", on).robustAdmit);   // a default-on part: the legacy switch restores today
    CHECK(veyra::engine::pushFgSchedDescribe(none) == "robustAdmit=0 phaseBehindWarmup=0 streamSmooth=0 holdCardsOnly=0 gridEarly=0");
    // The robust estimate: one slow group in many does not lift it; many do.
    veyra::engine::PushFgCostWindow w;
    for (int i = 0; i < 40; ++i) w.add(i * 333667LL, i % 10 == 0 ? 33.0 : 23.0);
    CHECK(w.robust(40 * 333667LL) <= 1.2 * 23.0 + 1e-9);
    veyra::engine::PushFgCostWindow heavy;
    for (int i = 0; i < 40; ++i) heavy.add(i * 333667LL, i % 2 == 0 ? 33.0 : 23.0);
    CHECK(heavy.robust(40 * 333667LL) == 33.0);
    CHECK(!veyra::engine::pushRobustAdmitFits(0, 300000, 0.3, 0.3, std::nullopt, 10.0));
}

// Directive 25 B: other schedules on equal terms (SIMULATED; docs/drafts/FG_SCHEDULING_NEXT.md). Not engine code.
TEST(fgsim_prints_the_schedule_comparison) {
    namespace F = fgsim;
    std::cout << "\n[fg-schedule-sim] schedules compared (SIMULATED; predicted-frame cost assumed 3.0 ms first, 1.0 ms second)\n";
    for (const F::Source source : {F::Source::Card, F::Source::Stream})
        for (const F::Costs& costs : F::costSets()) {
            std::cout << "== " << (source == F::Source::Card ? "capture card" : "PS5 stream") << ", costs " << costs.name << "\n" << F::scheduleHeader() << "\n";
            for (const F::Schedule k : {F::Schedule::Interpolate3X, F::Schedule::RealThenPredict, F::Schedule::PredictTwo, F::Schedule::Compressed, F::Schedule::Interpolate2X})
                std::cout << F::scheduleRow(k, F::simulateSchedule(costs, source, k, 7, 120)) << "\n";
        }
}

TEST(fgsim_schedules_the_wait_is_the_method) {
    namespace F = fgsim;
    for (const char* name : {"comfortable", "tight", "spikes", "drift", "near-limit"}) {
        const F::Costs& c = fgsim_tests::costsNamed(name);
        const auto today = F::simulateSchedule(c, F::Source::Card, F::Schedule::Interpolate3X, 7, 60);
        const auto one = F::simulateSchedule(c, F::Source::Card, F::Schedule::RealThenPredict, 7, 60);
        const auto two = F::simulateSchedule(c, F::Source::Card, F::Schedule::PredictTwo, 7, 60);
        // One predicted frame in three takes about one slot (11.1 ms) off the real frame; two take the whole wait.
        CHECK(one.realMeanMs < today.realMeanMs - 8.0);
        CHECK(two.realMeanMs < one.realMeanMs - 15.0);
        CHECK(one.predictedShare > 0.3 && one.predictedShare < 0.34);
        CHECK(one.offShare <= today.offShare + 0.005);
        CHECK(today.predictedShare == 0.0);
    }
}

// --- capture cards: the defaults judged by the owner's priority order (23 Nov 2026 22:51 ET) ---------------------------
namespace fgsim_tests {
// Lexicographic: -1 worse on (1)+(2) (generated frames shown, uneven share, longest gap, skips), 0 identical, 1 better
// on (1)+(2), 2 equal on (1)+(2) and better on delay, -2 equal on (1)+(2) and worse on delay.
inline int judge(const fgsim::Result& base, const fgsim::Result& x) {
    const double e = 1e-9;
    if (x.genShownPerS < base.genShownPerS - e || x.offShare > base.offShare + e || x.longestMs > base.longestMs + e || x.skippedPerS > base.skippedPerS + e) return -1;
    if (x.genShownPerS > base.genShownPerS + e || x.offShare < base.offShare - e || x.longestMs < base.longestMs - e || x.skippedPerS < base.skippedPerS - e) return 1;
    if (x.realMeanMs > base.realMeanMs + e || x.realP95Ms > base.realP95Ms + e) return -2;
    if (x.realMeanMs < base.realMeanMs - e || x.realP95Ms < base.realP95Ms - e) return 2;
    return 0;
}
inline fgsim::Fixes cardDefault() {   // kPushFgSchedCardDefault in the simulator's terms
    fgsim::Fixes f;
    f.admit = veyra::engine::kPushFgSchedCardDefault.robustAdmit ? 1 : 0;
    f.phaseBehindWarmup = veyra::engine::kPushFgSchedCardDefault.phaseBehindWarmup;
    f.gridEarly = veyra::engine::kPushFgSchedCardDefault.gridEarly;
    f.robustGuard = f.admit ? 2 : 0;   // PushRobustAdmitGuard(32, 0.5), part of robustAdmit in the engine
    return f;
}
}  // namespace fgsim_tests

TEST(fgsched_card_defaults_pass_the_priority_order_in_every_card_set) {
    namespace F = fgsim;
    unsigned better = 0;
    for (const F::Costs& c : F::costSets()) {
        const F::Result today = F::simulate(c, F::Source::Card, fgsim_tests::sched("today", fgsim_tests::fixes(0, false, false, false)), 7, 120);
        const F::Result now = F::simulate(c, F::Source::Card, fgsim_tests::sched("default", fgsim_tests::cardDefault()), 7, 120);
        const int verdict = fgsim_tests::judge(today, now);
        CHECK(verdict >= 0);
        if (today.skippedPerS == 0.0) CHECK(fgsim_tests::same(today, now));   // without skipped pairs nothing changes
        better += verdict > 0;
        // robustAdmit's real frames in pairs shown WITH generation are no later than today's generating pairs, except in
        // the sustained bimodal sets, where today generates only between the slow pairs (cheaper moments) and the new
        // default through them too (bimodal-5: 40.4 -> 42.5 ms).
        if (today.realGenMeanMs > 0 && now.realGenMeanMs > 0 && std::string(c.name).rfind("bimodal", 0) != 0) {
            CHECK(now.realGenMeanMs <= today.realGenMeanMs + 0.01);
            CHECK(now.realGenP95Ms <= today.realGenP95Ms + 0.01);
        }
    }
    CHECK(better >= 3);   // near-limit, overload, heavy
    // Streams: no default changes.
    CHECK(!veyra::engine::kPushFgSchedDefault.robustAdmit && !veyra::engine::kPushFgSchedDefault.gridEarly);
    CHECK(veyra::engine::kPushFgSchedCardDefault.robustAdmit && veyra::engine::kPushFgSchedCardDefault.gridEarly);
    CHECK(!veyra::engine::kPushFgSchedCardDefault.phaseBehindWarmup && !veyra::engine::kPushFgSchedCardDefault.streamSmooth && !veyra::engine::kPushFgSchedCardDefault.holdCardsOnly);
}

TEST(fgsched_phase_behind_warmup_fails_the_order_alone_and_changes_nothing_with_robust_admit) {
    namespace F = fgsim;
    for (const char* name : {"near-limit", "overload"}) {
        const F::Costs& c = fgsim_tests::costsNamed(name);
        const F::Result today = F::simulate(c, F::Source::Card, fgsim_tests::sched("t", fgsim_tests::fixes(0, false, false, false)), 7, 120);
        CHECK(fgsim_tests::judge(today, F::simulate(c, F::Source::Card, fgsim_tests::sched("a2", fgsim_tests::fixes(0, true, false, false)), 7, 120)) == -1);
    }
    for (const F::Costs& c : F::costSets())
        CHECK(fgsim_tests::same(F::simulate(c, F::Source::Card, fgsim_tests::sched("a1", fgsim_tests::fixes(1, false, false, false)), 7, 120),
                                F::simulate(c, F::Source::Card, fgsim_tests::sched("a12", fgsim_tests::fixes(1, true, false, false)), 7, 120)));
}

TEST(fgsched_grid_early_keeps_the_grid) {
    using veyra::engine::pushGridEarlyDelay;
    const std::int64_t T = 333667, phase = 336000;   // 33.6 ms phase, 3X: slots 11.1 ms apart
    // Base P95 3 ms: the real frame can take the first generated frame's slot (phase - 2 slots = 11.4 ms).
    CHECK(pushGridEarlyDelay(phase, T, 3, 3.0, 0) == phase - 2 * (T / 3));
    // Base P95 12 ms: only the second slot (22.5 ms) is reachable.
    CHECK(pushGridEarlyDelay(phase, T, 3, 12.0, 0) == phase - (T / 3));
    // Base P95 30 ms: the phase itself.
    CHECK(pushGridEarlyDelay(phase, T, 3, 30.0, 0) == phase);
    // No base cost yet, or not frame generation: the phase.
    CHECK(pushGridEarlyDelay(phase, T, 3, std::nullopt, 0) == phase);
    CHECK(pushGridEarlyDelay(phase, T, 1, 3.0, 0) == phase);
    // The anchor 0.5 ms before the callback (LiveArrivalSmoother's lead) counts: the slot must be base + 1 ms after the
    // callback, so 0.5 ms more after the anchor.
    CHECK(pushGridEarlyDelay(phase, T, 3, 9.8, 5000) == phase - 2 * (T / 3));
    CHECK(pushGridEarlyDelay(phase, T, 3, 10.3, 5000) == phase - (T / 3));
}

// --- "99 % steady" (the owner, 23 Nov 2026 23:59 ET): the engine's always-on counter (PushGridWindow) in the simulator ---
TEST(fgsched_grid_window_counts_like_the_owner_asks) {
    veyra::engine::PushGridWindow w;
    const std::int64_t slot = 111222;
    std::int64_t t = 1000000;
    for (int i = 0; i < 100; ++i, t += slot) w.observe(t, slot);
    t += slot;                               // one frame missing: a 22.2 ms gap
    w.observe(t, slot);
    t += slot + 30000;                       // one 3 ms late frame, then back on the grid
    w.observe(t, slot);
    t += slot - 30000;
    w.observe(t, slot);
    const auto s = w.take();
    CHECK(s.intervals == 102 && s.presents == 103);
    CHECK(s.within1ms == 99 && s.withinHalf == 101 && s.offRunMax == 3);   // the gap, the late frame and the one after it
    CHECK(std::abs(s.expected - 104.0) < 1e-9);   // the missing frame counts as expected
    CHECK(std::abs(s.longestMs - 22.2444) < 1e-3);
    CHECK(w.take().intervals == 0);
    w.observe(5000000, 0);                   // without frame generation nothing is counted
    CHECK(w.peek().presents == 0);
}

TEST(fgsched_steadiness_per_card_set) {
    namespace F = fgsim;
    std::cout << "\n[fg-schedule-sim] steadiness, capture card, 29.97 -> 3X (SIMULATED): on grid within 1 ms / within half a slot, "
                 "frames shown of expected, longest gap, longest run of off-grid intervals; 12A/13 default -> this branch's default\n";
    for (const F::Costs& c : F::costSets()) {
        const F::Result was = F::simulate(c, F::Source::Card, fgsim_tests::sched("t", fgsim_tests::fixes(0, false, false, false)), 7, 120);
        const F::Result now = F::simulate(c, F::Source::Card, fgsim_tests::sched("d", fgsim_tests::cardDefault()), 7, 120);
        char line[400];
        std::snprintf(line, sizeof line, "  %-12s %6.2f %% / %6.2f %%, %6.2f %%, %5.1f ms, %3u  ->  %6.2f %% / %6.2f %%, %6.2f %%, %5.1f ms, %3u", c.name,
                      was.grid.onGrid1msPct(), was.grid.onGridHalfPct(), was.grid.shownPct(), was.grid.longestMs, was.grid.offRunMax, now.grid.onGrid1msPct(),
                      now.grid.onGridHalfPct(), now.grid.shownPct(), now.grid.longestMs, now.grid.offRunMax);
        std::cout << line << "\n";
        CHECK(now.grid.onGrid1msPct() >= was.grid.onGrid1msPct() - 1e-9);   // never less steady than 12A / 13
        CHECK(now.grid.shownPct() >= was.grid.shownPct() - 1e-9);
    }
    // The realistic sets without GPU spikes meet 99 % on both counts.
    for (const char* name : {"comfortable", "tight", "drift", "thermal"}) {
        const F::Result r = F::simulate(fgsim_tests::costsNamed(name), F::Source::Card, fgsim_tests::sched("d", fgsim_tests::cardDefault()), 7, 120);
        CHECK(r.grid.onGrid1msPct() >= 99.0 && r.grid.shownPct() >= 99.0);
    }
}

// Review of Package 14: robustAdmit under sustained bimodal costs, and its stop (PushRobustAdmitGuard).
TEST(fgsched_robust_admit_guard_against_package13_in_every_card_set) {
    namespace F = fgsim;
    const auto run = [](const F::Costs& c, int admit, int guard) {
        F::Fixes f;
        f.admit = admit; f.gridEarly = admit != 0; f.robustGuard = guard;
        return F::simulate(c, F::Source::Card, F::Config{"x", F::parts(false, false, false, false), false, false, f}, 7, 120);
    };
    unsigned bimodal = 0, tightGuardWorse = 0;
    for (const F::Costs& c : F::costSets()) {
        const F::Result p13 = run(c, 0, 0), unguarded = run(c, 1, 0), guarded = run(c, 1, 2), tight = run(c, 1, 1);
        CHECK(fgsim_tests::judge(p13, unguarded) >= 0);   // without a guard: never worse than Package 13
        CHECK(fgsim_tests::judge(p13, guarded) >= 0);     // with the 32 / 50 % guard (the default): never worse either
        tightGuardWorse += fgsim_tests::judge(p13, tight) < 0;
        if (std::string(c.name).rfind("bimodal", 0) == 0) {
            ++bimodal;
            CHECK(fgsim_tests::same(unguarded, guarded));   // sustained bimodal: the guard never trips
        }
    }
    CHECK(bimodal == 6);
    CHECK(tightGuardWorse >= 3);   // a 16 / 25 % guard is worse than Package 13 (near-limit, bimodal-5, -stretch, -short)
}
