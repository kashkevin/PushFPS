// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace" experiment VEYRA_EXP_RP_PHASE_LOCK=1 (OFF by default): lock the Present phase to the display's own vblank
// clock on a composed (fixed refresh) desktop, so every frame is handed to Windows at the same safe point of a refresh.
//
// Why (his Package 16 log, out/research/SMOOTHNESS_60_VS_90.md): a Present called within about 2.5 ms before a vblank lands
// on the next-but-one refresh in some frames and on the next in others (mean whole refreshes waited 2.07 at 0-0.5 ms before
// the vblank, 1.17 at 3.5 ms or more). Our Presents follow the content grid (anchor + k T / M), which drifts slowly through
// every phase of the refresh, so for seconds at a time consecutive frames sit in that band and are held 1, 2 or 3 refreshes
// at random instead of 2 at 90 FPS (3 at 60).
//
// The rule: a paced frame's Present is moved EARLIER, never later, to the latest point at or before its deadline that lies
// `offset` after a vblank (default 1 ms: the part of the refresh with the fewest refreshes waited in his log). The frame is
// already GPU-ready when the scheduler asks (readiness is checked first), so presenting it earlier is possible. Consecutive
// frames 2 refreshes apart then share one phase; the slow drift of the source against the display moves all of them across
// a vblank at once, once per beat, as one deliberate long (or short) hold instead of random pairs. If the thread arrives
// later than the slot plus a small tolerance, the frame keeps its own deadline (today's behaviour): never later than today.
//
// Not applied (refusal names in [cadence] / the switch's start line): switch off; no vblank clock from the frame statistics,
// or one older than kMaxGridAge100ns; the refresh clock stands still (variable refresh / independent flip: the panel follows
// the Present spacing and phase means nothing); the driver reports independent flip or VRR active; the output interval is
// not a whole number of refreshes within 3 % (120 FPS on 180 Hz has no even hold to protect).
// Pure, header-only; tests and a simulation with his numbers: tests/remoteplay/OnScreenCadenceTests.inc. SIMULATED, not measured:
// whether DWM on his PC behaves like the measured bins at every moment is for his next log ([cadence] lock=1 vs 0).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace veyra::engine {

inline constexpr std::int64_t kPushPhaseLockMaxGridAge100ns = 2500000;   // 250 ms
inline constexpr double kPushPhaseLockDefaultOffsetMs = 1.0;

struct PushPhaseLockInputs {
    bool switchOn = false;
    bool clockFrozen = false;     // PushOnScreenCadence: the vblank count stood still (variable refresh / independent flip)
    bool directFlip = false;      // NVIDIA sleep status: fullscreen independent flip
    bool vrrActive = false;       // driver VRR active for the window
    std::int64_t now100ns = 0;
    std::int64_t lastVblank100ns = 0;
    double period100ns = 0;
    double outputInterval100ns = 0;   // the content interval / multiplier
};

// nullptr when the lock applies, else why not.
inline const char* pushPhaseLockRefusal(const PushPhaseLockInputs& in) {
    if (!in.switchOn) return "off";
    if (!(in.period100ns > 0) || in.lastVblank100ns <= 0) return "no-vblank-clock";
    if (in.now100ns - in.lastVblank100ns > kPushPhaseLockMaxGridAge100ns) return "stale-vblank-clock";
    if (in.clockFrozen) return "refresh-clock-frozen";
    if (in.directFlip) return "direct-flip";
    if (in.vrrActive) return "vrr";
    if (!(in.outputInterval100ns > 0)) return "no-interval";
    const double ratio = in.outputInterval100ns / in.period100ns, k = std::round(ratio);
    if (k < 1 || std::abs(ratio - k) > 0.03 * k) return "not-whole-refreshes";
    return nullptr;
}

// The offset after a vblank, clamped to [0.2 ms, half a refresh] (100 ns units).
inline double pushPhaseLockOffset100ns(double offsetMs, double period100ns) {
    const double wanted = (std::isfinite(offsetMs) ? offsetMs : kPushPhaseLockDefaultOffsetMs) * 10000.0;
    return std::clamp(wanted, 2000.0, std::max(2000.0, period100ns * 0.5));
}

// The latest time at or before `deadline` that lies `offset` after a vblank of the grid (lastVblank + n period).
inline std::int64_t pushPhaseLockSlot(std::int64_t deadline100ns, std::int64_t lastVblank100ns, double period100ns, double offset100ns) {
    if (!(period100ns > 0)) return deadline100ns;
    const double base = double(lastVblank100ns) + offset100ns;
    const double n = std::floor((double(deadline100ns) - base) / period100ns);
    const auto slot = static_cast<std::int64_t>(std::llround(base + n * period100ns));
    return std::min(slot, deadline100ns);
}

struct PushPhaseLockDue {
    std::int64_t due100ns = 0;   // never later than the deadline
    bool moved = false;          // due is the locked slot (earlier than the deadline, or now within its tolerance)
    bool missed = false;         // the slot had passed by more than the tolerance: the deadline is kept
    double advance100ns = 0;     // deadline - due when moved
};
// tolerance: how late after the slot the frame may still go (it stays in the safe part of the refresh): min(0.5 ms, a quarter
// refresh).
inline PushPhaseLockDue pushPhaseLockDue(std::int64_t deadline100ns, std::int64_t now100ns, std::int64_t lastVblank100ns, double period100ns, double offset100ns) {
    PushPhaseLockDue d;
    d.due100ns = deadline100ns;
    if (!(period100ns > 0)) return d;
    const std::int64_t slot = pushPhaseLockSlot(deadline100ns, lastVblank100ns, period100ns, offset100ns);
    const auto tolerance = static_cast<std::int64_t>(std::min(5000.0, period100ns * 0.25));
    if (now100ns > slot + tolerance) {
        d.missed = slot < deadline100ns;
        return d;
    }
    d.due100ns = slot;
    d.moved = true;
    d.advance100ns = double(deadline100ns - slot);
    return d;
}

}  // namespace veyra::engine
