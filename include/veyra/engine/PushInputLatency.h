// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS automatic Remote Play latency test. PushFPS presses a button on the PS5 through Remote Play
// (chiaki-ng), watches the presented picture for the answer (the PS button opens the control centre
// at the bottom of the screen) and times press-to-picture. PushFPS's own share is its measured
// processing time (arrival to presentation); the rest is the PS5, the network and decoding. Not a
// physical controller-to-photon measurement: the pad's own delay and the monitor are not included.
// This header: the pure part (change detection, summary; tests/unit/PushInputLatencyTests.cpp).
// PushInputLatencyProbe.h: the readback of presented frames (a PresentCapture).
#pragma once
#include "veyra/engine/PushQuality.h"

#include <array>
#include <cstdint>
#include <vector>

namespace veyra::engine::latency {

// Mean absolute luma difference inside the part of the picture below `top` (0..1).
double regionDiff(const quality::Luma& a, const quality::Luma& b, float top = 0.55f);
// Share (0..1) of grid cells below `top` whose luma changed by more than `cell`: finds a small change (a menu
// highlight moving) that a mean over the whole picture would hide.
double changedShare(const quality::Luma& a, const quality::Luma& b, float top = 0.0f, float cell = 0.1f);

// Decides when the picture answered: the difference to the last frame before the press stands out
// clearly from the frame-to-frame motion measured before it.
//
// Anywhere mode (latency test v2, docs/LATENCY_MEASUREMENT.md "Moving pictures"): the change that motion alone
// makes is learnt the way the answer is judged, against one fixed frame (the first of the frames before the press),
// and it is scaled to how long after the press a frame is judged. A press whose picture was not still enough
// before it, or whose picture changed sooner than the console could have answered (under one stream frame), is
// reported as PictureMoving: not measurable, never as a time. An answer is also confirmed by a jump: the frame differs
// that much from the frame kJumpFrames before it too, so a slow drift (a character starting to walk, a pan, a fade)
// that adds up over many frames is not one. The confirmation never moves the time (review delta1 blocker 2): the
// answer is the FIRST frame from which the change against the frame before the press stayed above the threshold
// (onsetFramesBack()), so steps, slides and fades report their onset. A change that is still not confirmed
// kOnsetFrames frames after that onset is too slow: not found (no answer), never a time.
class Detector {
public:
    // How the answer is recognised. Bottom (the default, the PS button's control centre): the mean difference
    // below 55 % of the height. Anywhere (OPTIONS, D-pad; latency test v2): the mean difference over the whole
    // picture, or enough changed cells anywhere (changedShare). reset() keeps the mode.
    enum class Mode : uint8_t { Bottom, Anywhere };
    enum class Verdict : uint8_t { Waiting, Answered, PictureMoving };
    // Anywhere: still enough to tell when what changed against the first frame before the press stays under these
    // (half the answer floors below once tripled: motion never raises a floor more than 2x).
    static constexpr double kFloorMean = 0.04, kFloorShare = 0.0015;
    static constexpr double kStillMean = 2.0 * kFloorMean / 3.0, kStillShare = 2.0 * kFloorShare / 3.0;
    static constexpr int kMinStillFrames = 4;   // frames before the press needed to call the picture still
    static constexpr int kJumpFrames = 4;       // the jump is measured against the frame this many frames earlier
    static constexpr int kOnsetFrames = 12;     // the jump must confirm within this many frames of the onset (200 ms)
    void setMode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }
    // Anywhere: a change seen sooner than this after the press cannot be the console's answer (one stream frame);
    // reset() keeps it. 0 (the default) = no limit.
    void setMinimumAnswerMs(double ms) { minAnswerMs_ = ms > 0 ? ms : 0; }
    void reset();
    void before(const quality::Luma& frame);           // frames before the press
    bool answered(const quality::Luma& frame) const;    // a frame after the press (Bottom; Anywhere: first frame's rule)
    // A frame after the press, msSincePress after it. Bottom: answered() and nothing else. Anywhere: the rules above;
    // call once per frame, in order.
    Verdict judge(const quality::Luma& frame, double msSincePress);
    double threshold() const;
    bool ready() const { return base_.valid() && samples_ >= 2; }
    // Anywhere: the frames before the press were still enough to tell an answer from motion (Bottom: ready()).
    bool still() const;
    // Anywhere: the last PictureMoving verdict came from a change under the minimum answer time after a picture that
    // was still before the press (the picture changed by itself), not from a picture that was moving before it.
    bool changedEarly() const { return early_; }
    // After Answered: how many judged frames before the one just judged the answer began (0 = this frame; Bottom
    // mode: always 0). The answer's time is that frame's.
    int onsetFramesBack() const { return onsetBack_; }
private:
    bool changed(const quality::Luma& reference, const quality::Luma& frame, int frames) const;
    void remember(const quality::Luma& frame);
    quality::Luma base_, previous_, first_;
    std::array<quality::Luma, kJumpFrames> recent_{};   // Anywhere: the last frames seen, oldest at recentNext_
    int recentNext_ = 0;
    Mode mode_ = Mode::Bottom;
    double noise_ = 0, shareNoise_ = 0;
    double driftMean_ = 0, driftShare_ = 0;   // Anywhere: most change against first_ before the press
    double minAnswerMs_ = 0;
    int samples_ = 0, after_ = 0;
    bool early_ = false;
    int streak_ = 0, onsetBack_ = 0;                     // Anywhere: judged frames in a row above the threshold
    std::array<double, kOnsetFrames> judgedMs_{};        // Anywhere: msSincePress of the last judged frames
};

struct Summary {
    uint32_t trials = 0, answered = 0;
    double meanMs = 0, medianMs = 0, minMs = 0, maxMs = 0;
};
Summary summarize(const std::vector<double>& ms, uint32_t trials);

} // namespace veyra::engine::latency
