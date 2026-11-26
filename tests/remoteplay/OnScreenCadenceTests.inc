// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s12-pace": the measured on-screen hold of each frame (engine/PushOnScreenCadence.h), the opt-in phase lock
// (engine/PushPhaseLock.h) and a simulation of his composed 179.952 Hz desktop with the cut-off band measured in his
// Package 16 log. SIMULATED: the model of DWM is fitted to his phase bins, it is not Windows.
#include "veyra/engine/PushOnScreenCadence.h"
#include "veyra/engine/PushPhaseLock.h"
#include "veyra/engine/PushFrameProbe.h"
#include "veyra/engine/PushDownscale.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace onscreen_tests {
inline std::string field(const std::string& line, const std::string& key) {
    const auto at = line.find(" " + key + "=");
    const auto start = line.rfind(key + "=", 0) == 0 ? key.size() + 1 : at == std::string::npos ? std::string::npos : at + key.size() + 2;
    if (start == std::string::npos) return "<missing>";
    return line.substr(start, line.find(' ', start) - start);
}
// Deterministic on every compiler (std:: distributions differ between standard libraries).
struct Lcg {
    std::uint64_t s;
    double next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return double((s >> 11) & ((1ULL << 53) - 1)) / double(1ULL << 53);
    }
};
// The composed desktop: a present lands on the first vblank whose own compositor cut-off it beats (cut-off lead drawn per
// vblank between 0.3 and 3.0 ms; plus, per vblank, a 12 % chance that the pass is late by a whole refresh: the 1.17 floor
// of his best bin), shown one refresh after that (composition), never before the vblank after the previous frame's.
struct Desktop {
    double periodMs = 1000.0 / 179.952;
    double startMs = 0.37;
    Lcg rng{12345};
    std::vector<double> lead, late;
    double vblank(std::int64_t j) const { return startMs + double(j) * periodMs; }
    void ensure(std::int64_t j) {
        while (std::int64_t(lead.size()) <= j) {
            lead.push_back(0.3 + 2.7 * rng.next());
            late.push_back(rng.next() < 0.12 ? 1.0 : 0.0);
        }
    }
    std::int64_t land(double presentMs, std::int64_t previous) {
        std::int64_t j = std::int64_t(std::floor((presentMs - startMs) / periodMs)) + 1;
        for (;; ++j) {
            ensure(j + 2);
            if (vblank(j) - presentMs >= lead[std::size_t(j)]) break;
        }
        std::int64_t shown = j + 1 + std::int64_t(late[std::size_t(j)]);
        return std::max(shown, previous + 1);
    }
};
// meanShownAfterDeadlineMs: the vblank that shows a frame, after the frame's own deadline (the display-side delay).
struct SimResult { double evenPct = 0; std::uint64_t holds = 0, longHolds = 0; double meanShownAfterDeadlineMs = 0; bool neverLater = true; };
// outputFps from the content clock (29.97 x 3 etc.); jitter as in his log: most presents within 0.05 ms, 3 % 1-2.5 ms late.
inline SimResult simulate(double contentHz, unsigned multiplier, bool lock, double seconds) {
    Desktop desk;
    Lcg noise{987654321};
    const double interval = 1000.0 / contentHz / multiplier;
    std::int64_t previous = -1000;
    std::vector<std::int64_t> shown;
    double toScreen = 0;
    SimResult r;
    const auto n = std::size_t(seconds * 1000.0 / interval);
    for (std::size_t i = 0; i < n; ++i) {
        const double deadline = 5.0 + double(i) * interval;
        double due = deadline;
        if (lock) {
            // The grid the presenter would know: the last vblank before the frame's deadline (statistics are read before
            // every Present, so it is at most a few refreshes old) and the period.
            const auto j = std::int64_t(std::floor((deadline - desk.startMs) / desk.periodMs)) - 2;
            const auto d = veyra::engine::pushPhaseLockDue(std::int64_t(deadline * 10000), std::int64_t((deadline - 3.0) * 10000),
                                                           std::int64_t(desk.vblank(j) * 10000), desk.periodMs * 10000,
                                                           veyra::engine::pushPhaseLockOffset100ns(1.0, desk.periodMs * 10000));
            due = double(d.due100ns) / 10000.0;
            if (due > deadline + 1e-9) r.neverLater = false;
        }
        const double u = noise.next();
        const double call = due + (u < 0.03 ? 1.0 + 1.5 * noise.next() : 0.05 * noise.next());
        const auto at = desk.land(call, previous);
        toScreen += desk.vblank(at) - deadline;
        previous = at;
        shown.push_back(at);
    }
    const int k = veyra::engine::PushOnScreenCadence::expectedHold(interval, desk.periodMs);
    std::uint64_t even = 0;
    for (std::size_t i = 1; i < shown.size(); ++i) {
        const auto h = shown[i] - shown[i - 1];
        ++r.holds;
        if (h == k) ++even;
        if (h > k) ++r.longHolds;
    }
    r.evenPct = 100.0 * double(even) / double(r.holds);
    r.meanShownAfterDeadlineMs = toScreen / double(shown.size());
    return r;
}
}  // namespace onscreen_tests

