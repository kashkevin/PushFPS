// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {
// Consistent frame-generation smoothness (sprint 5, stream "smooth"; analysis in
// docs/drafts/SMOOTHNESS_ANALYSIS.md). Portable and header-only: the C++ standard library, no Windows, no engine
// types. Tests: tests/remoteplay/SmoothnessTests.inc. Every decision here is reached from rates, sizes and GPU
// times the engine already measures; nothing names a GPU, a vendor or a refresh rate. Nothing here has run on a
// GPU: every threshold is a starting point.
//
//   pushSmoothSrTarget        VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1: the SR target lowered to the smallest box that covers
//                             the monitor (4K -> 1440p on a 1440p panel), so SR, VFG and every generated frame work
//                             at the size that is shown. Only ever lowers.
//   pushSmoothCapMultiplier   VEYRA_EXP_RP_SMOOTH_CAP_FIT=1: the output-cap reduction with the display hint's 2 % fit
//                             tolerance (59.94 x 3 reaches a 179.95 cap: 3X, where ceil() gave 4X).
//   PushSmoothnessMode        Smoothness Mode: a displayed-rate target (60/90/120/180), the multiplier from the
//                             content rate and the measured GPU headroom, step-downs only after sustained trouble,
//                             never below 2X, a reason for every change.
//   PushSmoothWindow / pushSmoothLine
//                             the 5 s `[slipstream-smooth]` line (VEYRA_EXP_RP_STATS=1; S5_PRESENT_API.md section
//                             "Smoothness").

// --- SR target that follows the display ---------------------------------------------------------------------
// Target values are pipeline::SrTarget's (ResolutionPlan.h): 0 2560x1440, 1 3840x2160, 2 7680x4320, 3 5120x2880,
// 4 6144x3456, 5 7168x4032. The glue checks the order with a static_assert.
struct PushSmoothBox { std::uint32_t width = 0, height = 0; };
inline constexpr std::array<PushSmoothBox, 6> kPushSmoothSrBoxes{{{2560, 1440}, {3840, 2160}, {7680, 4320}, {5120, 2880}, {6144, 3456}, {7168, 4032}}};
// The user's target, or a smaller one whose box still covers display x display (both sides). Unknown display
// (0) or a target the table does not hold: the user's target, unchanged.
constexpr std::uint32_t pushSmoothSrTarget(std::uint32_t userTarget, std::uint32_t displayWidth, std::uint32_t displayHeight) {
    if (userTarget >= kPushSmoothSrBoxes.size() || displayWidth == 0 || displayHeight == 0) return userTarget;
    const auto area = [](const PushSmoothBox& b) { return std::uint64_t(b.width) * b.height; };
    const PushSmoothBox user = kPushSmoothSrBoxes[userTarget];
    std::uint32_t best = userTarget;
    for (std::uint32_t i = 0; i < kPushSmoothSrBoxes.size(); ++i) {
        const PushSmoothBox& b = kPushSmoothSrBoxes[i];
        if (b.width < displayWidth || b.height < displayHeight) continue;          // must cover the display
        if (area(b) >= area(kPushSmoothSrBoxes[best])) continue;                    // only smaller than the best so far
        if (b.width > user.width || b.height > user.height) continue;             // never larger than the user's
        best = i;
    }
    return best;
}

// --- multiplier sets -----------------------------------------------------------------------------------------
// Bit m set = the backend offers m X (displayFgMultiplierSelectable); built by the glue, so this header needs no
// engine type.
using PushFgMask = std::uint32_t;
constexpr bool pushFgMaskHas(PushFgMask mask, unsigned m) { return m < 32 && ((mask >> m) & 1u) != 0; }

// The display hint's tolerance (DisplayFgBudget.h kDisplayFgFitTolerance): an output 2 % under the cap counts as
// reaching it, so a 59.94 stream at 3X "fills" a 179.95 Hz cap.
inline constexpr double kPushSmoothFit = 1.02;

