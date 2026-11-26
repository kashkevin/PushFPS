// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// OFFLINE (CPU, SIMULATED generator): crosshair ghosting AFTER the 1.5x shrink to his 2560x1440 screen (owner, 25 Nov
// 20:54 ET: "separate problems or do they amplify each other?"). Scenes of HudGuardScene.h at 384 x 216 "4K pixels" (a
// tenth of 3840 x 2160 each way: the guard's rules are per 4K pixel) shrunk to 256 x 144 (a tenth of 2560 x 1440) with
// Package 16's single bilinear tap or the stream "s12-pace" area filter (frames.h). Measured at the OUTPUT resolution:
//   ghost px  output pixels farther than 2 px from the crosshair's footprint that differ > 8 codes from the generator's
//             own overlay-free output shrunk the same way (the copies, as he sees them), per generated frame
//   sparkle   coefficient of variation of that count over the generated frames (copies kept or dropped by phase)
//   real err  mean |shown - ideal| on the crosshair's output pixels in REAL frames; ideal = the truth shrunk with the area
//             filter (thinning / dimming of the real crosshair by the filter itself)
//   gen err   the same in generated frames
//   flicker   mean over the crosshair's output pixels of the standard deviation over every displayed frame (real and
//             generated, in order): how much the crosshair changes from frame to frame on his screen
// The minimap scene is left out: its own scrolling map would count as copies against the overlay-free output.
// Not VFG, not a graphics card, SDR code values only (HDR10 in linear light is the quality bench's job).
//   g++ -std=c++20 -O2 -Iinclude -Itests/remoteplay tools/hud_guard_eval/downscale_eval.cpp -o downscale_eval
#include "frames.h"
#include <cstdio>
#include <map>

using namespace hud_guard_scene;
using namespace hud_guard_frames;

namespace {
constexpr unsigned W = 384, H = 216, OW = 256, OH = 144;

struct Row { double ghost = 0, sparkle = 0, realErr = 0, genErr = 0, flicker = 0; };

Row study(const Scene& s, unsigned multiplier, Mode mode, Filter filter) {
    // The crosshair's output footprint: output pixels the area filter gives any coverage.
    Plane cov{W, H, std::vector<double>(std::size_t(W) * H, 0.0)};
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) { bool inside = false; cov.v[std::size_t(y) * W + x] = s.coverage(int(x), int(y), inside); }
    const Plane covOut = downscale(cov, OW, OH, Filter::Area);
    std::vector<int> dist(std::size_t(OW) * OH, 1000);
    for (unsigned y = 0; y < OH; ++y)
        for (unsigned x = 0; x < OW; ++x)
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const int qx = int(x) + dx, qy = int(y) + dy;
                    if (qx < 0 || qy < 0 || qx >= int(OW) || qy >= int(OH) || covOut.v[std::size_t(qy) * OW + std::size_t(qx)] <= 0.01) continue;
                    dist[std::size_t(y) * OW + x] = std::min(dist[std::size_t(y) * OW + x], std::max(std::abs(dx), std::abs(dy)));
                }
    auto shrink = [&](const std::vector<std::uint32_t>& words, Filter f) { return downscale(Plane{W, H, codes(s, words)}, OW, OH, f); };
    std::map<std::pair<int, unsigned>, Plane> gen, plain, truth;
    std::map<int, Plane> real, realTruth;
    run(s, multiplier, mode, 16, 8, Generator::Simulated, [&](int k, unsigned sub, float t, const char* kind, const std::vector<std::uint32_t>& words) {
        const std::string what = kind;
        if (what == "generated") gen[{k, sub}] = shrink(words, filter);
        else if (what == "plain") plain[{k, sub}] = shrink(words, filter);
        else if (what == "truth") truth[{k, sub}] = shrink(words, Filter::Area);
        else if (what == "real") { real[k] = shrink(words, filter); realTruth[k] = shrink(s.frame(k, true), Filter::Area); }
        (void)t;
    });
    Row r;
    std::vector<double> counts;
    double genErr = 0, realErr = 0;
    std::size_t genN = 0, realN = 0;
    for (const auto& [key, g] : gen) {
        const auto& p = plain[key];
        const auto& tr = truth[key];
        double c = 0;
        for (std::size_t i = 0; i < g.v.size(); ++i) {
            if (dist[i] > 2 && std::fabs(g.v[i] - p.v[i]) * 255.0 > 8.0) ++c;
            if (dist[i] == 0) { genErr += std::fabs(g.v[i] - tr.v[i]) * 255.0; ++genN; }
        }
        counts.push_back(c);
    }
    for (const auto& [k, f] : real)
        for (std::size_t i = 0; i < f.v.size(); ++i)
            if (dist[i] == 0) { realErr += std::fabs(f.v[i] - realTruth[k].v[i]) * 255.0; ++realN; }
    double mean = 0, var = 0;
    for (double c : counts) mean += c;
    mean /= double(counts.size());
    for (double c : counts) var += (c - mean) * (c - mean);
    var /= double(counts.size());
    r.ghost = mean;
    r.sparkle = mean > 0 ? std::sqrt(var) / mean : 0;
    r.genErr = genN ? genErr / double(genN) : 0;
    r.realErr = realN ? realErr / double(realN) : 0;
    // Flicker: per crosshair output pixel, standard deviation over the displayed sequence (per pair: generated, then real).
    std::vector<const Plane*> shown;
    for (const auto& [k, f] : real) {
        for (unsigned sub = 1; sub < multiplier; ++sub) shown.push_back(&gen[{k, sub}]);
        shown.push_back(&f);
    }
    double flick = 0;
    std::size_t flickN = 0;
    for (std::size_t i = 0; i < std::size_t(OW) * OH; ++i) {
        if (dist[i] != 0) continue;
        double m = 0, v2 = 0;
        for (const Plane* p : shown) m += p->v[i];
        m /= double(shown.size());
        for (const Plane* p : shown) v2 += (p->v[i] - m) * (p->v[i] - m);
        flick += std::sqrt(v2 / double(shown.size())) * 255.0;
        ++flickN;
    }
    r.flicker = flickN ? flick / double(flickN) : 0;
    return r;
}
}  // namespace

