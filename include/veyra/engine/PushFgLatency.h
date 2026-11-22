// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "fglat" (the owner's seventeenth message): Ultra Low Latency Frame Generation, an experiment that is off unless
// VEYRA_EXP_RP_FG_ULL is set (docs/drafts/FG_LATENCY.md, docs/PS5_REMOTE_PLAY_EXPERIMENTS.md "Ultra Low Latency Frame
// Generation"). Portable and header-only: the decisions are here and tested (tests/remoteplay/FgLatencyTests.inc); the
// engine (EngineController.cpp) only reads the switch once and calls these. Times are host 100 ns ticks.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {

// --- the switch ------------------------------------------------------------------------------------------------
// VEYRA_EXP_RP_FG_ULL: "1" or "all" turns every part on; otherwise a comma-separated list of parts:
//   hold   no dead FG phase: a pair whose generated frames are not shown does not hold its real frame for them
//          (PushFgFallbackHold);
//   phase  a PS5 stream learns its pair phase as a capture card does (what VEYRA_EXP_RP_ADAPTIVE_PHASE=1 does);
//   headroom  under confirmed variable refresh, Smoothness Mode runs with a target that keeps 3 % of the panel's maximum
//          refresh free (pushUllHeadroomTarget), unless the user or the kit set a target;
//   grace  a generated frame later than half its output interval is dropped instead of shown (pushUllGeneratedGrace).
// Anything else (an unknown part, an empty part, more than 63 characters) turns nothing on: a typo must never run a
// half-understood experiment. Unset: nothing changes.
struct PushUllParts {
    bool hold = false, phase = false, headroom = false, grace = false;
    bool valid = true;   // false: the value was set but not understood (logged, nothing on)
    constexpr bool any() const { return hold || phase || headroom || grace; }
};
constexpr PushUllParts pushUllAll() {
    PushUllParts p;
    p.hold = p.phase = p.headroom = p.grace = true;
    return p;
}
inline PushUllParts pushUllParse(std::wstring_view value) {
    PushUllParts none;
    if (value.empty()) return none;
    if (value.size() > 63) { none.valid = false; return none; }
    if (value == L"1" || value == L"all") return pushUllAll();
    PushUllParts p;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(L',', start);
        const std::size_t end = comma == std::wstring_view::npos ? value.size() : comma;
        const std::wstring_view part = value.substr(start, end - start);
        if (part == L"hold") p.hold = true;
        else if (part == L"phase") p.phase = true;
        else if (part == L"headroom") p.headroom = true;
        else if (part == L"grace") p.grace = true;
        else { none.valid = false; return none; }
        if (comma == std::wstring_view::npos) break;
        start = comma + 1;
    }
    return p;
}
// For the log line: the parts that are on, "none" without any.
inline const char* pushUllPartsName(const PushUllParts& p) {
    if (p.hold && p.phase && p.headroom && p.grace) return "all";
    // Fixed strings only (no allocation): 16 combinations.
    static constexpr const char* names[16] = {"none", "hold", "phase", "hold,phase", "headroom", "hold,headroom", "phase,headroom",
                                              "hold,phase,headroom", "grace", "hold,grace", "phase,grace", "hold,phase,grace",
                                              "headroom,grace", "hold,headroom,grace", "phase,headroom,grace", "all"};
    const unsigned index = (p.hold ? 1u : 0u) | (p.phase ? 2u : 0u) | (p.headroom ? 4u : 0u) | (p.grace ? 8u : 0u);
    return names[index];
}

// --- part "hold": no dead FG phase -----------------------------------------------------------------------------------
// Frame generation shows a pair's real frame B (M-1)/M of a content interval plus the first generated frame's GPU time
// after it was ready (the "phase"), because the generated frames between A and B go out first. A pair whose generated
// frames are skipped still holds B for that phase today: on a PS5 stream for every skipped pair (T + the base cost's
// P95), on a capture card for every skipped pair until a whole second of skips has passed (LiveFgRejectionHold). That
// delay buys nothing. This hold shows such real frames as soon as they are ready:
//   - at once while a FgCostSettling hold runs (the budget will admit no pair before it ends: 2, 4 or 8 s);
//   - after kEnter100ns (250 ms) of nothing but skipped pairs (LiveFgRejectionHold: 1 s);
//   - a pair admitted while held stays held (its generated frames then expire unshown, while the budget still measures
//     their cost), and the phase returns only after kResumePairs admitted pairs in a row, as LiveFgRejectionHold does.
// A single skipped pair between admitted ones keeps the phase: showing its B early and the next B late would make two
// uneven intervals in a row for one missing group. Entering moves the presentation one phase earlier (one interval
// shorter by the phase), leaving moves it back (one interval longer by the phase): two uneven intervals per hold, the
// same as LiveFgRejectionHold has. Pure; one call per pair after its frame-generation decision.
class PushFgFallbackHold {
public:
    static constexpr std::int64_t kEnter100ns = 2500000;   // 250 ms of skipped pairs only
    static constexpr unsigned kResumePairs = 8;             // as LiveFgRejectionHold::kResumePairs
    enum class Why : std::uint8_t { None, Settling, Rejected };
    void reset() { *this = PushFgFallbackHold{}; }
    bool held() const { return held_; }
    Why why() const { return why_; }
    std::uint64_t entries() const { return entries_; }
    // admitted: the pair's generation group runs (any decision but Skip). settling: a FgCostSettling hold runs now.
    // Returns true when this pair's real frame goes out when ready (no FG phase).
    bool observe(std::int64_t now, bool admitted, bool settling) {
        if (admitted) {
            rejectedSince_ = 0;
            if (held_ && ++admittedRun_ < kResumePairs) return true;
            held_ = false;
            admittedRun_ = 0;
            why_ = Why::None;
            return false;
        }
        admittedRun_ = 0;
        if (!rejectedSince_) rejectedSince_ = now;
        if (!held_ && (settling || now - rejectedSince_ >= kEnter100ns)) {
            held_ = true;
            why_ = settling ? Why::Settling : Why::Rejected;
            ++entries_;
        }
        return held_;
    }

private:
    std::int64_t rejectedSince_ = 0;
    unsigned admittedRun_ = 0;
    bool held_ = false;
    Why why_ = Why::None;
    std::uint64_t entries_ = 0;
};
inline const char* pushFgHoldWhyName(PushFgFallbackHold::Why why) {
    switch (why) {
        case PushFgFallbackHold::Why::Settling: return "settling";
        case PushFgFallbackHold::Why::Rejected: return "rejected";
        default: return "none";
    }
}