TEST(onscreen_expected_hold_is_whole_refreshes_only) {
    using C = veyra::engine::PushOnScreenCadence;
    const double p = 1000.0 / 179.952;
    CHECK(C::expectedHold(1000.0 / 89.91, p) == 2);
    CHECK(C::expectedHold(1000.0 / 88.8, p) == 2);
    CHECK(C::expectedHold(1000.0 / 59.94, p) == 3);
    CHECK(C::expectedHold(1000.0 / 120.0, p) == 0);   // 1.5 refreshes: no even hold exists
    CHECK(C::expectedHold(-1, p) == 0);
    CHECK(C::expectedHold(11.1, 0) == 0);
}

TEST(onscreen_holds_come_from_consecutive_shown_presents) {
    veyra::engine::PushOnScreenCadence c;
    const double p = 1000.0 / 179.952;
    // Presents 1..8 every 11.11 ms; the statistics name each in turn with these first-shown vblanks.
    const std::uint32_t refresh[] = {100, 102, 104, 107, 108, 110, 112, 114};
    double t = 1000.0;
    for (std::uint32_t id = 1; id <= 8; ++id) {
        c.presented(id, id % 3 != 0, t, t - 1.0, p);
        c.statistics(id, refresh[id - 1], refresh[id - 1] + 1, t + 2.0);
        t += 11.11;
    }
    const auto s = c.take(t + 10000.0);
    CHECK(s.k == 2);
    CHECK(s.holds == 7);
    CHECK(s.histogram[2] == 5 && s.histogram[3] == 1 && s.histogram[1] == 1);
    CHECK(std::abs(s.evenPct - 100.0 * 5 / 7) < 1e-9);
    CHECK(s.longEvents == 1 && s.shortEvents == 1);
    CHECK(s.longestUnevenRun == 2);   // ids 3 (held 3) and 4 (held 1) in a row
    CHECK(std::abs(s.judderPct - 100.0 * 2 / 7 / 2) < 1e-9);
    CHECK(s.phase[std::size_t(1.0 / p * 8)] == 8);
}

TEST(onscreen_skipped_ids_are_unmeasured_not_guessed) {
    veyra::engine::PushOnScreenCadence c;
    const double p = 1000.0 / 179.952;
    double t = 1000.0;
    for (std::uint32_t id = 1; id <= 6; ++id) { c.presented(id, true, t, t - 1.0, p); t += 11.11; }
    c.statistics(1, 50, 51, 0);
    c.statistics(3, 54, 55, 0);   // 2 never named: neither 1 nor 2 gets a hold
    c.statistics(4, 56, 57, 0);   // 3 -> 4: one hold of 2
    c.statistics(4, 56, 58, 0);   // the same present again: nothing new
    c.statistics(6, 60, 61, 0);
    const auto s = c.take(t + 10000.0);
    CHECK(s.holds == 1 && s.histogram[2] == 1);
    CHECK(s.measuredPct > 16.0 && s.measuredPct < 17.0);
    CHECK(s.gaps == 2 && s.gapFrames == 4 && s.gapUneven == 0);   // 1..3 held 4 refreshes, 4..6 held 4: consistent with 2 each
    // A new swap chain: ids restart at 1; the old ring must not pair with them.
    c.presented(1, false, t, t - 1.0, p);
    c.statistics(1, 9000, 9001, 0);
    c.presented(2, false, t + 11.11, t + 10.0, p);
    c.statistics(2, 9002, 9003, 0);
    const auto again = c.take(t + 20000.0);
    CHECK(again.holds == 1 && again.histogram[2] == 1);
}

TEST(onscreen_frozen_refresh_clock_is_reported_as_direct) {
    veyra::engine::PushOnScreenCadence c;
    const double p = 1000.0 / 179.952;
    double t = 1000.0;
    for (std::uint32_t id = 1; id <= 60; ++id) {
        c.presented(id, false, t, 900.0, p);
        c.statistics(id, 500, 500, 900.0);   // his fullscreen-with-focus seconds: the vblank count stands still
        t += 11.11;
    }
    const auto s = c.take(t);
    CHECK(s.clockFrozen);
    CHECK(s.spacingEvenPct > 99.0);
    veyra::engine::PushCadenceContext ctx;
    CHECK(std::string(veyra::engine::pushCadencePath(s, ctx)) == "frozen");   // not "direct" without the driver's report
    CHECK(!s.holdsValid && s.holds == 0);
    const auto line = veyra::engine::formatPushCadence(s, ctx);
    CHECK(onscreen_tests::field(line, "refreshClock") == "frozen");
    CHECK(onscreen_tests::field(line, "path") == "frozen");
    CHECK(onscreen_tests::field(line, "holdHistogram") == "n/a");
    ctx.directPath = 1;
    CHECK(std::string(veyra::engine::pushCadencePath(s, ctx)) == "direct");
}

