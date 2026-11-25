// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s10-display": the "to screen" figure, the refresh-phase test, Present lateness, the precise-wait hold rule
// (engine/PushDisplayPath.h, engine/PushFgHealth.h) and the fullscreen region rule (engine/PushVideoOverlay.h).
#include "veyra/engine/PushDisplayPath.h"
#include "veyra/engine/PushFgHealth.h"
#include "veyra/engine/PushVideoOverlay.h"
#include "veyra/engine/PushCaptureStall.h"
#include <cstdint>
#include <string>

namespace display_path_tests {
inline std::string field(const std::string& line, const std::string& key) {
    const auto at = line.find(" " + key + "=");
    if (at == std::string::npos) return "<missing>";
    const auto start = at + key.size() + 2;
    return line.substr(start, line.find(' ', start) - start);
}
inline std::string windowAt(veyra::engine::PushFgHealth& h, int windows) {
    return h.second(10 + windows * veyra::engine::PushFgHealth::kWindow100ns, {}, {});
}
}  // namespace display_path_tests

TEST(display_refresh_phase_bins_by_time_to_the_next_vblank) {
    veyra::engine::PushRefreshPhase p;
    const double period = 1000.0 / 179.952;   // 5.557 ms
    p.add(period * 1 + 0.2, period);          // bin 0, 1 refresh
    p.add(period * 2 + 0.4, period);          // bin 0, 2 refreshes
    p.add(period * 1 + 0.7, period);          // bin 1
    p.add(period * 1 + 4.0, period);          // bin 7 (3.5 ms and more)
    p.add(-1.0, period);                      // ignored
    p.add(4.0, 0.0);                          // ignored: no period
    CHECK(p.count[0] == 2 && p.count[1] == 1 && p.count[7] == 1);
    CHECK(std::abs(p.mean(0) - 1.5) < 1e-9);
    CHECK(std::abs(p.mean(1) - 1.0) < 1e-9);
    CHECK(p.mean(3) < 0);
    CHECK(p.describe() == "0-0.5:1.500/2,0.5-1:1.000/1,1-1.5:-1/0,1.5-2:-1/0,2-2.5:-1/0,2.5-3:-1/0,3-3.5:-1/0,3.5+:1.000/1");
    // A 3 ms cut-off (the reviewer's case): the bins below it show the extra refresh at full size, the others none.
    veyra::engine::PushRefreshPhase c;
    for (int i = 0; i < 70; ++i) {
        const double toNext = 0.05 + 0.079 * i;   // 0.05 .. 5.5 ms
        c.add(toNext + (toNext < 3.0 ? 2.0 : 1.0) * period, period);
    }
    for (std::size_t b = 0; b < 6; ++b) CHECK(std::abs(c.mean(b) - 2.0) < 1e-9);
    CHECK(std::abs(c.mean(7) - 1.0) < 1e-9);
    p.clear();
    CHECK(p.mean(0) < 0);
}

TEST(display_to_screen_board_is_fresh_for_three_seconds_only) {
    veyra::engine::PushToScreenBoard b;
    CHECK(!b.read(1000.0).known);
    b.publish(9.4, 14.5, 80, 1000.0);
    auto r = b.read(2500.0);
    CHECK(r.known && std::abs(r.p50Ms - 9.4) < 1e-9 && std::abs(r.p95Ms - 14.5) < 1e-9);
    CHECK(!b.read(4001.0).known);              // stale
    b.publish(9.4, 14.5, 0, 5000.0);           // a second without dated presents: unknown, not a number
    CHECK(!b.read(5001.0).known);
}

TEST(display_deadline_hold_rule) {
    using veyra::engine::pushDeadlineHoldsReadyWake;
    CHECK(!pushDeadlineHoldsReadyWake(false, 10000));                 // switch off: never (today's behaviour)
    CHECK(pushDeadlineHoldsReadyWake(true, 10000));                   // 1 ms before the deadline: held
    CHECK(pushDeadlineHoldsReadyWake(true, veyra::engine::kPushDeadlineHold100ns));
    CHECK(!pushDeadlineHoldsReadyWake(true, veyra::engine::kPushDeadlineHold100ns + 1));   // earlier: the poll has time
    CHECK(!pushDeadlineHoldsReadyWake(true, 0));                      // due: nothing to hold
}

