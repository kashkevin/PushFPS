// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "present" (R1, R2, R11): the video window's region decision, the in-picture overlay's board and placement, the
// [fg-health] window, NVIDIA low latency's Sleep overrun and its guard. Portable logic only; the GPU blend is tested on a
// D3D12 device (tests/integration/PushGpuTests.cpp "video-overlay*", WARP in CI).
#include "veyra/engine/PushVideoOverlay.h"
#include "veyra/engine/PushFgHealth.h"
#include "veyra/engine/PushBlitAhead.h"
#include <vector>
#include <memory>
#include <string>

namespace present_path_tests {
inline bool has(const std::string& line, const std::string& field) { return line.find(" " + field) != std::string::npos || line.rfind(field, 0) == 0; }
}  // namespace present_path_tests

TEST(present_region_decision_follows_sync_video_covers) {
    using veyra::engine::PushRegionKind;
    using veyra::engine::PushRegionInput;
    using veyra::engine::pushRegionDecision;
    // Nothing over the picture: no region (the fast path's precondition).
    CHECK(pushRegionDecision(PushRegionInput{}) == PushRegionKind::Full);
    // The badge as a cover (Package 14, or the legacy switch): a hole, a complex region.
    CHECK(pushRegionDecision(PushRegionInput{1, false, false, false}) == PushRegionKind::Complex);
    // The badge drawn in the swap chain is no cover: the region stays full.
    CHECK(pushRegionDecision(PushRegionInput{0, false, false, false}) == PushRegionKind::Full);
    // A cover wholly outside the window cuts nothing.
    const veyra::engine::PushRect outside{-50, -50, -10, -10};
    CHECK(veyra::engine::pushCoversInPicture(1920, 1080, &outside, 1) == 0);
    // The cineIn band without covers is complex; a cover wins over the band; the rect experiment keeps one rectangle.
    CHECK(pushRegionDecision(PushRegionInput{0, true, false, false}) == PushRegionKind::Complex);
    CHECK(pushRegionDecision(PushRegionInput{2, true, false, false}) == PushRegionKind::Complex);
    CHECK(pushRegionDecision(PushRegionInput{2, false, true, false}) == PushRegionKind::Rect);
    CHECK(pushRegionDecision(PushRegionInput{2, true, false, true}) == PushRegionKind::Full);
    // Review S5: a cover wholly outside the picture (the retracted dock bar) still installs a region: one rectangle.
    CHECK(pushRegionDecision(PushRegionInput{0, false, false, false, 1}) == PushRegionKind::Rect);
    CHECK(pushRegionDecision(PushRegionInput{0, true, false, false, 1}) == PushRegionKind::Rect);   // holes win over the band
    CHECK(pushRegionDecision(PushRegionInput{1, false, false, false, 2}) == PushRegionKind::Complex);
    CHECK(std::string(veyra::engine::pushRegionKindName(PushRegionKind::Complex)) == "complex");
}

TEST(present_region_board_counts_changes_only) {
    veyra::engine::PushRegionBoard b;
    const auto s0 = b.serial();
    b.set({veyra::engine::PushRegionKind::Complex, 1, false, "PushLatencyBadge/pushfps-latency-badge"});
    CHECK(b.serial() == s0 + 1);
    b.set({veyra::engine::PushRegionKind::Complex, 1, false, "PushLatencyBadge/pushfps-latency-badge"});
    CHECK(b.serial() == s0 + 1);   // the same state again is no change
    b.set({});
    CHECK(b.serial() == s0 + 2 && b.get().kind == veyra::engine::PushRegionKind::Full);
}