TEST(onscreen_line_fields_and_board) {
    veyra::engine::PushOnScreenCadence c;
    const double p = 1000.0 / 179.952;
    double t = 1000.0;
    std::uint32_t refresh = 1000;
    for (std::uint32_t id = 1; id <= 100; ++id) {
        c.presented(id, id % 3 != 0, t, t - 0.5, p);
        c.statistics(id, refresh, refresh + 1, t + 0.1);
        refresh += 2;
        t += 11.11;
    }
    c.capacityWait(1.2);
    c.presentLateness(0.4, 1, -1, 0);    // not late: not counted
    c.presentLateness(1.6, 2, 0.8, 0);   // GPU work first seen done after the deadline
    c.presentLateness(1.4, 2, -0.5, 1.2);   // ready in time, waited for capacity
    c.presentLateness(1.2, 0, -3.0, 0);   // neither
    c.phaseLockPresented(2.5);
    c.phaseLockPresented(0.3);   // not counted: under 0.5 ms
    c.phaseLockMissed();
    const auto s = c.take(t);
    CHECK(s.evenPct == 100.0 && s.k == 2 && !s.clockFrozen);
    veyra::engine::PushCadenceContext ctx;
    ctx.lock = 1;
    const auto line = veyra::engine::formatPushCadence(s, ctx);
    CHECK(onscreen_tests::field(line, "path") == "composed");
    CHECK(onscreen_tests::field(line, "evenPct") == "100.00");
    CHECK(onscreen_tests::field(line, "expectedRefreshes") == "2");
    CHECK(onscreen_tests::field(line, "holdHistogram") == "2:99");
    CHECK(onscreen_tests::field(line, "evenPctMin") == "100.00" && onscreen_tests::field(line, "evenPctMax") == "100.00");
    CHECK(onscreen_tests::field(line, "beatFloorPerS") != "-1");
    CHECK(onscreen_tests::field(line, "capacityWaits") == "1");
    CHECK(onscreen_tests::field(line, "lockMoves") == "1" && onscreen_tests::field(line, "lockMissed") == "1");
    CHECK(onscreen_tests::field(line, "lateOver1ms") == "3" && onscreen_tests::field(line, "lateReadySeenLate") == "1");
    CHECK(onscreen_tests::field(line, "lateCapacity") == "1" && onscreen_tests::field(line, "lateOther") == "1");
    CHECK(onscreen_tests::field(line, "lateBySubframe") == "real:1,g1:0,g2:2,g3+:0");
    CHECK(line.find("not photons") != std::string::npos);
    veyra::engine::PushOnScreenBoard b;
    CHECK(!b.read(1000.0).known);
    b.publish(0, s.k, s.evenPctMin, s.coveredPct, s.spacingEvenPct, false, 1000.0);
    auto r = b.read(2000.0);
    CHECK(r.known && r.path == 0 && r.expectedRefreshes == 2 && r.evenPctMin == 100.0 && !r.directSeen);
    CHECK(!b.read(40000.0).known);
    b.publish(0, 2, 80.0, 70.0, 95.0, false, 45000.0);
    CHECK(!b.read(45001.0).known);   // under 90 % of the frames covered: nothing shown
    b.publish(2, 0, -1, -1, 97.0, false, 50000.0);
    r = b.read(50001.0);
    CHECK(r.known && r.path == 2 && !r.directSeen);   // a still clock alone is not a driver report
    b.publish(1, 0, -1, -1, 97.0, true, 60000.0);
    r = b.read(60001.0);
    CHECK(r.known && r.path == 1 && r.directSeen);
    // The window restarted: nothing of the previous one is in the next.
    const auto empty = c.take(t + 10000.0);
    CHECK(empty.holds == 0 && empty.presents == 0 && empty.capacityWaits == 0);
}

TEST(phase_lock_refusals) {
    using namespace veyra::engine;
    PushPhaseLockInputs in;
    const double p = 1e7 / 179.952;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "off");
    in.switchOn = true;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "no-vblank-clock");
    in.period100ns = p;
    in.lastVblank100ns = 1000000;
    in.now100ns = 1000000 + kPushPhaseLockMaxGridAge100ns + 1;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "stale-vblank-clock");
    in.now100ns = 1100000;
    in.outputInterval100ns = 1e7 / 89.91;
    CHECK(pushPhaseLockRefusal(in) == nullptr);
    in.clockFrozen = true;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "refresh-clock-frozen");
    in.clockFrozen = false;
    in.directFlip = true;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "direct-flip");
    in.directFlip = false;
    in.vrrActive = true;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "vrr");
    in.vrrActive = false;
    in.outputInterval100ns = 1e7 / 120.0;
    CHECK(std::string(pushPhaseLockRefusal(in)) == "not-whole-refreshes");
    in.outputInterval100ns = 1e7 / 59.94;
    CHECK(pushPhaseLockRefusal(in) == nullptr);
}

