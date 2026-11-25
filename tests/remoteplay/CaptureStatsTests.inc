// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "capture": include/veyra/engine/PushCaptureStats.h (the [slipstream-capture] window and card line), duplicate
// removal (ContentDedup.h) on capture signals the earlier tests did not cover (120 Hz, engine read gaps), and the
// interpolation delay of docs/drafts/CAPTURE_FG.md section 4 as LivePairLatency schedules it. Every rate, time and
// size is made up: a model of the code, not a measurement of a card, a PS5 or a GPU.
#include "veyra/engine/PushCaptureStats.h"
#include "veyra/engine/ContentDedup.h"
#include "veyra/engine/LivePairLatency.h"
#include "veyra/engine/PushPipelineHook.h"
#include "veyra/engine/PushNrRuntime.h"
#include "veyra/engine/SlipstreamSettings.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace capture_tests {
inline bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
constexpr std::uint64_t kSpace = 1ull << 56;   // the capture callback's numbering (PictureReference{1})
// The fields PushCaptureFormatSummary reads from source::CaptureFormat.
struct Mode { std::uint32_t width = 0, height = 0; double fps = 0; std::wstring pixel; bool interlaced = false, supported = true; int tier = 0; bool automatic = false; };
inline veyra::engine::PushCaptureSource source(std::uint64_t received, std::uint64_t dropped = 0) {
    veyra::engine::PushCaptureSource s;
    s.width = 2560; s.height = 1440; s.nominalFps = 59.94; s.format = "nv12"; s.dedup = true; s.received = received; s.mailboxDropped = dropped;
    return s;
}
// A capture signal: sample i shows picture pictureOf(i); the engine reads the samples in `reads`.
struct DedupRun { int kept = 0, droppedNew = 0, keptRepeats = 0, regular = 0, spans = 0; bool monotonic = true, neverLate = true; double fps = 0; };
template <class PictureOf>
DedupRun dedupRun(double signalHz, PictureOf pictureOf, const std::vector<int>& reads, std::int64_t expected) {
    veyra::engine::ContentDedup dedup;
    DedupRun r;
    const std::int64_t dt = std::int64_t(std::llround(1e7 / signalHz));
    std::int64_t lastContent = -1, lastPicture = -1;
    bool first = true;
    for (const int i : reads) {
        const std::int64_t pts = std::int64_t(i) * dt + 1000;
        const std::int64_t picture = pictureOf(i);
        const std::uint64_t id = kSpace | std::uint64_t(picture + 1);
        // The callback numbers every sample, also the ones the engine never reads: the capture sequence is the sample index.
        const auto d = dedup.observe(&id, pts, dt, first, 0, std::uint64_t(i) + 1);
        first = false;
        if (d.keep) {
            ++r.kept;
            if (picture == lastPicture) ++r.keptRepeats;
            if (lastPicture >= 0 && picture > lastPicture + 1) r.droppedNew += int(picture - lastPicture - 1);
            lastPicture = picture;
            r.monotonic = r.monotonic && d.contentPts > lastContent;
            r.neverLate = r.neverLate && d.contentPts <= pts;
            lastContent = d.contentPts;
            if (i > 240 && d.interval) { ++r.spans; if (std::llabs(d.interval - expected) <= 5000) ++r.regular; }
        }
    }
    r.fps = dedup.contentFps();
    return r;
}
}

