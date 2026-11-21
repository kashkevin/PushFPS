// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// The hook steps `sr:<seconds>`, `fg:<seconds>` and `quality` of VEYRA_TEST_RP_AUTORUN (S4_CONTRACT.md; the plan
// runner and its registry: engine/PushAutorunPlan.h, stream "auto"; the runner calls Start once, Poll every tick,
// Cancel on its timeout or when the plan is abandoned), and the 5 s `[slipstream-fg]` line with
// VEYRA_EXP_RP_STATS=1. Everything runs on the interface thread through the bridge's public setters, the same
// ones the user's controls call. The RTX Video SR levels are held through engine::pushSrLevelRequest(): no
// setting changes for them. Off, NIS, FSR and another SR target change settings (and rebuild the graph); the
// user's choice is put back at the end, on a failure and on cancel. Preferences (the prefs file) are never
// written here, and the session file is not written while a step runs (engine/PushHookHold.h), so a crash in
// the middle of a hold leaves the user's own choice there. Plan logic: engine/PushSrPlan.h (tested).
// Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushAutorunPlan.h"
#include "veyra/engine/PushCompareSteps.h"
#include "veyra/engine/PushHookHold.h"
#include "veyra/engine/PushSrPlan.h"
#include <QGuiApplication>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace veyra::ui {
namespace hook = engine::autorun;
// Starts step `name` ("sr", "fg", "quality") with `seconds` per hold (0 = the default: 20, or 10 for quality).
hook::HookResult pushCompareStartStep(QmlPlayerBridge& bridge, const std::string& name, int seconds);
// The running step's state, advancing it first (the runner's Poll).
hook::HookResult pushComparePollStep();
// Ends the running step and puts the user's settings back (the runner's Cancel).
void pushCompareCancelStep();

namespace {
using Clock = std::chrono::steady_clock;

bool compareStatsSwitch() {
    static const bool on = [] {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_RP_STATS", value, 4);
        return length == 1 && value[0] == L'1';
    }();
    return on;
}
int fgMethodId(const QString& name) {
    static const char* const names[] = {"dlss", "xess", "fsr3", "fsr4", "vfg", "anr", "amdfrc", "pushflow"};
    for (int i = 0; i < 8; ++i)
        if (name == QLatin1String(names[i])) return i;
    return -1;
}
int fgTierOf(QmlPlayerBridge& bridge) {
    const int method = fgMethodId(bridge.fgBackendName());
    return method == 4 || method == 7 ? bridge.vfgQuality() : -1;
}
double numberOr(const QVariantMap& m, const char* key, double fallback) {
    const auto v = m.value(QLatin1String(key));
    bool ok = false;
    const double d = v.toDouble(&ok);
    return v.isValid() && ok ? d : fallback;
}
bool capabilityAvailable(QmlPlayerBridge& bridge, int id) {
    return bridge.effectCapabilities().value(QStringLiteral("sr") + QString::number(id)).toMap().value(QStringLiteral("available")).toBool();
}
// The largest visible top-level window in device pixels; 0 x 0 when there is none.
std::pair<std::uint32_t, std::uint32_t> windowPixels() {
    std::uint32_t w = 0, h = 0;
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (!window || !window->isVisible()) continue;
        const double dpr = window->devicePixelRatio();
        const auto ww = static_cast<std::uint32_t>(std::max(0.0, double(window->width()) * dpr + 0.5));
        const auto wh = static_cast<std::uint32_t>(std::max(0.0, double(window->height()) * dpr + 0.5));
        if (std::uint64_t(ww) * wh > std::uint64_t(w) * h) { w = ww; h = wh; }
    }
    return {w, h};
}

struct HookRun {
    QmlPlayerBridge* bridge = nullptr;
    std::string name;
    bool active = false;
    // What the last step ended with; Running while it runs.
    hook::HookState outcome = hook::HookState::Running;
    std::string reason;
    unsigned seconds = 20;
    std::size_t index = 0;
    Clock::time_point holdStart{}, deadline{};
    bool benchmarkOpen = false, benchmarkStarted = false;
    unsigned refused = 0;
    // sr
    std::vector<engine::PushSrHold> srHolds;
    engine::PushSrUserState srUser;
    // fg
    std::vector<std::uint32_t> fgHolds;
    int fgUser = 1;
    int fgKept = 0;                  // the multiplier kept while the user had frame generation off
};
HookRun& hookRun() {
    static HookRun run;
    return run;
}
void resetRun() { hookRun() = HookRun{}; }