TEST(phase_lock_never_later_and_lands_after_a_vblank) {
    using namespace veyra::engine;
    const double p = 1e7 / 179.952;
    const double offset = pushPhaseLockOffset100ns(1.0, p);
    CHECK(offset == 10000.0);
    CHECK(pushPhaseLockOffset100ns(0.0, p) == 2000.0);
    CHECK(pushPhaseLockOffset100ns(9.0, p) == p * 0.5);
    const std::int64_t vblank = 50000000;
    for (int i = 0; i < 2000; ++i) {
        const std::int64_t deadline = vblank + 30000 + std::int64_t(i) * 37;   // every phase over several refreshes
        const auto d = pushPhaseLockDue(deadline, deadline - 60000, vblank, p, offset);
        CHECK(d.due100ns <= deadline);
        CHECK(d.moved && !d.missed);
        CHECK(double(deadline - d.due100ns) < p + 1.0);
        double phase = std::fmod(double(d.due100ns - vblank), p);
        if (phase < 0) phase += p;
        CHECK(std::abs(phase - offset) < 1.0 || std::abs(phase - offset - p) < 1.0 || d.due100ns == deadline);
    }
    // Arriving after the slot plus the tolerance keeps today's deadline.
    const std::int64_t deadline = vblank + 40000;   // 4 ms after the vblank: slot at +1 ms
    auto d = pushPhaseLockDue(deadline, vblank + 10000 + 4000, vblank, p, offset);
    CHECK(d.moved && d.due100ns == vblank + 10000);
    d = pushPhaseLockDue(deadline, vblank + 10000 + 6000, vblank, p, offset);
    CHECK(!d.moved && d.missed && d.due100ns == deadline);
    d = pushPhaseLockDue(deadline, vblank, vblank, 0.0, offset);
    CHECK(!d.moved && d.due100ns == deadline);
}

// SIMULATED with his numbers: 179.952 Hz, content 29.97 Hz (session B) and 29.60 Hz (session A), 3X and 2X, Present jitter as
// logged. The lock must never present later, must raise the share of even holds clearly on the 29.97 Hz source, must not lower
// it on any, and must not lengthen the time to the screen.
TEST(phase_lock_simulation_his_desktop) {
    using onscreen_tests::simulate;
    const struct { double hz; unsigned m; } cases[] = {{29.97, 3}, {29.97, 2}, {30.0, 3}, {29.60, 3}, {29.60, 2}};
    for (const auto& c : cases) {
        const auto off = simulate(c.hz, c.m, false, 120.0);
        const auto on = simulate(c.hz, c.m, true, 120.0);
        CHECK(on.neverLater);
        CHECK(on.evenPct >= off.evenPct - 0.5);
        CHECK(on.meanShownAfterDeadlineMs <= off.meanShownAfterDeadlineMs + 0.05);
        if (std::getenv("ONSCREEN_SIM_PRINT")) std::printf("sim %.2f Hz %uX: even %.1f -> %.1f %%, shown after deadline %.2f -> %.2f ms, long holds/s %.2f -> %.2f\n", c.hz, c.m, off.evenPct, on.evenPct, off.meanShownAfterDeadlineMs, on.meanShownAfterDeadlineMs, double(off.longHolds) / 120.0, double(on.longHolds) / 120.0);
        if (c.hz > 29.9) CHECK(on.evenPct > off.evenPct + 5.0);
    }
    // 29.60 Hz at 3X: 2.35 holds of 3 refreshes per second remain whatever the phase (88.8 FPS cannot be even on 180 Hz).
    const auto slow = simulate(29.60, 3, true, 120.0);
    const double perSecond = double(slow.longHolds) / 120.0;
    CHECK(perSecond > 2.0);
}

TEST(frame_probe_distances_blend_and_duplicates) {
    using namespace veyra::engine;
    constexpr std::size_t n = PushFrameProbe::kCells;
    std::vector<float> a(n), b(n), mid(n), blend(n), copyA(n);
    // A bright bar moving 30 cells to the right across a dark field: true motion puts it halfway; a cross-fade shows both at half.
    const auto bar = [](std::vector<float>& img, int x0) {
        for (std::size_t i = 0; i < img.size(); ++i) {
            const int x = int(i % PushFrameProbe::kWidth);
            img[i] = (x >= x0 && x < x0 + 8) ? 0.8f : 0.1f;
        }
    };
    bar(a, 20);
    bar(b, 50);
    bar(mid, 35);
    for (std::size_t i = 0; i < n; ++i) { blend[i] = 0.5f * (a[i] + b[i]); copyA[i] = a[i]; }
    CHECK(PushFrameProbe::distance(a.data(), a.data()) == 0.0);
    const double dab = PushFrameProbe::distance(a.data(), b.data());
    CHECK(dab > 0);
    CHECK(PushFrameProbe::blendLikeness(blend.data(), a.data(), b.data(), 0.5) < 0.01);
    CHECK(PushFrameProbe::blendLikeness(mid.data(), a.data(), b.data(), 0.5) > 0.4);
    // A group A, I(=copy of A), B: a near-duplicate and uneven steps; A, mid, B: even.
    PushFrameProbe probe;
    const float* dup[] = {a.data(), copyA.data(), b.data()};
    probe.group(dup, 3);
    const float* good[] = {a.data(), mid.data(), b.data()};
    probe.group(good, 3);
    const float* still[] = {a.data(), a.data(), a.data()};
    probe.group(still, 3);   // no motion: not counted in the motion figures
    const auto s = probe.take();
    CHECK(s.groups == 3 && s.motionGroups == 2);
    CHECK(s.generated == 2 && s.nearDuplicates == 1);
    CHECK(s.evennessP50 >= 0 && s.evennessMin < 0.05);
    CHECK(s.blendLikeP50 >= 0);
    CHECK(probe.take().groups == 0);
}

