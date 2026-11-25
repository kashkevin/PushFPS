// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine {
// Super-resolution measurement and experiments (sprint 4, stream "sr"). Portable and header-only; tests in
// tests/remoteplay/SrPlanTests.inc. docs/SUPER_RESOLUTION.md explains what the super-resolution stage does
// and why these pieces exist. Nothing here has run on a GPU: the thresholds are starting points.
//
//   PushSrWindow       GPU milliseconds of the SR stage over one statistics window (P50/P95/max).
//   pushSrStatsLine    the `[slipstream-sr]` line (S4_CONTRACT.md) for one window.
//   pushSrEffectiveLevel / pushSrLevelRequest
//                      the RTX Video SR level the graph passes to the SDK: the autorun `sr` step's request,
//                      then VEYRA_EXP_SR_QUALITY, then VEYRA_EXP_SR_BUDGET_MS's level, then the user's level.
//                      The level is an evaluate-time parameter of the SDK feature (VideoSrBackend::evaluate),
//                      so changing it rebuilds nothing (code reading; unverified on Windows).
//   PushSrBudget       VEYRA_EXP_SR_BUDGET_MS: steps the level down when the stage's P95 stays over the budget,
//                      back up slowly, never above the user's level.
//   pushSrPlan / pushFgPlan
//                      the holds of the autorun `sr:<s>` and `fg:<s>` steps, and what to put back afterwards.

// --- the RTX Video SR level -------------------------------------------------------------------------------
inline constexpr std::uint32_t kPushSrLevelMin = 1, kPushSrLevelMax = 4;
constexpr bool pushSrLevelValid(std::uint32_t level) { return level >= kPushSrLevelMin && level <= kPushSrLevelMax; }

// Where the level in force came from (the `levelSource=` key of the statistics line).
enum class PushSrLevelSource : std::uint8_t { User, Autorun, Switch, Budget };
constexpr std::string_view pushSrLevelSourceName(PushSrLevelSource s) {
    switch (s) {
    case PushSrLevelSource::User: return "user";
    case PushSrLevelSource::Autorun: return "autorun";
    case PushSrLevelSource::Switch: return "switch";
    case PushSrLevelSource::Budget: return "budget";
    }
    return "unknown";
}
struct PushSrLevel {
    std::uint32_t level = 0;
    PushSrLevelSource source = PushSrLevelSource::User;
};
// configured: the level the settings hold (the standard RTX Video SR family, 1..4). Anything else (DLSS SR,
// FSR, NIS, the Video Effects families, double SR) is returned unchanged: the experiments only touch the
// single standard RTX Video SR pass. request: the autorun step (0 = none); fixed: VEYRA_EXP_SR_QUALITY
// (0 = unset); budget: PushSrBudget's level (0 = none), never above the configured level.
constexpr PushSrLevel pushSrEffectiveLevel(std::uint32_t configured, std::uint32_t request, std::uint32_t fixed,
                                           std::uint32_t budget) {
    if (!pushSrLevelValid(configured)) return {configured, PushSrLevelSource::User};
    if (pushSrLevelValid(request)) return {request, PushSrLevelSource::Autorun};
    if (pushSrLevelValid(fixed)) return {fixed, PushSrLevelSource::Switch};
    if (pushSrLevelValid(budget) && budget < configured) return {budget, PushSrLevelSource::Budget};
    return {configured, PushSrLevelSource::User};
}
// The autorun `sr` step's level request, shared by the interface thread (writer) and the graph (reader, once
// per real frame). 0 = no request. One instance per process (an inline function's static).
inline std::atomic<std::uint32_t>& pushSrLevelRequest() {
    static std::atomic<std::uint32_t> request{0};
    return request;
}
// "1".."4" -> that level; anything else (unset, empty, "0", "5", "2x") -> 0 = no override.
constexpr std::uint32_t pushSrLevelSwitch(std::string_view text) {
    if (text.size() != 1) return 0;
    const char c = text[0];
    return c >= '1' && c <= '4' ? std::uint32_t(c - '0') : 0u;
}
// VEYRA_EXP_SR_BUDGET_MS: a decimal number of milliseconds, 0.5..50; anything else -> 0 = off.
inline double pushSrBudgetSwitch(std::string_view text) {
    if (text.empty() || text.size() > 8) return 0.0;
    double whole = 0.0, scale = 0.0;
    bool digits = false;
    for (const char c : text) {
        if (c == '.') {
            if (scale != 0.0) return 0.0;
            scale = 1.0;
            continue;
        }
        if (c < '0' || c > '9') return 0.0;
        digits = true;
        if (scale == 0.0) whole = whole * 10.0 + double(c - '0');
        else { scale /= 10.0; whole += double(c - '0') * scale; }
    }
    if (!digits || whole < 0.5 || whole > 50.0) return 0.0;
    return whole;
}

