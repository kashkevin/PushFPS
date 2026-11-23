// SPDX-License-Identifier: GPL-3.0-or-later
// The `[slipstream-nr]` line (veyra/engine/PushNrStats.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushNrStats.h"
namespace {
namespace pns = veyra::engine;
}  // namespace

TEST(nr_stats_series_nearest_rank_percentiles){
    pns::PushNrSeries s;
    CHECK(s.percentile(50.0) == -1.0 && s.samples() == 0);
    for (int i = 100; i >= 1; --i) s.add(double(i));   // 1..100 in any order
    CHECK(s.samples() == 100);
    CHECK(s.percentile(50.0) == 50.0 && s.percentile(95.0) == 95.0 && s.percentile(100.0) == 100.0 && s.percentile(0.0) == 1.0);
    // Values that are not times are left out: negative (= not measured), NaN, absurd.
    s.add(-1.0); s.add(std::nan("")); s.add(2.0e4);
    CHECK(s.samples() == 100);
    s.clear();
    CHECK(s.samples() == 0 && s.percentile(95.0) == -1.0);
    s.add(3.25);
    CHECK(s.percentile(50.0) == 3.25 && s.percentile(95.0) == 3.25);
    // More samples than the window stores: counted, the stored ones still answer, nothing is written past the end.
    s.clear();
    for (std::size_t i = 0; i < pns::PushNrSeries::kCapacity + 500; ++i) s.add(1.0);
    CHECK(s.samples() == pns::PushNrSeries::kCapacity + 500 && s.percentile(95.0) == 1.0);
}
TEST(nr_stats_window_keeps_each_stage_apart){
    pns::PushNrWindow w;
    // 10 real frames: the neural layer ran on every second one (a reused frame is recorded as 0 by the graph),
    // the upscaler on all, frame generation was off (-1: left out).
    for (int i = 0; i < 10; ++i) {
        pns::PushNrFrame f;
        f.nrMs = i % 2 ? 0.0 : 6.0; f.srMs = 2.0 + 0.1 * i; f.graphMs = 9.0 + i;
        w.add(f);
    }
    CHECK(w.frames() == 10);
    CHECK(w.nr().samples() == 10 && w.nr().percentile(50.0) == 0.0 && w.nr().percentile(95.0) == 6.0);
    CHECK(w.sr().percentile(50.0) > 2.39 && w.sr().percentile(50.0) < 2.41);
    CHECK(w.fg().samples() == 0 && w.fg().percentile(95.0) == -1.0);
    CHECK(w.graph().percentile(95.0) == 18.0);
    w.clear();
    CHECK(w.frames() == 0 && w.sr().samples() == 0);
}
TEST(nr_stats_window_is_due_once_per_five_seconds){
    pns::PushNrWindow w;
    const std::int64_t second = 10000000;
    CHECK(!w.due(100 * second));           // the first call starts the window
    CHECK(!w.due(104 * second + 9999999));
    CHECK(w.due(105 * second));
    w.add(pns::PushNrFrame{1.0, 1.0, 1.0, 3.0});
    w.restart(105 * second);
    CHECK(w.frames() == 0 && !w.due(109 * second) && w.due(110 * second));
}
TEST(nr_stats_line_has_the_contract_keys_in_order){
    pns::PushNrWindow w;
    for (int i = 1; i <= 20; ++i) w.add(pns::PushNrFrame{double(i), 0.5 * i, 0.25 * i, 2.0 * i});
    pns::PushNrStatsInfo info;
    info.layers = 1; info.internalW = 1920; info.internalH = 1080; info.outputW = 2560; info.outputH = 1440;
    info.copyCount = 4; info.vramMiB = 3100; info.budgetMiB = 11000; info.runtime = "original";
    info.versionMs = (310u << 16) | 8u; info.versionLs = 0;
    const std::string line = pns::pushNrStatsLine(w, info);
    CHECK(line == "frames=20 layers=1 internal=1920x1080 output=2560x1440 nrMsP50=10.000 nrMsP95=19.000 srMsP50=5.000 srMsP95=9.500 "
                  "fgMsP50=2.500 fgMsP95=4.750 graphMsP50=20.000 graphMsP95=38.000 copyCount=4 vramMiB=3100 budgetMiB=11000 "
                  "runtime=original version=310.8.0.0");
    // What the app cannot know is -1: no layer, nothing measured, no version, no memory reading.
    pns::PushNrWindow empty;
    const std::string none = pns::pushNrStatsLine(empty, pns::PushNrStatsInfo{});
    CHECK(none == "frames=0 layers=0 internal=0x0 output=0x0 nrMsP50=-1.000 nrMsP95=-1.000 srMsP50=-1.000 srMsP95=-1.000 "
                  "fgMsP50=-1.000 fgMsP95=-1.000 graphMsP50=-1.000 graphMsP95=-1.000 copyCount=-1 vramMiB=-1 budgetMiB=-1 "
                  "runtime=none version=-1");
    // Every contract key, once, in the contract's order; no path, no name of a card.
    const char* keys[] = {"frames=", " layers=", " internal=", " output=", " nrMsP50=", " nrMsP95=", " srMsP50=", " srMsP95=", " fgMsP50=",
                          " fgMsP95=", " graphMsP50=", " graphMsP95=", " copyCount=", " vramMiB=", " budgetMiB=", " runtime=", " version="};
    std::size_t at = 0;
    for (const char* key : keys) {
        const std::size_t found = line.find(key, at);
        CHECK(found != std::string::npos);
        CHECK(line.find(key, found + 1) == std::string::npos);
        at = found;
    }
    CHECK(line.find('\\') == std::string::npos && line.find('/') == std::string::npos);
    // A runtime name is cut, never read past its end.
    info.runtime = "abcdefghijklmnopqrstuvwxyz0123456789";
    CHECK(pns::pushNrStatsLine(w, info).find("runtime=abcdefghijklmnopqrstuvwx version=") != std::string::npos);
}
TEST(nr_stats_copy_count_follows_the_configuration){
    pns::PushNrCopyConfig c;
    CHECK(pns::pushNrCopyCount(c) == 0);                       // arm A: nothing between decode and present
    c.sr = pns::PushNrSrKind::Temporal;
    CHECK(pns::pushNrCopyCount(c) == 0);                       // arm B, SDR: DLSS SR reads and writes the graph's own textures
    c.hdr = true;
    CHECK(pns::pushNrCopyCount(c) == 1);                       // HDR: the restore pass after it
    c = {}; c.sr = pns::PushNrSrKind::VideoNgx;
    CHECK(pns::pushNrCopyCount(c) == 2);                       // RTX Video SR: 8-bit encode + decode
    c.hdr = true; c.supersampleShrink = true;
    CHECK(pns::pushNrCopyCount(c) == 2);                       // the HDR restore and the shrink take the decode's place
    c = {}; c.sr = pns::PushNrSrKind::VideoVfx;
    CHECK(pns::pushNrCopyCount(c) == 4);
    c = {}; c.sr = pns::PushNrSrKind::Nis;
    CHECK(pns::pushNrCopyCount(c) == 2);
    c = {}; c.sr = pns::PushNrSrKind::Resize;
    CHECK(pns::pushNrCopyCount(c) == 1);
    // Arm D: one neural layer at its input's size: encode, decode, composite.
    c = {}; c.nrLayers = 1;
    CHECK(pns::pushNrCopyCount(c) == 3);
    // Arm E on a 1440p work size with a 1080-line layer: + its downsample; DLSS SR adds none.
    c.nrLayersResized = 1; c.sr = pns::PushNrSrKind::Temporal;
    CHECK(pns::pushNrCopyCount(c) == 4);
    c.nrLayers = 2; c.nrLayersResized = 5;                     // more resized layers than layers: capped
    CHECK(pns::pushNrCopyCount(c) == 8);
    c.referenceCopies = 9;                                     // at most the two references the graph keeps
    CHECK(pns::pushNrCopyCount(c) == 10);
    // The stored upscaler choice to its kind.
    CHECK(pns::pushNrSrKind(false, 2, false) == pns::PushNrSrKind::None && pns::pushNrSrKind(false, 2, true) == pns::PushNrSrKind::Resize);
    CHECK(pns::pushNrSrKind(true, 0, true) == pns::PushNrSrKind::Temporal && pns::pushNrSrKind(true, 5, true) == pns::PushNrSrKind::Temporal);
    CHECK(pns::pushNrSrKind(true, 6, true) == pns::PushNrSrKind::Nis);
    for (std::uint32_t q = 1; q <= 4; ++q) CHECK(pns::pushNrSrKind(true, q, true) == pns::PushNrSrKind::VideoNgx);
    for (std::uint32_t q : {16u, 17u, 18u, 19u, 21u, 23u}) CHECK(pns::pushNrSrKind(true, q, true) == pns::PushNrSrKind::VideoVfx);
    CHECK(pns::pushNrSrKind(true, 20, true) == pns::PushNrSrKind::None && pns::pushNrSrKind(true, 7, true) == pns::PushNrSrKind::None);
}
TEST(nr_stats_runtime_names_and_configuration_change){
    CHECK(pns::pushNrStatsRuntimeName(3, true) == "original" && pns::pushNrStatsRuntimeName(2, true) == "sf");
    CHECK(pns::pushNrStatsRuntimeName(0, true) == "sf" && pns::pushNrStatsRuntimeName(1, true) == "sf" && pns::pushNrStatsRuntimeName(4, true) == "amd");
    CHECK(pns::pushNrStatsRuntimeName(3, false) == "none");
    pns::PushNrStatsInfo a, b;
    a.layers = b.layers = 1; a.internalH = b.internalH = 1080;
    a.vramMiB = 100; b.vramMiB = 900;                          // memory moves all the time: not a new configuration
    CHECK(a.sameConfiguration(b));
    b.internalH = 720;
    CHECK(!a.sameConfiguration(b));
}