TEST(frame_probe_groups_positions_and_gaps) {
    using namespace veyra::engine;
    constexpr std::size_t n = PushFrameProbe::kCells;
    const auto bar = [](int x0) {
        std::vector<float> img(n);
        for (std::size_t i = 0; i < n; ++i) { const int x = int(i % PushFrameProbe::kWidth); img[i] = (x >= x0 && x < x0 + 6) ? 0.9f : 0.1f; }
        return img;
    };
    PushFrameProbe probe;
    PushFrameGroups groups(probe);
    // Pan at 3X: real 20, gen 30 (good), gen = cross-fade of 40 and 50 (ghost), real 50; then real 50, gen copy of 50, gen 70, real 80.
    const auto a = bar(20), i1 = bar(30), b = bar(50), c = bar(80), copyB = bar(50), i2b = bar(70);
    std::vector<float> fade(n);
    const auto mid40 = bar(40);
    for (std::size_t i = 0; i < n; ++i) fade[i] = 0.5f * (mid40[i] + b[i]);
    std::uint64_t seq = 1;
    groups.frame(seq++, false, a.data());
    groups.frame(seq++, true, i1.data());
    groups.frame(seq++, true, fade.data());
    groups.frame(seq++, false, b.data());
    groups.frame(seq++, true, copyB.data());
    groups.frame(seq++, true, i2b.data());
    groups.frame(seq++, false, c.data());
    // A gap (frame 9 not probed): the group in progress breaks and does not count.
    groups.frame(seq++, true, i1.data());
    ++seq;
    groups.frame(seq++, false, b.data());
    const auto s = probe.take();
    CHECK(s.groups == 2 && s.motionGroups == 2 && s.multiplier == 3);
    CHECK(s.generated == 4 && s.nearDuplicates == 1);
    CHECK(s.positions[0].generated == 2 && s.positions[0].nearDuplicates == 1);   // the copy is in position 1 of the second group
    CHECK(s.positions[1].generated == 2 && s.positions[1].nearDuplicates == 0);
    CHECK(s.skipped == 1 && s.brokenGroups == 1);
    CHECK(s.stepShareP50[0] >= 0 && s.stepShareP50[2] >= 0);
    const auto line = formatPushFrameProbe(s);
    CHECK(line.find("byPosition=1:n=2,dupPct=50.0") != std::string::npos);
    CHECK(line.find("skippedFrames=1 brokenGroups=1") != std::string::npos);
}

TEST(frame_probe_cpu_thumbnail_taps) {
    using namespace veyra::engine;
    // A picture whose value is its column: each cell's mean is the mean of its 4 tap columns.
    std::vector<float> out(PushFrameProbe::kCells);
    pushFrameThumbCpu(3840, 2160, [](unsigned x, unsigned) { return float(x) / 3840.0f; }, out.data());
    CHECK(std::abs(out[0] - float(14.5 / 3840.0)) < 1e-6f);   // taps at columns 3, 11, 18, 26
    CHECK(pushThumbTapX(0, 3840) == 3 && pushThumbTapX(511, 3840) == 3836);
    CHECK(pushThumbTapY(0, 2160) == 3 && pushThumbTapY(287, 2160) == 2156);
    CHECK(out[PushFrameProbe::kCells - 1] > out[0]);
    CHECK(pushThumbLuma(2.0f, 2.0f, 2.0f, true) > 0.66f && pushThumbLuma(2.0f, 2.0f, 2.0f, false) == 1.0f);
}

