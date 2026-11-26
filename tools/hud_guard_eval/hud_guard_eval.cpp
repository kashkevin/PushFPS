// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// OFFLINE (CPU, SIMULATED generator): HUD Guard (pipeline/PushHudGuard.h) on synthetic scenes (tests/remoteplay/HudGuardScene.h)
// at 2X, 3X and 4X: the overlay's copies outside it (ghost), the overlay itself and the background around it, for the raw
// generator, compose only (O1) and pre-clean + compose (O2 + O1). Not VFG, not PushWarp, not a graphics card.
//   g++ -std=c++20 -O2 -Iinclude -Itests/remoteplay tools/hud_guard_eval/hud_guard_eval.cpp -o hud_guard_eval
#include "HudGuardScene.h"
#include <cstdio>

using namespace hud_guard_scene;

int main() {
    struct Case { const char* name; Overlay o; Camera c; };
    const Case cases[] = {
        {"crosshair 1px opaque, pan 24", {Overlay::Kind::Crosshair, 1, 14, 1.0f}, {false, 24}},
        {"crosshair 2px opaque, pan 24", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 24}},
        {"crosshair 3px opaque, pan 24", {Overlay::Kind::Crosshair, 3, 14, 1.0f}, {false, 24}},
        {"crosshair 2px opaque, pan 8", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 8}},
        {"crosshair 2px opaque, pan 48", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 48}},
        {"crosshair 2px opaque, diagonal 24", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {false, 24, 6, 0.7071f, 0.7071f}},
        {"crosshair 2px opaque, circle 24", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {true, 24}},
        {"crosshair 2px alpha 0.9, circle 24", {Overlay::Kind::Crosshair, 2, 14, 0.9f}, {true, 24}},
        {"crosshair 2px alpha 0.6, circle 24", {Overlay::Kind::Crosshair, 2, 14, 0.6f}, {true, 24}},
        {"text strokes 2px, pan 24", {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24}},
        {"minimap ring 3px + moving map, pan 24", {Overlay::Kind::Minimap, 2, 14, 1.0f}, {false, 24}},
    };
    std::printf("%-40s %2s | %-28s | %-28s | %-28s\n", "scene", "x", "raw: ghost/px overlay ring", "compose: ghost/px over ring", "clean+compose: ghost/px over ring");
    for (const auto& c : cases) {
        const Scene s(256, 160, c.o, c.c);
        for (unsigned m : {2u, 3u, 4u}) {
            const auto r = run(s, m, Mode::Raw), o1 = run(s, m, Mode::ComposeOnly), o2 = run(s, m, Mode::CleanAndCompose);
            auto cell = [](const Result& x) {
                static char b[4][64]; static int i = 0; i = (i + 1) & 3;
                std::snprintf(b[i], 64, "%5.2f/%5llu %5.2f(%3.0f%%) %5.2f", x.mean.ghostMean, (unsigned long long)x.mean.ghostPixels, x.mean.overlayMean,
                              x.mean.overlayPixels ? 100.0 * double(x.mean.overlayExact) / double(x.mean.overlayPixels) : 0.0, x.mean.ringMean);
                return b[i];
            };
            std::printf("%-40s %uX | %s | %s | %s\n", c.name, m, cell(r), cell(o1), cell(o2));
        }
    }
    return 0;
}