int main() {
    struct Case { const char* name; Overlay o; Camera c; };
    const Case cases[] = {
        {"crosshair 1px, circle 24", {Overlay::Kind::Crosshair, 1, 14, 1.0f}, {true, 24}},
        {"crosshair 2px, circle 24", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {true, 24}},
        {"crosshair 3px, circle 24", {Overlay::Kind::Crosshair, 3, 14, 1.0f}, {true, 24}},
        {"crosshair 2px, pan 8", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 8}},
        {"text strokes 2px, pan 24", {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24}},
    };
    // The real crosshair's fixed phase: a 1-px and a 2-px vertical line (contrast 1 on black) at x = 192 + phase in 4K pixels,
    // shrunk 1.5x: the brightest output column and the line's total energy (sum over output columns x 1.5, 1 = conserved).
    std::printf("A still line at a fixed 4K phase after the 1.5x shrink (peak output value / energy kept):\n");
    for (int width : {1, 2})
        for (int phase = 0; phase < 3; ++phase) {
            Plane line{W, 8, std::vector<double>(std::size_t(W) * 8, 0.0)};
            for (unsigned y = 0; y < 8; ++y)
                for (int k = 0; k < width; ++k) line.v[std::size_t(y) * W + std::size_t(192 + phase + k)] = 1.0;
            std::printf("  %d px, phase %d:", width, phase);
            for (Filter f : {Filter::SingleTap, Filter::Area}) {
                const Plane o = downscale(line, OW, 5, f);   // 8 rows shrunk to 5 (1.6x: the area filter n stays 3)
                double peak = 0, sum = 0;
                for (unsigned x = 0; x < OW; ++x) { peak = std::max(peak, o.at(int(x), 2)); sum += o.at(int(x), 2); }
                std::printf("  %s peak %.2f energy %.2f", f == Filter::SingleTap ? "single-tap" : "area", peak, sum * 1.5 / width);
            }
            std::printf("\n");
        }
    std::printf("384x216 -> 256x144 (1.5x). ghost px per generated frame / sparkle (CV) / real err / gen err / flicker, codes\n");
    std::printf("%-30s %2s %-10s | %-38s | %-38s\n", "scene", "x", "filter", "raw generator", "HUD Guard (pre-clean + compose)");
    for (const auto& c : cases) {
        const Scene s(W, H, c.o, c.c);
        for (unsigned m : {2u, 3u})
            for (Filter f : {Filter::SingleTap, Filter::Area}) {
                const Row raw = study(s, m, Mode::Raw, f), guard = study(s, m, Mode::CleanAndCompose, f);
                auto cell = [](const Row& r) {
                    static char b[2][80]; static int i = 0; i ^= 1;
                    std::snprintf(b[i], 80, "%6.1f %4.2f %5.1f %5.1f %5.1f", r.ghost, r.sparkle, r.realErr, r.genErr, r.flicker);
                    return b[i];
                };
                std::printf("%-30s %uX %-10s | %-38s | %-38s\n", c.name, m, f == Filter::SingleTap ? "single-tap" : "area", cell(raw), cell(guard));
            }
    }
    return 0;
}