// --- one statistics window ---------------------------------------------------------------------------------
// Fixed storage, no allocation after construction: at most kCapacity samples per window (a 5 s window at
// 240 real frames a second is 1200); later samples still count in frames() and max() but not in percentiles.
class PushSrWindow {
public:
    static constexpr std::size_t kCapacity = 2048;
    void add(double ms) {
        if (!(ms >= 0.0) || ms > 1.0e4) { ++rejected_; return; }
        ++frames_;
        max_ = std::max(max_, ms);
        if (count_ < kCapacity) samples_[count_++] = ms;
    }
    std::uint64_t frames() const { return frames_; }
    std::uint64_t rejected() const { return rejected_; }
    double max() const { return frames_ ? max_ : -1.0; }
    // Nearest-rank percentile (0..100) of the stored samples; -1 with none.
    double percentile(double p) const {
        if (!count_) return -1.0;
        std::array<double, kCapacity> sorted{};
        std::copy(samples_.begin(), samples_.begin() + std::ptrdiff_t(count_), sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + std::ptrdiff_t(count_));
        const double clamped = std::clamp(p, 0.0, 100.0);
        std::size_t rank = std::size_t(clamped / 100.0 * double(count_) + 0.999999);
        if (rank < 1) rank = 1;
        if (rank > count_) rank = count_;
        return sorted[rank - 1];
    }
    void clear() { count_ = 0; frames_ = 0; rejected_ = 0; max_ = 0.0; }
private:
    std::array<double, kCapacity> samples_{};
    std::size_t count_ = 0;
    std::uint64_t frames_ = 0, rejected_ = 0;
    double max_ = 0.0;
};

// What the window measured. choice = the stored SR choice (EnhancementSettings::videoSrQuality: 0 DLSS SR,
// 1..4 RTX Video SR, 5 FSR, 6 NIS, 16..23 RTX Video SR through Video Effects); quality = the RTX Video SR
// level evaluated (1..4), -1 for every other choice.
struct PushSrStatsInfo {
    int choice = -1;
    int quality = -1;
    std::uint32_t inW = 0, inH = 0, outW = 0, outH = 0, workW = 0, workH = 0;
    bool generatedToo = false;
    bool hdr = false;
    bool dual = false;
    PushSrLevelSource levelSource = PushSrLevelSource::User;
    double budgetMs = 0.0;
    bool equals(const PushSrStatsInfo& o) const {
        return choice == o.choice && quality == o.quality && inW == o.inW && inH == o.inH && outW == o.outW && outH == o.outH &&
               workW == o.workW && workH == o.workH && generatedToo == o.generatedToo && hdr == o.hdr && dual == o.dual &&
               levelSource == o.levelSource;
    }
};
// The `[slipstream-sr]` keys (S4_CONTRACT.md), then extra keys at the end: gpuMax, work (the size every later
// effect and the presenter's input have), hdr, dual, levelSource, budgetMs (0 = off).
inline std::string pushSrStatsLine(const PushSrWindow& w, const PushSrStatsInfo& i) {
    char text[384];
    const auto ms = [](double v) { return v < 0.0 ? -1.0 : v; };
    std::snprintf(text, sizeof text,
                  "choice=%d quality=%d in=%ux%u out=%ux%u gpuP50=%.3f gpuP95=%.3f frames=%llu generatedToo=%d "
                  "gpuMax=%.3f work=%ux%u hdr=%d dual=%d levelSource=%.*s budgetMs=%.2f",
                  i.choice, i.quality, unsigned(i.inW), unsigned(i.inH), unsigned(i.outW), unsigned(i.outH),
                  ms(w.percentile(50.0)), ms(w.percentile(95.0)), static_cast<unsigned long long>(w.frames()),
                  i.generatedToo ? 1 : 0, ms(w.max()), unsigned(i.workW), unsigned(i.workH), i.hdr ? 1 : 0, i.dual ? 1 : 0,
                  int(pushSrLevelSourceName(i.levelSource).size()), pushSrLevelSourceName(i.levelSource).data(), i.budgetMs);
    return text;
}