// --- the output-cap reduction with a tolerance --------------------------------------------------------------
// The smallest offered multiplier in [2, ceiling] whose output content x m reaches cap / 1.02; none reaches it: the
// largest offered one at or below the ceiling. Unknown rates: the ceiling (the caller then keeps its own rule).
// The existing rule (EngineController) is ceil(cap / content) clamped to [2, ceiling].
inline unsigned pushSmoothCapMultiplier(unsigned ceiling, double contentFps, double capFps, PushFgMask offered) {
    if (ceiling < 2) return ceiling;
    if (!std::isfinite(contentFps) || !std::isfinite(capFps) || contentFps <= 0.01 || capFps <= 0.0) return ceiling;
    unsigned largest = 0;
    for (unsigned m = 2; m <= ceiling; ++m) {
        if (!pushFgMaskHas(offered, m)) continue;
        if (contentFps * m * kPushSmoothFit >= capFps) return m;
        largest = m;
    }
    return largest ? largest : ceiling;
}

// --- reasons ------------------------------------------------------------------------------------------------
// Machine-readable reasons of Smoothness Mode (log `smoothReason=`, bridge pushSmoothState().reason, QML text).
enum class PushSmoothReason : std::uint8_t {
    Off = 0, Start, Target, Hold, GpuHeadroom, RejectedPairs, Recovered, AtCeiling, TargetBelow2x, Unmeasured,
};
constexpr std::string_view pushSmoothReasonName(PushSmoothReason r) {
    switch (r) {
    case PushSmoothReason::Off: return "off";
    case PushSmoothReason::Start: return "start";
    case PushSmoothReason::Target: return "target";
    case PushSmoothReason::Hold: return "hold";
    case PushSmoothReason::GpuHeadroom: return "step-down-gpu-headroom";
    case PushSmoothReason::RejectedPairs: return "step-down-rejected-pairs";
    case PushSmoothReason::Recovered: return "step-up-headroom-recovered";
    case PushSmoothReason::AtCeiling: return "at-ceiling";
    case PushSmoothReason::TargetBelow2x: return "target-below-2x";
    case PushSmoothReason::Unmeasured: return "unmeasured";
    }
    return "invalid";
}
// The target choices the interface offers (0 = off). Anything else is off.
constexpr bool pushSmoothTargetValid(int fps) { return fps == 60 || fps == 90 || fps == 120 || fps == 180; }

// --- Smoothness Mode -----------------------------------------------------------------------------------------
struct PushSmoothConfig {
    std::int64_t tick100ns = 2500000;          // decisions at most every 250 ms
    unsigned troubleTicks = 4;                 // trouble on 4 ticks in a row (1 s) before a step down: never one sample
    std::int64_t gate100ns = 5000000;          // a higher multiplier the target asks for (content rate fell) after 0.5 s
    std::int64_t upHold100ns = 50000000;       // 5 s of headroom at the level above before a step up after trouble
    std::int64_t upLockout100ns = 100000000;   // and at least 10 s after the last step down
    double highLoad = 0.92;                    // predicted GPU time / pair interval above this: trouble
    double lowLoad = 0.75;                     // the level above predicted under this: room to step up
    double rejectedShare = 0.10;               // more than 10 % of the tick's pairs rejected by admission: trouble
};

