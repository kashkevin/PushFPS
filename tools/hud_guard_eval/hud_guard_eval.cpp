// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// OFFLINE (CPU): HUD Guard (pipeline/PushHudGuard.h) on synthetic scenes (tests/remoteplay/HudGuardScene.h) at 2X, 3X
// and 4X, for the raw generator, compose only and pre-clean + compose. Two generators: SIMULATED (every pixel moves with
// the camera, the mechanism of the crosshair copies; not VFG, not PushWarp) and PERFECT (the truth: any wrong pixel then
// comes from the guard itself). Columns per mode:
//   ghost  pixels farther than 3 px from the overlay that differ > 8 codes from the generator's overlay-free output
//   ovl%   overlay pixels within half a code of the truth
//   wrong  pixels anywhere / farther than 3 px from the overlay > 8 codes from the truth (background damage)
//   d1/d2/d3  mean error (codes) at 1, 2, 3 px from the overlay; ring  mean error 4-12 px
// Per generated frame, 8 measured pairs (16 for the change scene). Build and run from the repository root:
//   g++ -std=c++20 -O2 -Iinclude -Itests/remoteplay tools/hud_guard_eval/hud_guard_eval.cpp -o hud_guard_eval && ./hud_guard_eval
// Frames for the quality bench (exp/s12-bench) and downscale studies:
//   ./hud_guard_eval --write <dir> [--size 384x216]
// writes, for the crosshair (2 px, circle 24, SDR and HDR10), text and minimap scenes at 2X and 3X, the raw generator's
// and HUD Guard's displayed frames in order (real B of each pair after its generated frames), the truth at each generated
// frame's time and the generator's overlay-free output, as 16-bit PGM (frames.h) with a manifest.json per run:
//   <dir>/<scene>/<raw|guard>-<m>x/<kind>-k<pair>-s<sub>.pgm
#include "frames.h"
#include <cstdio>
#include <filesystem>
#include <string>

using namespace hud_guard_scene;
using namespace hud_guard_frames;

