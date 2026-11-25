// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Package 15 "Lower delay" in the scheduling simulator (FgScheduleSim.h, Fixes::lowDelay), which runs the engine's own
// decision code (PushLowDelay.h). Every number here is SIMULATED (modelled costs, no GPU): it says what the scheduling
// does with those costs, nothing about a real PC or the picture. Uses fgsim_tests (FgScheduleSimTests.inc, included before).
#include "FgScheduleSim.h"
#include <iostream>

namespace lowdelay_sim_tests {
inline fgsim::Result run(const fgsim::Costs& c, bool lowDelay, double unusable = 0.0, double burst = 8.0, fgsim::Source src = fgsim::Source::Card) {
    fgsim::Fixes f = fgsim_tests::cardDefault();
    f.lowDelay = lowDelay; f.unusableShare = unusable; f.unusableBurst = burst;
    return fgsim::simulate(c, src, fgsim::Config{"ld", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
}
inline bool sameGrid(const fgsim::Result& a, const fgsim::Result& b) {
    return fgsim_tests::same(a, b) && a.grid.onGrid1msPct() == b.grid.onGrid1msPct() && a.grid.shownPct() == b.grid.shownPct() && a.grid.longestMs == b.grid.longestMs &&
           a.presents == b.presents;   // frame by frame
}
// The row switched on at onS and off at offS (content seconds) in one running session (Fixes::rowOnS / rowOffS).
inline fgsim::Result runSwitched(const fgsim::Costs& c, double onS, double offS, double unusable = 0.0) {
    fgsim::Fixes f = fgsim_tests::cardDefault();
    f.lowDelay = true; f.unusableShare = unusable; f.rowOnS = onS; f.rowOffS = offS;
    return fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"ld-row", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
}
inline std::vector<std::int64_t> presentsBefore(const fgsim::Result& r, std::int64_t until) {
    std::vector<std::int64_t> v;
    for (const auto t : r.presents) if (t < until) v.push_back(t);
    return v;
}
}  // namespace lowdelay_sim_tests

// Off, or where it does not apply (a PS5 stream), the simulated frame path is Package 14's default exactly.
TEST(lowdelay_sim_off_or_not_here_is_package14) {
    namespace L = lowdelay_sim_tests;
    for (const auto& c : fgsim::costSets()) {
        const auto off = L::run(c, false);
        CHECK(off.predShownPerS == 0.0 && off.switchesPerMin == 0.0);
        CHECK(L::sameGrid(L::run(c, false, 0.3), off));   // the scene parameter alone changes nothing
        CHECK(L::sameGrid(L::run(c, true, 0.0, 8.0, fgsim::Source::Stream), L::run(c, false, 0.0, 8.0, fgsim::Source::Stream)));
    }
}

// Without fallbacks: the real frame 5-11 ms earlier where Package 14 skips nothing, every card set as steady.
TEST(lowdelay_sim_without_fallbacks_saves_a_slot_in_every_card_set) {
    namespace L = lowdelay_sim_tests;
    std::cout << "  SIMULATED, 120 s, predictMs 3: set  P14 real mean/P95, on grid 1 ms, shown  ->  lower delay, no fallbacks\n";
    for (const auto& c : fgsim::costSets()) {
        const auto was = L::run(c, false), now = L::run(c, true);
        char line[300];
        std::snprintf(line, sizeof line, "  %-16s %5.1f/%5.1f ms %6.2f %% %6.2f %%  ->  %5.1f/%5.1f ms %6.2f %% %6.2f %%  pred %5.2f/s", c.name, was.realMeanMs, was.realP95Ms,
                      was.grid.onGrid1msPct(), was.grid.shownPct(), now.realMeanMs, now.realP95Ms, now.grid.onGrid1msPct(), now.grid.shownPct(), now.predShownPerS);
        std::cout << line << "\n";
        CHECK(now.grid.onGrid1msPct() >= was.grid.onGrid1msPct() - 0.05);
        CHECK(now.grid.shownPct() >= was.grid.shownPct() - 0.15);
        if (was.skippedPerS == 0.0) CHECK(now.realMeanMs <= was.realMeanMs - 5.0);   // 11.1 ms in most sets, 5.8 in bimodal-10
        CHECK(now.switchesPerMin <= 2.0);
    }
}

// Fallbacks in bursts (the prediction mostly held): it never shows fewer frames, stays within 0.25 points of Package
// 14's on-grid share, keeps 99 % where Package 14 had it, and is never later on average. The benefit fades between 10 %
// and 20 % of pairs unusable at 8-pair bursts (the back-off keeps it off).
TEST(lowdelay_sim_with_fallbacks_in_bursts) {
    namespace L = lowdelay_sim_tests;
    for (const double u : {0.05, 0.10, 0.20, 0.30}) {
        double saved = 0;
        for (const auto& c : fgsim::costSets()) {
            const auto was = L::run(c, false), now = L::run(c, true, u);
            CHECK(now.grid.shownPct() >= was.grid.shownPct() - 0.02);
            CHECK(now.grid.onGrid1msPct() >= was.grid.onGrid1msPct() - 0.25);
            if (was.grid.onGrid1msPct() >= 99.0) CHECK(now.grid.onGrid1msPct() >= 99.0);
            // Later only where it is clearly steadier (the priority order: overload, where Package 14 skips most pairs).
            if (now.grid.onGrid1msPct() <= was.grid.onGrid1msPct() + 0.5) CHECK(now.realMeanMs <= was.realMeanMs + 0.5);
            CHECK(now.switchesPerMin <= 40.0);
            saved += was.realMeanMs - now.realMeanMs;
        }
        saved /= double(fgsim::costSets().size());
        std::cout << "  SIMULATED unusable " << u * 100 << " %, 8-pair bursts: mean real-frame delay saved " << saved << " ms\n";
        if (u <= 0.10) CHECK(saved >= 5.0);
        if (u >= 0.20) CHECK(saved < 2.0);
    }
}

// One click (Package 15, TASK 1): the row switched on at 40 s and off at 80 s of one running session, each switch a graph
// rebuild as the engine does it (its duration not modelled). Before the switch the frames are Package 14's exactly; the
// predicted schedule starts without a restart; after the row goes off no predicted frame is shown; the steadiness over
// the whole run stays within what the two rebuilds' reset pairs cost (two real-frame-only pairs: SIMULATED).
TEST(lowdelay_sim_row_switched_in_the_running_session) {
    namespace L = lowdelay_sim_tests;
    constexpr std::int64_t kStart = 100 * 10000;   // FgScheduleSim's start (100 ms)
    for (const auto& c : fgsim::costSets()) {
        const auto off = L::run(c, false), always = L::run(c, true), sw = L::runSwitched(c, 40.0, 80.0);
        CHECK(sw.rowRebuilds == 2);
        // Before the switch: Package 14's frames, every one at the same time.
        const auto cut = kStart + 40 * 10000000LL - 1000000;   // 100 ms before the switching pair's arrival
        const auto before = L::presentsBefore(sw, cut);
        CHECK(!before.empty() && before == L::presentsBefore(off, cut));
        // It takes effect in the session: predicted frames are shown where the always-on run shows them.
        if (always.predShownPerS > 1.0) CHECK(sw.predShownPerS > 0.0);
        // Off: nothing predicted after the last predicted pair before the switch (its P is shown within a pair).
        CHECK(sw.lastPredShownS < 80.0 + 0.2);
        // Over the whole run: two reset pairs (no generated frames) cost a few frames and intervals, nothing more.
        CHECK(sw.grid.shownPct() >= off.grid.shownPct() - 0.2);
        CHECK(sw.grid.onGrid1msPct() >= std::min(off.grid.onGrid1msPct(), always.grid.onGrid1msPct()) - 0.2);
        char line[260];
        std::snprintf(line, sizeof line, "  SIMULATED row on 40-80 s %-16s real mean %5.1f ms (off %5.1f, always %5.1f)  grid %6.2f %%  shown %6.2f %%  pred %5.2f/s",
                      c.name, sw.realMeanMs, off.realMeanMs, always.realMeanMs, sw.grid.onGrid1msPct(), sw.grid.shownPct(), sw.predShownPerS);
        std::cout << line << "\n";
    }
}

// Scheduling review item A (SIMULATED, seed 7, P 3 ms, wholly unusable predictions in 8-pair bursts): the stay-in policy
// keeps the delay as stable as Package 14 and switches at most twice a minute up to 10 % unusable, where leaving on every
// bad prediction switches about 33 times a minute; the price is a held frame (B again) for each wholly bad prediction,
// counted here as content repeats. tools/remoteplay_latency_bench/lowdelay_three_way.cpp prints the full comparison.
TEST(lowdelay_sim_stay_in_keeps_the_delay_stable) {
    namespace L = lowdelay_sim_tests;
    for (const double u : {0.05, 0.10}) {
        double spreadP14 = 0, spreadStay = 0, spreadLeave = 0, repeatsStay = 0, swStay = 0, swLeave = 0;
        for (const auto& c : fgsim::costSets()) {
            auto f = fgsim_tests::cardDefault();
            const auto p14 = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"p14", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
            f.lowDelay = true; f.unusableShare = u; f.unusableBurst = 8;
            const auto leave = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"leave", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
            f.ldPolicy = 1;
            const auto stay = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"stay", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
            spreadP14 += p14.realP95Ms - p14.realP5Ms; spreadStay += stay.realP95Ms - stay.realP5Ms; spreadLeave += leave.realP95Ms - leave.realP5Ms;
            repeatsStay += stay.contentRepeatsPerMin; swStay += stay.switchesPerMin; swLeave += leave.switchesPerMin;
            CHECK(stay.switchesPerMin <= 2.0);
            CHECK(stay.grid.shownPct() >= p14.grid.shownPct() - 0.15);
        }
        const double n = double(fgsim::costSets().size());
        std::cout << "  SIMULATED unusable " << u * 100 << " %: delay P5-P95 spread P14 " << spreadP14 / n << " ms, leave " << spreadLeave / n << " ms, stay-in "
                  << spreadStay / n << " ms; switches/min leave " << swLeave / n << ", stay-in " << swStay / n << "; stay-in held frames (content repeats)/min " << repeatsStay / n << "\n";
        CHECK(spreadStay <= spreadP14 && spreadStay < spreadLeave);
        CHECK(repeatsStay / n > 0.5 * u * 1800.0);   // the price is real in this model: about one held frame per wholly bad prediction
    }
}

// Decision of 24 Nov, point 3 (SIMULATED, seed 7, P 3 ms, 10 % unusable in 8-pair bursts): a bad P holds a share h of the
// picture's motion at B (the pass holds only untrusted pixels). The content-step error grows with h; at h = 0.25 the
// stay-in mode's error stays below Package 14's (whose error comes from skipped pairs); h = 1 is the stated upper bound.
TEST(lowdelay_sim_held_share_of_a_bad_prediction) {
    double p14 = 0, quarter = 0, half = 0, whole = 0;
    for (const auto& c : fgsim::costSets()) {
        auto f = fgsim_tests::cardDefault();
        p14 += fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"p14", fgsim::parts(false, false, false, false), false, false, f}, 7, 120).contentErrorSlotsPerMin;
        f.lowDelay = true; f.unusableShare = 0.10; f.unusableBurst = 8; f.ldPolicy = 1;
        double* out[] = {&quarter, &half, &whole};
        const double hs[] = {0.25, 0.5, 1.0};
        for (int i = 0; i < 3; ++i) {
            f.heldShare = hs[i];
            const auto r = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"stay", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
            *out[i] += r.contentErrorSlotsPerMin;
        }
    }
    const double n = double(fgsim::costSets().size());
    std::cout << "  SIMULATED content-step error (slots/min, mean of 16 sets): P14 " << p14 / n << "; stay-in 10 % unusable h 0.25 " << quarter / n
              << ", h 0.5 " << half / n << ", h 1 " << whole / n << "\n";
    CHECK(quarter < half && half < whole);
    CHECK(quarter < p14);
}