TEST(display_fg_health_carries_to_screen_and_lateness) {
    veyra::engine::PushFgHealth h;
    (void)h.second(10, {}, {});
    const double period = 1000.0 / 179.952;
    for (int i = 0; i < 100; ++i) h.toScreen(period + 4.0, period);   // 9.56 ms, 4 ms before the vblank: "other"
    h.toScreen(2 * period + 0.4, period);                              // late cut, 2 refreshes
    for (int i = 0; i < 60; ++i) h.presentLate(-0.2, true);           // on time
    h.presentLate(1.6, true);
    h.presentLate(0.3, false);
    h.nearDeadlineWake(false);
    h.nearDeadlineWake(true);
    const auto line = h.second(10 + veyra::engine::PushFgHealth::kWindow100ns, {}, {});
    using display_path_tests::field;
    CHECK(field(line, "toScreenSamples") == "101");
    CHECK(field(line, "toScreenP50Ms") == "9.56");
    CHECK(field(line, "toScreenRefreshesByMsToVblank") == "0-0.5:2.000/1,0.5-1:-1/0,1-1.5:-1/0,1.5-2:-1/0,2-2.5:-1/0,2.5-3:-1/0,3-3.5:-1/0,3.5+:1.000/100");
    CHECK(field(line, "presentLateGenMaxMs") == "1.600");
    CHECK(field(line, "presentLateGenP50Ms") == "0.000");
    CHECK(field(line, "presentLateRealMaxMs") == "0.300");
    CHECK(field(line, "presentsLateOver1ms") == "1");
    CHECK(field(line, "readyWakesNearDeadline") == "1");
    CHECK(field(line, "readyWakesHeld") == "1");
    CHECK(line.find("NOT part of the real frame's delay") != std::string::npos);
    // The next window starts empty.
    const auto next = display_path_tests::windowAt(h, 2);
    CHECK(field(next, "toScreenSamples") == "0" && field(next, "toScreenP50Ms") == "-1" && field(next, "presentsLateOver1ms") == "0");
}

TEST(display_fullscreen_region_leaves_out_covers_that_cut_nothing) {
    using veyra::engine::pushCoverCutsWindow;
    using veyra::engine::pushFullscreenDropsOutsideCovers;
    // The retracted dock (TopDock.qml: 44 px tall at topMargin -56, +1 px rounding in syncVideoCovers): wholly above.
    CHECK(!pushCoverCutsWindow(1258, -56, 45, 45, 2560, 1440));
    CHECK(pushCoverCutsWindow(1258, 8, 45, 45, 2560, 1440));          // the dock shown
    CHECK(pushCoverCutsWindow(-10, -10, 11, 11, 2560, 1440));         // one pixel inside counts
    CHECK(!pushCoverCutsWindow(2560, 0, 40, 40, 2560, 1440));         // right of the window
    CHECK(pushFullscreenDropsOutsideCovers(true, true));              // fullscreen with the experiment
    CHECK(!pushFullscreenDropsOutsideCovers(false, true));            // windowed play unchanged
    CHECK(!pushFullscreenDropsOutsideCovers(true, false));            // the default: as Package 15
    // What the presenter then logs: no cover at all is region "full" (no SetWindowRgn region), a kept outside cover "rect".
    using veyra::engine::PushRegionInput;
    using veyra::engine::pushRegionDecision;
    using veyra::engine::PushRegionKind;
    PushRegionInput none;
    CHECK(pushRegionDecision(none) == PushRegionKind::Full);
    PushRegionInput legacy;
    legacy.covers = 1;
    CHECK(pushRegionDecision(legacy) == PushRegionKind::Rect);
}

TEST(display_fg_health_splits_late_presents_by_a_ready_wake_and_times_polls) {
    veyra::engine::PushFgHealth h;
    (void)h.second(10, {}, {});
    h.nearDeadlineWake(false);          // a ready wake ended the wait ...
    h.presentLate(1.4, true);           // ... and the Present after it was late
    h.presentLate(1.2, true);           // late without a wake before it
    h.nearDeadlineWake(true);           // held: the wait went on, so it does not mark the next Present
    h.presentLate(0.1, true);
    h.poll(1000, 1000 + 3000, 0);                   // 0.3 ms, no deadline
    h.poll(2000, 2000 + 15000, 2000 + 10000);       // 1.5 ms, began before a due deadline, ended 0.5 ms after it
    h.poll(5000, 5000 + 2000, 4000);                // began after the deadline: not counted as past it
    const auto line = h.second(10 + veyra::engine::PushFgHealth::kWindow100ns, {}, {});
    using display_path_tests::field;
    CHECK(field(line, "presentsAfterReadyWake") == "1");
    CHECK(field(line, "lateOver1msAfterReadyWake") == "1");
    CHECK(field(line, "lateOver1msOtherwise") == "1");
    CHECK(field(line, "polls") == "3");
    CHECK(field(line, "pollMaxMs") == "1.500");
    CHECK(field(line, "pollsPastDeadline") == "1");
    CHECK(field(line, "pollOverrunMaxMs") == "0.500");
    {   // the RAII watch adds one poll
        veyra::engine::PushPollWatch w(h, 0);
    }
    const auto next = h.second(10 + 2 * veyra::engine::PushFgHealth::kWindow100ns, {}, {});
    CHECK(field(next, "polls") == "1" && field(next, "pollsPastDeadline") == "0");
}