namespace {
int writeFrames(const std::string& dir, unsigned w, unsigned h) {
    struct Case { const char* slug; Overlay o; Camera c; bool hdr; };
    const Case cases[] = {
        {"crosshair2-circle24", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {true, 24}, false},
        {"crosshair2-circle24-hdr10", {Overlay::Kind::Crosshair, 2, 14, 1.0f}, {true, 24}, true},
        {"crosshair1-circle24", {Overlay::Kind::Crosshair, 1, 14, 1.0f}, {true, 24}, false},
        {"text2-pan24", {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24}, false},
        {"minimap3-pan24", {Overlay::Kind::Minimap, 2, 14, 1.0f}, {false, 24}, false},
    };
    unsigned files = 0;
    for (const auto& c : cases) {
        const Scene s(w, h, c.o, c.c, {}, {}, c.hdr);
        for (unsigned m : {2u, 3u})
            for (Mode mode : {Mode::Raw, Mode::CleanAndCompose}) {
                const std::string folder = dir + "/" + c.slug + "/" + (mode == Mode::Raw ? "raw-" : "guard-") + std::to_string(m) + "x";
                std::filesystem::create_directories(folder);
                std::string manifest = "{\n \"scene\": \"" + std::string(c.slug) + "\", \"width\": " + std::to_string(w) + ", \"height\": " +
                                       std::to_string(h) + ", \"multiplier\": " + std::to_string(m) + ", \"mode\": \"" + (mode == Mode::Raw ? "raw" : "guard") +
                                       "\", \"values\": \"" + (c.hdr ? "PQ code" : "sRGB code") + " x 65535\", \"generator\": \"SIMULATED block motion (not VFG)\",\n \"frames\": [\n";
                bool first = true;
                run(s, m, mode, 16, 8, Generator::Simulated, [&](int k, unsigned sub, float t, const char* kind, const std::vector<std::uint32_t>& words) {
                    const std::string name = std::string(kind) + "-k" + std::to_string(k) + "-s" + std::to_string(sub) + ".pgm";
                    if (writePgm16(folder + "/" + name, w, h, codes(s, words))) ++files;
                    char line[256];
                    std::snprintf(line, sizeof line, "%s  {\"file\": \"%s\", \"pair\": %d, \"sub\": %u, \"t\": %.6f, \"kind\": \"%s\"}", first ? "" : ",\n",
                                  name.c_str(), k, sub, double(t), kind);
                    manifest += line;
                    first = false;
                });
                manifest += "\n ]\n}\n";
                std::FILE* f = std::fopen((folder + "/manifest.json").c_str(), "wb");
                if (f) { std::fwrite(manifest.data(), 1, manifest.size(), f); std::fclose(f); }
            }
    }
    std::printf("wrote %u frames under %s\n", files, dir.c_str());
    return files ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    std::string writeDir;
    unsigned ww = 384, wh = 216;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--write") writeDir = argv[i + 1];
        else if (a == "--size") { unsigned x = 0, y = 0; if (std::sscanf(argv[i + 1], "%ux%u", &x, &y) == 2 && x >= 64 && y >= 64) { ww = x; wh = y; } }
    }
    if (!writeDir.empty()) return writeFrames(writeDir, ww, wh);
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    struct Case { const char* name; Overlay o; Camera c; World w; Mover m; bool hdr; Generator g; int frames, from; bool all; };
    const Overlay cross2{Overlay::Kind::Crosshair, 2, 14, 1.0f};
    const Overlay none{Overlay::Kind::None};
    const Case cases[] = {
        {"crosshair 1px opaque, pan 24", {Overlay::Kind::Crosshair, 1, 14, 1.0f}, {false, 24}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 2px opaque, pan 24", cross2, {false, 24}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 3px opaque, pan 24", {Overlay::Kind::Crosshair, 3, 14, 1.0f}, {false, 24}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 2px, pan 8", cross2, {false, 8}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 2px, pan 48", cross2, {false, 48}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 2px, diagonal 24", cross2, {false, 24, 6, 0.7071f, 0.7071f}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair 2px, circle 24", cross2, {true, 24}, {}, {}, false, Generator::Simulated, 16, 8, true},
        {"crosshair 2px, circle 24, HDR10", cross2, {true, 24}, {}, {}, true, Generator::Simulated, 16, 8, true},
        {"crosshair 2px, circle 24, aims (changes) at 10", {Overlay::Kind::Crosshair, 2, 14, 1.0f, 0.96f, 10, 0.55f}, {true, 24}, {}, {}, false, Generator::Simulated, 18, 10, true},
        {"crosshair alpha 0.9, circle 24", {Overlay::Kind::Crosshair, 2, 14, 0.9f}, {true, 24}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"crosshair alpha 0.6, circle 24", {Overlay::Kind::Crosshair, 2, 14, 0.6f}, {true, 24}, {}, {}, false, Generator::Simulated, 16, 8, false},
        {"text strokes 2px, pan 24", {Overlay::Kind::Text, 2, 14, 1.0f}, {false, 24}, {}, {}, false, Generator::Simulated, 16, 8, true},
        {"minimap ring 3px + moving map, pan 24", {Overlay::Kind::Minimap, 2, 14, 1.0f}, {false, 24}, {}, {}, false, Generator::Simulated, 16, 8, true},
        // Damage scenes (review MUST 1), PERFECT generator: raw is the truth.
        {"DAMAGE thin bar 12 px/f, still camera, textured", none, {false, 0}, {}, {true, 12}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE thin bar 20 px/f, still camera, textured", none, {false, 0}, {}, {true, 20}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE thin bar 12 px/f, still camera, crosshair", cross2, {false, 0}, {}, {true, 12}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE thin bar 12 px/f, still, low contrast", none, {false, 0}, {World::Kind::LowContrast, 0.05f}, {true, 12}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE sky + trunks, pan 24", none, {false, 24}, {World::Kind::SkyTrunks}, {}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE sky + trunks, pan 24, crosshair", cross2, {false, 24}, {World::Kind::SkyTrunks}, {}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE low-contrast 0.05 pan 24", none, {false, 24}, {World::Kind::LowContrast, 0.05f}, {}, false, Generator::Perfect, 16, 8, true},
        {"DAMAGE low-contrast 0.05 pan 24, HDR10", none, {false, 24}, {World::Kind::LowContrast, 0.05f}, {}, true, Generator::Perfect, 16, 8, true},
        {"DAMAGE low-contrast 0.05 pan 24 (simulated gen)", none, {false, 24}, {World::Kind::LowContrast, 0.05f}, {}, false, Generator::Simulated, 16, 8, true},
        {"DAMAGE sky + trunks, pan 24 (simulated gen)", none, {false, 24}, {World::Kind::SkyTrunks}, {}, false, Generator::Simulated, 16, 8, true},
    };
    std::printf("%-48s %2s | %-48s | %-48s | %-48s\n", "scene", "x", "raw: ghost ovl% wrong/far d1/d2/d3 ring", "compose only", "pre-clean + compose");
    for (const auto& c : cases) {
        if (quick && !c.all) continue;
        const Scene s(256, 160, c.o, c.c, c.w, c.m, c.hdr);
        for (unsigned m : {2u, 3u, 4u}) {
            if (quick && m == 4) continue;
            const auto r = run(s, m, Mode::Raw, c.frames, c.from, c.g), o1 = run(s, m, Mode::ComposeOnly, c.frames, c.from, c.g),
                       o2 = run(s, m, Mode::CleanAndCompose, c.frames, c.from, c.g);
            auto cell = [](const Result& x) {
                static char b[4][112]; static int i = 0; i = (i + 1) & 3;
                std::snprintf(b[i], 112, "%4llu %3.0f%% %5llu/%5llu %4.1f/%4.1f/%4.1f %4.1f", (unsigned long long)x.mean.ghostPixels,
                              x.mean.overlayPixels ? 100.0 * double(x.mean.overlayExact) / double(x.mean.overlayPixels) : 100.0,
                              (unsigned long long)x.mean.wrongPixels, (unsigned long long)x.mean.wrongFar, x.mean.near1, x.mean.near2, x.mean.near3, x.mean.ringMean);
                return b[i];
            };
            std::printf("%-48s %uX | %s | %s | %s\n", c.name, m, cell(r), cell(o1), cell(o2));
        }
    }
    return 0;
}