// Review 2, SHOULD 1 (SIMULATED, stay-in, P 3 ms, no unusable predictions, seeds 1 and 7): per pair, the real frame is
// at most 5 ms later than Package 14's in any set where Package 14 neither skips nor holds a pair (bimodal-5: +2-3 ms on
// up to 1.6 % of pairs, a real frame waiting behind its own late I). Where Package 14 skips or holds pairs it shows
// their real frames within about 4 ms (gridEarly, the rejection hold), and the mode, which generates those pairs, shows
// them later
// (near-limit about 9 %, overload about 55 %, bimodal-stretch / -short 2-5 % of pairs; up to +38 ms). That cannot be
// removed by construction: whether Package 14 skips a pair depends on its own state (its phase learned from two-run
// groups, its cost window, its holds), which does not exist inside the mode (an estimate of two first-frame runs did not
// reproduce its skips). The mean and P95 are lower in every set but overload. The test pins both facts.
TEST(lowdelay_sim_per_pair_later_only_where_package14_skips_or_holds) {
    double worstShare = 0;
    for (const auto& c : fgsim::costSets()) {
        for (const unsigned seed : {1u, 7u}) {
            auto f = fgsim_tests::cardDefault();
            const auto p14 = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"p14", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            f.lowDelay = true; f.ldPolicy = 1;
            const auto ld = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"ld", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            std::vector<double> byPair(p14.realPairs.empty() ? 0 : p14.realPairs.back() + 1, -1.0);
            for (std::size_t i = 0; i < p14.realPairs.size(); ++i) byPair[p14.realPairs[i]] = p14.realDelays[i];
            std::size_t later = 0, total = 0;
            double worst = 0;
            for (std::size_t i = 0; i < ld.realPairs.size(); ++i) {
                const auto k = ld.realPairs[i];
                if (k >= byPair.size() || byPair[k] < 0) continue;
                ++total;
                if (ld.realDelays[i] > byPair[k] + 1.0) ++later;
                worst = std::max(worst, ld.realDelays[i] - byPair[k]);
            }
            const double share = total ? double(later) / double(total) : 0.0;
            if (p14.skippedPerS == 0.0 && p14.holdsPerMin == 0.0) CHECK(worst <= 5.0 && share <= 0.02);
            if (std::string(c.name) != "overload") CHECK(ld.realMeanMs < p14.realMeanMs && ld.realP95Ms <= p14.realP95Ms + 0.5);
            worstShare = std::max(worstShare, share);
        }
    }
    std::cout << "  SIMULATED per pair: worst share of real frames later than Package 14 (> 1 ms) " << worstShare * 100.0 << " % (overload)\n";
    CHECK(worstShare < 0.7);
}

