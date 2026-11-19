// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Picture numbers for hardware-decoded Remote Play pictures (experiment VEYRA_EXP_RP_STREAM_DEDUP).
//
// The engine's duplicate removal (ContentDedup.h) needs a picture number per frame. A CPU frame
// gets one from a whole-picture comparison; a hardware frame had none, so every frame was kept.
// Here the GPU measurement of the decoded picture (pipeline/StreamChange.h) goes through the
// cadence classifier (remoteplay/StreamCadence.h) and comes out as a number, or as no number.
//
// No number means exactly what happened before this existed: the frame is kept. That is the
// answer whenever anything is in doubt:
//   - the GPU gave no valid measurement (error, timeout, pass not available);
//   - the classifier has not found the doubled rhythm (a 60 fps game never gets numbers), or
//     its trust in the rhythm is below the confidence floor;
//   - the classifier found a likely repeat but not with enough confidence (its fallback).
// A measurement that arrived late (after the engine's bounded wait) is still shown to the
// classifier, so one slow answer does not cost it the rhythm; the frame it belonged to was kept.
//
// Portable: no platform types; tests in tests/remoteplay/StreamCadenceTests.inc.
#include "veyra/pipeline/StreamChange.h"
#include "veyra/remoteplay/StreamCadence.h"
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace veyra::engine {

class PushStreamDedup {
public:
    // Numbers live in their own space (top byte), apart from the engine's and the capture
    // callback's whole-picture comparisons (pipeline::PictureReference spaces 1 and 2).
    static constexpr std::uint64_t kSpace = std::uint64_t(3) << 56;

    struct Decision {
        bool numbered = false;       // false: give ContentDedup no picture number (keep the frame)
        std::uint64_t picture = 0;
        bool repeat = false;         // numbered and the same picture as the frame before
        remoteplay::StreamCadence::Result cadence;   // what the classifier said (all zero when unmeasured)
    };
    struct Stats {
        std::uint64_t pictures = 0;    // frames offered
        std::uint64_t measured = 0;    // with a valid GPU measurement
        std::uint64_t unmeasured = 0;  // without one: kept, no number
        std::uint64_t late = 0;        // measurements that arrived one frame late and were still used
        std::uint64_t numbered = 0;    // frames given a number
        std::uint64_t repeats = 0;     // of those, repeats
        std::uint64_t restarts = 0;    // times the classifier was told to start again
    };

    explicit PushStreamDedup(remoteplay::StreamCadenceConfig config = {}) : cadence_(config) {}

    // The experiment's switch, VEYRA_EXP_RP_STREAM_DEDUP: on only for the value "1". Unset, empty,
    // "0" and everything else leave it off.
    static constexpr bool switchedOn(std::wstring_view value) { return value == L"1"; }

    // Whether the engine measures a frame at all. The measurement makes the engine thread wait,
    // so it runs only where a removed repeat pays for it: the experiment is on, frame generation
    // runs, duplicate removal is active, and the frame is a hardware picture without a number.
    // Frames that are not measured leave a gap in `sequence`, which restarts the rhythm.
    static constexpr bool measures(bool experiment, bool frameGeneration, bool duplicateRemoval, bool hardwarePicture, bool numbered) {
        return experiment && frameGeneration && duplicateRemoval && hardwarePicture && !numbered;
    }

    // outcome: EnhanceGraph::measureStreamChange for this frame.
    // sequence: the source's count of decoded pictures; a gap means the engine never saw one.
    // boundary: open, seek, reset, discontinuity: nothing before this frame is comparable.
    // sceneChange: the caller knows this picture starts another scene (the engine does not; the
    // evaluation tool can tell the classifier, to score it with and without).
    Decision observe(const pipeline::StreamChangeOutcome& outcome, std::uint64_t sequence, std::int64_t pts100ns, bool boundary, bool sceneChange = false) {
        ++stats_.pictures;
        Decision decision;
        bool restart = boundary || restartNext_ || (haveSequence_ && sequence != lastSequence_ + 1);
        haveSequence_ = true; lastSequence_ = sequence; restartNext_ = false;
        if (lateExpected_) {
            lateExpected_ = false;
            // The frame before this one was kept without a number; its measurement is here now.
            if (outcome.hasLate && outcome.late.valid && !restart) {
                ++stats_.late;
                (void)cadence_.observe(input(outcome.late, latePts_, lateRestart_));
            } else restart = true;
        }
        if (!outcome.now.valid) {
            ++stats_.unmeasured;
            if (outcome.queued) { lateExpected_ = true; lateRestart_ = restart; latePts_ = pts100ns; }
            else restartNext_ = true;
            return decision;
        }
        ++stats_.measured;
        if (restart || !outcome.now.comparable) ++stats_.restarts;
        auto now = input(outcome.now, pts100ns, restart);
        now.sceneChange = sceneChange;
        decision.cadence = cadence_.observe(now);
        // A number only while the rhythm is trusted and the classifier's confidence in this answer
        // reaches its floor; never for a likely repeat that was kept for lack of evidence, and
        // never outside the rhythm, however little the picture's blocks moved.
        decision.numbered = decision.cadence.locked && !decision.cadence.fallback &&
                            decision.cadence.confidence >= cadence_.config().confidenceFloor;
        if (decision.numbered) {
            ++stats_.numbered;
            decision.picture = kSpace | (decision.cadence.picture & 0x00FFFFFFFFFFFFFFull);
            decision.repeat = decision.cadence.repeat;
            if (decision.repeat) ++stats_.repeats;
        }
        return decision;
    }

    const remoteplay::StreamCadence& cadence() const { return cadence_; }
    const Stats& stats() const { return stats_; }

private:
    static remoteplay::StreamCadence::Input input(const pipeline::StreamChangeMeasurement& m, std::int64_t pts100ns, bool restart) {
        remoteplay::StreamCadence::Input in;
        in.change = m.maxBlock; in.meanChange = m.meanBlock; in.changedShare = m.changedShare;
        in.pixelMax = m.pixelMax; in.pixelsOver = m.pixelsOver;
        in.pts100ns = pts100ns; in.boundary = restart || !m.comparable;
        return in;
    }

    remoteplay::StreamCadence cadence_;
    Stats stats_;
    std::uint64_t lastSequence_ = 0;
    std::int64_t latePts_ = 0;
    bool haveSequence_ = false, restartNext_ = false, lateExpected_ = false, lateRestart_ = false;
};

} // namespace veyra::engine