// --- sr ------------------------------------------------------------------------------------------------------
bool applySrHold(QmlPlayerBridge& b, const engine::PushSrHold& h) {
    engine::pushSrLevelRequest().store(h.levelRequest);
    if (!h.enabled) {
        if (b.srEnabled()) b.setSrEnabled(false);
        return !b.srEnabled();
    }
    if (b.dualSrEnabled()) b.setDualSrEnabled(false);
    if (b.videoSrQuality() != int(h.quality)) b.setVideoSrQuality(int(h.quality));
    if (h.targetIndex >= 1 && b.srTargetIndex() != h.targetIndex) b.setSrTargetIndex(h.targetIndex);
    if (!b.srEnabled()) b.setSrEnabled(true);
    return b.srEnabled() && b.videoSrQuality() == int(h.quality) && (h.targetIndex < 1 || b.srTargetIndex() == h.targetIndex);
}
void restoreSr(QmlPlayerBridge& b, const engine::PushSrUserState& u, const char* why) {
    engine::pushSrLevelRequest().store(0);
    if (b.dualSrEnabled() && !u.dual) b.setDualSrEnabled(false);
    if (b.videoSrQuality() != int(u.quality)) b.setVideoSrQuality(int(u.quality));
    if (u.targetIndex >= 1 && b.srTargetIndex() != u.targetIndex) b.setSrTargetIndex(u.targetIndex);
    if (u.dual && !b.dualSrEnabled()) b.setDualSrEnabled(true);
    if (b.srEnabled() != u.enabled) b.setSrEnabled(u.enabled);
    const bool ok = b.srEnabled() == u.enabled && b.videoSrQuality() == int(u.quality) && b.dualSrEnabled() == u.dual &&
                    (u.targetIndex < 1 || b.srTargetIndex() == u.targetIndex);
    veyra::log::info("slipstream-sr", std::format("event=restored why={} ok={} enabled={} choice={} target={} dual={}", why, ok ? 1 : 0,
        u.enabled ? 1 : 0, u.quality, u.targetIndex, u.dual ? 1 : 0));
}

// --- fg ------------------------------------------------------------------------------------------------------
bool applyFgHold(QmlPlayerBridge& b, std::uint32_t multiplier) {
    if (b.fgMultiplier() != int(multiplier)) b.setFgMultiplier(int(multiplier));
    return b.fgMultiplier() == int(multiplier);
}
engine::PushFgStats liveFgStats(QmlPlayerBridge& b) {
    const QVariantMap m = b.pushLiveStats();
    engine::PushFgStats s;
    s.method = fgMethodId(b.fgBackendName());
    s.multiplier = int(numberOr(m, "fgMultiplierNow", double(b.fgMultiplier())));
    s.tier = fgTierOf(b);
    const double content = numberOr(m, "contentFps", -1.0);
    s.sourceFps = content > 0.0 ? content : numberOr(m, "sourceFps", -1.0);
    const double real = numberOr(m, "realPresentFps", -1.0), generated = numberOr(m, "generatedPresentFps", -1.0);
    s.outputFps = real >= 0.0 && generated >= 0.0 ? real + generated : -1.0;
    s.costP50 = numberOr(m, "pushFlowP50Ms", -1.0);
    s.costP95 = numberOr(m, "pushFlowP95Ms", -1.0);
    return s;
}