// Review 2, SHOULD 2 (SIMULATED, seeds 1 and 7): the late-frame / on-grid watch. With a predicted frame far dearer than
// assumed (P 18 ms) and no watch, the mode runs away near the GPU limit (near-limit 116-325 ms, overload about 500 ms
// mean real-frame delay, under 6 % on the grid): the watch leaves and the delay stays near Package 14's. At the costs of
// the earlier tables (P 3 ms) the watch never acts (results identical with and without it), because it leaves on the
// grid only when interpolation's own last whole window was steadier (>= 98 %), and late drops of P do not occur there.
TEST(lowdelay_sim_watch_stops_a_runaway_with_a_dear_prediction) {
    for (const char* name : {"near-limit", "overload"}) {
        const auto& c = fgsim_tests::costsNamed(name);
        for (const unsigned seed : {1u, 7u}) {
            auto f = fgsim_tests::cardDefault();
            const auto p14 = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"p14", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            f.lowDelay = true; f.ldPolicy = 1; f.predictMs = 18.0;
            f.ldWatch = 0;
            const auto without = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"no-watch", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            f.ldWatch = 3;
            const auto with = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"watch", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            std::cout << "  SIMULATED P 18 ms " << name << " seed " << seed << ": no watch " << without.realMeanMs << " ms, " << without.grid.onGrid1msPct()
                      << " % on grid; watch " << with.realMeanMs << " ms, " << with.grid.onGrid1msPct() << " %; Package 14 " << p14.realMeanMs << " ms, " << p14.grid.onGrid1msPct() << " %\n";
            CHECK(without.realMeanMs > 100.0);                    // the runaway the watch exists for
            CHECK(with.realMeanMs < p14.realMeanMs + 20.0);
            CHECK(with.grid.onGrid1msPct() >= p14.grid.onGrid1msPct() - 5.0);
        }
    }
    for (const auto& c : fgsim::costSets()) {                       // P 3 ms: the watch changes nothing
        auto f = fgsim_tests::cardDefault();
        f.lowDelay = true; f.ldPolicy = 1;
        f.ldWatch = 0;
        const auto without = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"no-watch", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
        f.ldWatch = 3;
        const auto with = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"watch", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
        CHECK(with.presents == without.presents);
    }
}

