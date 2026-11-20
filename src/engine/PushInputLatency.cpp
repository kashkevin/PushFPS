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

void Detector::reset() { base_ = {}; previous_ = {}; noise_ = 0; shareNoise_ = 0; samples_ = 0; }
void Detector::before(const quality::Luma& frame) {
    if (!frame.valid()) return;
    const float top = mode_ == Mode::Bottom ? 0.55f : 0.0f;
    if (previous_.valid()) {
        noise_ = std::max(noise_ * 0.8, regionDiff(previous_, frame, top));   // decaying peak of normal motion
        if (mode_ == Mode::Anywhere) shareNoise_ = std::max(shareNoise_ * 0.8, changedShare(previous_, frame));
    }
    previous_ = frame; base_ = frame; ++samples_;
}
double Detector::threshold() const { return std::max(0.04, noise_ * 3.0); }
bool Detector::answered(const quality::Luma& frame) const {
    if (!ready() || !frame.valid()) return false;
    if (mode_ == Mode::Bottom) return regionDiff(base_, frame) > threshold();
    // About 90 cells of the 320 x 180 grid (a highlight's outline), and well above what motion alone changed.
    return regionDiff(base_, frame, 0.0f) > threshold() || changedShare(base_, frame) > std::max(0.0015, shareNoise_ * 3.0);
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