TEST(present_overlay_board_versions_and_acknowledgement) {
    veyra::engine::PushVideoOverlayBoard b;
    auto image = std::make_shared<veyra::engine::PushVideoOverlayImage>();
    image->x = 15; image->y = 15; image->width = 4; image->height = 2; image->rgba.assign(4 * 2 * 4, 200);
    const auto v1 = b.publish(image);
    std::uint64_t seen = 0;
    CHECK(b.take(seen) != nullptr && seen == v1 && b.version() == v1);
    // Not drawn yet: the QML item stays a cover.
    CHECK(!b.drawnSince(v1, 1000));
    b.drawn(v1, 1000);
    CHECK(b.drawnSince(v1, 1500));
    CHECK(!b.drawnSince(v1, 2001));          // nothing drawn for over 1 s (paused, closed): a cover again
    const auto v2 = b.publish(image);
    CHECK(!b.drawnSince(v2, 1200));          // a newer picture than the one drawn
    b.drawn(v2, 1300);
    CHECK(b.drawnSince(v1, 1300) && b.drawnSince(v2, 1300));
    b.stopped();
    CHECK(!b.drawnSince(v2, 1300));          // stopped: every item a cover at once
    // An invalid picture clears it.
    auto broken = std::make_shared<veyra::engine::PushVideoOverlayImage>();
    broken->width = 3; broken->height = 3;   // no pixels
    b.publish(broken);
    CHECK(b.take(seen) == nullptr);
    b.publish(nullptr);
    CHECK(b.take(seen) == nullptr && !b.drawnSince(0, 1300));
}

TEST(present_overlay_placement_in_buffer_pixels) {
    using veyra::engine::pushOverlayPlacement;
    // Buffer = window (the normal case): the picture lands where the item is, pixel for pixel.
    auto p = pushOverlayPlacement(15, 15, 200, 38, 2560, 1440, 2560, 1440);
    CHECK(p.left == 15 && p.top == 15 && p.right == 215 && p.bottom == 53 && p.scaleX == 1.0f && p.scaleY == 1.0f && !p.empty());
    // A deferred resize: the old 1280x720 buffer is stretched to a 2560x1440 window: half the buffer pixels.
    p = pushOverlayPlacement(16, 16, 200, 38, 2560, 1440, 1280, 720);
    CHECK(p.left == 8 && p.top == 8 && p.right == 108 && p.bottom == 27 && p.scaleX == 0.5f);
    // Clipped to the buffer; nothing when outside or sizeless.
    p = pushOverlayPlacement(2500, 1400, 200, 100, 2560, 1440, 2560, 1440);
    CHECK(p.right == 2560 && p.bottom == 1440 && !p.empty());
    CHECK(pushOverlayPlacement(3000, 10, 20, 20, 2560, 1440, 2560, 1440).empty());
    CHECK(pushOverlayPlacement(10, 10, 0, 20, 2560, 1440, 2560, 1440).empty());
    CHECK(pushOverlayPlacement(10, 10, 20, 20, 0, 1440, 2560, 1440).empty());
}

TEST(present_fg_health_window_every_ten_seconds) {
    using namespace present_path_tests;
    veyra::engine::PushFgHealth h;
    veyra::engine::PushHealthTotals t;
    std::int64_t now = 1000000000;
    CHECK(h.second(now, {}, t).empty());   // the first second opens the window
    // 10 s of a 30 -> 90 FPS session: 300 real frames at 30 ms with one 40 ms spike, presents every 11.1 ms.
    std::int64_t call = now;
    for (int i = 0; i < 300; ++i) {
        h.real(i == 150 ? 40.0 : 30.0);
        for (int k = 0; k < 3; ++k) { h.presented(call, k < 2); call += 111111; }
        h.gpu(5, 6.5);
        h.gpu(13, 0.12);
        h.gpu(7, 99.0);   // a stage [fg-health] does not report
    }
    veyra::engine::PushHealthGrid g{89, 88, 90, 90.0, 12.0, 1};
    std::string line;
    for (int s = 1; s <= 10 && line.empty(); ++s) {
        if (s == 10) { t.dropDeadline = 2; t.duplicates = 300; t.mailboxDropped = 1; }
        line = h.second(now + std::int64_t(s) * 10000000, g, t);
    }
    CHECK(!line.empty());
    CHECK(has(line, "realFrames=300") && has(line, "realP50Ms=30.00") && has(line, "realMaxMs=40.00") && has(line, "realP99Ms=30.00"));
    CHECK(has(line, "presents=900") && has(line, "generatedPresents=600") && has(line, "intervalP50Ms=11.11"));
    CHECK(has(line, "dropDeadline=2") && has(line, "duplicates=300") && has(line, "mailboxDropped=1") && has(line, "dropCapacity=0"));
    CHECK(has(line, "gpu_fg1P50Ms=6.500") && has(line, "gpu_blitP99Ms=0.120") && has(line, "gpu_colorP50Ms=-1"));
    CHECK(has(line, "onGrid1msPct=98.88") && has(line, "shownPct=100.00") && has(line, "gridLongestMs=12.00"));
    CHECK(has(line, "reflex=0") && has(line, "reflexSleeps=0") && has(line, "reflexOverrunP95Ms=-1"));
    // The next window starts empty and counts totals from where this one ended.
    std::string next;
    for (int s = 11; s <= 21 && next.empty(); ++s) next = h.second(now + std::int64_t(s) * 10000000, {}, t);
    CHECK(has(next, "realFrames=0") && has(next, "realP50Ms=-1") && has(next, "dropDeadline=0") && has(next, "duplicates=0"));
    // A total that falls (a new session) counts from zero.
    t.duplicates = 5;
    std::string third;
    for (int s = 22; s <= 32 && third.empty(); ++s) third = h.second(now + std::int64_t(s) * 10000000, {}, t);
    CHECK(has(third, "duplicates=5"));
}