// Package 16, item 1: Smoothness Mode with the owner's target of 90 at 30 FPS content AND "Lower delay" (Package 15 never
// asked a pair with a target set). SIMULATED: decision code only, modelled costs; the owner set takes its inputs from the
// parts his log measured (fgsim::ownerCosts). Where Smoothness Mode keeps 3X, at least 95 % of pairs run predicted; where it
// steps down, no pair runs predicted at 2X and the schedule pauses at most once per step down.
TEST(lowdelay_sim_with_smoothness_target_90) {
    std::vector<fgsim::Costs> sets = fgsim::costSets();
    sets.insert(sets.begin(), fgsim::ownerCosts());
    for (const auto& c : sets)
        for (const std::uint32_t seed : {1u, 7u}) {
            fgsim::Fixes f = fgsim_tests::cardDefault();
            f.lowDelay = true; f.ldPolicy = 1; f.smoothTarget = 90;
            const auto r = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"ld-smooth90", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
            CHECK(r.predictedPairShare <= 1.0 - r.twoXShare + 1e-9);    // never a predicted pair at 2X
            CHECK(r.multiplierPauses <= r.smoothStepDowns + 1);
            if (r.twoXShare == 0.0 && r.skippedPerS == 0.0) CHECK(r.predictedPairShare >= 0.95);
            CHECK(r.switchesPerMin <= 3.0);
        }
    fgsim::Fixes f = fgsim_tests::cardDefault();
    f.lowDelay = true; f.ldPolicy = 1; f.smoothTarget = 90;
    for (const std::uint32_t seed : {1u, 2u, 3u, 7u, 11u, 99u}) {
        const auto r = fgsim::simulate(fgsim::ownerCosts(), fgsim::Source::Card, fgsim::Config{"owner", fgsim::parts(false, false, false, false), false, false, f}, seed, 120);
        CHECK(r.twoXShare == 0.0 && r.predictedPairShare >= 0.95);
    }
}