// Review 1 MUST 1: one reading per Present names the last shown present only; short holds fall between readings. A known hold
// sequence and a random one, at 2X (3 refreshes) and 3X (2 refreshes), with the statistics reporting 0, 1 or 2 refreshes late:
// the true even share always lies in evenPctMin..evenPctMax, whatever the multiplier or the delay.
namespace onscreen_tests {
struct Truth { double evenPct = 0; double minPct = 0, maxPct = 0, biasedPct = 0, coveredPct = 0; };
inline Truth sampledHolds(int k, int delay, const std::vector<int>& holds) {
    veyra::engine::PushOnScreenCadence c;
    const double period = 1000.0 / 179.952;
    // shown[i]: the vblank frame i first appeared at; frames are presented every k refreshes, each before its vblank.
    std::vector<std::int64_t> shown(holds.size() + 1);
    shown[0] = 12;   // every frame appears after its own Present (called near vblank 10 + i k)
    for (std::size_t i = 0; i < holds.size(); ++i) shown[i + 1] = shown[i] + holds[i];
    std::size_t named = 0;
    std::uint64_t even = 0;
    for (std::size_t i = 0; i + 1 < shown.size(); ++i) {
        const double vblankNow = double(shown[0]) + double(i) * double(k) - 0.4;   // Present i is called k refreshes after Present i-1
        const auto readAt = std::int64_t(std::floor(vblankNow)) - delay;            // the statistics know vblanks up to this one
        while (named + 1 < i && shown[named + 1] <= readAt) ++named;                // the last shown present at the reading
        if (i > 0 && shown[named] <= readAt) c.statistics(std::uint32_t(named + 1), std::uint32_t(shown[named]), std::uint32_t(readAt), vblankNow * period);
        c.presented(std::uint32_t(i + 1), i % std::size_t(k == 2 ? 3 : 2) != 0, vblankNow * period, std::floor(vblankNow) * period, period);
        if (i + 1 < holds.size() && holds[i] == k) ++even;
    }
    const auto s = c.take(double(shown.back() + 50) * period);
    Truth t;
    t.evenPct = 100.0 * double(even) / double(holds.size() - 1);
    t.minPct = s.evenPctMin;
    t.maxPct = s.evenPctMax;
    t.biasedPct = s.evenPct;
    t.coveredPct = s.coveredPct;
    return t;
}
}  // namespace onscreen_tests

TEST(onscreen_even_share_range_contains_the_truth_at_2x_and_3x_and_any_report_delay) {
    using onscreen_tests::sampledHolds;
    // The reviewer's case: 1,3,2,2 repeated at 3X (2 refreshes), read every 2 refreshes.
    std::vector<int> known;
    for (int r = 0; r < 200; ++r) for (int h : {1, 3, 2, 2}) known.push_back(h);
    for (int delay = 0; delay <= 2; ++delay) {
        const auto t = sampledHolds(2, delay, known);
        if (std::getenv("ONSCREEN_SIM_PRINT"))
            std::printf("known 1,3,2,2 k=2 delay=%d: true %.1f %%, range %.1f..%.1f %%, own-hold only %.1f %%, covered %.1f %%\n", delay, t.evenPct, t.minPct, t.maxPct, t.biasedPct, t.coveredPct);
        CHECK(t.minPct <= t.evenPct + 1.0 && t.evenPct <= t.maxPct + 1.0);
        CHECK(t.coveredPct > 95.0);
    }
    onscreen_tests::Lcg rng{777};
    for (int k : {2, 3})
        for (int delay = 0; delay <= 2; ++delay) {
            // Uneven pairs (k-1, k+1) at random, 15 % of frames, as a composed desktop makes them.
            std::vector<int> holds;
            while (holds.size() < 3000) {
                if (rng.next() < 0.15) { holds.push_back(k - 1); holds.push_back(k + 1); }
                else holds.push_back(k);
            }
            const auto t = sampledHolds(k, delay, holds);
            if (std::getenv("ONSCREEN_SIM_PRINT"))
                std::printf("sampling k=%d delay=%d: true %.1f %%, range %.1f..%.1f %%, own-hold only %.1f %%, covered %.1f %%\n", k, delay, t.evenPct, t.minPct, t.maxPct, t.biasedPct, t.coveredPct);
            CHECK(t.minPct <= t.evenPct + 1.0 && t.evenPct <= t.maxPct + 1.0);
            CHECK(t.coveredPct > 95.0);
        }
}

// Stream "s12-pace": PresentBlit's area filter, CPU reference (engine/PushDownscale.h). OFFLINE CHECK on synthetic pictures at
// his sizes' ratio (3840 -> 2560 wide, 1.5x): a one-pixel bright line panned by a quarter pixel per frame (the brightness the
// screen shows for it should not pulse), and a fine grating beyond the output's Nyquist limit (should become flat grey, not a
// moire pattern). Not the GPU: the shader is compiled and run only by the Windows build.
namespace downscale_tests {
using veyra::engine::PushPlane;
inline PushPlane line(unsigned w, unsigned h, double x0, double level, double background) {
    PushPlane p{w, h, std::vector<double>(std::size_t(w) * h, background)};
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const double cover = std::clamp(std::min(double(x) + 1, x0 + 1) - std::max(double(x), x0), 0.0, 1.0);   // a 1 px wide box at x0
            p.v[std::size_t(y) * w + x] = background + (level - background) * cover;
        }
    return p;
}
// Peak and total output brightness of the line over 16 quarter-pixel steps: their relative spread (pulsing while it moves).
inline void pulse(bool area, bool pq, double& peakSpread, double& sumSpread) {
    constexpr unsigned W = 96, H = 4, OW = 64, OH = 4;
    std::vector<double> peaks, sums;
    for (int step = 0; step < 16; ++step) {
        const auto src = line(W, H, 40.0 + 0.25 * step, pq ? 0.75 : 1.0, pq ? 0.1 : 0.0);
        double peak = 0, sum = 0;
        for (unsigned x = 0; x < OW; ++x) {
            const double v = veyra::engine::pushBlitSample(src, OW, OH, x, 1, area, pq);
            const double light = pq ? veyra::engine::pushPqToLinear(v) - veyra::engine::pushPqToLinear(0.1) : v;
            peak = std::max(peak, light);
            sum += light;
        }
        peaks.push_back(peak);
        sums.push_back(sum);
    }
    const auto spread = [](const std::vector<double>& a) {
        const auto [lo, hi] = std::minmax_element(a.begin(), a.end());
        double mean = 0;
        for (double x : a) mean += x;
        mean /= double(a.size());
        return mean > 0 ? (*hi - *lo) / mean : 0.0;
    };
    peakSpread = spread(peaks);
    sumSpread = spread(sums);
}
}  // namespace downscale_tests

