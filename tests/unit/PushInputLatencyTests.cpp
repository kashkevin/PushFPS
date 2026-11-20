// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test, pure part: telling the PS5's answer (the control centre
// at the bottom of the picture) apart from normal game motion, and the summary.
#include "veyra/engine/PushInputLatency.h"

#include <cstdio>
#include <string>

using namespace veyra::engine;
namespace {
int failures = 0;
void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}
quality::Luma frame(float base, float wobble, float bottom = -1) {
    quality::Luma l; l.y.resize(size_t(quality::kGridW) * quality::kGridH);
    for (uint32_t y = 0; y < quality::kGridH; ++y)
        for (uint32_t x = 0; x < quality::kGridW; ++x)
            l.y[size_t(y) * quality::kGridW + x] = (bottom >= 0 && y > quality::kGridH * 0.7) ? bottom : base + wobble * float((x + y) % 7) / 7.0f;
    return l;
}
} // namespace

int main() {
    latency::Detector d;
    check(!d.ready(), "no frames yet: not ready");
    // Game motion: frames differ a little.
    d.before(frame(0.40f, 0.00f)); d.before(frame(0.40f, 0.02f)); d.before(frame(0.40f, 0.01f));
    check(d.ready(), "ready after a few frames");
    check(!d.answered(frame(0.40f, 0.02f)), "normal motion is not an answer");
    check(d.answered(frame(0.40f, 0.01f, 0.9f)), "the control centre at the bottom is");
    check(d.threshold() >= 0.04, "threshold never below the floor");
    // Busy game: the threshold follows the motion.
    latency::Detector busy;
    busy.before(frame(0.2f, 0.0f)); busy.before(frame(0.5f, 0.0f)); busy.before(frame(0.25f, 0.0f));
    check(busy.threshold() > 0.4, "a busy picture raises the threshold");
    check(latency::regionDiff(frame(0.1f, 0), frame(0.1f, 0, 0.6f), 0.55f) > 0.1, "difference is measured in the bottom part");
    check(latency::regionDiff(frame(0.1f, 0), frame(0.1f, 0, 0.6f), 0.9f) > latency::regionDiff(frame(0.1f, 0), frame(0.1f, 0, 0.6f), 0.0f),
          "a lower region concentrates on the answer");
    // Latency test v2, OPTIONS / D-pad: a small highlight moving anywhere in a still menu is an answer.
    {
        latency::Detector menu; menu.setMode(latency::Detector::Mode::Anywhere);
        menu.before(frame(0.30f, 0.0f)); menu.before(frame(0.30f, 0.0f)); menu.before(frame(0.30f, 0.0f));
        quality::Luma moved = frame(0.30f, 0.0f);
        for (uint32_t y = 20; y < 32; ++y) for (uint32_t x = 40; x < 60; ++x) moved.y[size_t(y) * quality::kGridW + x] = 0.9f;   // 240 cells, top left
        check(!menu.answered(frame(0.30f, 0.0f)), "anywhere: an unchanged menu is not an answer");
        check(menu.answered(moved), "anywhere: a highlight in the top half is an answer");
        check(!d.answered(moved) || d.mode() == latency::Detector::Mode::Bottom, "the PS button's detector keeps its bottom region");
        check(latency::changedShare(frame(0.3f, 0), moved) > 0.004 && latency::changedShare(frame(0.3f, 0), frame(0.3f, 0)) == 0, "changed share");
        menu.reset();
        check(menu.mode() == latency::Detector::Mode::Anywhere && !menu.ready(), "reset keeps the mode");
    }
    const auto s = latency::summarize({80, 60, 100, 70}, 5);
    check(s.trials == 5 && s.answered == 4 && s.minMs == 60 && s.maxMs == 100 && s.medianMs == 75 && s.meanMs == 77.5, "summary");
    check(latency::summarize({}, 3).answered == 0, "no answers");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