// --- the run -------------------------------------------------------------------------------------------------
void finish(bool ok, const std::string& reason) {
    auto& r = hookRun();
    if (!r.active) return;
    // The session file may be written again: the restore below is saved like any change.
    engine::pushHookHoldActive().store(false, std::memory_order_relaxed);
    QmlPlayerBridge* b = r.bridge;
    if (b && r.benchmarkOpen) { b->pushBenchmarkEnd(); r.benchmarkOpen = false; }
    if (b && r.name == "sr") restoreSr(*b, r.srUser, ok ? "done" : "failed");
    if (b && r.name == "fg") {
        // Also the multiplier frame generation keeps while off, when it was off before the step (engine/PushSrPlan.h).
        const auto steps = engine::pushFgRestoreSteps(std::uint32_t(std::max(1, r.fgUser)), std::uint32_t(std::max(0, r.fgKept)), r.fgHolds);
        bool back = true;
        for (const auto m : steps) back = applyFgHold(*b, m) && back;
        if (steps.size() > 1) back = back && b->pushFgKeptMultiplier() == r.fgKept;
        // A kept multiplier the current method does not offer cannot be put back: the last hold's value stays kept.
        const bool keptLost = r.fgUser < 2 && r.fgKept >= 2 && b->pushFgKeptMultiplier() != r.fgKept;
        veyra::log::info("slipstream-fg", std::format("event=restored why={} ok={} multiplier={} kept={}{}", ok ? "done" : "failed",
            back && !keptLost ? 1 : 0, r.fgUser, r.fgKept, keptLost ? " reason=multiplier-not-restorable" : ""));
    }
    if (b && r.name == "quality" && !ok) b->pushQualityCancel();
    r.active = false;
    r.outcome = ok ? hook::HookState::Done : hook::HookState::Failed;
    r.reason = ok ? std::string() : engine::pushStepReason(reason);
}
// One hold ends: the measurement window over its settled part, as a line.
void closeHold(QmlPlayerBridge& b) {
    auto& r = hookRun();
    if (!r.benchmarkOpen) return;
    const QVariantMap m = b.pushBenchmarkEnd();
    r.benchmarkOpen = false;
    if (r.name == "fg") {
        engine::PushFgStats s = liveFgStats(b);
        s.multiplier = r.index < r.fgHolds.size() ? int(r.fgHolds[r.index]) : s.multiplier;
        s.outputFps = numberOr(m, "fps", s.outputFps);
        s.generatedShown = static_cast<long long>(numberOr(m, "generatedPresents", -1.0));
        s.seconds = numberOr(m, "seconds", -1.0);
        s.hold = true;
        // With the adaptive multiplier on, the multiplier in force can be below the hold's (labelled) one.
        veyra::log::info("slipstream-fg", engine::pushFgStatsLine(s) + (b.fgAdaptive() ? " adaptive=1" : ""));
    } else if (r.name == "sr" && r.index < r.srHolds.size()) {
        veyra::log::info("slipstream-sr", std::format("event=hold-end index={} label={} valid={} seconds={:.1f} fps={:.2f} presents={} gaps={} "
            "latencyP50Ms={:.3f} latencyP95Ms={:.3f} latencySamples={}", r.index, r.srHolds[r.index].label, m.value("valid").toBool() ? 1 : 0,
            numberOr(m, "seconds", -1.0), numberOr(m, "fps", -1.0), m.value("presents").toULongLong(), m.value("gaps").toULongLong(),
            numberOr(m, "latencyP50Ms", -1.0), numberOr(m, "latencyP95Ms", -1.0), m.value("latencySamples").toULongLong()));
    }
}
// Applies hold r.index (skipping refused ones); false when none is left.
bool startHold(QmlPlayerBridge& b) {
    auto& r = hookRun();
    const std::size_t count = r.name == "sr" ? r.srHolds.size() : r.fgHolds.size();
    for (; r.index < count; ++r.index) {
        bool ok = false;
        if (r.name == "sr") {
            const auto& h = r.srHolds[r.index];
            ok = applySrHold(b, h);
            veyra::log::info("slipstream-sr", std::format("event=hold index={} label={} accepted={} choice={} level={} target={} seconds={}",
                r.index, h.label, ok ? 1 : 0, h.enabled ? int(h.quality) : 0, h.levelRequest, h.targetIndex, r.seconds));
        } else {
            ok = applyFgHold(b, r.fgHolds[r.index]);
            veyra::log::info("slipstream-fg", std::format("event=hold index={} multiplier={} accepted={} seconds={}{}", r.index, r.fgHolds[r.index],
                ok ? 1 : 0, r.seconds, b.fgAdaptive() ? " adaptive=1" : ""));
        }
        if (ok) {
            r.holdStart = Clock::now();
            r.benchmarkStarted = false;
            return true;
        }
        ++r.refused;
    }
    return false;
}
void tick() {
    auto& r = hookRun();
    if (!r.active || !r.bridge) return;
    QmlPlayerBridge& b = *r.bridge;
    const auto now = Clock::now();
    if (r.name == "quality") {
        const QVariantMap st = b.pushQualityStatus();
        if (st.value("done").toBool()) { finish(true, {}); return; }
        if (st.value("failed").toBool()) { finish(false, st.value("error").toString().toStdString()); return; }
        if (now >= r.deadline) { finish(false, "timeout"); return; }
        if (!b.hasSource()) finish(false, "stopped");
        return;
    }
    if (!b.hasSource()) { finish(false, "stopped"); return; }
    const auto hold = std::chrono::seconds(r.seconds);
    // The first third (at most 5 s) of a hold lets a rebuild settle; the measurement window is the rest.
    const auto settle = std::chrono::seconds(std::min(5u, r.seconds / 3u));
    if (!r.benchmarkStarted && now - r.holdStart >= settle) {
        b.pushBenchmarkBegin();
        r.benchmarkOpen = r.benchmarkStarted = true;
    }
    if (now - r.holdStart < hold) return;
    closeHold(b);
    ++r.index;
    if (startHold(b)) return;
    const std::size_t count = r.name == "sr" ? r.srHolds.size() : r.fgHolds.size();
    finish(r.refused < count, "refused");
}
} // namespace

int QmlPlayerBridge::pushFgKeptMultiplier() const { return int(impl_->chain.fgMultiplier); }