// GPU time of one generation group by how many frames it generates (multiplier - 1), kept per level as a low-weight
// moving average (like the engine's own vfgChosenFgMs). A level not measured yet is predicted from the two measured
// levels nearest to it as a fixed part (the optical flow, once per pair) plus a per-frame part; from one measured
// level as all per-frame (cost x k / j: too high above it, which only delays a step up). -1: nothing measured.
class PushSmoothCostModel {
public:
    static constexpr unsigned kLevels = 32;
    PushSmoothCostModel() { reset(); }
    void reset() { ms_.fill(-1.0); }
    void record(unsigned generated, double ms) {
        if (generated == 0 || generated >= kLevels || !std::isfinite(ms) || ms < 0) return;
        double& c = ms_[generated];
        c = c >= 0 ? c + 0.1 * (ms - c) : ms;
    }
    double measured(unsigned generated) const { return generated < kLevels ? ms_[generated] : -1.0; }
    double predict(unsigned generated) const {
        if (generated == 0) return 0.0;
        if (generated >= kLevels) return -1.0;
        if (ms_[generated] >= 0) return ms_[generated];
        unsigned first = 0, second = 0;   // the two measured levels nearest to `generated`
        for (unsigned j = 1; j < kLevels; ++j) {
            if (ms_[j] < 0) continue;
            const auto distance = [generated](unsigned level) { return level > generated ? level - generated : generated - level; };
            if (!first || distance(j) < distance(first)) { second = first; first = j; }
            else if (!second || distance(j) < distance(second)) second = j;
        }
        if (!first) return -1.0;
        if (!second) return ms_[first] * double(generated) / double(first);
        const unsigned lo = std::min(first, second), hi = std::max(first, second);
        const double perFrame = std::max(0.0, (ms_[hi] - ms_[lo]) / double(hi - lo));
        const double fixed = std::max(0.0, ms_[lo] - perFrame * double(lo));
        return fixed + perFrame * double(generated);
    }

private:
    std::array<double, kLevels> ms_{};
};

struct PushSmoothInput {
    double targetFps = 0;          // the chosen displayed rate (60/90/120/180)
    double contentFps = 0;         // new pictures per second (the engine's pushContentFps)
    double capFps = 0;             // output cap in force (Follow display = the refresh); 0 = none
    double refreshHz = 0;          // 0 = unknown
    unsigned ceiling = 0;          // the multiplier the graph was built for (the user's)
    PushFgMask offered = 0;        // multipliers the backend offers
    double baseMs = -1;            // GPU time of a pair without generation (FgRecoveryBudget::baseCost), -1 unknown
    const PushSmoothCostModel* costs = nullptr;   // generation group costs by level; null = unknown
    double blitMs = 0;             // presenter's blit P95
    std::uint64_t admitted = 0, rejected = 0;  // cumulative admission counts of this session
    std::int64_t now100ns = 0;
};
struct PushSmoothDecision {
    unsigned multiplier = 0;       // to use for this pair (0 = the mode does not apply: keep the existing rules)
    unsigned preferred = 0;        // what the target alone asks for
    PushSmoothReason reason = PushSmoothReason::Off;
    bool changed = false;          // the multiplier changed on this call (log it)
    bool lowered = false;          // ... downwards (show the reason)
    double load = -1;              // predicted GPU load at `multiplier` (model), -1 unknown
};

// The multiplier the target asks for: the largest offered m in [2, ceiling] with content x m <= target x 1.02, where
// the target is also bounded by the cap and the refresh (a frame above them is thrown away). 0 when even 2X is above
// it (the source already fills the target).
inline unsigned pushSmoothPreferred(double targetFps, double contentFps, double capFps, double refreshHz, unsigned ceiling, PushFgMask offered) {
    if (!std::isfinite(targetFps) || !std::isfinite(contentFps) || targetFps <= 0 || contentFps <= 0.01) return 0;
    double limit = targetFps;
    if (std::isfinite(capFps) && capFps > 0) limit = std::min(limit, capFps);
    if (std::isfinite(refreshHz) && refreshHz > 0) limit = std::min(limit, refreshHz);
    unsigned best = 0;
    for (unsigned m = 2; m <= ceiling && m < 32; ++m)
        if (pushFgMaskHas(offered, m) && contentFps * m <= limit * kPushSmoothFit) best = m;
    return best;
}
// Predicted share of the pair interval the GPU needs at multiplier m (a model: base + the group's predicted cost +
// blit). -1 when the costs are not measured.
inline double pushSmoothLoad(unsigned m, double contentFps, double baseMs, const PushSmoothCostModel* costs, double blitMs) {
    if (m < 2 || !std::isfinite(contentFps) || !std::isfinite(baseMs) || contentFps <= 0.01 || baseMs < 0 || !costs) return -1;
    const double group = costs->predict(m - 1);
    if (group < 0 || !std::isfinite(group)) return -1;
    const double load = (baseMs + group + (std::isfinite(blitMs) ? std::max(0.0, blitMs) : 0.0)) / (1000.0 / contentFps);
    return std::isfinite(load) ? load : -1;
}