TEST(capture_window_counts_new_repeated_and_missed_pictures){
    using namespace veyra::engine;
    PushCaptureWindow w(true);
    PushCaptureWindowStats s;
    CHECK(!w.due(0, capture_tests::source(0), s));          // the first call starts the window
    // 30 fps game in a 60 Hz signal, 5 s: 300 samples, the engine reads all, 150 pictures.
    for (int i = 0; i < 300; ++i) w.sample(true, capture_tests::kSpace | std::uint64_t(i / 2 + 1));
    CHECK(!w.due(40000000, capture_tests::source(240), s));   // 4 s: not yet
    CHECK(w.due(50000000, capture_tests::source(300), s));
    CHECK(s.pictures && std::abs(s.transportFps - 60) < 1e-9 && std::abs(s.readFps - 60) < 1e-9);
    CHECK(std::abs(s.newFps - 30) < 1e-9 && std::abs(s.repeatFps - 30) < 1e-9 && s.missedNewFps == 0);
    // Next window: the engine is late and reads every third sample only (the mailbox replaced the others).
    std::uint64_t read = 0;
    for (int i = 300; i < 600; i += 3) { w.sample(true, capture_tests::kSpace | std::uint64_t(i / 2 + 1)); ++read; }
    CHECK(w.due(100000000, capture_tests::source(600, 200), s));
    // Pictures 151..299 up to the last read: new when a read lands on them, missed when none did (the chain of numbers
    // carries over the window boundary; picture 300 comes after the last read and is not known yet).
    CHECK(read == 100 && std::abs(s.readFps - 20) < 1e-9 && std::abs(s.mailboxDropFps - 40) < 1e-9);
    CHECK(std::abs((s.newFps + s.missedNewFps) * 5 - 149) < 1e-9 && std::abs(s.missedNewFps * 5 - 49) < 1e-9 && s.repeatFps == 0);
}

TEST(capture_window_unknown_pictures_are_minus_one_never_zero){
    using namespace veyra::engine;
    PushCaptureWindow w(true);
    PushCaptureWindowStats s;
    w.due(0, capture_tests::source(0), s);
    for (int i = 0; i < 100; ++i) w.sample(i % 2 == 0, capture_tests::kSpace | std::uint64_t(i + 1));   // half without numbers
    for (int i = 0; i < 50; ++i) w.pair(false, 0, false, 3, false);
    CHECK(w.due(50000000, capture_tests::source(100), s));
    CHECK(!s.pictures && s.newFps < 0 && s.repeatFps < 0 && s.missedNewFps < 0);
    CHECK(s.stillGenFps < 0 && s.motionGenFps < 0 && std::abs(s.genFps - 30) < 1e-9);
    const auto line = pushCaptureLine(s, capture_tests::source(100));
    CHECK(capture_tests::has(line, " newFps=-1 ") && capture_tests::has(line, " stillGenFps=-1 ") && capture_tests::has(line, " motionGenFps=-1 "));
    CHECK(capture_tests::has(line, " pictures=0 ") && capture_tests::has(line, " genFps=30.00 "));
    // A window without any read sample is not "every sample numbered".
    PushCaptureWindow empty(true);
    empty.due(0, capture_tests::source(0), s);
    CHECK(empty.due(50000000, capture_tests::source(0), s) && !s.pictures && s.newFps < 0);
}

TEST(capture_window_generated_between_identical_pictures_is_never_motion){
    using namespace veyra::engine;
    PushCaptureWindow w(true);
    PushCaptureWindowStats s;
    w.due(0, capture_tests::source(0), s);
    // Duplicate removal OFF, 30 fps game in 60 Hz, 2X: pairs (A,A), (A,B), (B,B), ... half of them copies.
    for (int i = 0; i < 300; ++i) {
        const std::uint64_t id = capture_tests::kSpace | std::uint64_t(i / 2 + 1);
        w.sample(true, id);
        w.pair(true, id, i == 0, i == 0 ? 0u : 1u, false);
    }
    CHECK(w.due(50000000, capture_tests::source(300), s));
    CHECK(std::abs(s.pairsFps - 60) < 1e-9 && std::abs(s.genFps - 299.0 / 5) < 1e-9);
    CHECK(std::abs(s.stillPairsFps - 30) < 1e-9 && std::abs(s.stillGenFps - 30) < 1e-9);
    CHECK(std::abs(s.stillGenFps + s.motionGenFps - s.genFps) < 1e-9 && std::abs(s.motionGenFps - 149.0 / 5) < 1e-9);
    // Duplicate removal ON, 4X: only new pictures reach the pairs; a reset pair never counts as a copy.
    for (int i = 0; i < 150; ++i) w.pair(true, capture_tests::kSpace | std::uint64_t(1000 + i), false, 3, i % 10 == 0);
    w.pair(true, capture_tests::kSpace | std::uint64_t(1149), true, 0, false);   // the same picture after a reset
    CHECK(w.due(100000000, capture_tests::source(600), s));
    CHECK(s.stillPairsFps == 0 && s.stillGenFps == 0 && std::abs(s.motionGenFps - 90) < 1e-9);
    CHECK(std::abs(s.fgHeldPct - 100.0 * 15 / 151) < 1e-9);
    // A held still picture (dedup's hold) generates copies: counted as such.
    // (the first of the 30 follows another picture: 29 copies, 3 frames of motion).
    for (int i = 0; i < 30; ++i) w.pair(true, capture_tests::kSpace | 7, false, 3, false);
    CHECK(w.due(150000000, capture_tests::source(900), s));
    CHECK(std::abs(s.stillPairsFps * 5 - 29) < 1e-9 && std::abs(s.stillGenFps * 5 - 87) < 1e-9 && std::abs(s.motionGenFps * 5 - 3) < 1e-9);
}

