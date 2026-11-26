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
    CHECK(onscreen_tests::field(line, "earlyPresents") == "1" && onscreen_tests::field(line, "lockMissed") == "1");
    CHECK(onscreen_tests::field(line, "lateOver1ms") == "3" && onscreen_tests::field(line, "lateReadySeenLate") == "1");
    CHECK(onscreen_tests::field(line, "lateCapacity") == "1" && onscreen_tests::field(line, "lateOther") == "1");
    CHECK(onscreen_tests::field(line, "lateBySubframe") == "real:1,g1:0,g2:2,g3+:0");
    CHECK(line.find("not photons") != std::string::npos);
    veyra::engine::PushOnScreenBoard b;
    CHECK(!b.read(1000.0).known);
    b.publish(0, s.k, s.evenPctMin, s.evenPctMax, s.spacingEvenPct, false, 1000.0);
    auto r = b.read(2000.0);
    CHECK(r.known && r.narrow && r.path == 0 && r.expectedRefreshes == 2 && r.evenPctMin == 100.0 && r.evenPctMax == 100.0 && !r.directSeen);
    CHECK(!b.read(40000.0).known);
    b.publish(0, 2, 47.0, 90.0, 95.0, false, 45000.0);
    r = b.read(45001.0);
    CHECK(r.known && !r.narrow);   // a 43-point range: the line shows nothing, the reminder uses the upper bound
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
    groups.frame(seq++, false, 0, 3, a.data());
    groups.frame(seq++, true, 1, 3, i1.data());
    groups.frame(seq++, true, 2, 3, fade.data());
    groups.frame(seq++, false, 0, 3, b.data());
    groups.frame(seq++, true, 1, 3, copyB.data());
    groups.frame(seq++, true, 2, 3, i2b.data());
    groups.frame(seq++, false, 0, 3, c.data());
    // A gap (frame 9 not probed): the group in progress breaks and does not count.
    groups.frame(seq++, true, 1, 3, i1.data());
    ++seq;
    groups.frame(seq++, false, 0, 3, b.data());
    // Review 2 SHOULD 4: a dropped generated frame (subframe 1 missing) breaks the group; it is never judged as 2X.
    groups.frame(seq++, true, 2, 3, i2b.data());
    groups.frame(seq++, false, 0, 3, c.data());
    const auto s = probe.take();
    CHECK(s.groups == 2 && s.motionGroups == 2 && s.multiplier == 3);
    CHECK(s.largeMotionGroups == 2);
    CHECK(s.generated == 4 && s.nearDuplicates == 1);
    CHECK(s.positions[0].generated == 2 && s.positions[0].nearDuplicates == 1);   // the copy is in position 1 of the second group
    CHECK(s.positions[1].generated == 2 && s.positions[1].nearDuplicates == 0);
    CHECK(s.skipped == 1 && s.brokenGroups == 2);
    CHECK(s.stepShareP50[0] >= 0 && s.stepShareP50[2] >= 0);
    const auto line = formatPushFrameProbe(s);
    CHECK(line.find("byPosition=1:n=2,dupPct=50.0") != std::string::npos);
    CHECK(line.find("skippedFrames=1 brokenGroups=2") != std::string::npos);
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
// readings: statistics readings per Present interval (1 = Package 16: before each Present; k = one per refresh, review 2);
// jitter: the reading's phase varies at random by up to this many refreshes (review 2: the old test read at a fixed phase).
inline Truth sampledHolds(int k, int delay, const std::vector<int>& holds, int readings = 1, double jitter = 0.0, std::uint64_t seed = 99) {
    veyra::engine::PushOnScreenCadence c;
    Lcg rng{seed};
    const double period = 1000.0 / 179.952;
    std::vector<std::int64_t> shown(holds.size() + 1);
    shown[0] = 12;   // every frame appears after its own Present (called near vblank 10 + i k)
    for (std::size_t i = 0; i < holds.size(); ++i) shown[i + 1] = shown[i] + holds[i];
    std::size_t named = 0;
    std::uint64_t even = 0;
    const auto read = [&](double at, std::size_t presented) {
        const auto readAt = std::int64_t(std::floor(at)) - delay;   // the statistics know vblanks up to this one
        while (named + 1 < presented && shown[named + 1] <= readAt) ++named;
        if (presented > 0 && shown[named] <= readAt) c.statistics(std::uint32_t(named + 1), std::uint32_t(shown[named]), std::uint32_t(readAt), at * period);
    };
    for (std::size_t i = 0; i + 1 < shown.size(); ++i) {
        const double vblankNow = double(shown[0]) - 2.0 + double(i) * double(k) - 0.4;   // Present i, k refreshes after Present i-1
        read(vblankNow - jitter * rng.next(), i);                                       // the reading taken before Present i
        c.presented(std::uint32_t(i + 1), i % std::size_t(k == 2 ? 3 : 2) != 0, vblankNow * period, std::floor(vblankNow) * period, period);
        for (int r = 1; r < readings; ++r) read(vblankNow + double(r) * double(k) / double(readings) - jitter * rng.next(), i + 1);   // extra readings
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

// ITEM 1, the UNBIASED measurement. The same display-side jitter (one shared sequence of delays, 0 or 1 refresh after the ideal
// vblank, drawn once) is applied to a 2X timeline (3 refreshes per frame at 179.952 Hz: 60 FPS) and a 3X timeline (2 refreshes:
// 90 FPS); statistics read once per refresh, as the presenter does. The scores in milliseconds (showJitterMs, holdSdMs, judderMs) must
// agree, and the old judderPct (relative to the frame length) is shown to differ by exactly k3/k2 = 1.5: a bias by construction, now
// documented. A delay of two refreshes is left out of the equality: at 3X a frame cannot be shown before its predecessor, so two
// refreshes of delay are squeezed (a property of the multiplier, not of the estimator). SIMULATED: a model, not a Windows trace.
namespace onscreen_tests {
inline veyra::engine::PushOnScreenCadence::Summary unbiasedTimeline(int k, const std::vector<int>& delays, std::size_t frames) {
    veyra::engine::PushOnScreenCadence c;
    const double period = 1000.0 / 179.952;
    std::vector<std::int64_t> shown(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const std::int64_t ideal = 1000 + std::int64_t(n) * k + delays[n];
        shown[n] = n ? std::max(shown[n - 1] + 1, ideal) : ideal;
    }
    std::size_t nextCall = 0, named = 0;
    const std::int64_t end = shown.back() + 4;
    for (std::int64_t v = 1000 - 1; v < end; ++v) {
        while (nextCall < frames && std::int64_t(1000) + std::int64_t(nextCall) * k - 1 == v) {   // the Present call, 0.3 refresh after vblank v
            c.presented(std::uint32_t(nextCall + 1), nextCall % std::size_t(k == 2 ? 3 : 2) != 0, (double(v) + 0.3) * period, double(v) * period, period);
            ++nextCall;
        }
        while (named < frames && shown[named] <= v) ++named;   // present ids 1 .. named are on screen by vblank v
        if (named > 0 && shown[named - 1] <= v) c.statistics(std::uint32_t(named), std::uint32_t(shown[named - 1]), std::uint32_t(v), (double(v) + 0.9) * period);
    }
    return c.take((double(end) + 1) * period);
}
}  // namespace onscreen_tests

TEST(onscreen_unbiased_scores_are_identical_at_2x_and_3x_for_identical_jitter) {
    using namespace onscreen_tests;
    Lcg rng{4242};
    std::vector<int> delays(3000);
    for (auto& d : delays) d = rng.next() < 0.3 ? 1 : 0;   // 30 % of the frames are shown one refresh late
    const auto s3 = unbiasedTimeline(2, delays, delays.size());   // 3X: 2 refreshes per frame
    const auto s2 = unbiasedTimeline(3, delays, delays.size());   // 2X: 3 refreshes per frame
    if (std::getenv("ONSCREEN_SIM_PRINT"))
        std::printf("unbiased 3X: k=%d holds=%llu covered %.1f %% even %.2f..%.2f %% judderMs %.4f holdSdMs %.4f showJitterMs %.4f judderPct %.2f | 2X: k=%d holds=%llu covered %.1f %% even %.2f..%.2f %% judderMs %.4f holdSdMs %.4f showJitterMs %.4f judderPct %.2f\n",
                    s3.k, static_cast<unsigned long long>(s3.holds), s3.coveredPct, s3.evenPctMin, s3.evenPctMax, s3.judderMs, s3.holdSdMs, s3.showJitterMs, s3.judderPct,
                    s2.k, static_cast<unsigned long long>(s2.holds), s2.coveredPct, s2.evenPctMin, s2.evenPctMax, s2.judderMs, s2.holdSdMs, s2.showJitterMs, s2.judderPct);
    CHECK(s3.k == 2 && s2.k == 3);
    CHECK(s3.coveredPct > 99.0 && s2.coveredPct > 99.0 && s3.holds == s2.holds);
    CHECK(s3.judderMs > 0.5 && s3.holdSdMs > 0.5 && s3.showJitterMs > 0.5);   // the jitter is really measured
    CHECK(std::abs(s3.judderMs - s2.judderMs) < 1e-6);
    CHECK(std::abs(s3.holdSdMs - s2.holdSdMs) < 1e-6);
    CHECK(std::abs(s3.showJitterMs - s2.showJitterMs) < 0.03 * s2.showJitterMs);   // the drift-removing line fit sees a different spread of times
    CHECK(std::abs(s3.evenPctMax - s2.evenPctMax) < 1.0 && std::abs(s3.evenPctMin - s2.evenPctMin) < 1.0);
    // The old figure is relative to the frame's length: 1.5 times larger at 3X for the very same jitter (what "unbiased" removes).
    CHECK(std::abs(s3.judderPct / s2.judderPct - 1.5) < 0.01);
}

TEST(onscreen_unbiased_scores_do_not_depend_on_the_window_length_or_which_frames_are_real) {
    using namespace onscreen_tests;
    Lcg rng{9001};
    std::vector<int> delays(3000);
    for (auto& d : delays) d = rng.next() < 0.3 ? 1 : 0;
    // The same refresh grid, 10 s of 3X (900 frames at 90 FPS) against 10 s of 2X (600 frames): prefixes of one jitter sequence.
    const auto long3 = unbiasedTimeline(2, delays, 900), long2 = unbiasedTimeline(3, delays, 600);
    const auto whole3 = unbiasedTimeline(2, delays, 3000), whole2 = unbiasedTimeline(3, delays, 3000);
    // Sampling error only: within 15 % of the long-window value at either multiplier, and 2X against 3X within 15 % of each other.
    const auto close = [](double a, double b) { return std::abs(a - b) <= 0.15 * std::max(a, b); };
    CHECK(close(long3.judderMs, whole3.judderMs) && close(long2.judderMs, whole2.judderMs) && close(long3.judderMs, long2.judderMs));
    CHECK(close(long3.holdSdMs, whole3.holdSdMs) && close(long2.holdSdMs, whole2.holdSdMs) && close(long3.holdSdMs, long2.holdSdMs));
    CHECK(close(long3.showJitterMs, whole3.showJitterMs) && close(long2.showJitterMs, whole2.showJitterMs) && close(long3.showJitterMs, long2.showJitterMs));
    // No jitter: both read exactly zero (a perfect schedule scores perfectly at either multiplier).
    const std::vector<int> none(1000, 0);
    for (int k : {2, 3}) {
        const auto s = unbiasedTimeline(k, none, none.size());
        CHECK(s.judderMs == 0.0 && s.holdSdMs < 1e-9 && s.showJitterMs < 1e-6);
    }
    // The line carries the new fields.
    const auto line = veyra::engine::formatPushCadence(whole3, veyra::engine::PushCadenceContext{});
    CHECK(field(line, "judderMs") != "-1" && field(line, "holdSdMs") != "-1" && field(line, "showJitterMs") != "-1");
}

// Stream "s12-pace": the present blit's downscale (engine/PushDownscale.h, CPU reference of shaders/PresentBlit.hlsl PushShrink).
// OFFLINE NUMBERS on synthetic pictures at his ratios: 3840 -> 2560 (f = 1.5, full screen) and 3840 -> 1462 (f = 2.63, his
// window), HDR10 (PQ codes) and SDR (sRGB codes). Not the GPU: the shader itself is compared with this in PushGpuTests
// (case "present-shrink", WARP). Measures, all in linear light (what the screen emits):
//   stroke   a 1-px 4K line of 300 nits on 1 nit (HDR10) / 100 % on 1 % (SDR) at every 4K column of a period of the ratio:
//            the brightest screen pixel above the background, mean and worst over the columns, relative to the stroke's
//            level (1.0 = full level; at f = 1.5 an exact filter can keep at most 1/1.5 = 0.67 of a 1-px line's level).
//   pulse    the line's total light on screen over the same columns and quarter-pixel steps: (max - min) / mean (0 = steady).
//   contrast a linear-light sine of period 4 / 3 / 2.5 screen pixels: the amplitude of that sine on screen (least-squares fit at
//            its frequency, so aliasing is not counted as detail), relative to the source's, mean over 8 phases.
//   overshoot a step 1 -> 300 nits (SDR 1 -> 100 %): the largest excursion past either level, % of the step.
//   moire    a sine of period 1.25 source px (beyond the screen's Nyquist limit): RMS left, relative to its amplitude.
namespace downscale_tests {
using namespace veyra::engine;
struct Case { const char* name; double f; PushTransfer t; };
inline double hi(PushTransfer t) { return t == PushTransfer::Pq ? 300.0 / 10000.0 : 1.0; }
inline double lo(PushTransfer t) { return t == PushTransfer::Pq ? 1.0 / 10000.0 : 0.01; }
// A row of codes for a linear-light profile sampled as exact box averages per 4K texel.
template <class F> std::vector<double> row(unsigned n, PushTransfer t, F light) {
    std::vector<double> r(n);
    for (unsigned i = 0; i < n; ++i) r[i] = pushEncode(light(i), t);
    return r;
}
inline unsigned outWidth(double f, unsigned n) { return unsigned(std::lround(n / f)); }
struct Numbers { double strokeMean = 0, strokeWorst = 1e9, pulse = 0, contrast4 = 0, contrast3 = 0, contrast25 = 0, overshoot = 0, moire = 0; };
inline Numbers measure(PushShrinkKernel k, const Case& c) {
    Numbers r;
    const unsigned n = c.f < 2 ? 3840 / 8 : 3840 / 4;   // a slice of the 4K row keeps the ratio exact: 480 -> 320, 960 -> 365 (2.63)
    const unsigned w = outWidth(c.f, n);
    const double H = hi(c.t), L = lo(c.t);
    // stroke and pulse: columns 200 .. 200 + 29 (a whole number of periods of both ratios' phase patterns), plus quarter steps
    std::vector<double> energies;
    int columns = 0;
    for (int q = 0; q < 30 * 4; ++q) {
        const double x0 = 200 + q * 0.25;
        const auto codes = row(n, c.t, [&](unsigned i) { const double cover = std::clamp(std::min(double(i) + 1, x0 + 1) - std::max(double(i), x0), 0.0, 1.0); return L + (H - L) * cover; });
        const auto out = pushShrinkRow(codes, w, k, c.t);
        double peak = 0, energy = 0;
        for (double v : out) { const double l = pushDecode(v, c.t) - L; peak = std::max(peak, l); energy += l; }
        energies.push_back(energy);
        if (q % 4 == 0) { r.strokeMean += peak / (H - L); r.strokeWorst = std::min(r.strokeWorst, peak / (H - L)); ++columns; }
    }
    r.strokeMean /= columns;
    double emin = 1e9, emax = 0, esum = 0;
    for (double e : energies) { emin = std::min(emin, e); emax = std::max(emax, e); esum += e; }
    r.pulse = (emax - emin) / (esum / double(energies.size()));
    const double pi = 3.14159265358979323846;
    const auto contrast = [&](double periodOut) {
        const double periodSrc = periodOut * double(n) / w;
        double sum = 0;
        for (int ph = 0; ph < 8; ++ph) {
            const auto codes = row(n, c.t, [&](unsigned i) {   // exact box average of the sine over the texel
                const double a = 2 * pi / periodSrc, x = i + ph * periodSrc / 8;
                return (L + H) / 2 + (H - L) / 2 * (std::sin(a * (x + 1)) - std::sin(a * x)) / a;
            });
            const auto out = pushShrinkRow(codes, w, k, c.t);
            // the amplitude of the sine itself on screen (least squares at its frequency: what the screen shows of the detail;
            // max - min would also count the aliased beat Package 16 adds, which is not detail)
            double scc = 0, sss = 0, scs = 0, sc = 0, ss = 0, sy = 0, syc = 0, sys = 0, m = 0;
            for (unsigned o = 20; o + 20 < w; ++o) {
                const double l = pushDecode(out[o], c.t), t = 2 * pi * (o + 0.5) / periodOut, cs = std::cos(t), sn = std::sin(t);
                scc += cs * cs; sss += sn * sn; scs += cs * sn; sc += cs; ss += sn; sy += l; syc += l * cs; sys += l * sn; m += 1;
            }
            // solve [scc scs sc; scs sss ss; sc ss m] [a b d] = [syc sys sy]
            const double A[3][4] = {{scc, scs, sc, syc}, {scs, sss, ss, sys}, {sc, ss, m, sy}};
            double M[3][4];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) M[i][j] = A[i][j];
            for (int i = 0; i < 3; ++i)
                for (int r2 = 0; r2 < 3; ++r2) {
                    if (r2 == i) continue;
                    const double g = M[r2][i] / M[i][i];
                    for (int j = 0; j < 4; ++j) M[r2][j] -= g * M[i][j];
                }
            const double a = M[0][3] / M[0][0], b = M[1][3] / M[1][1];
            sum += 2 * std::hypot(a, b) / (H - L);
        }
        return sum / 8;
    };
    r.contrast4 = contrast(4.0);
    r.contrast3 = contrast(3.0);
    r.contrast25 = contrast(2.5);
    for (int q = 0; q < 16; ++q) {
        const double x0 = 200 + q / 16.0;
        const auto codes = row(n, c.t, [&](unsigned i) { return L + (H - L) * std::clamp(double(i) + 1 - x0, 0.0, 1.0); });
        const auto out = pushShrinkRow(codes, w, k, c.t);
        for (double v : out) { const double l = pushDecode(v, c.t); r.overshoot = std::max({r.overshoot, (l - H) / (H - L), (L - l) / (H - L)}); }
    }
    {
        const auto codes = row(n, c.t, [&](unsigned i) { return (L + H) / 2 + (H - L) / 2 * std::cos(2 * pi * i / 1.25); });
        const auto out = pushShrinkRow(codes, w, k, c.t);
        double s = 0, sq = 0, m = 0;
        for (unsigned o = 20; o + 20 < w; ++o) { const double l = pushDecode(out[o], c.t); s += l; sq += l * l; ++m; }
        const double mean = s / m;
        r.moire = std::sqrt(std::max(0.0, sq / m - mean * mean)) / ((H - L) / 2);
    }
    r.overshoot *= 100;
    return r;
}
inline const char* kernelName(PushShrinkKernel k) { return pushShrinkKernelName(k); }
// Texel loads per screen pixel (2D): the shader's loop bounds for the footprint.
inline int loads(PushShrinkKernel k, double f) {
    if (!pushShrinkApplies(k, f)) return 4;
    const int span = int(std::ceil(2 * pushShrinkRadius(k) * f)) + 1;
    return span * span;
}
}  // namespace downscale_tests

TEST(downscale_rules) {
    using namespace veyra::engine;
    CHECK(!pushShrinkApplies(PushShrinkKernel::Area, 1.0) && !pushShrinkApplies(PushShrinkKernel::Area, 0.5) && !pushShrinkApplies(PushShrinkKernel::Bilinear1, 1.5) && pushShrinkApplies(PushShrinkKernel::Area, 1.5));
    for (double v : {0.0, 0.1, 0.5, 0.75, 1.0}) {
        CHECK(std::abs(pushLinearToPq(pushPqToLinear(v)) - v) < 1e-6);
        CHECK(std::abs(pushLinearToSrgb(pushSrgbToLinear(v)) - v) < 1e-9);
    }
    // Area weights at f = 1.5 are exact coverage: screen pixel 0 (centre 0.75) = texel 0 whole + half of texel 1, pixel 1 (2.25) =
    // the other half of texel 1 + texel 2.
    CHECK(std::abs(pushShrinkWeight(PushShrinkKernel::Area, 0.75, 1.5, 0) - 1.0) < 1e-12 && std::abs(pushShrinkWeight(PushShrinkKernel::Area, 0.75, 1.5, 1) - 0.5) < 1e-12);
    CHECK(pushShrinkWeight(PushShrinkKernel::Area, 0.75, 1.5, 2) == 0.0 && std::abs(pushShrinkWeight(PushShrinkKernel::Area, 2.25, 1.5, 1) - 0.5) < 1e-12);
    // 1:1 and magnification never use the new path: the result is Package 16's tap exactly.
    PushPlane src{64, 4, std::vector<double>(256)};
    for (unsigned i = 0; i < 256; ++i) src.v[i] = (i * 37 % 101) / 100.0;
    for (unsigned x = 0; x < 64; ++x) CHECK(pushShrinkSample(src, 64, 4, x, 1, PushShrinkKernel::Area, PushTransfer::Pq) == pushShrinkSample(src, 64, 4, x, 1, PushShrinkKernel::Bilinear1, PushTransfer::Pq));
    for (unsigned x = 0; x < 128; ++x) CHECK(pushShrinkSample(src, 128, 8, x, 3, PushShrinkKernel::CatmullRom, PushTransfer::Srgb) == pushShrinkSample(src, 128, 8, x, 3, PushShrinkKernel::Bilinear1, PushTransfer::Srgb));
    // A flat picture stays exactly flat (weights normalised), in both transfers.
    PushPlane flat{96, 8, std::vector<double>(768, 0.58)};
    for (unsigned x = 0; x < 64; ++x) CHECK(std::abs(pushShrinkSample(flat, 64, 5, x, 2, PushShrinkKernel::Area, PushTransfer::Pq) - 0.58) < 1e-9);
}

// The Package 16 defect, exactly: at f = 1.5 screen pixel o takes ONE bilinear tap at source position 1.5 o + 0.25 (texel
// centres at integers): 75 % of column 3k and 25 % of 3k+1 (o = 2k), or 25 % of 3k+1 and 75 % of 3k+2 (o = 2k+1). Column 3k+1
// never gets more than a quarter, and the quarter is taken of its PQ CODE: a 1-px 300-nit stroke on black shows about 1 nit.
TEST(downscale_package16_defect_reproduced) {
    using namespace veyra::engine;
    const auto peaks = [](double backgroundNits, PushShrinkKernel k, double out[3]) {
        std::vector<double> codes(480, pushLinearToPq(backgroundNits / 10000.0));
        for (int phase = 0; phase < 3; ++phase) {
            auto c = codes;
            c[std::size_t(201 + phase)] = pushLinearToPq(300.0 / 10000.0);   // columns 201 (3k), 202 (3k+1), 203 (3k+2)
            const auto row = pushShrinkRow(c, 320, k, PushTransfer::Pq);
            out[phase] = 0;
            for (double v : row) out[phase] = std::max(out[phase], pushPqToLinear(v) * 10000.0);
        }
    };
    double black[3], dim[3], area[3], sharp[3];
    peaks(0.0, PushShrinkKernel::Bilinear1, black);
    peaks(1.0, PushShrinkKernel::Bilinear1, dim);
    peaks(0.0, PushShrinkKernel::Area, area);
    peaks(0.0, PushShrinkKernel::AreaSharp, sharp);
    if (std::getenv("ONSCREEN_SIM_PRINT")) {
        std::printf("package16 1.5x HDR10, 1-px 300-nit stroke, brightest screen pixel (nits) in column 3k / 3k+1 / 3k+2: on black %.1f / %.2f / %.1f, on 1 nit %.1f / %.1f / %.1f\n",
                    black[0], black[1], black[2], dim[0], dim[1], dim[2]);
        std::printf("area 1.5x HDR10, the same stroke on black: %.1f / %.1f / %.1f nits; area+sharpen %.1f / %.1f / %.1f nits\n", area[0], area[1], area[2], sharp[0], sharp[1], sharp[2]);
    }
    CHECK(black[1] < 2.0 && black[1] > 0.5);          // about 1 nit: a quarter of the stroke's PQ code
    CHECK(black[0] > 50.0 && black[2] > 50.0);        // three quarters of the code: ~96 nits
    CHECK(dim[1] < 10.0);
    for (int i = 0; i < 3; ++i) CHECK(area[i] > 99.0 && sharp[i] > 99.0);   // the new path: never the collapse (>= 1/3 of the light)
}

// The interaction with crosshair ghosting (SIMULATED, a model, not his frames): a fixed 1-px 300-nit crosshair stroke on black at
// 4K column X; the generator's ghost = a copy displaced by d = v t (v = the background's motion in 4K px per real frame, t = 1/3 and
// 2/3 at 3X) at strength a = 0.4 of its PQ code (the HDR10 surfaces hold PQ codes), the crosshair itself at 1 - a in those frames.
// Shown at 1.5x. For each filter: the ghost's brightest screen pixel over v = 0.25 .. 12 in quarter steps (it moves with the
// camera, so it visits every column phase), and the crosshair's own light in generated frames relative to real frames.
TEST(downscale_crosshair_ghost_interaction) {
    using namespace veyra::engine;
    const double a = 0.4, stroke = pushLinearToPq(0.03), X = 201;
    struct Light { double peak, total; };
    const auto ghost = [&](PushShrinkKernel k, double d) {
        std::vector<double> codes(480, 0.0);
        const double x0 = X + d;   // a 1-px box at x0, split by coverage between two 4K columns when d is fractional
        for (int i = 190; i < 240; ++i) codes[std::size_t(i)] = a * stroke * std::clamp(std::min(i + 1.0, x0 + 1) - std::max(double(i), x0), 0.0, 1.0);
        Light l{0, 0};
        for (double v : pushShrinkRow(codes, 320, k, PushTransfer::Pq)) { const double nits = pushPqToLinear(v) * 10000.0; l.peak = std::max(l.peak, nits); l.total += nits; }
        return l;
    };
    const auto crosshair = [&](PushShrinkKernel k, double strength, int column) {
        std::vector<double> codes(480, 0.0);
        codes[std::size_t(column)] = strength * stroke;
        double sum = 0;
        for (double v : pushShrinkRow(codes, 320, k, PushTransfer::Pq)) sum += pushPqToLinear(v) * 10000.0;
        return sum;
    };
    struct Spread { double lo = 1e9, hi = 0, mean = 0, n = 0; void add(double x) { lo = std::min(lo, x); hi = std::max(hi, x); mean += x; n += 1; } double rel() const { return (hi - lo) / (mean / n); } };
    double integerTotalSpread[2] = {}, integerPeakSpread[2] = {};
    for (auto k : {PushShrinkKernel::Bilinear1, PushShrinkKernel::AreaSharp}) {
        Spread wholePeak, wholeTotal, anyPeak, anyTotal;
        for (int d = 1; d <= 12; ++d) { const auto l = ghost(k, d); wholePeak.add(l.peak); wholeTotal.add(l.total); }
        for (double v = 0.25; v <= 12.0; v += 0.25)
            for (double t : {1.0 / 3.0, 2.0 / 3.0}) { const auto l = ghost(k, v * t); anyPeak.add(l.peak); anyTotal.add(l.total); }
        const int i = k == PushShrinkKernel::Bilinear1 ? 0 : 1;
        integerTotalSpread[i] = wholeTotal.rel();
        integerPeakSpread[i] = wholePeak.rel();
        const double ratio3k = crosshair(k, 1 - a, 201) / crosshair(k, 1, 201), ratio3k1 = crosshair(k, 1 - a, 202) / crosshair(k, 1, 202);
        if (std::getenv("ONSCREEN_SIM_PRINT"))
            std::printf("ghost x downscale (%s): whole-pixel offsets 1..12: ghost peak %.2f..%.2f nits (spread %.2f), total light spread %.2f; "
                        "any offset (v = 0.25..12, t = 1/3, 2/3): peak %.2f..%.2f nits (spread %.2f), total spread %.2f; crosshair light generated/real: column 3k %.3f, 3k+1 %.3f\n",
                        i ? "area+sharpen" : "package16", wholePeak.lo, wholePeak.hi, wholePeak.rel(), wholeTotal.rel(), anyPeak.lo, anyPeak.hi, anyPeak.rel(), anyTotal.rel(), ratio3k, ratio3k1);
    }
    // The downscale's own term, isolated with whole-pixel offsets: Package 16 multiplies the ghost by its column phase
    // (it flickers as the camera speed changes); the new path shows the same light at every offset.
    CHECK(integerTotalSpread[0] > 0.5 && integerTotalSpread[1] < 0.05);
    CHECK(integerPeakSpread[1] < integerPeakSpread[0]);
}

TEST(downscale_kernel_table) {
    using namespace veyra::engine;
    using namespace downscale_tests;
    const Case cases[] = {{"1.5x HDR10", 1.5, PushTransfer::Pq}, {"1.5x SDR", 1.5, PushTransfer::Srgb}, {"2.63x HDR10", 3840.0 / 1462.0, PushTransfer::Pq}, {"2.63x SDR", 3840.0 / 1462.0, PushTransfer::Srgb}};
    const PushShrinkKernel kernels[] = {PushShrinkKernel::Bilinear1, PushShrinkKernel::Area, PushShrinkKernel::AreaSharp, PushShrinkKernel::AreaSharpClamp, PushShrinkKernel::CatmullRom,
                                        PushShrinkKernel::CatmullRomClamp, PushShrinkKernel::Mitchell, PushShrinkKernel::Lanczos2, PushShrinkKernel::Lanczos2Clamp};
    const bool print = std::getenv("ONSCREEN_SIM_PRINT") != nullptr;
    if (print) std::printf("| case | kernel | stroke mean | stroke worst | pulse | contrast 4 | contrast 3 | contrast 2.5 | overshoot %% | moire | loads/px |\n");
    for (const auto& c : cases) {
        Numbers p16 = measure(PushShrinkKernel::Bilinear1, c);
        for (auto k : kernels) {
            const Numbers r = k == PushShrinkKernel::Bilinear1 ? p16 : measure(k, c);
            if (print)
                std::printf("| %s | %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.1f | %.3f | %d |\n", c.name, kernelName(k), r.strokeMean, r.strokeWorst, r.pulse,
                            r.contrast4, r.contrast3, r.contrast25, r.overshoot, r.moire, loads(k, c.f));
            if (pushShrinkClamps(k)) {   // the clamped variants: no ringing at all, whatever the weights
                CHECK(r.overshoot < 1e-6);
                CHECK(r.strokeWorst > 3 * p16.strokeWorst && r.strokeWorst > 0.2);
            }
            if (k == PushShrinkKernel::AreaSharpClamp) {   // THE DEFAULT KERNEL of the choice: Package 16's contrast, no overshoot, no pulse
                CHECK(r.pulse < 0.1);
                if (c.f < 2) CHECK(r.contrast4 >= p16.contrast4 - 0.01 && r.contrast3 >= p16.contrast3 - 0.01 && r.contrast25 >= p16.contrast25 - 0.01);
            }
            if (k == PushShrinkKernel::AreaSharp) {   // the unclamped one (PushDownscale.h), against the bar set for default-on
                CHECK(r.strokeWorst > 3 * p16.strokeWorst && r.strokeWorst > 0.2);   // the thin-stroke collapse is gone
                CHECK(r.pulse < 0.05 && p16.pulse > 0.9);            // the energy pulse is gone
                CHECK(r.overshoot < 3.0);
                if (c.f < 2) CHECK(r.contrast4 >= p16.contrast4 - 0.005 && r.contrast3 >= p16.contrast3 - 0.005 && r.contrast25 >= p16.contrast25 - 0.005);
                // 2.63x: Package 16's single tap is a narrower filter (it shows 38 % of the columns and drops the rest), so its
                // contrast is higher there; no kernel under 3 % overshoot reaches it. Recorded, not hidden: the table prints it.
                else CHECK(r.contrast4 < p16.contrast4 && r.contrast4 > 0.9);
            }
        }
    }
}

// HDR accuracy of the choice (SIMULATED, PQ, 1.5x): a flat 1000-nit patch keeps its nits exactly with every kernel; a 1000-nit step edge
// never goes past the source peak with a clamped kernel (the unclamped sharpening lobes do: tools/pace_downscale/downscale_ref.py prints
// 20-52 nits), and a thin highlight's neighbours never go negative (a negative channel would shift the hue on the display).
TEST(downscale_hdr_accuracy_and_no_overshoot_beyond_the_source_peak) {
    using namespace veyra::engine;
    const double peakCode = pushLinearToPq(0.1), darkCode = pushLinearToPq(5.0 / 10000.0);
    for (auto k : {PushShrinkKernel::Area, PushShrinkKernel::AreaSharp, PushShrinkKernel::AreaSharpClamp, PushShrinkKernel::CatmullRom, PushShrinkKernel::CatmullRomClamp,
                   PushShrinkKernel::Lanczos2, PushShrinkKernel::Lanczos2Clamp}) {
        for (double v : pushShrinkRow(std::vector<double>(480, peakCode), 320, k, PushTransfer::Pq)) CHECK(std::abs(pushPqToLinear(v) * 10000.0 - 1000.0) < 0.01);   // exact nits
        double over = 0;
        for (int q = 0; q < 16; ++q) {
            std::vector<double> codes(480, darkCode);
            const double x0 = 200 + q / 16.0;
            for (int i = 0; i < 480; ++i) codes[std::size_t(i)] = pushLinearToPq((5.0 + 995.0 * std::clamp(x0 - i, 0.0, 1.0)) / 10000.0);
            for (double v : pushShrinkRow(codes, 320, k, PushTransfer::Pq)) {
                const double nits = pushPqToLinear(v) * 10000.0;
                over = std::max(over, nits - 1000.0);
            }
        }
        if (pushShrinkClamps(k)) CHECK(over < 0.05);
        else if (k != PushShrinkKernel::Area) CHECK(over > 5.0);   // the unclamped sharpening lobes do overshoot (so the clamp is doing something)
        else CHECK(over < 0.05);
    }
    // Weights keep the family: the clamp does not change the unclamped weights, only the result.
    for (int i = 198; i < 206; ++i) CHECK(pushShrinkWeight(PushShrinkKernel::AreaSharpClamp, 201.75, 1.5, i) == pushShrinkWeight(PushShrinkKernel::AreaSharp, 201.75, 1.5, i));
    // Flags: every selectable kernel has its own shader flags; Mitchell and Package 16 have none.
    CHECK(pushShrinkFlags(PushShrinkKernel::AreaSharpClamp, PushTransfer::Pq) == (kPushShrinkFlagOn | kPushShrinkFlagSharpen | kPushShrinkFlagClamp | kPushShrinkFlagPq));
    CHECK(pushShrinkFlags(PushShrinkKernel::CatmullRom, PushTransfer::Srgb) == (kPushShrinkFlagOn | kPushShrinkFlagCatmull | kPushShrinkFlagSrgb));
    CHECK(pushShrinkFlags(PushShrinkKernel::Lanczos2Clamp, PushTransfer::Linear) == (kPushShrinkFlagOn | kPushShrinkFlagLanczos | kPushShrinkFlagClamp));
    CHECK(pushShrinkFlags(PushShrinkKernel::Mitchell, PushTransfer::Pq) == 0 && pushShrinkFlags(PushShrinkKernel::Bilinear1, PushTransfer::Pq) == 0);
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

// Review 2 MUST 1: one reading per refresh (k per Present interval), at a reading phase that varies at random, names nearly every
// shown frame, so the range narrows to a few points at 2X and 3X alike; with one reading per Present it stays wide.
TEST(onscreen_range_narrows_with_one_reading_per_refresh_at_any_phase) {
    using onscreen_tests::sampledHolds;
    onscreen_tests::Lcg rng{4242};
    for (int k : {2, 3})
        for (int delay = 0; delay <= 2; ++delay) {
            std::vector<int> holds;
            while (holds.size() < 3000) {
                if (rng.next() < 0.10) { holds.push_back(k - 1); holds.push_back(k + 1); }
                else holds.push_back(k);
            }
            const auto one = sampledHolds(k, delay, holds, 1, 1.0, 11);
            const auto each = sampledHolds(k, delay, holds, k, 1.0, 12);
            if (std::getenv("ONSCREEN_SIM_PRINT"))
                std::printf("readings k=%d delay=%d: true %.1f %%; one per Present %.1f..%.1f %%; one per refresh %.1f..%.1f %%\n", k, delay, one.evenPct,
                            one.minPct, one.maxPct, each.minPct, each.maxPct);
            CHECK(one.minPct <= one.evenPct + 1.0 && one.evenPct <= one.maxPct + 1.0);
            CHECK(each.minPct <= each.evenPct + 1.0 && each.evenPct <= each.maxPct + 1.0);
            CHECK(each.maxPct - each.minPct <= 10.0);   // narrow enough to be shown (the interface shows a range only then)
        }
}

TEST(frame_probe_motion_floor_follows_the_noise) {
    using namespace veyra::engine;
    constexpr std::size_t n = PushFrameProbe::kCells;
    PushFrameProbe probe;
    // Grain only (pictures differ by about 0.004 per cell): never motion, whatever a generated frame does.
    std::vector<float> a(n, 0.5f), b(n), g(n, 0.5f);
    onscreen_tests::Lcg rng{5};
    for (int round = 0; round < 60; ++round) {
        for (std::size_t i = 0; i < n; ++i) b[i] = 0.5f + float(rng.next() - 0.5) * 0.016f;
        const float* group[] = {a.data(), g.data(), g.data(), b.data()};
        probe.group(group, 4);
    }
    const auto s = probe.take();
    CHECK(s.groups == 60 && s.motionGroups == 0 && s.stillGroups == 60 && s.generated == 0);
    CHECK(s.motionFloor >= PushFrameProbe::kMotionFloor);
}