// Two limits decide the multiplier: what the target asks for (`want`, from the content rate) and, after sustained
// trouble, a GPU limit kept per `want` (a level that failed at one content rate is not retried after each 30 <-> 60
// flip; another `want` has its own). A lower `want` applies at once; a higher one after a 0.5 s gate, never waiting
// for modelled headroom (the 30 -> 60 -> 30 content case). Only a level the GPU limit holds down needs headroom to
// come back: predicted load under 0.75 for 5 s, or, with no costs measured, 5 s without trouble; 10 s after the last
// step down at the earliest.
class PushSmoothnessMode {
public:
    explicit PushSmoothnessMode(PushSmoothConfig c = {}) : c_(c) {}
    void reset() { const PushSmoothConfig keep = c_; *this = PushSmoothnessMode(keep); }
    unsigned applied() const { return applied_; }
    unsigned troubleCap() const { return lastWant_ < kLimits ? capFor_[lastWant_] : 0u; }
    PushSmoothReason reason() const { return reason_; }
    std::uint64_t stepDowns() const { return stepDowns_; }
    std::uint64_t stepUps() const { return stepUps_; }

    PushSmoothDecision update(const PushSmoothInput& in) {
        PushSmoothDecision d;
        if (!pushSmoothTargetValid(static_cast<int>(std::lround(in.targetFps))) || in.ceiling < 2) {
            if (applied_) reset();
            return d;  // off: multiplier 0, the existing rules apply
        }
        const unsigned preferred = pushSmoothPreferred(in.targetFps, in.contentFps, in.capFps, in.refreshHz, in.ceiling, in.offered);
        d.preferred = preferred;
        if (!std::isfinite(in.contentFps) || in.contentFps <= 0.01) {
            // No content rate yet: keep what is applied, or wait (the existing rules) until there is one.
            d.multiplier = applied_;
            d.reason = applied_ ? reason_ : PushSmoothReason::Unmeasured;
            return d;
        }
        const unsigned floorM = lowestOffered(in);
        const unsigned want = std::min(preferred ? preferred : floorM, kLimits - 1);
        lastWant_ = want;
        const unsigned cap = capFor_[want];   // the GPU limit learnt for this `want` (0 = none)
        const PushSmoothReason wantReason = preferred ? PushSmoothReason::Target : PushSmoothReason::TargetBelow2x;
        if (!applied_) {
            set(d, want, preferred ? PushSmoothReason::Start : PushSmoothReason::TargetBelow2x);
            lastTick_ = in.now100ns;
            lastAdmitted_ = in.admitted;
            lastRejected_ = in.rejected;
        } else {
            const unsigned limit = cap ? std::min(want, cap) : want;
            if (limit < applied_) {
                // The target (or the content rate) now asks for less: follow at once (less work, less delay).
                set(d, limit, wantReason);
                d.lowered = true;
                troubleTicks_ = 0;
                pendingUp_ = 0;
            } else if (limit > applied_) {
                // The target asks for more again (the content rate fell): after the gate, no headroom needed.
                if (pendingUp_ != limit) { pendingUp_ = limit; pendingSince_ = in.now100ns; }
                else if (in.now100ns - pendingSince_ >= c_.gate100ns) { set(d, limit, wantReason); pendingUp_ = 0; }
            } else {
                pendingUp_ = 0;
            }
            if (in.now100ns - lastTick_ >= c_.tick100ns) tick(d, in, want, floorM);
        }
        if (capFor_[want] >= want) capFor_[want] = 0;   // the GPU limit no longer binds
        d.multiplier = applied_;
        // A start, target or recovery reason belongs to the call that changed the multiplier; afterwards the state
        // says whether the mode is at what the target asks for or holding below it. A step-down reason stays while the
        // GPU limit holds the multiplier down, so the interface can show why it is lower.
        if (!d.changed && (reason_ == PushSmoothReason::Start || reason_ == PushSmoothReason::Target || reason_ == PushSmoothReason::Recovered))
            reason_ = applied_ >= want ? PushSmoothReason::AtCeiling : PushSmoothReason::Hold;
        d.reason = reason_;
        d.load = pushSmoothLoad(applied_, in.contentFps, in.baseMs, in.costs, in.blitMs);
        return d;
    }

private:
    static unsigned lowestOffered(const PushSmoothInput& in) {
        for (unsigned m = 2; m <= in.ceiling && m < 32; ++m)
            if (pushFgMaskHas(in.offered, m)) return m;
        return in.ceiling;
    }
    static unsigned below(const PushSmoothInput& in, unsigned m) {
        for (unsigned k = m - 1; k >= 2; --k)
            if (pushFgMaskHas(in.offered, k)) return k;
        return m;
    }
    static unsigned above(const PushSmoothInput& in, unsigned m, unsigned limit) {
        for (unsigned k = m + 1; k <= limit && k < 32; ++k)
            if (pushFgMaskHas(in.offered, k)) return k;
        return m;
    }
    void set(PushSmoothDecision& d, unsigned m, PushSmoothReason r) {
        d.changed = d.changed || m != applied_;
        applied_ = m;
        reason_ = r;
    }
    void tick(PushSmoothDecision& d, const PushSmoothInput& in, unsigned want, unsigned floorM) {
        lastTick_ = in.now100ns;
        const std::uint64_t admitted = in.admitted >= lastAdmitted_ ? in.admitted - lastAdmitted_ : 0;
        const std::uint64_t rejected = in.rejected >= lastRejected_ ? in.rejected - lastRejected_ : 0;
        lastAdmitted_ = in.admitted;
        lastRejected_ = in.rejected;
        const std::uint64_t pairs = admitted + rejected;
        const bool rejecting = pairs >= 4 && double(rejected) > c_.rejectedShare * double(pairs);
        const double load = pushSmoothLoad(applied_, in.contentFps, in.baseMs, in.costs, in.blitMs);
        const bool heavy = load > c_.highLoad;
        if (rejecting || heavy) {
            okSince_ = 0;
            if (++troubleTicks_ >= c_.troubleTicks && applied_ > floorM) {
                const unsigned next = below(in, applied_);
                if (next < applied_) {
                    set(d, next, rejecting ? PushSmoothReason::RejectedPairs : PushSmoothReason::GpuHeadroom);
                    d.lowered = true;
                    capFor_[want] = next;
                    pendingUp_ = 0;
                    ++stepDowns_;
                    lastDown_ = in.now100ns;
                }
                troubleTicks_ = 0;
            }
            return;
        }
        troubleTicks_ = 0;
        // Only a level the GPU limit holds below the target needs headroom to come back.
        if (!capFor_[want] || applied_ != capFor_[want] || applied_ >= want) { okSince_ = 0; return; }
        const unsigned up = above(in, applied_, want);
        if (up == applied_) { okSince_ = 0; return; }
        const double upLoad = pushSmoothLoad(up, in.contentFps, in.baseMs, in.costs, in.blitMs);
        if (upLoad > c_.lowLoad) { okSince_ = 0; return; }   // measured and too high (unmeasured: time without trouble decides)
        if (!okSince_) okSince_ = in.now100ns;
        if (in.now100ns - okSince_ < c_.upHold100ns || (lastDown_ && in.now100ns - lastDown_ < c_.upLockout100ns)) return;
        set(d, up, PushSmoothReason::Recovered);
        capFor_[want] = up >= want ? 0 : up;
        ++stepUps_;
        okSince_ = 0;
    }