TEST(capture_window_real_frame_latency_percentiles){
    using namespace veyra::engine;
    PushCaptureWindow w(true);
    PushCaptureWindowStats s;
    w.due(0, capture_tests::source(0), s);
    for (int i = 1; i <= 100; ++i) w.realPresented(double(i));
    w.realPresented(-1); w.realPresented(std::nan("")); w.realPresented(20000);   // ignored
    CHECK(w.due(50000000, capture_tests::source(300), s));
    CHECK(s.latencySamples == 100 && s.realLatP50Ms == 50 && s.realLatP95Ms == 95 && s.realLatP99Ms == 99);
    const auto line = pushCaptureLine(s, capture_tests::source(300));
    CHECK(capture_tests::has(line, "realLatN=100 realLatP50Ms=50.00 realLatP95Ms=95.00 realLatP99Ms=99.00 "));
    CHECK(capture_tests::has(line, "revision=0 window=5.0s mode=2560x1440@59.94 format=nv12 compressed=0 hdr=0 dedup=1 "));
    // Without samples: -1, and the window after it starts empty.
    CHECK(w.due(100000000, capture_tests::source(600), s) && s.latencySamples == 0 && s.realLatP50Ms < 0);
}

TEST(capture_window_off_and_restart){
    using namespace veyra::engine;
    PushCaptureWindow off;
    PushCaptureWindowStats s;
    off.sample(true, 1); off.pair(true, 1, false, 3, false); off.realPresented(5);
    CHECK(!off.enabled() && !off.due(0, capture_tests::source(0), s) && !off.due(1000000000, capture_tests::source(1000), s));
    PushCaptureWindow w(true);
    w.due(0, capture_tests::source(500, 10), s);
    // The source reconnected: its counters start again from zero. No line with a negative rate; a new window.
    CHECK(!w.due(60000000, capture_tests::source(20, 0), s));
    CHECK(w.due(110000000, capture_tests::source(320, 5), s) && std::abs(s.transportFps - 60) < 1e-9 && std::abs(s.mailboxDropFps - 1) < 1e-9);
    // A jump far beyond a few pictures (a new numbering after a reset of the callback) is not counted as misses.
    PushCaptureWindow jump(true);
    jump.due(0, capture_tests::source(0), s);
    jump.sample(true, capture_tests::kSpace | 5);
    jump.sample(true, capture_tests::kSpace | 100000);
    jump.sample(true, (2ull << 56) | 3);          // another numbering
    CHECK(jump.due(50000000, capture_tests::source(3), s) && s.missedNewFps == 0 && std::abs(s.newFps * 5 - 3) < 1e-9);
    // A new settings revision (another arm's backend or multiplier) drops the part already counted: no mixed window.
    PushCaptureWindow rev(true);
    auto at = [](std::uint64_t received, std::uint64_t revision) { auto c = capture_tests::source(received); c.revision = revision; return c; };
    rev.due(0, at(0, 7), s);
    for (int i = 0; i < 90; ++i) { rev.pair(true, capture_tests::kSpace | std::uint64_t(i + 1), false, 5, false); rev.realPresented(40); }
    CHECK(!rev.due(30000000, at(180, 8), s));                  // 3 s into the window: revision 8, a new window
    for (int i = 0; i < 150; ++i) { rev.pair(true, capture_tests::kSpace | std::uint64_t(i + 200), false, 3, false); rev.realPresented(30); }
    CHECK(!rev.due(70000000, at(420, 8), s));                  // 4 s of the new window
    CHECK(rev.due(80000000, at(480, 8), s) && std::abs(s.seconds - 5) < 1e-9 && std::abs(s.pairsFps - 30) < 1e-9 &&
          std::abs(s.genFps - 90) < 1e-9 && std::abs(s.transportFps - 60) < 1e-9 && s.realLatP99Ms == 30);
    CHECK(capture_tests::has(pushCaptureLine(s, at(480, 8)), "revision=8 window=5.0s "));
}