// --- VEYRA_EXP_SR_BUDGET_MS ----------------------------------------------------------------------------------
struct PushSrBudgetConfig {
    double budgetMs = 0.0;                 // the stage's allowed P95 GPU time; 0 = off
    unsigned overWindows = 2;              // windows in a row over the budget before one level down
    unsigned underWindows = 6;             // windows in a row under underShare x budget (or, after the full
                                           // lockout, under the budget) before one level up
    double underShare = 0.6;
    std::uint64_t minFrames = 30;          // a window with fewer measured frames is not evidence
    std::int64_t minChange100ns = 100000000;   // 10 s between two changes
    std::int64_t upLockout100ns = 600000000;   // 60 s after a step down before any step up (base value)
    // Back-off: a step up taken back within upLockout100ns (the level it reached went over the budget again)
    // doubles the lockout before the next step up, up to 2^maxBackoffShift times (16x = 16 min); a step up that
    // holds for as long as the lockout before it clears the back-off. The latency governor
    // (PushGovernorControl.h) backs off its returns the same way.
    unsigned maxBackoffShift = 4;
};
// One decision per statistics window. The level is an evaluate-time parameter (no graph rebuild, code
// reading), so a 10 s spacing is enough; if a hardware run shows a rebuild after all, raise minChange100ns to
// a minute. At level 1 and still over budget it says so (overAtFloor) and changes nothing: switching the
// choice off or to NIS is the user's decision. A cost just over the budget at one level and well under it at
// the next would otherwise cycle about once a minute (the review's model: 103 level changes an hour, each cycle
// 10 s over budget); the back-off stretches that (tests sr_budget_backoff_*).
class PushSrBudget {
public:
    explicit PushSrBudget(PushSrBudgetConfig c = {}) : config_(c) {}
    const PushSrBudgetConfig& config() const { return config_; }
    bool enabled() const { return config_.budgetMs > 0.0; }
    // 0 = the user's level is in force.
    std::uint32_t level() const { return level_; }
    bool overAtFloor() const { return overAtFloor_; }
    unsigned changes() const { return changes_; }
    // The current back-off shift (0 = the base lockout) and the lockout before the next step up.
    unsigned backoff() const { return backoff_; }
    std::int64_t upLockout100ns() const { return config_.upLockout100ns << backoff_; }
    // Returns true when the level changed. configured = the user's level (1..4); anything else resets.
    bool observe(double p95Ms, std::uint64_t frames, std::uint32_t configured, std::int64_t now100ns) {
        overAtFloor_ = false;
        if (!enabled() || !pushSrLevelValid(configured)) { resetTo(0); configured_ = configured; return false; }
        if (configured != configured_) { configured_ = configured; resetTo(0); }   // the user chose again: new ceiling
        if (frames < config_.minFrames || p95Ms < 0.0) return false;
        const std::uint32_t current = level_ ? level_ : configured;
        const bool spaced = !changedAt_ || now100ns - changedAt_ >= config_.minChange100ns;
        // A step up that held as long as the lockout before it: the back-off starts again.
        if (upAt_ && now100ns - upAt_ >= upLockout100ns()) { upAt_ = 0; backoff_ = 0; }
        if (p95Ms > config_.budgetMs) {
            under_ = 0;
            ++over_;
            if (current <= kPushSrLevelMin) { overAtFloor_ = true; return false; }
            if (over_ >= config_.overWindows && spaced) {
                // Taking back the last step up soon after it: wait twice as long before the next one.
                if (upAt_ && current == upTo_ && now100ns - upAt_ < config_.upLockout100ns)
                    backoff_ = std::min(backoff_ + 1u, config_.maxBackoffShift);
                upAt_ = 0;
                set(current - 1, configured, now100ns);
                downAt_ = now100ns;
                return true;
            }
            return false;
        }
        over_ = 0;
        // Under underShare x budget, the windows count towards a step up at any time. Under the budget only, they
        // count once the full lockout (with its back-off) has passed: one level up is then tried even when the
        // lower level costs more than underShare x budget, because the higher one may still fit (the delta2
        // review's model: level 3 at 2.0 ms, level 4 at 2.8 ms, budget 3 ms sat at level 3 for 6 hours). A try
        // that goes over the budget again is taken back and doubles the lockout as above.
        const bool unlocked = !downAt_ || now100ns - downAt_ >= upLockout100ns();
        if (p95Ms <= config_.budgetMs * config_.underShare || unlocked) {
            ++under_;
            if (current < configured && under_ >= config_.underWindows && spaced && unlocked) {
                set(current + 1, configured, now100ns);
                upAt_ = now100ns;
                upTo_ = current + 1;
                return true;
            }
            return false;
        }
        under_ = 0;
        return false;
    }
private:
    void set(std::uint32_t level, std::uint32_t configured, std::int64_t now100ns) {
        level_ = level >= configured ? 0u : level;
        changedAt_ = now100ns;
        over_ = under_ = 0;
        ++changes_;
    }
    void resetTo(std::uint32_t level) {
        level_ = level; over_ = under_ = 0; changedAt_ = downAt_ = upAt_ = 0; upTo_ = 0; backoff_ = 0;
    }
    PushSrBudgetConfig config_;
    std::uint32_t level_ = 0, configured_ = 0, upTo_ = 0;
    unsigned over_ = 0, under_ = 0, changes_ = 0, backoff_ = 0;
    std::int64_t changedAt_ = 0, downAt_ = 0, upAt_ = 0;
    bool overAtFloor_ = false;
};