// --- part "headroom": a Smoothness Mode target below the VRR ceiling -------------------------------------------------
// Under variable refresh the panel never refreshes faster than its maximum. An output at that rate (60 x 3X or 30 x 6X =
// 180 on a 179.95 Hz panel) presents at the ceiling: a present that comes a little early waits for the panel, and with
// DXGI's maximum latency 1 the next one waits behind it (FG_LATENCY.md section 1.10, estimate: up to a refresh). Every
// higher multiplier also holds the real frame longer ((M-1)/M of the content interval). So this part gives Smoothness
// Mode (PushSmoothness.h, which never goes below 2X and steps down on load or rejected pairs) the largest of its
// targets, 180 / 120 / 90 / 60, that is at most 97 % of the refresh: 180 Hz -> 120, 240 Hz -> 180, 144 / 165 Hz -> 120,
// 120 Hz -> 90. Only with variable refresh confirmed (vrr == 1); on a fixed refresh every present waits for the vblank
// with frame generation off too, and an even divisor matters more there, so the target stays 0 (the existing rules).
inline constexpr double kPushUllHeadroom = 0.97;
inline int pushUllHeadroomTarget(double refreshHz, int vrr) {
    if (vrr != 1 || !std::isfinite(refreshHz) || refreshHz <= 0.0) return 0;
    constexpr std::array<int, 4> targets{{180, 120, 90, 60}};
    for (const int t : targets)
        if (double(t) <= refreshHz * kPushUllHeadroom) return t;
    return 0;
}

// --- part "grace": drop a late generated frame instead of showing it late ----------------------------------------------
// A live generated frame may be shown up to 10 ms past its deadline (kLiveGeneratedGrace100ns) plus the distance between
// its callback and its smoothed anchor. A frame that late shows a picture from the wrong time and leaves the frame after
// it (often the real one) less than half an output interval, which under explicit VSync can push the real frame to the
// next refresh (the capacity wait: the real frame waits). This part keeps the anchor distance and lowers the fixed part
// to half an output interval (content interval / M / 2), at least 1 ms, never more than the default.
// grace100ns: the default grace for this pair (kLiveGeneratedGrace100ns + the anchor distance); defaultGrace100ns:
// kLiveGeneratedGrace100ns; interval100ns: the pair's content interval; multiplier: the pair's multiplier.
inline std::int64_t pushUllGeneratedGrace(std::int64_t grace100ns, std::int64_t defaultGrace100ns, std::int64_t interval100ns, unsigned multiplier) {
    if (interval100ns <= 0 || multiplier < 2 || defaultGrace100ns <= 0) return grace100ns;
    const std::int64_t anchorPart = std::max<std::int64_t>(0, grace100ns - defaultGrace100ns);
    const std::int64_t half = interval100ns / (2 * std::int64_t(multiplier));
    return std::clamp<std::int64_t>(half, std::min<std::int64_t>(10000, defaultGrace100ns), defaultGrace100ns) + anchorPart;
}