TEST(capture_card_line_words_and_format_summary){
    using namespace veyra::engine;
    CHECK(pushCaptureWord(std::wstring_view(L"Game Capture 4K X")) == "Game_Capture_4K_X");
    CHECK(pushCaptureWord(std::wstring_view(L"  \\\\?\\usb#vid_0fd9&pid_009b  ")) == "usbvid_0fd9pid_009b");
    CHECK(pushCaptureWord(std::wstring_view(L"\u4E2D\u6587")) == "unknown" && pushCaptureWord(std::string_view("")) == "unknown");
    CHECK(pushCaptureWord(std::string_view("NV12")) == "NV12" && pushCaptureWord(std::wstring_view(std::wstring(80, L'a'))).size() == 40);
    std::vector<capture_tests::Mode> modes;
    modes.push_back({1920, 1080, 120.0, L"NV12", false, true, 0, false});
    modes.push_back({2560, 1440, 59.94, L"NV12", false, true, 0, false});
    modes.push_back({3840, 2160, 60.0, L"MJPEG", false, true, 3, false});
    modes.push_back({3840, 2160, 30.0, L"P010", false, true, 0, false});
    modes.push_back({2560, 1440, 120.0, L"MJPEG", false, true, 3, false});
    modes.push_back({1920, 1080, 60.0, L"YUY2", true, true, 0, false});     // interlaced: not counted
    modes.push_back({7680, 4320, 60.0, L"NV12", false, false, 0, false});   // the player does not support it
    modes.push_back({0, 0, 0.0, L"", false, true, 0, true});                // the device default entry
    const auto s = pushCaptureFormatSummary(modes);
    CHECK(s.modes == 5 && s.w60 == 2560 && s.h60 == 1440 && s.w120 == 1920 && s.h120 == 1080);
    CHECK(s.w60any == 3840 && s.h60any == 2160 && s.w120any == 2560 && s.h120any == 1440);
    CHECK(s.tenBit && s.maxFps == 120.0 && s.pixels == "NV12+MJPEG+P010");
    const auto line = pushCaptureCardLine("Game Capture 4K X", s);
    CHECK(capture_tests::has(line, "event=card card=Game_Capture_4K_X modes=5 max60=2560x1440 max120=1920x1080 max60Any=3840x2160 max120Any=2560x1440 tenBit=1 maxFps=120.00 pixels=NV12+MJPEG+P010 "));
    const auto none = pushCaptureFormatSummary(std::vector<capture_tests::Mode>{});
    CHECK(none.modes == 0 && none.pixels == "none" && capture_tests::has(pushCaptureCardLine("", none), "card=unknown modes=0 max60=none max120=none"));
    std::vector<capture_tests::Mode> compressedTen{{3840, 2160, 60.0, L"P010", false, true, 3, false}};
    CHECK(!pushCaptureFormatSummary(compressedTen).tenBit);   // only an uncompressed 10-bit format counts
}

