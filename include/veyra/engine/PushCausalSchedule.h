// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg", step 4: the engine's side of the experimental fully causal schedule (pipeline/PushCausal.h). One object
// per playback beside the one-predicted schedule (PushLowDelaySchedule, the other engineer's): the ladder, its own phase
// learner, the per-pair ask, and the log lines on a change of level. EngineController calls it through short hooks marked
// "stream ownfg"; nothing here asks for anything unless the user chose the causal schedule (pref pushLowDelayCausal).
// Pure; unit tests: tests/remoteplay/CausalTests.inc.
#include "veyra/engine/LivePairLatency.h"
#include "veyra/pipeline/PushCausal.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::engine {

struct PushCausalRun {
    pipeline::PushCausalLadder ladder;
    LivePairLatency latency;          // the causal schedule's own phase learner (its frames: B, B + d/3, B + 2d/3)
    bool everChosen = false;          // the evidence is added to the logs from the first choice on
    bool asked = false;               // the last pair was asked to run causal (setPairSchedule)
    bool previousCausal = false;      // the last pair that generated ran causal (a switch restarts the learner)
    std::int64_t phase = 0;
    std::uint64_t windowPairs = 0;    // causal pairs in the current log window
    pipeline::PushCausalLevel loggedLevel = pipeline::PushCausalLevel::Causal;
    pipeline::PushCausalExit loggedStop = pipeline::PushCausalExit::None;

    // Per pair, before it is scheduled. modeAsks3x: the one-predicted schedule asks this pair (3X); modeAsks2x: the mode
    // would run at 2X (Smoothness Mode's multiplier) where [B, P(1/2)] replaces [I(1/2), B]; graphReady: the graph can run
    // a causal pair (EnhanceGraph::causalReady).
    bool ask(bool chosen, bool modeAsks3x, bool modeAsks2x, bool graphReady) {
        ladder.chosen(chosen);
        everChosen = everChosen || chosen;
        if (!chosen) { loggedLevel = pipeline::PushCausalLevel::Causal; loggedStop = pipeline::PushCausalExit::None; previousCausal = false; }
        return graphReady && ladder.ask(modeAsks3x || modeAsks2x);
    }
    // The phase of an asked pair (LivePairLatency::select with pushCausalPhaseArgs); a switch into it starts afresh.
    std::int64_t select(std::int64_t now100ns, std::int64_t interval100ns, unsigned multiplier, std::int64_t legacy100ns, std::int64_t allowance100ns,
                        std::int64_t predictedPhase100ns) {
        if (!previousCausal) latency.reset();
        const auto args = pipeline::pushCausalPhaseArgs(interval100ns, multiplier, legacy100ns, allowance100ns, predictedPhase100ns);
        phase = latency.select(now100ns, args.start, interval100ns, multiplier, args.allowance);
        return phase;
    }
    // After the pair: what ran. Returns a log line when the ladder changed level (empty otherwise).
    std::string pair(bool generated, bool ranCausal, bool ranOnePredicted) {
        if (generated) { previousCausal = ranCausal; windowPairs += ranCausal; }
        ladder.pair(ranCausal, ranOnePredicted && !ranCausal);
        std::string t = pending;
        pending.clear();
        const auto now = changeText();
        return t.empty() ? now : now.empty() ? t : t + "; " + now;
    }
    std::string verdict(const pipeline::PushCausalVerdict& v, double budgetMs) {
        ladder.verdict(v, budgetMs);
        return changeText();
    }
    // From the presentation loop (no log there): a change is logged with the next pair.
    void outcome(bool droppedLate) {
        ladder.outcome(droppedLate);
        const auto t = changeText();
        if (!t.empty()) pending += (pending.empty() ? "" : "; ") + t;
    }
    // The admission group of an asked causal pair (ms): both predictions' measured P95 once timed, else the interpolated
    // group's cost.
    double admissionGroupMs(double interpolatedGroupMs) const {
        return pipeline::pushCausalAdmissionGroupMs(interpolatedGroupMs, ladder.lastCostP95(), ladder.costSamples());
    }
    // The evidence for [capture-timing] (once a window: the unusable shares and the window's pair count start again) and
    // for [fg-health] (`clear` false). Empty while the choice was never on.
    std::string evidence(const char* scheduleNow, bool clear) {
        auto t = pipeline::pushCausalText(ladder, everChosen, scheduleNow);
        if (!t.empty()) {
            char b[64];
            std::snprintf(b, sizeof b, " causalWindowPairs=%llu", static_cast<unsigned long long>(windowPairs));
            t += b;
        }
        if (clear) {
            pipeline::pushCausalShared().causalPerSecond.store(static_cast<std::uint32_t>(windowPairs), std::memory_order_relaxed);
            windowPairs = 0;
            ladder.clearShares();
            publish();
        }
        return t;
    }
    void publish() const {
        auto& s = pipeline::pushCausalShared();
        s.level.store(static_cast<std::uint8_t>(ladder.level()), std::memory_order_relaxed);
        s.lastExit.store(static_cast<std::uint8_t>(ladder.lastExit()), std::memory_order_relaxed);
        s.stoppedFor.store(static_cast<std::uint8_t>(ladder.stoppedFor()), std::memory_order_relaxed);
        s.returnHave.store(ladder.returnHave(), std::memory_order_relaxed);
        s.returnNeed.store(ladder.returnNeed(), std::memory_order_relaxed);
    }

private:
    std::string pending;
    std::string changeText() {
        std::string t;
        if (ladder.level() != loggedLevel) {
            char b[256];
            if (ladder.level() == pipeline::PushCausalLevel::OnePredicted)
                std::snprintf(b, sizeof b, "causal schedule left for ONE predicted frame (one slot, not interpolation) reason=%s exits=%llu; returns after %u calm pairs",
                              pipeline::pushCausalExitName(ladder.lastExit()), static_cast<unsigned long long>(ladder.exitsTotal()), ladder.returnNeed());
            else
                std::snprintf(b, sizeof b, "causal schedule entered again after calm one-predicted pairs (exits so far %llu)", static_cast<unsigned long long>(ladder.exitsTotal()));
            t = b;
            loggedLevel = ladder.level();
        }
        if (ladder.stoppedFor() != loggedStop) {
            loggedStop = ladder.stoppedFor();
            if (loggedStop != pipeline::PushCausalExit::None)
                t += std::string(t.empty() ? "" : "; ") + "causal schedule stopped for this playback after three exits for " + pipeline::pushCausalExitName(loggedStop) +
                     " (switch the choice off and on to try again)";
        }
        if (!t.empty()) publish();
        return t;
    }
};

// The outcome of a presented or dropped predicted frame: a causal pair's to the causal ladder, any other to the
// one-predicted schedule (PushLowDelaySchedule::predictedOutcome), so a causal frame never drives that schedule's exits.
template <class OnePredicted>
inline void pushCausalOutcome(OnePredicted& onePredicted, PushCausalRun& run, bool causalLease, bool droppedLate) {
    if (causalLease) run.outcome(droppedLate);
    else onePredicted.predictedOutcome(droppedLate);
}

}  // namespace veyra::engine