    PushSmoothConfig c_;
    static constexpr unsigned kLimits = 32;
    std::array<unsigned, kLimits> capFor_{};   // per `want`: the level sustained trouble lowered it to (0 = none)
    unsigned applied_ = 0, lastWant_ = 0, troubleTicks_ = 0, pendingUp_ = 0;
    PushSmoothReason reason_ = PushSmoothReason::Off;
    std::int64_t lastTick_ = 0, okSince_ = 0, lastDown_ = 0, pendingSince_ = 0;
    std::uint64_t lastAdmitted_ = 0, lastRejected_ = 0, stepDowns_ = 0, stepUps_ = 0;
};

// --- process-wide state between the interface and the engine ------------------------------------------------
// The interface writes the target (pref pushSmoothTarget; 0 = off); the engine reads it once per pair and writes
// back what it did. Relaxed atomics: one value each, no ordering between them is needed.
struct PushSmoothShared {
    std::atomic<int> targetFps{0};
    std::atomic<unsigned> chosen{0}, preferred{0};
    std::atomic<std::uint8_t> reason{0};
    std::atomic<std::uint64_t> stepDowns{0};
    std::atomic<std::int64_t> loadPermille{-1};
    std::atomic<std::uint32_t> displayW{0}, displayH{0};   // the monitor live playback last used (SR_DISPLAY only)
};
inline PushSmoothShared& pushSmoothShared() {
    static PushSmoothShared shared;
    return shared;
}