TEST(capture_dedup_30fps_in_120hz_keeps_every_picture_on_33ms_steps){
    // A 30 fps game in a 120 Hz signal: each picture in 4 samples. The engine reads every sample.
    std::vector<int> all;
    for (int i = 0; i < 2400; ++i) all.push_back(i);
    const auto r = capture_tests::dedupRun(120.0, [](int i) { return std::int64_t(i / 4); }, all, 333333);
    CHECK(r.kept == 600 && r.keptRepeats == 0 && r.droppedNew == 0 && r.monotonic && r.neverLate);
    CHECK(r.spans > 0 && r.regular == r.spans && std::abs(r.fps - 30) < 0.1);
    // 40 fps in 120 Hz (3 samples each) and a 60 fps game in 120 Hz (2 each).
    const auto forty = capture_tests::dedupRun(120.0, [](int i) { return std::int64_t(i / 3); }, all, 250000);
    CHECK(forty.kept == 800 && forty.keptRepeats == 0 && forty.droppedNew == 0 && forty.regular == forty.spans && std::abs(forty.fps - 40) < 0.1);
    const auto sixty = capture_tests::dedupRun(120.0, [](int i) { return std::int64_t(i / 2); }, all, 166667);
    CHECK(sixty.kept == 1200 && sixty.keptRepeats == 0 && sixty.droppedNew == 0 && sixty.regular == sixty.spans && std::abs(sixty.fps - 60) < 0.1);
}

TEST(capture_dedup_engine_read_gaps_keep_the_rate_and_the_content_clock){
    // 30 fps in 120 Hz while the engine, busy with frame generation, reads only some samples: here a pattern of 1-3
    // sample gaps that always lands on a picture at least once (mailbox overwrite, nothing lost).
    std::vector<int> some;
    const int gaps[] = {1, 2, 3, 2, 1, 3, 1, 1, 2, 3};
    for (int i = 0, k = 0; i < 2400; i += gaps[k++ % 10]) some.push_back(i);
    const auto r = capture_tests::dedupRun(120.0, [](int i) { return std::int64_t(i / 4); }, some, 333333);
    CHECK(r.keptRepeats == 0 && r.droppedNew == 0 && r.monotonic && r.neverLate);
    CHECK(std::abs(r.fps - 30) < 0.1 && r.regular * 100 >= r.spans * 95);
    // Reads 41.7 ms apart (the engine late on every frame) miss whole pictures: 119 of 600 here, never seen by the
    // engine; [slipstream-capture] missedNewFps is the counter that shows it. The rate still comes from the callback's
    // numbers (about 29.3, not the 24 the engine read): this model of the code reads it about 2 % low in this extreme case.
    std::vector<int> late;
    for (int i = 0; i < 2400; i += 5) late.push_back(i);
    const auto l = capture_tests::dedupRun(120.0, [](int i) { return std::int64_t(i / 4); }, late, 333333);
    CHECK(l.kept == 480 && l.droppedNew == 119 && l.keptRepeats == 0 && l.monotonic && l.neverLate);
    CHECK(l.fps > 29.0 && l.fps < 30.5);
}

TEST(capture_dedup_still_picture_in_120hz_is_held_at_the_content_rate){
    // A 30 fps game that pauses on one picture for 2 s inside a 120 Hz signal, then moves again.
    veyra::engine::ContentDedup dedup;
    constexpr std::int64_t dt = 83333;
    int keptStill = 0, keptMoving = 0;
    for (int i = 0; i < 1440; ++i) {
        const int picture = i < 480 ? i / 4 : i < 720 ? 120 : 121 + (i - 720) / 4;
        const std::uint64_t id = capture_tests::kSpace | std::uint64_t(picture + 1);
        const auto d = dedup.observe(&id, std::int64_t(i) * dt + 1000, dt, i == 0, 0, std::uint64_t(i) + 1);
        if (d.keep && i >= 484 && i < 720) ++keptStill;
        if (d.keep && i >= 720) ++keptMoving;
    }
    // Held at about 30 a second (never 120), at least 15 a second.
    CHECK(keptStill >= 30 && keptStill <= 62);
    CHECK(keptMoving == 180 && std::abs(dedup.contentFps() - 30) < 0.2);
}