// One statistics window into the budget; the budget's level is then published to `shared` after EVERY observe,
// whether it changed or not: observe also resets the level when the user chooses another one, which it reports
// as no change (the review found the shared value stale: budget 4 -> 2, then the user chose 3 on an idle GPU and
// level 2 stayed in force). Returns observe's result (true = a step, for the event line). Used by
// EnhanceGraph::noteSrTimings.
inline bool pushSrBudgetObserve(PushSrBudget& budget, std::atomic<std::uint32_t>& shared, double p95Ms, std::uint64_t frames,
                                std::uint32_t configured, std::int64_t now100ns) {
    const bool changed = budget.observe(p95Ms, frames, configured, now100ns);
    shared.store(budget.level(), std::memory_order_relaxed);
    return changed;
}

// --- the autorun `sr:<seconds>` step -------------------------------------------------------------------------
// The user's super-resolution settings, put back after the step (and after any failure).
struct PushSrUserState {
    bool enabled = false;
    std::uint32_t quality = 2;       // EnhancementSettings::videoSrQuality
    int targetIndex = 2;             // QmlPlayerBridge::srTargetIndex (1 2560x1440, 2 3840x2160, ...)
    bool dual = false;
};
struct PushSrAvailable {
    bool rtxStandard = false;        // capability "sr4" (the standard RTX Video SR family)
    bool nis = false;                // capability "sr6"
    bool fsr = false;                // capability "sr5"
    int displayTargetIndex = 0;      // the smallest SR target at least as large as the window; 0 = unknown
};
// One hold: the settings in force while it lasts. levelRequest goes to pushSrLevelRequest() (0 = none).
struct PushSrHold {
    std::string label;
    bool enabled = false;
    std::uint32_t quality = 0;
    int targetIndex = 0;             // 0 = keep the user's target
    std::uint32_t levelRequest = 0;
};
// Order: off, NIS, FSR, then RTX Video SR level 1..4 at the user's target, then (when the window is smaller
// than the user's target) level 1..4 at the window's size. Holds that need the same settings follow each other
// so that only off/NIS/FSR/the target change rebuild the graph; the levels do not (evaluate-time parameter).
// Double SR is never touched (the user's double SR is put back at the end like everything else).
inline std::vector<PushSrHold> pushSrPlan(const PushSrUserState& user, const PushSrAvailable& a) {
    std::vector<PushSrHold> holds;
    const int userTarget = user.targetIndex >= 1 && user.targetIndex <= 6 ? user.targetIndex : 2;
    holds.push_back({"off", false, user.quality, 0, 0});
    if (a.nis) holds.push_back({"nis", true, 6u, userTarget, 0});
    if (a.fsr) holds.push_back({"fsr", true, 5u, userTarget, 0});
    if (a.rtxStandard) {
        // The standard family's stored value is the level itself; the request pins the level evaluated.
        for (std::uint32_t level = kPushSrLevelMin; level <= kPushSrLevelMax; ++level)
            holds.push_back({"rtx" + std::to_string(level), true, 2u, userTarget, level});
        if (a.displayTargetIndex >= 1 && a.displayTargetIndex <= 6 && a.displayTargetIndex != userTarget)
            for (std::uint32_t level = kPushSrLevelMin; level <= kPushSrLevelMax; ++level)
                holds.push_back({"rtx" + std::to_string(level) + "-window", true, 2u, a.displayTargetIndex, level});
    }
    return holds;
}
// SR target index (QmlPlayerBridge::srTargetIndex) of the smallest target that covers a window of w x h;
// 0 when nothing does or the size is unknown. Targets: 1 2560x1440, 2 3840x2160, 4 5120x2880, 5 6144x3456,
// 6 7168x4032, 3 7680x4320.
constexpr int pushSrTargetForWindow(std::uint32_t w, std::uint32_t h) {
    if (!w || !h) return 0;
    struct T { int index; std::uint32_t w, h; };
    constexpr T targets[] = {{1, 2560, 1440}, {2, 3840, 2160}, {4, 5120, 2880}, {5, 6144, 3456}, {6, 7168, 4032}, {3, 7680, 4320}};
    for (const auto& t : targets)
        if (t.w >= w && t.h >= h) return t.index;
    return 0;
}