TEST(display_present_before_poll_hold_follows_the_measured_polls) {
    veyra::engine::PushPresentBeforePoll p;
    CHECK(p.hold100ns() == veyra::engine::PushPresentBeforePoll::kMaxHold100ns);   // too few polls: 2 ms
    for (int i = 0; i < 200; ++i) p.observePoll(i == 7 ? 5.0 : 0.3);              // one outlier above the P99
    CHECK(p.hold100ns() == 3000 + veyra::engine::PushPresentBeforePoll::kGuard100ns);
    CHECK(p.applies(true, 0, 5000));
    CHECK(!p.applies(true, 0, 6000));
    CHECK(!p.applies(false, 0, 5000));     // the switch off: never
    CHECK(!p.applies(true, 5000, 5000));   // due now: today's pre-advance presents it
    for (int i = 0; i < 300; ++i) p.observePoll(3.0);
    CHECK(p.hold100ns() == veyra::engine::PushPresentBeforePoll::kMaxHold100ns);  // capped at 2 ms
}

TEST(display_present_before_poll_never_presents_the_head_later_model) {
    // Randomized owner-loop cases (a model, not the Windows loop): the head frame is never presented later than today, a
    // sample arriving meanwhile is read at most the hold later, and cases outside the hold are unchanged.
    std::uint64_t x = 88172645463325252ull;
    const auto rnd = [&x](double lo, double hi) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return lo + (hi - lo) * double(x % 1000003) / 1000003.0; };
    int held = 0, earlier = 0;
    for (int i = 0; i < 200000; ++i) {
        veyra::engine::PushOwnerCase c;
        c.t0 = 0;
        c.hold = rnd(0.25, 2.0);
        c.due = rnd(0.0, 3.0);
        c.poll = rnd(0.0, 3.0);
        c.process = rnd(0.2, 2.0);
        c.arrival = rnd(0, 1) < 0.5 ? -1.0 : rnd(0.0, 4.0);
        const auto today = veyra::engine::pushOwnerToday(c), mine = veyra::engine::pushOwnerPresentBeforePoll(c);
        CHECK(mine.headPresent <= today.headPresent + 1e-12);
        CHECK(mine.headPresent >= c.due - 1e-12);                       // never early
        if (c.arrival >= 0) CHECK(mine.arrivalRead - today.arrivalRead <= c.hold + 1e-9);
        const bool inHold = c.due > c.t0 && c.due - c.t0 <= c.hold;
        if (!inHold) CHECK(mine.headPresent == today.headPresent && mine.arrivalRead == today.arrivalRead);
        held += inHold ? 1 : 0;
        earlier += mine.headPresent < today.headPresent - 1e-9 ? 1 : 0;
    }
    CHECK(held > 10000 && earlier > 1000);   // the cases exist and the reorder helps in some of them
}

TEST(display_capture_stall_line_for_a_late_callback_and_its_burst) {
    veyra::engine::PushCaptureStall c;
    const double t = 33.338;
    for (int i = 0; i < 10; ++i) CHECK(!c.observe(t + (i % 2 ? 3.0 : -3.0), t, 1.1, t, std::uint64_t(i)));   // ordinary jitter: nothing
    CHECK(!c.observe(0, t, 1.1, t, 10));                                    // the first callback: nothing
    const auto gap = c.observe(158.2, t, 6.9, t, 11);                        // the owner's 23:00:05 callback
    CHECK(gap && gap->rfind("kind=gap gapMs=158.2 ptsStepMs=33.338 intervals=4.7 copyMs=6.90 previousCopyMaxMs=1.10 received=11 gaps=1", 0) == 0);
    CHECK(!c.observe(7.0, t, 6.8, t, 12));                                   // the burst ...
    CHECK(!c.observe(7.1, t, 6.7, t, 13));
    const auto burst = c.observe(25.0, t, 1.2, t, 14);                       // ... ends
    CHECK(burst && burst->rfind("kind=burst callbacks=2 spanMs=14.1 copyMaxMs=6.90 nextDeltaMs=25.0", 0) == 0);
    CHECK(!c.observe(33.3, t, 1.1, t, 15));
    CHECK(c.gaps() == 1);
    CHECK(!c.observe(100.0, t, 1.1, 0.0, 16));                               // no interval known: nothing
}