TEST(downscale_area_filter_rules) {
    using namespace veyra::engine;
    CHECK(!pushAreaApplies(true, 1.0) && !pushAreaApplies(true, 0.5) && !pushAreaApplies(false, 1.5) && pushAreaApplies(true, 1.5));
    CHECK(pushAreaTaps(1.5) == 3 && pushAreaTaps(2.63) == 4 && pushAreaTaps(9.0) == 5 && pushAreaTaps(1.01) == 3);
    for (double v : {0.0, 0.1, 0.5, 0.75, 1.0}) CHECK(std::abs(pushLinearToPq(pushPqToLinear(v)) - v) < 1e-6);
    // 1:1 and magnification: the area path never runs, so the result is today's bilinear tap exactly.
    const auto src = downscale_tests::line(64, 4, 20.3, 1.0, 0.0);
    for (unsigned x = 0; x < 64; ++x) CHECK(pushBlitSample(src, 64, 4, x, 1, true, false) == pushBlitSample(src, 64, 4, x, 1, false, false));
    for (unsigned x = 0; x < 128; ++x) CHECK(pushBlitSample(src, 128, 8, x, 3, true, true) == pushBlitSample(src, 128, 8, x, 3, false, true));
}

TEST(downscale_area_filter_reduces_pulsing_and_moire) {
    double bilinearPeak = 0, bilinearSum = 0, areaPeak = 0, areaSum = 0, pqBilinearPeak = 0, pqBilinearSum = 0, pqAreaPeak = 0, pqAreaSum = 0;
    downscale_tests::pulse(false, false, bilinearPeak, bilinearSum);
    downscale_tests::pulse(true, false, areaPeak, areaSum);
    downscale_tests::pulse(false, true, pqBilinearPeak, pqBilinearSum);
    downscale_tests::pulse(true, true, pqAreaPeak, pqAreaSum);
    // A grating of period 1.25 source pixels (0.83 output pixels: beyond the output's Nyquist limit): RMS left over around the mean.
    using veyra::engine::PushPlane;
    PushPlane grating{120, 4, std::vector<double>(480)};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 120; ++x) grating.v[std::size_t(y) * 120 + x] = 0.5 + 0.5 * std::cos(2 * 3.14159265358979 * x / 1.25);
    const auto rms = [&](bool area) {
        double sum = 0, sq = 0;
        for (unsigned x = 4; x < 76; ++x) { const double v = veyra::engine::pushBlitSample(grating, 80, 4, x, 1, area, false); sum += v; sq += v * v; }
        const double n = 72, mean = sum / n;
        return std::sqrt(std::max(0.0, sq / n - mean * mean));
    };
    const double moireBilinear = rms(false), moireArea = rms(true);
    if (std::getenv("ONSCREEN_SIM_PRINT"))
        std::printf("downscale 1.5x: line peak pulse %.3f -> %.3f, line energy pulse %.3f -> %.3f; HDR10 (PQ) peak pulse %.3f -> %.3f, energy pulse %.3f -> %.3f; grating moire RMS %.3f -> %.3f\n",
                    bilinearPeak, areaPeak, bilinearSum, areaSum, pqBilinearPeak, pqAreaPeak, pqBilinearSum, pqAreaSum, moireBilinear, moireArea);
    // Contrast of a coarse grating (period 6 source = 4 output pixels): the price in sharpness.
    PushPlane coarse{120, 4, std::vector<double>(480)};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 120; ++x) coarse.v[std::size_t(y) * 120 + x] = 0.5 + 0.5 * std::cos(2 * 3.14159265358979 * x / 6.0);
    const auto contrast = [&](bool area) {
        double hi = 0, lo = 1;
        for (unsigned x = 4; x < 76; ++x) { const double v = veyra::engine::pushBlitSample(coarse, 80, 4, x, 1, area, false); hi = std::max(hi, v); lo = std::min(lo, v); }
        return hi - lo;
    };
    if (std::getenv("ONSCREEN_SIM_PRINT")) std::printf("downscale 1.5x: contrast at a 4-output-pixel period %.3f -> %.3f\n", contrast(false), contrast(true));
    CHECK(areaPeak < 0.6 * bilinearPeak);           // the line's brightest pixel pulses less while it moves
    CHECK(areaSum < 0.05 * bilinearSum + 1e-6);     // its total brightness on screen stays the same at every sub-pixel position
    CHECK(pqAreaSum < 0.5 * pqBilinearSum);         // HDR10: the same in linear light
    CHECK(moireArea <= moireBilinear);              // a box is a weak stop band: moire beyond Nyquist only somewhat lower (honest)
    CHECK(contrast(true) > 0.85 * contrast(false)); // at most 15 % less contrast for detail the screen can show
}