// --- diagnostics: the latency fields of the [slipstream-smooth] window -------------------------------------------------
// Written only with VEYRA_EXP_RP_STATS=1 (PushSmoothHook owns one window), appended to the 5 s `[slipstream-smooth]`
// line. Software times on the PC's steady clock, from the frame's decode (PS5 stream) or capture callback (card) to the
// Present call / return; never photons. Per presented frame the engine passes:
//   real frames: age      decode -> Present return (streams and screen capture; a card writes -1 here because
//                         `[slipstream-capture] realLatP*Ms` is the same measure);
//                hold     the step saw the frame ready -> Present call: the wait for its FG deadline (the "future-frame
//                         wait"), plus any capacity wait;
//                phase    deadline - decode/callback: the delay the code scheduled (0 when shown when ready);
//                first    decode/callback -> the first generated frame of the pair seen ready (fg1's readiness, CPU
//                         observation), -1 without generated frames;
//                onPhase  presented on an FG phase; generatedShown: generated frames of its pair presented before it;
//                fallback presented when ready because a hold (PushFgFallbackHold or LiveFgRejectionHold) holds FG off;
//   every frame: late     Present call - deadline (paced frames; how late the thread presented it).
// Fields: realAgeP50/P95/P99Ms, addedEstMs (the ESTIMATE of FG's PC-side added delay of a real frame: the P50 of its
// hold; it leaves out the GPU time frame generation takes from the frame's own processing, the display and everything
// outside the PC), realHoldP95/P99Ms, phaseMs (P50), phaseLearned, firstReadyP50/P95Ms, presentLateP95/P99Ms,
// deadPhaseFps (real frames presented on an FG phase with no generated frame of their pair presented: delay paid for
// nothing), fallbackFps. Unknown values are -1. Not thread-safe: the engine thread only.
class PushFgLatencyWindow {
public:
    enum class Metric : std::uint8_t { Age, Hold, Phase, First, Late, Count };
    static constexpr std::size_t kMax = 1024;   // samples per metric and window (5 s at 200 frames/s)
    void clear() {
        for (auto& n : count_) n = 0;
        deadPhase_ = fallback_ = real_ = 0;
    }
    void add(Metric m, double ms) {
        const auto i = static_cast<std::size_t>(m);
        if (!std::isfinite(ms) || ms < 0.0 || ms > 10000.0 || count_[i] >= kMax) return;
        values_[i][count_[i]++] = ms;
    }
    void real(double ageMs, double holdMs, double phaseMs, double firstMs, bool onPhase, unsigned generatedShown, bool fallback) {
        ++real_;
        add(Metric::Age, ageMs);
        add(Metric::Hold, holdMs);
        add(Metric::Phase, phaseMs);
        add(Metric::First, firstMs);
        if (onPhase && generatedShown == 0) ++deadPhase_;
        if (fallback) ++fallback_;
    }
    void presented(double lateMs) { add(Metric::Late, lateMs); }
    std::size_t samples(Metric m) const { return count_[static_cast<std::size_t>(m)]; }
    std::uint64_t realFrames() const { return real_; }
    std::uint64_t deadPhase() const { return deadPhase_; }
    std::uint64_t fallback() const { return fallback_; }
    // Nearest rank (as PushSmoothWindow::percentile); -1 without samples.
    double percentile(Metric m, double p) const {
        const auto i = static_cast<std::size_t>(m);
        if (!count_[i]) return -1.0;
        std::vector<double> v(values_[i].begin(), values_[i].begin() + static_cast<std::ptrdiff_t>(count_[i]));
        const std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * double(v.size())));
        const std::size_t index = std::min(v.size() - 1, rank ? rank - 1 : 0);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(index), v.end());
        return v[index];
    }
    // The fields, each preceded by a space; seconds: the window's length; phaseLearned: the pair phase rule in force.
    std::string fields(double seconds, bool phaseLearned) const {
        std::string s;
        const auto num = [&s](const char* name, double value) {
            char buffer[64];
            const int n = std::snprintf(buffer, sizeof buffer, " %s=%.3f", name, value);
            if (n > 0) s.append(buffer, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof buffer) - 1)));
        };
        const double perSecond = seconds > 0.001 ? 1.0 / seconds : -1.0;
        num("realAgeP50Ms", percentile(Metric::Age, 50));
        num("realAgeP95Ms", percentile(Metric::Age, 95));
        num("realAgeP99Ms", percentile(Metric::Age, 99));
        num("addedEstMs", percentile(Metric::Hold, 50));
        num("realHoldP95Ms", percentile(Metric::Hold, 95));
        num("realHoldP99Ms", percentile(Metric::Hold, 99));
        num("phaseMs", percentile(Metric::Phase, 50));
        s += phaseLearned ? " phaseLearned=1" : " phaseLearned=0";
        num("firstReadyP50Ms", percentile(Metric::First, 50));
        num("firstReadyP95Ms", percentile(Metric::First, 95));
        num("presentLateP95Ms", percentile(Metric::Late, 95));
        num("presentLateP99Ms", percentile(Metric::Late, 99));
        num("deadPhaseFps", perSecond > 0 ? double(deadPhase_) * perSecond : -1.0);
        num("fallbackFps", perSecond > 0 ? double(fallback_) * perSecond : -1.0);
        return s;
    }

private:
    std::array<std::array<double, kMax>, static_cast<std::size_t>(Metric::Count)> values_{};
    std::array<std::size_t, static_cast<std::size_t>(Metric::Count)> count_{};
    std::uint64_t deadPhase_ = 0, fallback_ = 0, real_ = 0;
};

}  // namespace veyra::engine