TEST(capture_interpolation_delay_is_what_the_pair_phase_learns){
    // docs/drafts/CAPTURE_FG.md section 4: a real frame is due (M-1)/M x T + ready_1 + margin after its anchor, where
    // ready_1 is B's processing plus one generated frame. EngineController feeds LivePairLatency, per pair, the largest
    // ready_k - anchor + (M-k)/M x T over its generated frames; the phase settles at its P95 + 0.5 ms.
    for (const unsigned m : {2u, 3u, 4u, 6u}) {
        for (const std::int64_t t : {std::int64_t(333333), std::int64_t(166667)}) {
            veyra::engine::LivePairLatency phase;
            const std::int64_t base = 15000, fg = 40000;   // 1.5 ms of processing, 4 ms per generated frame (made up)
            std::int64_t required = 0;
            for (unsigned k = 1; k < m; ++k) required = std::max(required, base + std::int64_t(k) * fg + std::int64_t(m - k) * t / m);
            const std::int64_t legacy = t + base, allowance = 83333 / 2;
            std::int64_t now = 0, selected = 0;
            for (int pair = 0; pair < 600; ++pair) {
                now += t;
                phase.observe(now, required);
                selected = phase.select(now, legacy, t, m, allowance);
            }
            const std::int64_t formula = std::int64_t(m - 1) * t / m + base + fg;   // the first generated frame decides
            if (std::int64_t(m) * fg < t) CHECK(required == formula);
            CHECK(selected == std::min(legacy + allowance, required + veyra::engine::LivePairLatency::kMargin100ns));
        }
    }
}

TEST(capture_pipeline_keys_for_the_kit_arms){
    // The kit's capture arms: generation tier, duplicate removal and HUD Lock as VEYRA_TEST_RP_PIPELINE keys.
    using namespace veyra::engine;
    const auto items = pipeHookParse("fg=vfg;fgMultiplier=4;fgTier=4;dedup=auto;hudLock=1;profile=original");
    CHECK(items.size() == 6 && items[0].key == "profile");   // the profile first, the rest as written
    for (const auto& item : items) CHECK(item.refusal.empty());
    CHECK(items[3].key == "fgTier" && items[3].value == "4" && items[4].key == "dedup" && items[4].value == "auto" && items[5].value == "1");
    CHECK(pipeHookParse("dedup=OFF")[0].refusal.empty() && pipeHookParse("dedup=half")[0].refusal.empty());
    CHECK(pipeHookParse("fgTier=6")[0].refusal == "bad-value" && pipeHookParse("dedup=on")[0].refusal == "bad-value");
    CHECK(pipeHookParse("hudLock=yes")[0].refusal == "bad-value" && pipeHookParse("hudlock=1")[0].refusal == "unknown-key");
    CHECK(pipeHookBaseKey("fgTier") && pipeHookBaseKey("dedup") && pipeHookBaseKey("hudLock") && !pushNrPipeKnown("fgTier") && !pushNrPipeKnown("dedup") && !pushNrPipeKnown("hudLock"));
}

TEST(capture_autostart_switch_never_comes_from_the_settings_file){
    // VEYRA_TEST_RP_CAPTURE=1 starts the remembered capture card: only from the real environment (the kit), like
    // VEYRA_TEST_RP_AUTOCONNECT, so a line left in the Slipstream settings file never starts a capture at every start.
    const auto f = veyra::engine::parseSlipstreamSettings("VEYRA_TEST_RP_CAPTURE=1\nVEYRA_EXP_RP_STATS=1\n");
    bool stats = false;
    for (const auto& sw : f.switches) { CHECK(sw.name != "VEYRA_TEST_RP_CAPTURE"); if (sw.name == "VEYRA_EXP_RP_STATS") stats = true; }
    CHECK(stats && f.unknownKeys.size() == 1 && f.unknownKeys[0] == "VEYRA_TEST_RP_CAPTURE");
}