// Stream "s12-pace": the 2X + duplicate removal defect (engine/PushAdmissionAnchor.h), reproduced with the pure learner
// (LivePairLatency), the pure timeline (PresentationScheduler) and his log's numbers: content anchor = callback + T/2 - lead
// (callbackMinusAnchorMs -16.2), readiness after the callback: real frame 4.5 ms, generated frames 7.0 / 9.0 ms (fg1, fg2 P50
// in his log), admission elapsed 0.3 ms. Admission is modelled as "the last generated frame's deadline is still ahead" (his
// log: every pair with remainingDeadlineMs < 0 was rejected, every pair at 1.9 ms or more was admitted). SIMULATED.
#include "veyra/engine/LivePairLatency.h"
#include "veyra/engine/PushAdmissionAnchor.h"
namespace admission_tests {
struct Result { unsigned admitted = 0, rejected = 0; double lastPhaseMs = 0, lastRemainingMs = 0; std::int64_t lastPresentationDeadline = 0; };
inline Result run(unsigned multiplier, bool legacy) {
    using namespace veyra::engine;
    const std::int64_t T = 333667, lead = 5000;   // 29.97 Hz content, LiveArrivalSmoother::kLead100ns
    LivePairLatency learner;
    PresentationScheduler admission, presentation;
    Result r;
    std::int64_t callback = 100000000;
    for (int pair = 0; pair < 300; ++pair, callback += T) {
        const std::int64_t b = std::int64_t(pair) * T, anchor = callback + T / 2 - lead;
        const std::int64_t legacyDelay = T + 10000;   // liveInterval + a processing allowance
        const std::int64_t phase = learner.select(callback, legacyDelay, T, multiplier, T / 2);
        const std::int64_t lastGenerated = b - T / multiplier;   // the last generated frame of the group, at (M-1)/M
        admission.resetPair(1, b, pushAdmissionAnchor(true, legacy, callback, anchor), phase);
        presentation.resetPair(1, b, anchor, phase);
        const std::int64_t remaining = admission.deadline(lastGenerated) - (callback + 3000);
        r.lastPhaseMs = double(phase) / 10000;
        r.lastRemainingMs = double(remaining) / 10000;
        r.lastPresentationDeadline = presentation.deadline(lastGenerated) - callback;
        if (remaining < 0) { ++r.rejected; continue; }   // a rejected pair teaches nothing (no generated frames)
        ++r.admitted;
        std::int64_t required = 0;
        for (unsigned i = 1; i <= multiplier; ++i) {
            const std::int64_t pts = b - T + std::int64_t(i) * T / multiplier;   // i = multiplier: the real frame B
            const std::int64_t ready = callback + (i == multiplier ? 45000 : (i == 1 ? 70000 : 90000));
            required = std::max(required, ready - anchor + b - pts);
        }
        learner.observe(callback, required);
    }
    return r;
}
}  // namespace admission_tests

TEST(admission_2x_dedup_rejects_with_the_callback_anchor_and_not_with_the_content_anchor) {
    const auto legacy2 = admission_tests::run(2, true), legacy3 = admission_tests::run(3, true);
    const auto fixed2 = admission_tests::run(2, false), fixed3 = admission_tests::run(3, false);
    if (std::getenv("ONSCREEN_SIM_PRINT"))
        std::printf("admission 2X legacy: admitted %u rejected %u (phase %.1f ms, remaining %.1f ms); 3X legacy: %u / %u (phase %.1f, remaining %.1f); "
                    "2X fixed: %u / %u (remaining %.1f); 3X fixed: %u / %u (remaining %.1f)\n",
                    legacy2.admitted, legacy2.rejected, legacy2.lastPhaseMs, legacy2.lastRemainingMs, legacy3.admitted, legacy3.rejected, legacy3.lastPhaseMs,
                    legacy3.lastRemainingMs, fixed2.admitted, fixed2.rejected, fixed2.lastRemainingMs, fixed3.admitted, fixed3.rejected, fixed3.lastRemainingMs);
    CHECK(legacy2.rejected > 50);    // the defect: 2X with a content anchor rejects pairs again and again
    CHECK(legacy3.rejected == 0);    // 3X with the same clock: stable, as in his log
    CHECK(fixed2.rejected == 0 && fixed3.rejected == 0);
    // The presentation deadlines do not depend on the admission anchor: the fix changes only which pairs are judged late.
    CHECK(fixed3.lastPresentationDeadline == legacy3.lastPresentationDeadline);
    using veyra::engine::pushAdmissionAnchor;
    CHECK(pushAdmissionAnchor(false, false, 10, 99) == 10);   // duplicate removal off (transport anchor): unchanged
    CHECK(pushAdmissionAnchor(true, true, 10, 99) == 10);     // restored
    CHECK(pushAdmissionAnchor(true, false, 10, 99) == 99);
}