// Package 16, item 3: 2 % i.i.d. skipped pairs forced on top of the cost model with Smoothness target 90 (5 % without), the
// owner set and three 4K-like sets. SIMULATED (decision code + modelled costs). The engine's Tolerant rule runs at least
// 90 % of pairs predicted at 2 % (Package 15's Strict rule, same draws, far fewer: printed, asserted lower).
TEST(lowdelay_sim_two_percent_skips_keep_the_mode) {
    std::vector<fgsim::Costs> sets{fgsim::ownerCosts()};
    for (const auto& c : fgsim::costSets()) if (std::string(c.name) == "vfg-4k" || std::string(c.name) == "comfortable" || std::string(c.name) == "thermal") sets.push_back(c);
    for (const auto& c : sets) {
        fgsim::Fixes f = fgsim_tests::cardDefault();
        f.lowDelay = true; f.ldPolicy = 1; f.smoothTarget = 90; f.forcedSkipShare = 0.02;
        const auto tolerant = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"skip2", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
        f.ldStrict = true;
        const auto strict = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"skip2-strict", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
        // 5 % without a Smoothness target: with one, Smoothness Mode's own rule (more than 10 % of a 250 ms tick's pairs
        // rejected on 4 ticks in a row) steps down to 2X under i.i.d. skips of 3 % and more, and the mode pauses (correct).
        f.ldStrict = false; f.forcedSkipShare = 0.05; f.smoothTarget = 0;
        const auto five = fgsim::simulate(c, fgsim::Source::Card, fgsim::Config{"skip5", fgsim::parts(false, false, false, false), false, false, f}, 7, 120);
        std::printf("  SIMULATED %-16s 2 %% skips: predicted pairs %.3f (Package 15's rule %.3f); 5 %% skips: %.3f; switches/min %.2f\n", c.name,
                    tolerant.predictedPairShare, strict.predictedPairShare, five.predictedPairShare, tolerant.switchesPerMin);
        CHECK(tolerant.predictedPairShare >= 0.90);
        CHECK(five.predictedPairShare >= 0.85);
        CHECK(strict.predictedPairShare < tolerant.predictedPairShare);
        CHECK(tolerant.switchesPerMin <= 2.0);
    }
}
