// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "s12-settings": [capture-driver-age] measures graph clock now - (run reference + the driver's sample time).
// The driver stamps samples with the DEVICE's clock, so the difference drifts with the two clocks (his Package 16 log:
// p50 138 -> 155 -> 174 ms in 40 s, about 0.9 ms per second: clock drift, not a queue). This fits a line through one
// window's (callback time, age) points and reports the drift and the spread around the line, which is what a queue in
// the driver would change. Pure; tests in tests/remoteplay/SettingsEffectTests.inc.
#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace veyra::source {

struct PushDriverAgeFit {
    bool valid = false;
    double driftMsPerS = 0;      // slope of the age against callback time
    double residualP95Ms = 0;    // |age - line|, 95th percentile: the variation a driver queue would show
};

// points: (callback time in seconds, age in ms), in arrival order.
inline PushDriverAgeFit pushDriverAgeFit(const std::vector<std::pair<double, double>>& points) {
    PushDriverAgeFit fit;
    const std::size_t n = points.size();
    if (n < 3) return fit;
    double st = 0, sa = 0;
    for (const auto& p : points) { st += p.first; sa += p.second; }
    const double mt = st / double(n), ma = sa / double(n);
    double stt = 0, sta = 0;
    for (const auto& p : points) { stt += (p.first - mt) * (p.first - mt); sta += (p.first - mt) * (p.second - ma); }
    if (!(stt > 0)) return fit;
    fit.driftMsPerS = sta / stt;
    std::vector<double> residuals;
    residuals.reserve(n);
    for (const auto& p : points) residuals.push_back(std::fabs(p.second - (ma + fit.driftMsPerS * (p.first - mt))));
    std::sort(residuals.begin(), residuals.end());
    fit.residualP95Ms = residuals[(n - 1) * 95 / 100];
    fit.valid = std::isfinite(fit.driftMsPerS) && std::isfinite(fit.residualP95Ms);
    return fit;
}

}  // namespace veyra::source