// --- the [slipstream-smooth] window --------------------------------------------------------------------------
// GPU stages the line reports (indices into PushSmoothWindow's sample arrays).
enum class PushSmoothStage : std::uint8_t { Color, Clean, Sr, Nr, Flow, Fg, Blit, Span, Count };
inline constexpr std::array<std::string_view, 8> kPushSmoothStageNames{{"Color", "Clean", "Sr", "Nr", "Flow", "Fg", "Blit", "Span"}};

// Session totals the engine keeps (cumulative); the window reports their change per second.
struct PushSmoothCounters {
    std::uint64_t received = 0;        // frames that reached the engine loop (decoded stream frames)
    std::uint64_t unique = 0;          // of those, kept as new pictures (after duplicate removal)
    std::uint64_t evaluated = 0;       // pairs whose generation group was admitted
    std::uint64_t rejected = 0;        // pairs admission skipped whole (no generated frame)
    std::uint64_t fast = 0;            // pairs run with the faster VFG model (vfg-fast)
    std::uint64_t generated = 0;       // generated frames that came out valid
    std::uint64_t invalid = 0;         // generated frames marked invalid (duplicate, failed, provider-disabled)
    std::uint64_t dropCap = 0;         // generated frames dropped before Present: output-cap slot passed
    std::uint64_t dropDeadline = 0;    // ... deadline passed
    std::uint64_t dropCapacity = 0;    // ... no present capacity
    std::uint64_t resets = 0;          // temporal history resets
    std::uint64_t holds = 0;           // FgCostSettling holds started (generation off for 2-8 s)
};
struct PushSmoothConfigInfo {
    std::string backend = "none";
    unsigned ceiling = 1;              // the user's multiplier (1 = off)
    std::uint32_t sourceW = 0, sourceH = 0, workW = 0, workH = 0, flowW = 0, flowH = 0;
    std::uint32_t displayW = 0, displayH = 0;
    int srTarget = -1, srTargetUser = -1;   // SrTarget values in force / asked for (-1 = SR off)
    bool srFollow = false, capFit = false, steady = false;   // the switches
    bool sameConfiguration(const PushSmoothConfigInfo& o) const {
        return backend == o.backend && ceiling == o.ceiling && workW == o.workW && workH == o.workH && sourceW == o.sourceW && sourceH == o.sourceH;
    }
};

