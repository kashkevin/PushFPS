// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Closed-loop trace simulator for the latency governor's active control (PushGovernorControl.h) over the
// model in include/veyra/engine/PushGovernorSim.h. A LINUX MODEL: every cost is invented, nothing here was
// measured on a GPU, a PC or a PS5. Deterministic: same output on every run.
//   g++ -std=c++20 -O2 -Iinclude tools/remoteplay_latency_bench/governor_sim.cpp -o /tmp/governor_sim
//   /tmp/governor_sim > tools/remoteplay_latency_bench/governor_sim_results.json   (table on stderr)
#include "veyra/engine/PushGovernorSim.h"
#include <cstdio>
#include <string>

namespace ge = veyra::engine;

static std::string number(double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", v);
    return buffer;
}

int main() {
    const ge::PushControlConfig config;
    std::string json = "{\n  \"model\": \"Linux model (PushGovernorSim.h); invented costs, not a measurement\",\n";
    json += "  \"request\": \"PushFlow 3X Quality\",\n  \"latencyBudgetMs\": " + number(config.latencyBudgetMs) + ",\n  \"scenarios\": [\n";
    std::fprintf(stderr, "| scenario | changes | graph rebuilds | down/up | rollbacks | lock-outs | reversals | mean latency gov / none / ideal (ms) | P95 gov / none / ideal (ms) | s over budget gov / none / ideal | output fps gov / none / ideal | seconds at level 0..4 | invariants |\n");
    std::fprintf(stderr, "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    unsigned violations = 0;
    for (std::size_t i = 0; i < ge::kPushSimScenarioCount; ++i) {
        const auto s = ge::pushRunGovernorScenario(static_cast<ge::PushSimScenario>(i), config);
        if (!s.firstViolation.empty()) ++violations;
        std::string levels;
        for (std::size_t l = 0; l < 5; ++l) levels += (l ? "/" : "") + std::to_string(s.secondsAtIndex[l]);
        std::fprintf(stderr, "| %s | %u | %u | %u/%u | %u | %u | %u | %s / %s / %s | %s / %s / %s | %u / %u / %u | %s / %s / %s | %s | %s |\n", s.scenario.c_str(), s.changes, s.rebuilds,
            s.downs, s.ups, s.rollbacks, s.lockouts, s.oscillations, number(s.latencyGovernor).c_str(), number(s.latencyNone).c_str(),
            number(s.latencyIdeal).c_str(), number(s.p95Governor).c_str(), number(s.p95None).c_str(), number(s.p95Ideal).c_str(), s.overBudgetGovernor,
            s.overBudgetNone, s.overBudgetIdeal, number(s.outputGovernor).c_str(), number(s.outputNone).c_str(), number(s.outputIdeal).c_str(), levels.c_str(),
            s.firstViolation.empty() ? "hold" : s.firstViolation.c_str());
        json += "    {\"scenario\": \"" + s.scenario + "\", \"changes\": " + std::to_string(s.changes) + ", \"graphRebuilds\": " + std::to_string(s.rebuilds) + ", \"down\": " + std::to_string(s.downs) +
            ", \"up\": " + std::to_string(s.ups) + ", \"rollbacks\": " + std::to_string(s.rollbacks) + ", \"lockouts\": " + std::to_string(s.lockouts) +
            ", \"reversals\": " + std::to_string(s.oscillations) + ", \"meanLatencyMs\": {\"governor\": " + number(s.latencyGovernor) + ", \"none\": " +
            number(s.latencyNone) + ", \"ideal\": " + number(s.latencyIdeal) + "}, \"p95LatencyMs\": {\"governor\": " + number(s.p95Governor) + ", \"none\": " +
            number(s.p95None) + ", \"ideal\": " + number(s.p95Ideal) + "}, \"secondsOverBudget\": {\"governor\": " + std::to_string(s.overBudgetGovernor) +
            ", \"none\": " + std::to_string(s.overBudgetNone) + ", \"ideal\": " + std::to_string(s.overBudgetIdeal) + "}, \"meanOutputFps\": {\"governor\": " +
            number(s.outputGovernor) + ", \"none\": " + number(s.outputNone) + ", \"ideal\": " + number(s.outputIdeal) + "}, \"secondsAtLevel\": [" ;
        for (std::size_t l = 0; l < 5; ++l) json += (l ? ", " : "") + std::to_string(s.secondsAtIndex[l]);
        json += "], \"decisions\": [";
        bool first = true;
        for (const auto& r : s.trace) {
            if (!r.changed) continue;
            json += std::string(first ? "" : ", ") + "{\"s\": " + std::to_string(r.nowMs / 1000) + ", \"to\": \"" + std::to_string(r.after.multiplier) + "x/t" +
                std::to_string(r.after.tier) + "\", \"cause\": \"" + ge::pushControlCauseName(r.tick.cause) + "\"}";
            first = false;
        }
        json += "], \"invariants\": \"" + (s.firstViolation.empty() ? std::string("hold") : s.firstViolation) + "\"}";
        json += i + 1 < ge::kPushSimScenarioCount ? ",\n" : "\n";
    }
    // Periodic short bursts over 24 h (model): the share of time spent reduced is the bound to watch.
    {
        const int day = 86400;
        const auto b = ge::pushRunBurstSim(day, config);
        const double reduced = 100.0 * (1.0 - static_cast<double>(b.secondsAtIndex[0]) / day);
        if (!b.firstViolation.empty()) ++violations;
        std::fprintf(stderr, "periodic-short-bursts (24 h): changes %u, graph rebuilds %u, s over budget %u with / %u without, rollbacks %u, lock-outs %u, reduced %.1f %% of the time, invariants %s\n", b.changes,
            b.rebuilds, b.overBudgetGovernor, b.overBudgetNone, b.rollbacks, b.lockouts, reduced, b.firstViolation.empty() ? "hold" : b.firstViolation.c_str());
        json += "  ],\n  \"periodicShortBursts24h\": {\"changes\": " + std::to_string(b.changes) + ", \"graphRebuilds\": " + std::to_string(b.rebuilds) +
            ", \"secondsOverBudget\": {\"governor\": " + std::to_string(b.overBudgetGovernor) + ", \"none\": " + std::to_string(b.overBudgetNone) + "}, \"rollbacks\": " + std::to_string(b.rollbacks) +
            ", \"lockouts\": " + std::to_string(b.lockouts) + ", \"reducedPercent\": " + number(reduced) + ", \"invariants\": \"" +
            (b.firstViolation.empty() ? std::string("hold") : b.firstViolation) + "\"},\n  \"scenariosEnd\": [\n";
    }
    // Review 4's 24 h traces (PushGovernorSim.h pushRunReviewTrace; sprint 4).
    json += "  ],\n  \"reviewTraces24h\": [\n";
    std::fprintf(stderr, "\n| trace (24 h, Linux model) | changes | rebuilds | max rebuilds / 60 s | rollbacks | s over budget gov / none | output fps gov / none | time reduced | longest reduced (s) | last hour reduced | invariants |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (int k = 0; k < ge::kPushSimReviewTraceCount; ++k) {
        const auto s = ge::pushRunReviewTrace(k, 86400, config);
        const auto f = ge::pushSimTraceFacts(s);
        if (!s.firstViolation.empty()) ++violations;
        const std::string inv = s.firstViolation.empty() ? std::string("hold") : s.firstViolation;
        std::fprintf(stderr, "| %s | %u | %u | %u | %u | %u / %u | %s / %s | %s %% | %u | %s %% | %s |\n", s.scenario.c_str(), s.changes, s.rebuilds, f.maxRebuildsPerMinute,
            s.rollbacks, s.overBudgetGovernor, s.overBudgetNone, number(s.outputGovernor).c_str(), number(s.outputNone).c_str(), number(f.reducedPercent).c_str(),
            f.longestReducedS, number(f.lastHourReducedPercent).c_str(), inv.c_str());
        json += "    {\"trace\": \"" + s.scenario + "\", \"changes\": " + std::to_string(s.changes) + ", \"graphRebuilds\": " + std::to_string(s.rebuilds) +
            ", \"maxRebuildsPer60s\": " + std::to_string(f.maxRebuildsPerMinute) + ", \"rollbacks\": " + std::to_string(s.rollbacks) +
            ", \"secondsOverBudget\": {\"governor\": " + std::to_string(s.overBudgetGovernor) + ", \"none\": " + std::to_string(s.overBudgetNone) +
            "}, \"meanOutputFps\": {\"governor\": " + number(s.outputGovernor) + ", \"none\": " + number(s.outputNone) + "}, \"reducedPercent\": " + number(f.reducedPercent) +
            ", \"longestReducedS\": " + std::to_string(f.longestReducedS) + ", \"lastHourReducedPercent\": " + number(f.lastHourReducedPercent) +
            ", \"invariants\": \"" + inv + "\"}" + (k + 1 < ge::kPushSimReviewTraceCount ? ",\n" : "\n");
    }
    // Review 5's traces (PushGovernorSim.h pushRunReview5Trace).
    json += "  ],\n  \"review5Traces24h\": [\n";
    std::fprintf(stderr, "\n| review 5 trace (24 h, Linux model) | changes | rebuilds | max rebuilds / 60 s | rollbacks | s over budget gov / none | output fps gov / none | time reduced | longest reduced (s) | invariants |\n|---|---|---|---|---|---|---|---|---|---|\n");
    for (int k = 0; k < ge::kPushSimReview5TraceCount; ++k) {
        const auto s = ge::pushRunReview5Trace(k, 86400, config);
        const auto f = ge::pushSimTraceFacts(s);
        if (!s.firstViolation.empty()) ++violations;
        const std::string inv = s.firstViolation.empty() ? std::string("hold") : s.firstViolation;
        std::fprintf(stderr, "| %s | %u | %u | %u | %u | %u / %u | %s / %s | %s %% | %u | %s |\n", s.scenario.c_str(), s.changes, s.rebuilds, f.maxRebuildsPerMinute,
            s.rollbacks, s.overBudgetGovernor, s.overBudgetNone, number(s.outputGovernor).c_str(), number(s.outputNone).c_str(), number(f.reducedPercent).c_str(),
            f.longestReducedS, inv.c_str());
        json += "    {\"trace\": \"" + s.scenario + "\", \"changes\": " + std::to_string(s.changes) + ", \"graphRebuilds\": " + std::to_string(s.rebuilds) +
            ", \"maxRebuildsPer60s\": " + std::to_string(f.maxRebuildsPerMinute) + ", \"rollbacks\": " + std::to_string(s.rollbacks) +
            ", \"secondsOverBudget\": {\"governor\": " + std::to_string(s.overBudgetGovernor) + ", \"none\": " + std::to_string(s.overBudgetNone) +
            "}, \"meanOutputFps\": {\"governor\": " + number(s.outputGovernor) + ", \"none\": " + number(s.outputNone) + "}, \"reducedPercent\": " + number(f.reducedPercent) +
            ", \"longestReducedS\": " + std::to_string(f.longestReducedS) + ", \"invariants\": \"" + inv + "\"}" + (k + 1 < ge::kPushSimReview5TraceCount ? ",\n" : "\n");
    }
    // Randomized traces (the same seeds as the tests use, and more).
    unsigned randomViolations = 0, randomChanges = 0;
    const int traces = 400;
    for (int k = 0; k < traces; ++k) {
        const auto t = ge::pushSimRandomTrace(1000u + static_cast<std::uint64_t>(k), 600);
        const auto s = ge::pushRunGovernorSim("random", 600, [&t](int sec) { return t.worlds[static_cast<std::size_t>(sec)]; }, 77u + static_cast<std::uint64_t>(k), config, t.request, t.actions);
        randomChanges += s.changes;
        if (!s.firstViolation.empty()) { ++randomViolations; std::fprintf(stderr, "random trace %d: %s\n", k, s.firstViolation.c_str()); }
    }
    json += "  ],\n  \"randomTraces\": {\"count\": " + std::to_string(traces) + ", \"secondsEach\": 600, \"changes\": " + std::to_string(randomChanges) +
        ", \"invariantViolations\": " + std::to_string(randomViolations) + "}\n}\n";
    std::fputs(json.c_str(), stdout);
    std::fprintf(stderr, "random traces: %d x 600 s, %u changes, %u with a violation\n", traces, randomChanges, randomViolations);
    return violations + randomViolations == 0 ? 0 : 1;
}
