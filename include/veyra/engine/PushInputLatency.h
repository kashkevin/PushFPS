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
class Detector {
public:
    // How the answer is recognised. Bottom (the default, the PS button's control centre): the mean difference
    // below 55 % of the height. Anywhere (OPTIONS, D-pad; latency test v2): the mean difference over the whole
    // picture, or enough changed cells anywhere (changedShare). reset() keeps the mode.
    enum class Mode : uint8_t { Bottom, Anywhere };
    void setMode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }
    void reset();
    void before(const quality::Luma& frame);           // frames before the press
    bool answered(const quality::Luma& frame) const;    // a frame after the press
    double threshold() const;
    bool ready() const { return base_.valid() && samples_ >= 2; }
private:
    quality::Luma base_, previous_;
    Mode mode_ = Mode::Bottom;
    double noise_ = 0, shareNoise_ = 0;
    int samples_ = 0;
};

struct Summary {
    uint32_t trials = 0, answered = 0;
    double meanMs = 0, medianMs = 0, minMs = 0, maxMs = 0;
};
Summary summarize(const std::vector<double>& ms, uint32_t trials);

} // namespace veyra::engine::latency