class PushSmoothWindow {
public:
    static constexpr std::size_t kMaxSamples = 1024;   // per stage and window (5 s at 180 groups/s is 900)
    static constexpr std::int64_t kWindow100ns = 50000000;
    PushSmoothWindow() {
        for (auto& v : samples_) v.reserve(kMaxSamples);
    }
    void restart(std::int64_t now100ns, const PushSmoothCounters& c) {
        start_ = now100ns;
        base_ = c;
        for (auto& v : samples_) v.clear();
        changes_ = 0;
        minM_ = 0;
        maxM_ = 0;
        spanSum_ = 0;
        contentSum_ = 0;
        contentN_ = 0;
        started_ = true;
    }
    bool started() const { return started_; }
    bool due(std::int64_t now100ns) const { return started_ && now100ns - start_ >= kWindow100ns; }
    void stage(PushSmoothStage s, double ms) {
        auto& v = samples_[static_cast<std::size_t>(s)];
        if (std::isfinite(ms) && ms >= 0 && v.size() < kMaxSamples) v.push_back(ms);
        if (s == PushSmoothStage::Span && std::isfinite(ms) && ms >= 0) spanSum_ += ms;
    }
    // One pair's multiplier choice (the multiplier actually used, and the content rate it was chosen for).
    void pair(unsigned multiplier, double contentFps) {
        if (lastM_ && multiplier != lastM_) ++changes_;
        lastM_ = multiplier;
        minM_ = minM_ ? std::min(minM_, multiplier) : multiplier;
        maxM_ = std::max(maxM_, multiplier);
        if (std::isfinite(contentFps) && contentFps > 0) { contentSum_ += contentFps; ++contentN_; }
    }
    unsigned lastMultiplier() const { return lastM_; }
    unsigned changes() const { return changes_; }
    unsigned minMultiplier() const { return minM_; }
    unsigned maxMultiplier() const { return maxM_; }
    double contentFps() const { return contentN_ ? contentSum_ / double(contentN_) : 0.0; }
    std::int64_t start() const { return start_; }
    const PushSmoothCounters& base() const { return base_; }
    double spanSum() const { return spanSum_; }
    // Nearest-rank percentile of one stage's samples (-1 without samples).
    double percentile(PushSmoothStage s, double p) const {
        std::vector<double> v = samples_[static_cast<std::size_t>(s)];
        if (v.empty()) return -1.0;
        const std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * double(v.size())));
        const std::size_t index = std::min(v.size() - 1, rank ? rank - 1 : 0);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(index), v.end());
        return v[index];
    }
    std::size_t samples(PushSmoothStage s) const { return samples_[static_cast<std::size_t>(s)].size(); }

private:
    std::array<std::vector<double>, static_cast<std::size_t>(PushSmoothStage::Count)> samples_;
    PushSmoothCounters base_;
    std::int64_t start_ = 0;
    unsigned changes_ = 0, minM_ = 0, maxM_ = 0, lastM_ = 0;
    double spanSum_ = 0, contentSum_ = 0;
    std::uint64_t contentN_ = 0;
    bool started_ = false;
};

struct PushSmoothLineInputs {
    double capFps = 0, refreshHz = 0;       // 0 = none / unknown
    int vrr = -1;                            // pushVrrCadenceState: 1 / 0 / -1
    int smoothTarget = 0;                    // Smoothness Mode target (0 = off)
    PushSmoothReason smoothReason = PushSmoothReason::Off;
    std::uint64_t smoothDowns = 0, smoothUps = 0;
    std::string why = "user";                // what chose the last multiplier: user | cap | cap-fit | adaptive | smooth
};

namespace detail_smooth {
inline void appendf(std::string& out, const char* format, double value) {
    char buffer[48];
    const int n = std::snprintf(buffer, sizeof buffer, format, value);
    if (n > 0) out.append(buffer, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof buffer) - 1)));
}
inline std::string extent(std::uint32_t w, std::uint32_t h) { return w && h ? std::to_string(w) + "x" + std::to_string(h) : std::string("-"); }
inline std::uint64_t delta(std::uint64_t now, std::uint64_t then) { return now >= then ? now - then : 0; }
}