hook::HookResult pushCompareStartStep(QmlPlayerBridge& bridge, const std::string& name, int seconds) {
    const auto failed = [](const char* why) { return hook::HookResult{hook::HookState::Failed, why}; };
    auto& r = hookRun();
    if (r.active) return failed("busy");
    if (name != "sr" && name != "fg" && name != "quality") return failed("unsupported");
    if (!bridge.hasSource()) return failed("no-source");
    resetRun();
    r.bridge = &bridge;
    r.name = name;
    r.seconds = engine::pushStepSeconds(seconds > 0 ? std::to_string(seconds) : std::string(), name == "quality" ? 10u : 20u);
    if (name == "sr") {
        r.srUser.enabled = bridge.srEnabled();
        r.srUser.quality = std::uint32_t(std::max(0, bridge.videoSrQuality()));
        r.srUser.targetIndex = bridge.srTargetIndex();
        r.srUser.dual = bridge.dualSrEnabled();
        engine::PushSrAvailable a;
        a.rtxStandard = capabilityAvailable(bridge, 4);
        a.nis = capabilityAvailable(bridge, 6);
        a.fsr = capabilityAvailable(bridge, 5);
        const auto [w, h] = windowPixels();
        a.displayTargetIndex = engine::pushSrTargetForWindow(w, h);
        r.srHolds = engine::pushSrPlan(r.srUser, a);
        veyra::log::info("slipstream-sr", std::format("event=plan holds={} seconds={} rtx={} nis={} fsr={} window={}x{} windowTarget={} userEnabled={} "
            "userChoice={} userTarget={} userDual={}", r.srHolds.size(), r.seconds, a.rtxStandard ? 1 : 0, a.nis ? 1 : 0, a.fsr ? 1 : 0, w, h,
            a.displayTargetIndex, r.srUser.enabled ? 1 : 0, r.srUser.quality, r.srUser.targetIndex, r.srUser.dual ? 1 : 0));
    } else if (name == "fg") {
        r.fgUser = std::max(1, bridge.fgMultiplier());
        r.fgKept = bridge.pushFgKeptMultiplier();
        std::vector<std::uint32_t> choices;
        for (const auto& v : bridge.fgMultiplierChoices()) {
            bool ok = false;
            const int m = v.toMap().value(QStringLiteral("id")).toString().toInt(&ok);
            if (ok && m >= 2) choices.push_back(std::uint32_t(m));
        }
        r.fgHolds = engine::pushFgPlan(choices);
        if (r.fgHolds.empty()) { resetRun(); return failed("no-frame-generation"); }
    } else {
        if (!bridge.pushQualityBegin(int(r.seconds))) { resetRun(); return failed("not-started"); }
        r.deadline = Clock::now() + std::chrono::seconds(r.seconds + 60u);
    }
    r.active = true;
    engine::pushHookHoldActive().store(true, std::memory_order_relaxed);
    if (name != "quality" && !startHold(bridge)) finish(false, "refused");
    return {r.outcome, r.reason};
}

hook::HookResult pushComparePollStep() {
    tick();
    const auto& r = hookRun();
    if (!r.active && r.outcome == hook::HookState::Running) return {hook::HookState::Failed, "not-started"};
    return {r.outcome, r.reason};
}

void pushCompareCancelStep() { finish(false, "cancelled"); }

// The ONE place that joins these steps to the autorun registry (engine::autorun::registerStep). Also starts the 5 s
// [slipstream-fg] line under VEYRA_EXP_RP_STATS=1. Call once, on the interface thread, after the bridge exists.
void registerPushCompareAutorunSteps(QmlPlayerBridge& bridge) {
    for (const char* step : {"sr", "fg", "quality"})
        hook::registerStep(step, [&bridge, name = std::string(step)](hook::HookEvent event, int seconds) -> hook::HookResult {
            switch (event) {
            case hook::HookEvent::Start: return pushCompareStartStep(bridge, name, seconds);
            case hook::HookEvent::Poll: return pushComparePollStep();
            case hook::HookEvent::Cancel: pushCompareCancelStep(); return {hook::HookState::Failed, "cancelled"};
            }
            return {hook::HookState::Failed, "unsupported"};
        });
    if (!compareStatsSwitch()) return;
    auto* timer = new QTimer(&bridge);
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, &bridge, [&bridge] {
        const auto& r = hookRun();
        if (r.active && r.name == "fg") return;   // the hold lines cover it
        if (!bridge.hasSource() || !bridge.pushLiveStats().value(QStringLiteral("fgActive")).toBool()) return;
        veyra::log::info("slipstream-fg", engine::pushFgStatsLine(liveFgStats(bridge)));
    });
    timer->start();
}
} // namespace veyra::ui