TEST(present_fg_health_pause_is_no_interval) {
    using namespace present_path_tests;
    veyra::engine::PushFgHealth h;
    h.second(1, {}, {});
    h.presented(100000000, false);
    h.presented(100111111, true);
    h.presented(110000000, false);   // 1 s later: a pause, not an interval
    const auto line = h.second(1 + veyra::engine::PushFgHealth::kWindow100ns, {}, {});
    CHECK(has(line, "intervalMaxMs=11.11") && has(line, "presents=3"));
}

TEST(present_reflex_overrun_arithmetic) {
    using veyra::engine::pushReflexOverrunMs;
    // Sleep 100..300 (x100 ns), sample arrived at 400: ended before it, nothing added.
    CHECK(pushReflexOverrunMs(1000000, 1030000, 1040000) == 0.0);
    // Sample arrived during the Sleep, 1 ms before its end: 1 ms added to that frame.
    CHECK(pushReflexOverrunMs(1000000, 1030000, 1020000) == 1.0);
    // Sample already waiting before the Sleep began: the whole Sleep (3 ms) is added, never more.
    CHECK(pushReflexOverrunMs(1000000, 1030000, 900000) == 3.0);
    CHECK(pushReflexOverrunMs(0, 1030000, 900000) == -1.0);
    CHECK(pushReflexOverrunMs(1000000, 1030000, 0) == -1.0);
}

TEST(present_reflex_guard_ends_low_latency_on_overruns_only) {
    using G = veyra::engine::PushReflexOverrunGuard;
    // Samples every 16.7 ms (a 60 Hz card); `at` advances with each.
    double at = 1000.0;
    const auto feed = [&at](G& g, double overrun) { at += 16.7; return g.observe(at, overrun); };
    {
        // Sleeps that end before the sample arrives (the engine's idle wait) never end it.
        G g;
        for (int i = 0; i < 2000; ++i) CHECK(!feed(g, 0.0));
        CHECK(!g.ended());
    }
    {
        // The reviewer's case: two isolated transients 15 sleeps apart (after warm-up) do NOT trip it.
        G g;
        for (int i = 0; i < 200; ++i) feed(g, 0.0);
        CHECK(!feed(g, 3.0));
        for (int i = 0; i < 14; ++i) CHECK(!feed(g, 0.0));
        CHECK(!feed(g, 2.0));
        for (int i = 0; i < 1000; ++i) CHECK(!feed(g, 0.0));
        CHECK(!g.ended());
        // Even several per block, if never in two consecutive blocks.
        for (int block = 0; block < 6; ++block)
            for (unsigned i = 0; i < G::kBlock; ++i) CHECK(!feed(g, block % 2 == 0 && i < 5 ? 1.0 : 0.0));
        CHECK(!g.ended());
    }
    {
        // Overruns during warm-up (start-up, a scene load right after a reconfiguration) are ignored.
        G g;
        for (int i = 0; i < 150; ++i) CHECK(!feed(g, 5.0));   // 150 x 16.7 ms = 2.5 s < 3 s warm-up
        CHECK(!g.ended());
        g.restartWarmup(at);                                   // a reconfiguration
        for (int i = 0; i < 150; ++i) CHECK(!feed(g, 5.0));
        CHECK(!g.ended());
        // A gap of more than 250 ms (a source change or pause) starts the warm-up again.
        at += 400.0;
        for (int i = 0; i < 150; ++i) CHECK(!feed(g, 5.0));
        CHECK(!g.ended());
    }
    {
        // Unknown measurements (a mailbox replacement) count neither way.
        G g;
        for (int i = 0; i < 200; ++i) feed(g, 0.0);
        for (int i = 0; i < 1000; ++i) CHECK(!feed(g, -1.0));
        CHECK(!g.ended());
    }
    {
        // Sustained: at least 4 of every 60 sleeps more than 0.5 ms past the arrival, in two consecutive blocks.
        G g;
        for (int i = 0; i < 200; ++i) feed(g, 0.0);   // past the warm-up, and some blocks counted
        bool tripped = false;
        for (int i = 0; i < 4 * int(G::kBlock) && !tripped; ++i) tripped = feed(g, i % 12 == 0 ? 1.0 : 0.0);   // 5 per block
        CHECK(tripped && g.ended());
        CHECK(!feed(g, 0.0) && g.ended());   // reports once; stays ended
        at += 5000.0;                        // even after a gap: ended for the presenter's life
        CHECK(!feed(g, 9.0) && g.ended());
    }
    // Only a live capture card with frame generation, and not with the rollback switch.
    CHECK(veyra::engine::pushReflexGuardApplies(true, true, false));
    CHECK(!veyra::engine::pushReflexGuardApplies(true, false, false));
    CHECK(!veyra::engine::pushReflexGuardApplies(false, true, false));
    CHECK(!veyra::engine::pushReflexGuardApplies(true, true, true));
}