TEST(capture_dedup_span_floor_keeps_a_120hz_signal_as_tolerant_as_60hz){
    // An engine stall of 80 ms (the mailbox replaced the samples meanwhile) in a 30 fps game. Six transport periods are
    // 100 ms at 60 Hz but 50 ms at 120 Hz: there the cadence broke and the content rate was unknown for about a quarter
    // of a second (no output-cap reduction meanwhile). VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN=1 sets a 100 ms floor.
    const auto unknownAfterStall = [](double hz, std::int64_t floor100ns) {
        veyra::engine::ContentDedup dedup;
        dedup.setMinimumContinuity(floor100ns);
        const std::int64_t dt = std::int64_t(1e7 / hz + 0.5);
        const int per = int(hz / 30 + 0.5), stallFrom = int(hz * 3), stallTo = int(hz * 3) + int(hz * 0.080);
        int unknown = 0, keptRepeats = 0;
        std::int64_t lastPicture = -1;
        for (int i = 0; i < int(hz * 10); ++i) {
            if (i > stallFrom && i < stallTo) continue;
            const std::int64_t picture = i / per;
            const std::uint64_t id = capture_tests::kSpace | std::uint64_t(picture + 1);
            const auto d = dedup.observe(&id, std::int64_t(i) * dt + 1000, dt, i == 0, 0, std::uint64_t(i) + 1);
            if (d.keep) { if (picture == lastPicture) ++keptRepeats; lastPicture = picture; }
            if (i > stallFrom && dedup.contentFps() == 0) ++unknown;
        }
        return std::pair<int, int>(unknown, keptRepeats);
    };
    CHECK(unknownAfterStall(60.0, 0).first == 0);
    const auto before = unknownAfterStall(120.0, 0);
    CHECK(before.first > 12 && before.second == 0);                 // more than 100 ms of samples without a rate
    const auto after = unknownAfterStall(120.0, 1000000);
    CHECK(after.first == 0 && after.second == 0);
    CHECK(unknownAfterStall(60.0, 1000000).first == 0);             // no change at 60 Hz
}

TEST(capture_card_name_drops_an_owners_name){
    using namespace veyra::engine;
    CHECK(pushCaptureCardName("Game Capture 4K X") == "Game_Capture_4K_X");
    CHECK(pushCaptureCardName("Alex's iPhone Camera") == "iPhone_Camera");
    CHECK(pushCaptureCardName("Alex\xE2\x80\x99s iPhone Camera") == "iPhone_Camera");
    CHECK(pushCaptureCardName("Sam's Alex's Capture") == "Capture" && pushCaptureCardName("Plain's") == "unknown");
    CHECK(pushCaptureCardName("James' Capture Card") == "Capture_Card");
    CHECK(pushCaptureCardName("Alex`s Capture Card") == "Capture_Card" && pushCaptureCardName("Alex`s") == "unknown");
    CHECK(pushCaptureCardName("Alex\xC2\xB4s Capture Card") == "Capture_Card" && pushCaptureCardName("Alex\xC2\xB4s") == "unknown");
    CHECK(pushCaptureCardName("Alex\xE2\x80\x98s Capture Card") == "Capture_Card");
    CHECK(pushCaptureCardName("Alex\xCA\xBCs Capture Card") == "Capture_Card" && pushCaptureCardName("Alex\xEF\xBC\x87s Cam") == "Cam");
    CHECK(pushCaptureCardName("Alex'S USB Video") == "USB_Video" && pushCaptureCardName("O'Brien Capture") == "Capture");
    CHECK(pushCaptureCardName("Alex's") == "unknown" && pushCaptureCardName("Alex' ") == "unknown");
    CHECK(capture_tests::has(pushCaptureCardLine("Alex's Cam Link 4K", pushCaptureFormatSummary(std::vector<capture_tests::Mode>{})), "card=Cam_Link_4K "));
}