// The line itself (without the "[slipstream-smooth]" tag the logger adds). Rates are per second over the window;
// unknown values are -1; GPU times in ms. gpuLoadPct is a MODEL (sum of graph spans over the window), not the
// driver's utilisation.
inline std::string pushSmoothLine(const PushSmoothWindow& w, const PushSmoothConfigInfo& info, const PushSmoothCounters& now,
                                  std::int64_t now100ns, const PushSmoothLineInputs& x) {
    using namespace detail_smooth;
    const double seconds = double(now100ns - w.start()) / 1e7;
    const double perSecond = seconds > 0.001 ? 1.0 / seconds : 0.0;
    const PushSmoothCounters& b = w.base();
    const auto rate = [&](std::uint64_t value, std::uint64_t then) { return double(delta(value, then)) * perSecond; };
    std::string s;
    appendf(s, "window=%.1f", seconds);
    s += " backend=" + info.backend + " ceiling=" + std::to_string(info.ceiling) + "X";
    s += " chosen=" + std::to_string(w.lastMultiplier()) + "X chosenMin=" + std::to_string(w.minMultiplier()) + "X chosenMax=" +
         std::to_string(w.maxMultiplier()) + "X changes=" + std::to_string(w.changes()) + " why=" + x.why;
    appendf(s, " contentFps=%.2f", w.contentFps());
    appendf(s, " capFps=%.2f", x.capFps > 0 ? x.capFps : -1.0);
    appendf(s, " refreshHz=%.3f", x.refreshHz > 0 ? x.refreshHz : -1.0);
    s += " vrr=" + std::to_string(x.vrr);
    s += " source=" + extent(info.sourceW, info.sourceH) + " work=" + extent(info.workW, info.workH) +
         " flow=" + extent(info.flowW, info.flowH) + " display=" + extent(info.displayW, info.displayH);
    s += " srTarget=" + std::to_string(info.srTarget) + " srTargetAsked=" + std::to_string(info.srTargetUser);
    appendf(s, " receivedFps=%.2f", rate(now.received, b.received));
    appendf(s, " uniqueFps=%.2f", rate(now.unique, b.unique));
    appendf(s, " evaluatedPairsFps=%.2f", rate(now.evaluated, b.evaluated));
    appendf(s, " rejectedPairsFps=%.2f", rate(now.rejected, b.rejected));
    appendf(s, " fastPairsFps=%.2f", rate(now.fast, b.fast));
    appendf(s, " generatedFps=%.2f", rate(now.generated, b.generated));
    appendf(s, " invalidFps=%.2f", rate(now.invalid, b.invalid));
    appendf(s, " dropCapFps=%.2f", rate(now.dropCap, b.dropCap));
    appendf(s, " dropDeadlineFps=%.2f", rate(now.dropDeadline, b.dropDeadline));
    appendf(s, " dropCapacityFps=%.2f", rate(now.dropCapacity, b.dropCapacity));
    s += " resets=" + std::to_string(delta(now.resets, b.resets)) + " holds=" + std::to_string(delta(now.holds, b.holds));
    for (std::size_t i = 0; i < static_cast<std::size_t>(PushSmoothStage::Count); ++i) {
        const auto st = static_cast<PushSmoothStage>(i);
        const std::string name(kPushSmoothStageNames[i]);
        s += " gpu" + name + "P50Ms=";
        appendf(s, "%.3f", w.percentile(st, 50));
        s += " gpu" + name + "P95Ms=";
        appendf(s, "%.3f", w.percentile(st, 95));
    }
    s += " gpuFrames=" + std::to_string(w.samples(PushSmoothStage::Span));
    const double content = w.contentFps();
    appendf(s, " pairBudgetMs=%.3f", content > 0.01 ? 1000.0 / content : -1.0);
    appendf(s, " gpuLoadPct=%.1f", seconds > 0.001 ? w.spanSum() / (seconds * 1000.0) * 100.0 : -1.0);
    s += " smooth=" + std::to_string(x.smoothTarget) + " smoothReason=" + std::string(pushSmoothReasonName(x.smoothReason)) +
         " stepDowns=" + std::to_string(x.smoothDowns) + " stepUps=" + std::to_string(x.smoothUps);
    s += std::string(" srFollow=") + (info.srFollow ? "1" : "0") + " capFit=" + (info.capFit ? "1" : "0") + " steady=" + (info.steady ? "1" : "0");
    s += " (counts before Present; what reached the panel: [slipstream-cadence] / PresentMon; gpuLoadPct is a model)";
    return s;
}
}  // namespace veyra::engine