namespace present_path_tests {
// A fixed pseudo-random sequence (the same on every compiler): x in [0, 1).
struct Lcg {
    std::uint64_t state;
    double next() { state = state * 6364136223846793005ull + 1442695040888963407ull; return double(state >> 11) / 9007199254740992.0; }
};
// 3X from 30 FPS: three frames per 33.37 ms pair, deadlines on the 11.12 ms grid; readiness before or after the deadline,
// record and blit costs, and other GPU work in random windows (a model; the costs are made up, not measured).
inline void blitScenario(std::uint64_t seed, std::vector<veyra::engine::PushBlitFrame>& frames, std::vector<veyra::engine::PushGpuBusy>& busy) {
    Lcg r{seed};
    frames.clear();
    busy.clear();
    const double slot = 1000.0 / 29.97 / 3.0;
    for (int k = 0; k < 270; ++k) {
        veyra::engine::PushBlitFrame f;
        f.deadlineMs = 100.0 + slot * k;
        f.readyMs = f.deadlineMs - 8.0 + 10.0 * r.next();   // up to 8 ms early, up to 2 ms late
        f.recordMs = 0.05 + 0.6 * r.next();
        f.blitMs = 0.08 + 0.4 * r.next();
        frames.push_back(f);
    }
    for (double t = 90.0; t < 100.0 + slot * 272; t += 1.0 + 6.0 * r.next()) {
        const double len = 0.2 + 5.0 * r.next();
        busy.push_back({t, t + len});
        t += len;
    }
}
}  // namespace present_path_tests

TEST(present_blit_ahead_queue_model_is_never_later_for_any_frame) {
    using namespace present_path_tests;
    std::vector<veyra::engine::PushBlitFrame> frames;
    std::vector<veyra::engine::PushGpuBusy> busy;
    std::size_t aheadFrames = 0, earlier = 0;
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        blitScenario(seed, frames, busy);
        const auto today = veyra::engine::pushSimulateBlits(frames, busy, false);
        const auto ahead = veyra::engine::pushSimulateBlits(frames, busy, true);
        CHECK(today.size() == frames.size() && ahead.size() == frames.size());
        for (std::size_t k = 0; k < frames.size(); ++k) {
            const auto& a = ahead[k];
            // Present never before the frame's deadline; never before its blit was submitted.
            CHECK(a.presentCallMs >= frames[k].deadlineMs);
            CHECK(a.presentCallMs >= a.submitMs);
            // The flip can happen only with the blit complete (the queue orders it: the earliest flip is after both).
            CHECK(a.flipReadyMs >= a.blitEndMs && a.flipReadyMs >= a.presentCallMs);
            // Never later than today, for every frame: the Present call and the earliest flip.
            CHECK(a.presentCallMs <= today[k].presentCallMs + 1e-9);
            CHECK(a.flipReadyMs <= today[k].flipReadyMs + 1e-9);
            if (k > 0) {
                // In order: Presents, blits and flips never overtake the previous frame's.
                CHECK(a.presentCallMs > ahead[k - 1].presentCallMs);
                CHECK(a.blitStartMs >= ahead[k - 1].blitEndMs);
                CHECK(a.flipReadyMs >= ahead[k - 1].flipReadyMs);
                // One prepared frame at a time: a blit is submitted only after the previous frame's Present.
                CHECK(a.submitMs >= ahead[k - 1].presentCallMs);
            }
            if (a.ahead) ++aheadFrames;
            if (a.flipReadyMs + 1e-9 < today[k].flipReadyMs) ++earlier;
        }
    }
    // The model exercises both paths (frames ready early, and late ones handled as today) and finds earlier flips.
    CHECK(aheadFrames > 1000 && earlier > 1000);
    // Without the switch the outcome is today's exactly.
    blitScenario(7, frames, busy);
    const auto a = veyra::engine::pushSimulateBlits(frames, busy, false), b = veyra::engine::pushSimulateBlits(frames, busy, false);
    for (std::size_t k = 0; k < a.size(); ++k) CHECK(a[k].flipReadyMs == b[k].flipReadyMs && !a[k].ahead);
}

