// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test, pure part: telling the PS5's answer (the control centre
// at the bottom of the picture) apart from normal game motion, and the summary.
#include "veyra/engine/PushInputLatency.h"
#include "veyra/engine/PushLatencyStats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
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
// Review reproduction (latency test v2, B1): a textured scene panned by `pan` cells, an optional 6 x 12-cell bright
// object at column objX, optional grain (deterministic, +-grain per cell, new every frame), a brightness offset
// (fades), a small blinking cursor and a menu panel (the answer).
struct Scene {
    double pan = 0; int objX = -1; float grain = 0, fade = 0; bool cursor = false, menu = false; uint32_t seed = 1;
    bool tile = false; int tileW = 8;   // a bright tile of tileW x 16 cells at (200, 40) (review delta1 det4)
};
quality::Luma scene(const Scene& s) {
    quality::Luma l; l.y.resize(size_t(quality::kGridW) * quality::kGridH);
    uint32_t r = s.seed * 2654435761u + 12345u;
    for (uint32_t y = 0; y < quality::kGridH; ++y)
        for (uint32_t x = 0; x < quality::kGridW; ++x) {
            const double u = double(x) + s.pan;
            float v = 0.4f + 0.25f * float(std::sin(u * 0.15) * std::cos(double(y) * 0.11)) + s.fade;
            if (s.objX >= 0 && int(x) >= s.objX && int(x) < s.objX + 6 && y >= 100 && y < 112) v = 0.95f;
            if (s.cursor && x >= 200 && x < 202 && y >= 40 && y < 52) v = 0.95f;            // 24 cells
            if (s.menu && x >= 30 && x < 130 && y >= 30 && y < 150) v = 0.12f;              // a menu panel
            if (s.tile && int(x) >= 200 && int(x) < 200 + s.tileW && y >= 40 && y < 56) v = 0.95f;
            if (s.grain > 0) { r = r * 1664525u + 1013904223u; v += s.grain * (float(r >> 8) / 8388608.0f - 1.0f); }
            l.y[size_t(y) * quality::kGridW + x] = v;
        }
    return l;
}
constexpr double kFrameMs = 1000.0 / 60.0;
// `pre` frames before the press, then up to 72 (1.2 s) after it; the frame index (1-based) of the first verdict that
// is not Waiting, and that verdict. sceneAt(i): i counts all frames from the first one before the press.
template <class F> std::pair<int, latency::Detector::Verdict> run(F sceneAt, int pre = 18, double minAnswerMs = kFrameMs) {
    latency::Detector d; d.setMode(latency::Detector::Mode::Anywhere); d.setMinimumAnswerMs(minAnswerMs);
    for (int i = 0; i < pre; ++i) d.before(scene(sceneAt(i)));
    for (int k = 1; k <= 72; ++k) {
        const auto v = d.judge(scene(sceneAt(pre + k - 1)), double(k) * kFrameMs);
        if (v != latency::Detector::Verdict::Waiting) return {k, v};
    }
    return {0, latency::Detector::Verdict::Waiting};
}
// Review delta1 blocker 1: the sham-press protocol as the bridge runs it (PushLatencyTest.cpp), on a model clock.
// Each slot: a random number of frames before the (real or sham) press, then up to 72 frames (the 1.2 s answer
// wait), then the rest of a ~3.3 s slot before the next one. sceneAt(globalFrame, framesSincePress (<= 0 before
// it), realPress) gives the picture. The detector is judged exactly as for a press; shams go to shamPress().
struct ShamRun { latencystats::TestSummary summary; int realAnswered = 0; };
template <class F> ShamRun shamProtocol(F sceneAt, int real, uint64_t seed) {
    latencystats::Random random(seed);
    const auto schedule = latencystats::shamSchedule(random, real, latencystats::shamCount(real));
    std::vector<latencystats::PressSplit> presses;
    ShamRun out;
    long frameNo = 0;
    for (const bool sham : schedule) {
        latency::Detector d; d.setMode(latency::Detector::Mode::Anywhere); d.setMinimumAnswerMs(kFrameMs);
        const int pre = 18 + int(random.below(5));
        const double phase = random.unit() * kFrameMs;
        for (int i = 0; i < pre; ++i) d.before(scene(sceneAt(frameNo++, i - pre + 1, !sham)));
        auto verdict = latency::Detector::Verdict::Waiting; int k = 1;
        for (; k <= 72; ++k) {
            verdict = d.judge(scene(sceneAt(frameNo++, k, !sham)), double(k) * kFrameMs - phase);
            if (verdict != latency::Detector::Verdict::Waiting) break;
        }
        const bool answered = verdict == latency::Detector::Verdict::Answered;
        const bool moving = verdict == latency::Detector::Verdict::PictureMoving;
        if (sham) presses.push_back(latencystats::shamPress(answered || (moving && d.changedEarly()), moving, answered ? double(k) * kFrameMs : -1));
        else if (moving) presses.push_back(latencystats::pictureMovingPress());
        else if (answered) { latencystats::PressSplit p; p.totalMs = p.sendToArrivalMs = double(k) * kFrameMs; presses.push_back(p); ++out.realAnswered; }
        else presses.push_back(latencystats::PressSplit{});
        frameNo += 200 - pre - std::min(k, 72);   // the rest of the slot (close, settle)
    }
    out.summary = latencystats::summarizeTest(presses, real, latencystats::Button::Options);
    return out;
}
// Review delta1 blocker 2 (the reviewers' det2 / det3 shapes): a panel appearing at frame `at` after the press over
// `frames` frames. shape 0 ease-out, 1 slide-in, 2 smoothstep, 3 linear (amplitude 0.15), 4 ease-in (t^2), 5 linear.
quality::Luma transition(int k, int at, int frames, int shape, bool small) {
    Scene base; quality::Luma l = scene(base);
    if (k < at) return l;
    double t = std::min(1.0, double(k - at + 1) / double(frames));
    if (shape == 0) t = 1 - (1 - t) * (1 - t);
    if (shape == 2) t = t * t * (3 - 2 * t);
    if (shape == 4) t = t * t;
    const uint32_t x1 = small ? 60 : 130, y1 = small ? 45 : 150;
    if (shape == 1) {
        const uint32_t edge = uint32_t(double(x1) * t);
        for (uint32_t y = 30; y < y1; ++y) for (uint32_t x = 0; x < edge; ++x) l.y[size_t(y) * quality::kGridW + x] = 0.12f;
        return l;
    }
    const float amp = shape == 3 ? 0.15f : 0.3f;
    for (uint32_t y = 30; y < y1; ++y) for (uint32_t x = 30; x < x1; ++x) l.y[size_t(y) * quality::kGridW + x] -= float(amp * t);
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
    // Latency test v2, B1 (review): a moving picture never gives an answer. Anywhere mode.
    {
        using V = latency::Detector::Verdict;
        bool none = true; std::string where;
        for (const double speed : {0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0})
            for (const int pre : {6, 18, 30}) {
                // The reviewer's scene: one object walking over a still picture, no press answered.
                const auto walk = run([&](int i) { Scene s; s.objX = 50 + int(std::lround(double(i) * speed)); return s; }, pre);
                // A camera pan with the object, and a pan alone.
                const auto both = run([&](int i) { Scene s; s.pan = double(i) * speed; s.objX = 50 + i; return s; }, pre);
                const auto pan = run([&](int i) { Scene s; s.pan = double(i) * speed; return s; }, pre);
                for (const auto& r : {walk, both, pan})
                    if (r.second == V::Answered) { none = false; where += " speed=" + std::to_string(speed) + " pre=" + std::to_string(pre) + " k=" + std::to_string(r.first); }
            }
        check(none, "anywhere: an object walking or a pan, no press, never answers at any speed" + where);
        const auto walk = run([](int i) { Scene s; s.objX = 50 + i; return s; });
        check(walk.second == V::PictureMoving && walk.first == 1, "anywhere: the reviewer's walking object is reported as not measurable on the first frame");
        // A real menu change on a still picture: found on the frame it appears (frames 2..12 after the press).
        bool exact = true;
        for (int at = 2; at <= 12; ++at) {
            const auto r = run([&](int i) { Scene s; s.menu = i >= 18 + at - 1; return s; });
            exact = exact && r.second == V::Answered && r.first == at;
        }
        check(exact, "anywhere: a menu on a still picture is found on the frame it appears");
        // Grain: no false answer, and the menu still found on its frame.
        bool grainQuiet = true, grainFound = true;
        for (const float g : {0.01f, 0.03f}) {
            uint32_t seed = 1;
            const auto quiet = run([&](int) { Scene s; s.grain = g; s.seed = seed++; return s; });
            grainQuiet = grainQuiet && quiet.second != V::Answered;
            seed = 1;
            const auto found = run([&](int i) { Scene s; s.grain = g; s.seed = seed++; s.menu = i >= 18 + 5; return s; });
            grainFound = grainFound && found.second == V::Answered && found.first == 6;
        }
        check(grainQuiet, "anywhere: grain alone is not an answer");
        check(grainFound, "anywhere: a menu under grain is found on its frame");
        // A slow fade (brightness rising every frame, before and after the press): never an answer.
        bool fade = true;
        for (const double step : {0.0002, 0.0005, 0.001, 0.002, 0.005, 0.02}) {
            const auto r = run([&](int i) { Scene s; s.fade = float(double(i) * step); return s; });
            fade = fade && r.second != V::Answered;
        }
        check(fade, "anywhere: a slow fade is never an answer");
        // A blinking cursor (24 cells, 500 ms on / 500 ms off, any phase): never an answer.
        bool blink = true;
        for (int phase = 0; phase < 60; phase += 7) {
            const auto r = run([&](int i) { Scene s; s.cursor = ((i + phase) / 30) % 2 == 0; return s; });
            blink = blink && r.second != V::Answered;
        }
        check(blink, "anywhere: a blinking cursor is never an answer");
        // A change sooner than one stream frame after the press cannot be the console's: not measurable.
        const auto early = run([](int i) { Scene s; s.menu = i >= 18; return s; }, 18, 2.0 * kFrameMs);
        check(early.second == V::PictureMoving && early.first == 1, "anywhere: a change under one stream frame is not measurable");
        // Too few frames before the press to call the picture still: not measurable.
        const auto short_ = run([](int i) { Scene s; s.menu = i >= 3 + 4; return s; }, 3);
        check(short_.second == V::PictureMoving, "anywhere: three frames before the press are too few to tell");
        // The PS button's detector (Bottom) is unchanged: judge() is answered().
        latency::Detector bottom;
        bottom.before(frame(0.40f, 0.00f)); bottom.before(frame(0.40f, 0.02f)); bottom.before(frame(0.40f, 0.01f));
        check(bottom.judge(frame(0.40f, 0.02f), 0.0) == V::Waiting && bottom.judge(frame(0.40f, 0.01f, 0.9f), 0.0) == V::Answered && bottom.still(),
              "bottom: judge() is answered(), no time limit");
    }
    // Review delta1, statistics bias: noise is not scaled with elapsed frames (only drift is). Gaussian grain sd 0.02
    // (new every frame) and a small panel (30 x 15 cells, -0.1) at frame 6, 33 or 60 after the press: found equally
    // often (the reviewer's harness: 20/20 at 6-33, 0/20 at 60 before the fix), on its own frame.
    {
        using V = latency::Detector::Verdict;
        int foundAt[3] = {0, 0, 0}, exactAt[3] = {0, 0, 0};
        const int when[3] = {6, 33, 60};
        for (int w = 0; w < 3; ++w)
            for (uint32_t run = 0; run < 20; ++run) {
                latencystats::Random r(1000u + run);
                const auto grainy = [&](int k) {
                    Scene base; quality::Luma l = scene(base);
                    for (auto& v : l.y) {   // Box-Muller, deterministic
                        const double u1 = std::max(1e-12, r.unit()), u2 = r.unit();
                        v += float(0.02 * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
                    }
                    if (k >= when[w]) for (uint32_t y = 30; y < 45; ++y) for (uint32_t x = 30; x < 60; ++x) l.y[size_t(y) * quality::kGridW + x] -= 0.1f;
                    return l;
                };
                latency::Detector d; d.setMode(latency::Detector::Mode::Anywhere); d.setMinimumAnswerMs(kFrameMs);
                for (int i = 0; i < 18; ++i) d.before(grainy(-1));
                for (int k = 1; k <= 72; ++k) {
                    const auto v = d.judge(grainy(k), double(k) * kFrameMs);
                    if (v == V::Answered) { ++foundAt[w]; exactAt[w] += k - d.onsetFramesBack() == when[w] ? 1 : 0; break; }
                    if (v == V::PictureMoving) break;
                }
            }
        check(foundAt[0] == 20 && foundAt[1] == 20 && foundAt[2] == 20 && exactAt[0] == 20 && exactAt[1] == 20 && exactAt[2] == 20,
              "anywhere: under grain 0.02 a late answer is found as often as an early one (frame 6/33/60: " + std::to_string(foundAt[0]) + "/"
              + std::to_string(foundAt[1]) + "/" + std::to_string(foundAt[2]) + " of 20, on its frame " + std::to_string(exactAt[0]) + "/"
              + std::to_string(exactAt[1]) + "/" + std::to_string(exactAt[2]) + ")");
    }
    // Review delta1 blocker 2: the jump confirmation never moves the time. Every shape the reviewers tried (steps,
    // slides, ease-out / smoothstep / linear / ease-in fades of 1-30 frames, large and small panels, starting 2-40
    // frames after the press): when found, the reported frame (judged frame - onsetFramesBack) is the onset defined by
    // the threshold (the first frame whose change against the frame before the press is above it, the old first-frame
    // rule) within one frame; when that onset is more than kOnsetFrames before the confirmation, never a time.
    {
        using V = latency::Detector::Verdict;
        int found = 0, cases = 0, worst = 0, timedTooSlow = 0;
        std::string where;
        for (const int shape : {0, 1, 2, 3, 4, 5}) for (const int frames : {1, 2, 4, 6, 8, 12, 15, 20, 30})
            for (const int at : {2, 5, 8, 20, 40}) for (const bool small : {false, true}) {
                latency::Detector d; d.setMode(latency::Detector::Mode::Anywhere); d.setMinimumAnswerMs(kFrameMs);
                latency::Detector threshold; threshold.setMode(latency::Detector::Mode::Anywhere);
                for (int i = 0; i < 20; ++i) { const auto l = transition(i - 19, at, frames, shape, small); d.before(l); threshold.before(l); }
                int reported = -1, confirmed = -1, onset = -1;
                for (int k = 1; k <= 72; ++k) {
                    const auto l = transition(k, at, frames, shape, small);
                    if (onset < 0 && threshold.answered(l)) onset = k;
                    if (reported == -1) {
                        const auto v = d.judge(l, double(k) * kFrameMs);
                        if (v == V::Answered) { reported = k - d.onsetFramesBack(); confirmed = k; }
                        else if (v == V::PictureMoving) reported = -2;
                    }
                }
                ++cases;
                if (reported > 0) {
                    ++found;
                    const int off = std::abs(reported - onset);
                    worst = std::max(worst, off);
                    if (off > 1) where += " shape=" + std::to_string(shape) + " frames=" + std::to_string(frames) + " at=" + std::to_string(at);
                    if (confirmed - reported >= latency::Detector::kOnsetFrames) ++timedTooSlow;
                } else if (reported == -2) where += " moving: shape=" + std::to_string(shape) + " at=" + std::to_string(at);
            }
        check(worst <= 1 && timedTooSlow == 0 && where.empty(), "anywhere: the reported time is the threshold onset (+-1 frame) for every shape; found "
              + std::to_string(found) + "/" + std::to_string(cases) + ", worst " + std::to_string(worst) + " frame(s)" + where);
        // The reviewers' worst case: a 333 ms (20-frame) ease-in fade was +7 frames late against the threshold onset.
        latency::Detector d; d.setMode(latency::Detector::Mode::Anywhere); d.setMinimumAnswerMs(kFrameMs);
        latency::Detector threshold; threshold.setMode(latency::Detector::Mode::Anywhere);
        for (int i = 0; i < 20; ++i) { const auto l = transition(i - 19, 10, 20, 4, false); d.before(l); threshold.before(l); }
        int reported = -1, onset = -1;
        for (int k = 1; k <= 72 && reported < 0; ++k) {
            const auto l = transition(k, 10, 20, 4, false);
            if (onset < 0 && threshold.answered(l)) onset = k;
            if (d.judge(l, double(k) * kFrameMs) == V::Answered) reported = k - d.onsetFramesBack();
        }
        check(reported > 0 && std::abs(reported - onset) <= 1, "anywhere: a 333 ms ease-in fade is timed at its threshold onset (frame "
              + std::to_string(reported) + ", onset " + std::to_string(onset) + ")");
    }
    // Review delta1 blocker 1: sham presses. A picture that changes by itself after a still moment ends "not valid";
    // a still menu passes with no sham answered.
    {
        // A still menu: the real press opens it 5 frames later; a sham changes nothing.
        bool menuValid = true; int menuShamAnswers = 0, menuAnswered = 0, menuReal = 0;
        for (const int real : {8, 20, 56}) for (uint64_t seed = 1; seed <= 4; ++seed) {
            const auto r = shamProtocol([](long, int k, bool press) { Scene s; s.menu = press && k >= 5; return s; }, real, seed);
            menuValid = menuValid && r.summary.valid && r.summary.shams == latencystats::shamCount(real);
            menuShamAnswers += r.summary.shamAnswered; menuAnswered += r.realAnswered; menuReal += real;
        }
        check(menuValid && menuShamAnswers == 0 && menuAnswered == menuReal, "shams: a still menu is valid, 0 sham answers, every press found ("
              + std::to_string(menuAnswered) + "/" + std::to_string(menuReal) + ")");
        // The reviewer's det2: a 6 x 12-cell object that starts walking at the press (sham or real), 1 and 3 cells per
        // frame, over a still picture (the reviewer: 30/30 and 25/30 false answers). Not valid at every speed and seed.
        // At 0.25 cells per frame the detector sees nothing at all (det2: 0/30 false answers): no sham answers either.
        bool slowQuiet = true;
        for (uint64_t seed = 1; seed <= 4; ++seed)
            slowQuiet = slowQuiet && shamProtocol([](long, int k, bool) { Scene s; s.objX = 50 + (k > 0 ? k / 4 : 0); return s; }, 8, seed).summary.shamAnswered == 0;
        check(slowQuiet, "shams: an object starting to walk at 0.25 cells/frame is never seen (no sham answer)");
        bool walkInvalid = true; std::string walkWhere;
        for (const double speed : {1.0, 3.0}) for (uint64_t seed = 1; seed <= 4; ++seed) {
            const auto r = shamProtocol([&](long, int k, bool) { Scene s; s.objX = 50 + (k > 0 ? int(double(k) * speed) : 0); return s; }, 8, seed);
            if (r.summary.valid) { walkInvalid = false; walkWhere += " speed=" + std::to_string(speed) + " seed=" + std::to_string(seed); }
            walkWhere += " [" + std::to_string(r.summary.shamAnswered) + "/" + std::to_string(r.summary.shams) + "]";
        }
        check(walkInvalid, "shams: an object starting to walk at the press ends not valid; sham answers" + walkWhere);
        // The reviewer's det4: a tile of 8..16 x 16 cells pulsing on its own clock (every 250 ms, 500 ms, 1 s) with
        // the menu answering real presses. Not valid at every size, period and seed.
        bool pulseInvalid = true; std::string pulseWhere;
        for (const int width : {8, 12, 16}) for (const int half : {15, 30, 60}) for (uint64_t seed = 1; seed <= 3; ++seed) {
            const auto r = shamProtocol([&](long frameNo, int k, bool press) {
                Scene s; s.menu = press && k >= 5; s.tileW = width; s.tile = ((frameNo + long(seed) * 7) / half) % 2 == 0; return s; }, 8, seed);
            if (r.summary.valid) { pulseInvalid = false; pulseWhere += " w=" + std::to_string(width) + " half=" + std::to_string(half) + " seed=" + std::to_string(seed); }
        }
        check(pulseInvalid, "shams: a pulsing tile (128-256 cells, 250 ms-1 s) ends not valid" + pulseWhere);
    }
    const auto s = latency::summarize({80, 60, 100, 70}, 5);
    check(s.trials == 5 && s.answered == 4 && s.minMs == 60 && s.maxMs == 100 && s.medianMs == 75 && s.meanMs == 77.5, "summary");
    check(latency::summarize({}, 3).answered == 0, "no answers");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
