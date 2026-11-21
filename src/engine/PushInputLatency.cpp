// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test: change detection and summary (pure).
#include "veyra/engine/PushInputLatency.h"

#include <algorithm>
#include <cmath>

namespace veyra::engine::latency {

double regionDiff(const quality::Luma& a, const quality::Luma& b, float top) {
    if (!a.valid() || !b.valid()) return 0;
    const uint32_t y0 = std::min(quality::kGridH - 1, uint32_t(std::clamp(top, 0.0f, 1.0f) * float(quality::kGridH)));
    double sum = 0; size_t n = 0;
    for (uint32_t y = y0; y < quality::kGridH; ++y)
        for (uint32_t x = 0; x < quality::kGridW; ++x) {
            const size_t i = size_t(y) * quality::kGridW + x;
            sum += std::abs(double(a.y[i]) - double(b.y[i])); ++n;
        }
    return n ? sum / double(n) : 0;
}

double changedShare(const quality::Luma& a, const quality::Luma& b, float top, float cell) {
    if (!a.valid() || !b.valid()) return 0;
    const uint32_t y0 = std::min(quality::kGridH - 1, uint32_t(std::clamp(top, 0.0f, 1.0f) * float(quality::kGridH)));
    size_t changed = 0, n = 0;
    for (uint32_t y = y0; y < quality::kGridH; ++y)
        for (uint32_t x = 0; x < quality::kGridW; ++x) {
            const size_t i = size_t(y) * quality::kGridW + x;
            changed += std::abs(a.y[i] - b.y[i]) > cell ? 1u : 0u; ++n;
        }
    return n ? double(changed) / double(n) : 0;
}

void Detector::reset() {
    base_ = {}; previous_ = {}; first_ = {}; recent_ = {}; recentNext_ = 0; noise_ = 0; shareNoise_ = 0; driftMean_ = 0; driftShare_ = 0; samples_ = 0; after_ = 0; early_ = false;
    streak_ = 0; onsetBack_ = 0; judgedMs_ = {};
}
void Detector::before(const quality::Luma& frame) {
    if (!frame.valid()) return;
    const float top = mode_ == Mode::Bottom ? 0.55f : 0.0f;
    if (previous_.valid()) {
        noise_ = std::max(noise_ * 0.8, regionDiff(previous_, frame, top));   // decaying peak of normal motion
        if (mode_ == Mode::Anywhere) shareNoise_ = std::max(shareNoise_ * 0.8, changedShare(previous_, frame));
    }
    if (mode_ == Mode::Anywhere) {
        // The change against one fixed frame, as an answer is judged: motion adds up here, noise does not.
        if (!first_.valid()) first_ = frame;
        else { driftMean_ = std::max(driftMean_, regionDiff(first_, frame, 0.0f)); driftShare_ = std::max(driftShare_, changedShare(first_, frame)); }
        remember(frame);
    }
    previous_ = frame; base_ = frame; ++samples_;
}
double Detector::threshold() const { return std::max(kFloorMean, noise_ * 3.0); }
bool Detector::still() const {
    if (mode_ == Mode::Bottom) return ready();
    return ready() && samples_ >= kMinStillFrames && driftMean_ <= kStillMean && driftShare_ <= kStillShare;
}
bool Detector::answered(const quality::Luma& frame) const {
    if (!ready() || !frame.valid()) return false;
    if (mode_ == Mode::Bottom) return regionDiff(base_, frame) > threshold();
    return changed(base_, frame, 1);
}
void Detector::remember(const quality::Luma& frame) {
    recent_[size_t(recentNext_)] = frame;
    recentNext_ = (recentNext_ + 1) % kJumpFrames;
}
bool Detector::changed(const quality::Luma& reference, const quality::Luma& frame, int frames) const {
    // About 90 cells of the 320 x 180 grid (a highlight's outline), and well above what motion alone changed over
    // the same number of frames (the drift was measured over samples_ - 1 frames; a frame judged later is allowed
    // proportionally more).
    // Only the drift is scaled (review delta1): noise (grain, encoder flicker) against a fixed frame is as large one
    // frame after it as sixty frames after, so scaling it would lose late answers under grain and bias the median
    // fast. The drift is what the change against the first frame added beyond the frame-to-frame noise.
    const double span = std::max(1, samples_ - 1);
    const double scale = std::max(1.0, double(frames) / span);
    const double driftMean = std::max(0.0, driftMean_ - noise_), driftShare = std::max(0.0, driftShare_ - shareNoise_);
    const double mean = std::max(kFloorMean, (noise_ + driftMean * scale) * 3.0);
    const double share = std::max(kFloorShare, (shareNoise_ + driftShare * scale) * 3.0);
    return regionDiff(reference, frame, 0.0f) > mean || changedShare(reference, frame) > share;
}
Detector::Verdict Detector::judge(const quality::Luma& frame, double msSincePress) {
    if (mode_ == Mode::Bottom) return answered(frame) ? Verdict::Answered : Verdict::Waiting;
    if (!ready() || !frame.valid()) return Verdict::Waiting;
    ++after_;
    if (!still()) return Verdict::PictureMoving;
    // The frame kJumpFrames before this one (a frame before the press while fewer have followed it).
    const quality::Luma& earlier = recent_[size_t(recentNext_)];
    const bool above = changed(base_, frame, after_);
    streak_ = above ? streak_ + 1 : 0;
    judgedMs_[size_t((after_ - 1) % kOnsetFrames)] = msSincePress;
    const bool jump = above && (!earlier.valid() || changed(earlier, frame, kJumpFrames));
    remember(frame);
    if (!jump) return Verdict::Waiting;
    // Confirmed: the answer began where the change against the frame before the press last rose above the threshold.
    // Too long ago to be one answer (a slow fade or drift): not found.
    if (streak_ > kOnsetFrames) return Verdict::Waiting;
    onsetBack_ = streak_ - 1;
    const double onsetMs = judgedMs_[size_t((after_ - 1 - onsetBack_) % kOnsetFrames)];
    // Changed before the console could have answered: the picture moves by itself.
    if (onsetMs < minAnswerMs_) { early_ = true; return Verdict::PictureMoving; }
    return Verdict::Answered;
}

Summary summarize(const std::vector<double>& ms, uint32_t trials) {
    Summary s;
    s.trials = trials; s.answered = uint32_t(ms.size());
    if (ms.empty()) return s;
    std::vector<double> v = ms;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double x : v) sum += x;
    s.meanMs = sum / double(v.size());
    s.medianMs = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    s.minMs = v.front(); s.maxMs = v.back();
    return s;
}

} // namespace veyra::engine::latency