// --- the autorun `fg:<seconds>` step -------------------------------------------------------------------------
// Holds: frame generation off (1), then each multiplier the current method offers, in order; afterwards the
// user's multiplier (1 = off) is put back. Empty when the method offers nothing above 1.
inline std::vector<std::uint32_t> pushFgPlan(const std::vector<std::uint32_t>& choices) {
    std::vector<std::uint32_t> holds;
    for (const auto m : choices)
        if (m >= 2 && std::find(holds.begin(), holds.end(), m) == holds.end()) holds.push_back(m);
    std::sort(holds.begin(), holds.end());
    if (holds.empty()) return holds;
    holds.insert(holds.begin(), 1u);
    return holds;
}
// What to apply, in order, to put the user's frame generation back after the `fg` step. user = the multiplier in
// force before it (1 = off); kept = the multiplier frame generation kept while off (the chain's value, which the
// holds overwrite). With frame generation off and a kept multiplier the plan held (so the setter accepts it), turn
// it on at that one and then off: the setter keeps the last multiplier when it turns frame generation off. The
// review found the old restore (setFgMultiplier(1) only) left the last hold's multiplier as the kept one.
inline std::vector<std::uint32_t> pushFgRestoreSteps(std::uint32_t user, std::uint32_t kept, const std::vector<std::uint32_t>& holds) {
    if (user >= 2) return {user};
    if (kept >= 2 && std::find(holds.begin(), holds.end(), kept) != holds.end()) return {kept, 1u};
    return {1u};
}
// The `[slipstream-fg]` line. Unknown numbers are -1. pairDelayMs: the delay frame generation adds to a real
// frame by design (it is shown one content interval later, EngineController's pacing comment), a model value:
// 1000 / sourceFps with a multiplier above 1, else 0.
struct PushFgStats {
    int method = -1, multiplier = 1, tier = -1;
    double sourceFps = -1.0, outputFps = -1.0;
    long long generatedShown = -1, generatedExpired = -1, skippedPairs = -1;
    double costP50 = -1.0, costP95 = -1.0, shownGapP95 = -1.0, shownGapMax = -1.0;
    double seconds = -1.0;
    bool hold = false;               // the end-of-hold line of the autorun step
};
constexpr double pushFgPairDelayMs(int multiplier, double sourceFps) {
    return multiplier > 1 && sourceFps > 1.0 ? 1000.0 / sourceFps : multiplier > 1 ? -1.0 : 0.0;
}
inline std::string pushFgStatsLine(const PushFgStats& s) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "method=%d multiplier=%d tier=%d sourceFps=%.2f outputFps=%.2f generatedShown=%lld generatedExpired=%lld "
                  "skippedPairs=%lld costP50=%.3f costP95=%.3f shownGapP95=%.3f shownGapMax=%.3f pairDelayMs=%.3f hold=%d seconds=%.1f",
                  s.method, s.multiplier, s.tier, s.sourceFps, s.outputFps, s.generatedShown, s.generatedExpired, s.skippedPairs,
                  s.costP50, s.costP95, s.shownGapP95, s.shownGapMax, pushFgPairDelayMs(s.multiplier, s.sourceFps), s.hold ? 1 : 0,
                  s.seconds);
    return text;
}
// A reason word for `[slipstream-autorun] ... reason=<word>`: lowercase ASCII letters, digits and '-', at most
// 32 characters, "failed" when nothing is left.
inline std::string pushStepReason(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (out.size() >= 32) break;
        if (c >= 'A' && c <= 'Z') out.push_back(char(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("failed") : out;
}
// Hold length from the step argument ("sr:20" -> 20): 3..600 s; anything else -> fallback.
inline unsigned pushStepSeconds(std::string_view text, unsigned fallback) {
    if (text.empty() || text.size() > 4) return fallback;
    unsigned v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return fallback;
        v = v * 10u + unsigned(c - '0');
    }
    return v >= 3 && v <= 600 ? v : fallback;
}
} // namespace veyra::engine
