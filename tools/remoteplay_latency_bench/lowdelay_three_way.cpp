// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// SIMULATED three-way comparison for "Lower delay" (scheduling review of 24 Nov, item A): Package 14's default, the mode
// as first built (leave the schedule on an unusable prediction), the stay-in design (PushLowDelayPolicy::StayIn) and a
// hybrid (stay in on isolated bad predictions, leave after 3 of 4, return after 60 calm pairs), "stay-in25" (stay in,
// leave when 15 of the last 60 predictions were unusable),
// over the 16 card cost sets, seeds 1 2 3 7 11 99, P (predicted frame) costing 3 / 6 / 10 ms of GPU, and a share of
// unusable predictions in 8-pair bursts. Everything is the scheduling simulator's (tests/remoteplay/FgScheduleSim.h):
// modelled costs, no GPU, no picture; an unusable prediction is modelled as WHOLLY held (P shows B).
//   g++ -std=c++20 -O2 -Iinclude -Itests/remoteplay/shim -Itests/remoteplay tools/remoteplay_latency_bench/lowdelay_three_way.cpp -o /tmp/ld3
//   /tmp/ld3 [seconds per run, default 120] [--sets] [--hybrids]   (--sets: one line per cost set at P 3 ms, seed 7, h 1;
//   --hybrids: also the two hybrid policies). h: the share of a bad P's motion held at B (Fixes::heldShare).
#include "FgScheduleSim.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace F = fgsim;
struct Acc {
    std::vector<double> realMean, spread, p99, grid, shown, jumps, repeats, even, switches, worstGrid;
    int gridWorse = 0, lost99 = 0, runs = 0;
};
static double meanOf(const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / double(v.size()); }
int main(int argc, char** argv) {
    const double secs = argc > 1 && argv[1][0] != '-' ? std::atof(argv[1]) : 120.0;
    bool perSet = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--sets")) perSet = true;
    const unsigned seeds[] = {1, 2, 3, 7, 11, 99};
    const auto def = [] {   // kPushFgSchedCardDefault in the simulator's terms (as fgsim_tests::cardDefault)
        F::Fixes f;
        f.admit = veyra::engine::kPushFgSchedCardDefault.robustAdmit ? 1 : 0;
        f.phaseBehindWarmup = veyra::engine::kPushFgSchedCardDefault.phaseBehindWarmup;
        f.gridEarly = veyra::engine::kPushFgSchedCardDefault.gridEarly;
        f.robustGuard = f.admit ? 2 : 0;
        return f;
    };
    const auto run = [&](const F::Costs& c, unsigned seed, int design, double u, double pm, double h = 1.0) {
        auto f = def();
        if (design > 0) { f.lowDelay = true; f.unusableShare = u; f.unusableBurst = 8; f.predictMs = pm; f.ldPolicy = design - 1; f.heldShare = h; }
        return F::simulate(c, F::Source::Card, F::Config{"x", F::parts(false, false, false, false), false, false, f}, seed, secs);
    };
    // Package 14 per set and seed.
    std::vector<std::vector<F::Result>> base(F::costSets().size());
    for (std::size_t i = 0; i < F::costSets().size(); ++i) for (unsigned s : seeds) base[i].push_back(run(F::costSets()[i], s, 0, 0, 0));
    std::printf("SIMULATED, %.0f s per run, 16 card cost sets x 6 seeds. Per row: mean over sets (min..max of that mean across seeds).\n", secs);
    std::printf("delay = real frame capture -> Present returned; spread = P95 - P5; grid = intervals within 1 ms of T/3; content = steps between shown frames\n\n");
    std::printf("%-10s %4s %4s %4s | %-19s %-17s %-17s | %-15s %-13s %-17s | %-15s %-15s %-13s %-15s | %-13s %s\n", "design", "u%", "Pms", "h",
                "delay mean ms", "spread ms", "P99 ms", "grid %", "shown %", "worst-set grid %", "jumps/min", "repeats/min", "even %", "step err/min", "switch/min", "runs: grid<P14-0.25 / below 99%");
    auto fmt = [](const std::vector<double>& perSeed, char* out, std::size_t n) {
        const auto [lo, hi] = std::minmax_element(perSeed.begin(), perSeed.end());
        std::snprintf(out, n, "%6.2f (%.2f..%.2f)", meanOf(perSeed), *lo, *hi);
    };
    auto row = [&](const char* name, double u, double pm, int design, double h) {
        std::vector<double> realMean, spread, p99, grid, shown, jumps, repeats, even, switches, worst, err;
        int gridWorse = 0, lost99 = 0;
        for (std::size_t k = 0; k < std::size(seeds); ++k) {
            double rm = 0, sp = 0, pp = 0, g = 0, sh = 0, j = 0, r = 0, e = 0, sw = 0, w = 1e9, er = 0;
            for (std::size_t i = 0; i < F::costSets().size(); ++i) {
                const auto x = design == 0 ? base[i][k] : run(F::costSets()[i], seeds[k], design, u, pm, h);
                const auto& b = base[i][k];
                rm += x.realMeanMs; sp += x.realP95Ms - x.realP5Ms; pp += x.realP99Ms; g += x.grid.onGrid1msPct(); sh += x.grid.shownPct();
                j += x.contentJumpsPerMin; r += x.contentRepeatsPerMin; e += x.contentEvenPct; sw += x.switchesPerMin; w = std::min(w, x.grid.onGrid1msPct()); er += x.contentErrorSlotsPerMin;
                if (x.grid.onGrid1msPct() < b.grid.onGrid1msPct() - 0.25) ++gridWorse;
                if (x.grid.onGrid1msPct() < 99.0) ++lost99;   // runs below the owner's 99 % (review 2: "lost 99 %" was vacuous)
            }
            const double n = double(F::costSets().size());
            realMean.push_back(rm / n); spread.push_back(sp / n); p99.push_back(pp / n); grid.push_back(g / n); shown.push_back(sh / n);
            jumps.push_back(j / n); repeats.push_back(r / n); even.push_back(e / n); switches.push_back(sw / n); worst.push_back(w); err.push_back(er / n);
        }
        char a[64], b2[64], c[64], d[64], e[64], f[64], g[64], hh[64], i2[64], j2[64], k2[64];
        fmt(realMean, a, 64); fmt(spread, b2, 64); fmt(p99, c, 64); fmt(grid, d, 64); fmt(shown, e, 64); fmt(worst, f, 64);
        fmt(jumps, g, 64); fmt(repeats, hh, 64); fmt(even, i2, 64); fmt(switches, j2, 64); fmt(err, k2, 64);
        std::printf("%-10s %4.0f %4.0f %4.2f | %-19s %-17s %-17s | %-15s %-13s %-17s | %-15s %-15s %-13s %-15s | %-13s %d / %d of %d\n", name, u * 100, pm, h, a, b2, c, d, e, f, g, hh, i2, k2, j2,
                    gridWorse, lost99, int(F::costSets().size() * std::size(seeds)));
        std::fflush(stdout);
    };
    row("P14", 0, 0, 0, 1.0);
    // The schedule does not depend on h (only what a bad P shows does): h = 1, 0.5, 0.25 for the two policies.
    const bool hybrids = std::any_of(argv + 1, argv + argc, [](const char* a) { return !std::strcmp(a, "--hybrids"); });
    for (double pm : {3.0, 6.0, 10.0})
        for (double u : {0.0, 0.05, 0.10, 0.20, 0.30})
            for (double h : {1.0, 0.5, 0.25}) {
                if (u == 0.0 && h != 1.0) continue;   // nothing is unusable: h changes nothing
                row("leave", u, pm, 1, h);
                row("stay-in", u, pm, 2, h);
                if (hybrids) { row("hybrid", u, pm, 3, h); row("stay-in25", u, pm, 4, h); }
            }
    if (perSet) {
        std::printf("\nPer set, seed 7, P 3 ms, unusable 10 %%: P14 | leave | stay-in | hybrid | stay-in25: delay mean, spread, grid %%, jumps+repeats/min, switches/min\n");
        for (std::size_t i = 0; i < F::costSets().size(); ++i) {
            const auto c = F::costSets()[i];
            const auto p = base[i][3], l = run(c, 7, 1, 0.10, 3.0), s = run(c, 7, 2, 0.10, 3.0), h = run(c, 7, 3, 0.10, 3.0), q = run(c, 7, 4, 0.10, 3.0);
            auto one = [](const F::Result& r) { static char t[96]; std::snprintf(t, 96, "%5.1f %5.1f %6.2f %6.1f %5.1f", r.realMeanMs, r.realP95Ms - r.realP5Ms, r.grid.onGrid1msPct(), r.contentJumpsPerMin + r.contentRepeatsPerMin, r.switchesPerMin); return std::string(t); };
            std::printf("%-16s %s | %s | %s | %s | %s\n", c.name, one(p).c_str(), one(l).c_str(), one(s).c_str(), one(h).c_str(), one(q).c_str());
        }
    }
}