TEST(present_blit_ahead_refused_while_any_capture_consumer_is_active) {
    using veyra::engine::pushBlitAheadAllowed;
    CHECK(pushBlitAheadAllowed(true, true, false, false));
    CHECK(!pushBlitAheadAllowed(false, true, false, false));   // switch off
    CHECK(!pushBlitAheadAllowed(true, false, false, false));   // shared queue
    CHECK(!pushBlitAheadAllowed(true, true, true, false));     // XeSS presents
    CHECK(!pushBlitAheadAllowed(true, true, false, true));     // recording / replay / stream / clip / HUD / probes
    // With a consumer active the model's outcome is today's, frame for frame (every frame takes the old path).
    std::vector<veyra::engine::PushBlitFrame> frames;
    std::vector<veyra::engine::PushGpuBusy> busy;
    present_path_tests::blitScenario(3, frames, busy);
    const auto today = veyra::engine::pushSimulateBlits(frames, busy, false);
    const auto gated = veyra::engine::pushSimulateBlits(frames, busy, pushBlitAheadAllowed(true, true, false, true));
    for (std::size_t k = 0; k < frames.size(); ++k)
        CHECK(!gated[k].ahead && gated[k].submitMs == today[k].submitMs && gated[k].presentCallMs == today[k].presentCallMs);
}

TEST(present_blit_ahead_flip_discard_three_buffers_stays_legal) {
    using namespace present_path_tests;
    // Random play: frames prepared (blitted ahead) after the previous Present, some dropped after their blit (a capacity or
    // cap drop) and replaced by the next frame's blit, Presents only with capacity (maximum frame latency 1), the display
    // latching queued frames at random moments.
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        Lcg r{seed};
        veyra::engine::PushFlipChain chain;
        int frame = 0, lastShown = -1;
        std::vector<int> presented;
        for (int step = 0; step < 2000; ++step) {
            const unsigned before = chain.current();
            CHECK(chain.blit(frame));                 // never into the buffer on screen or the queued one
            CHECK(chain.current() == before);         // the index moves only at Present
            if (r.next() < 0.1) { ++frame; continue; }   // dropped after its blit: the next blit overwrites it
            while (!chain.present(1)) {               // waits for capacity, as presentationReady() does
                const int shown = chain.latch();
                if (shown >= 0) { CHECK(shown > lastShown); lastShown = shown; }
            }
            presented.push_back(frame);
            if (r.next() < 0.6) {
                const int shown = chain.latch();
                if (shown >= 0) { CHECK(shown > lastShown); lastShown = shown; }
            }
            ++frame;
        }
        // Everything shown was presented, in order, and no dropped frame ever reached the screen.
        while (chain.queued()) { const int shown = chain.latch(); CHECK(shown > lastShown); lastShown = shown; }
        CHECK(!presented.empty() && lastShown == presented.back());
    }
    // With a deeper queue (maximum latency 2) the current buffer CAN be a queued one: the model notices it, which is why the
    // experiment relies on maximum frame latency 1 (the presenter's setting while pacing) and is off by default.
    veyra::engine::PushFlipChain deep;
    bool conflict = false;
    for (int k = 0; k < 6; ++k) { conflict = !deep.blit(k) || conflict; deep.present(2); }
    CHECK(conflict);
}
