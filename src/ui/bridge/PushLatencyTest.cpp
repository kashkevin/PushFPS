// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test (engine/PushInputLatency.h): presses the PS button on the
// PS5 through Remote Play, waits for the control centre to appear in the presented picture, closes
// it again, and repeats. PushFPS's share is its measured processing time (arrival to presentation).
//
// Latency test v2 (docs/LATENCY_MEASUREMENT.md), only when asked for: more than 20 presses (up to
// engine::latencystats::kTrialsMax), another button (OPTIONS, or the D-pad right/left so a highlight moves and
// comes back), or VEYRA_EXP_RP_STATS=1. Then the wait before each press is randomised over four stream frame
// intervals (presses land at every phase of the stream's frame clock), every answered press is split into
// send -> network arrival (console + network) and arrival -> present (the PC), and with VEYRA_EXP_RP_STATS=1
// the [slipstream-press] / [slipstream-latency-test] lines are written. The original test (PS button, up to 20
// presses, no switch) runs exactly as before. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushInputLatencyProbe.h"
#include "veyra/engine/PushLatencyStats.h"
#include "veyra/Log.h"

#include <QDateTime>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

namespace veyra::ui {
namespace {
constexpr qint64 kBaselineMs = 300, kHoldMs = 90, kAnswerTimeoutMs = 1200, kCloseWaitMs = 700, kSettleMs = 1300;
// D-pad: no menu to close, only the highlight to let settle before the next press.
constexpr qint64 kDpadSettleMs = 400;
constexpr int kOriginalTrialsMax = 20;
namespace ls = engine::latencystats;
// The controller path exists only in builds with PS5 Remote Play; without it the test never starts
// (the snapshot never reports a Remote Play source), and this keeps such builds compiling.
template <class Engine> void sendController(Engine& engine, const remoteplay::ControllerState& state) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    engine.remotePlayController(state);
#else
    (void)engine; (void)state;
#endif
}
// VEYRA_EXP_RP_STATS=1 (read once): the [slipstream-*] statistics lines.
bool statsSwitch() {
    static const bool on = [] {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_RP_STATS", value, 4);
        return length == 1 && value[0] == L'1';
    }();
    return on;
}
// Test v2 state. One bridge exists per process and only its thread (the interface's) touches this.
struct TestV2 {
    bool on = false;                         // false: the original test, unchanged
    ls::Button button = ls::Button::Ps;
    std::optional<ls::Random> random;
    qint64 baselineMs = kBaselineMs;          // this press's wait (randomised under v2)
    double intervalMs = 1000.0 / 60.0;
    std::vector<ls::PressSplit> presses;
    std::optional<ls::TestSummary> summary;   // at the end
};
TestV2& v2() { static TestV2 state; return state; }
uint32_t pressButton(ls::Button button, int trial) {
    switch (button) {
    case ls::Button::Options: return remoteplay::ControllerState::Options;
    case ls::Button::DpadRightLeft: return trial % 2 == 0 ? remoteplay::ControllerState::Right : remoteplay::ControllerState::Left;
    default: return remoteplay::ControllerState::PS;
    }
}
}

remoteplay::ControllerState QmlPlayerBridge::pushLatencyPress() const {
    remoteplay::ControllerState state;
    state.inputActive = true;   // a released pad, not focus loss
    if (impl_->latency.pressing) state.buttons = v2().on ? pressButton(v2().button, impl_->latency.trial) : remoteplay::ControllerState::PS;
    return state;
}

bool QmlPlayerBridge::pushLatencyTestBegin(int trials) {
    return pushLatencyTestBeginWith(trials, QStringLiteral("ps"));
}

bool QmlPlayerBridge::pushLatencyTestBeginWith(int trials, const QString& buttonName) {
    auto& t = impl_->latency;
    const auto s = impl_->engine.snapshot();
    if (!s.remotePlay || !s.running) { emit notice(tr("延迟测试需要正在进行的 PS5 串流"), true); return false; }
    if (t.active) return true;
    const auto button = ls::parseButton(buttonName.toStdString());
    if (!button) return false;
    t = {};
    t.timer = impl_->latency.timer;
    t.active = true; t.trials = std::clamp(trials, ls::kTrialsMin, ls::kTrialsMax); t.state = QStringLiteral("running");
    auto& x = v2();
    x = {};
    x.button = *button;
    x.on = statsSwitch() || *button != ls::Button::Ps || t.trials > kOriginalTrialsMax;
    if (x.on) {
        const auto fps = s.remoteStream.requestedProfile.fps;
        x.intervalMs = fps > 0 ? 1000.0 / double(fps) : 1000.0 / 60.0;
        x.random.emplace(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
        x.baselineMs = qint64(std::llround(ls::pressGapMs(*x.random, double(kBaselineMs), x.intervalMs)));
        x.presses.reserve(size_t(t.trials));
    }
    t.phaseStart = QDateTime::currentMSecsSinceEpoch();
    impl_->engine.inputLatencyProbe().setDetectorMode(x.button == ls::Button::Ps ? engine::latency::Detector::Mode::Bottom
                                                                                   : engine::latency::Detector::Mode::Anywhere);
    impl_->engine.inputLatencyProbe().beginWindow();
    if (!t.timer) {
        t.timer = new QTimer(this);
        t.timer->setTimerType(Qt::PreciseTimer);
        connect(t.timer, &QTimer::timeout, this, [this] { pushLatencyTick(); });
        impl_->latency.timer = t.timer;
    }
    t.timer->start(4);
    veyra::log::info("push-latency", std::format("remote play latency test started trials={}", t.trials));
    if (x.on)
        veyra::log::info("push-latency", std::format("v2 button={} randomGapMs=0..{:.1f} split=1", ls::buttonName(x.button), 4 * x.intervalMs));
    return true;
}

void QmlPlayerBridge::pushLatencyTestCancel() {
    auto& t = impl_->latency;
    if (!t.active) return;
    t.pressing = false;
    sendController(impl_->engine, pushLatencyPress());
    t.active = false; t.state = QStringLiteral("idle");
    if (t.timer) t.timer->stop();
    impl_->engine.inputLatencyProbe().endWindow();
    veyra::log::info("push-latency", "cancelled");
}

void QmlPlayerBridge::pushLatencyTick() {
    auto& t = impl_->latency;
    auto& x = v2();
    if (!t.active) { if (t.timer) t.timer->stop(); return; }
    auto& probe = impl_->engine.inputLatencyProbe();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto s = impl_->engine.snapshot();
    if (!s.remotePlay || !s.running) {
        t.error = tr("PS5 串流已停止，延迟测试已取消"); t.state = QStringLiteral("failed");
        t.active = false; t.pressing = false; if (t.timer) t.timer->stop(); probe.endWindow(); return;
    }
    const double answer = probe.poll();
    const auto press = [&](bool down) { t.pressing = down; sendController(impl_->engine, pushLatencyPress()); };
    // v2: the answered press, split with the timestamps the frame carries.
    const auto answered = [&] {
        t.results.push_back(answer);
        if (!x.on) return;
        const auto timing = probe.timing();
        const auto split = ls::split(timing.sent, timing.arrival, timing.present, 0, timing.lastArrivalBeforeSent, x.intervalMs);
        x.presses.push_back(split);
        if (statsSwitch()) veyra::log::info("slipstream-press", ls::pressLine(int(x.presses.size()), split));
    };
    const bool dpad = x.on && x.button == ls::Button::DpadRightLeft;
    switch (t.phase) {
    case 0:   // baseline frames, then the press
        if (now - t.phaseStart >= (x.on ? x.baselineMs : kBaselineMs)) {
            press(true);
            probe.markSent(engine::PushInputLatencyProbe::now100ns());
            t.phase = 1; t.phaseStart = now;
        }
        break;
    case 1:   // hold the button briefly
        if (now - t.phaseStart >= kHoldMs) { press(false); t.phase = 2; }
        if (answer >= 0) { answered(); t.phase = 3; t.phaseStart = now; }
        break;
    case 2:   // wait for the picture to answer
        if (answer >= 0) { answered(); t.phase = 3; t.phaseStart = now; }
        else if (now - t.phaseStart >= kAnswerTimeoutMs) {
            veyra::log::info("push-latency", std::format("trial {} no answer seen", t.trial + 1));
            t.phase = 3; t.phaseStart = now;
        }
        break;
    case 3: {  // close the control centre (PS again; OPTIONS again), let it settle, next trial
        const qint64 since = now - t.phaseStart;
        // v2: a press answered while still held (a fast button) is released first, so the close press is a press.
        if (x.on && t.pressing && since >= kHoldMs && since < kCloseWaitMs) press(false);
        if (dpad) {
            // The next press goes the other way and brings the highlight back: nothing to close.
        } else if (since >= kCloseWaitMs && since < kCloseWaitMs + kHoldMs) { if (!t.pressing) press(true); }
        else if (since >= kCloseWaitMs + kHoldMs && t.pressing) press(false);
        if (dpad ? since >= kDpadSettleMs && !t.pressing : since >= kCloseWaitMs + kHoldMs + kSettleMs) {
            // PushFPS's own processing time, sampled between trials.
            const auto& flow = impl_->snapshot.metrics.flow;
            if (flow.softwareLatencyMs && std::isfinite(*flow.softwareLatencyMs) && *flow.softwareLatencyMs > 0) {
                t.pushfpsMs += *flow.softwareLatencyMs; ++t.pushfpsSamples;
            }
            if (++t.trial >= t.trials) {
                t.active = false; t.state = QStringLiteral("done");
                if (t.timer) t.timer->stop();
                probe.endWindow();
                const auto sum = engine::latency::summarize(t.results, uint32_t(t.trials));
                veyra::log::info("push-latency", std::format("done answered={}/{} meanMs={:.1f} medianMs={:.1f} minMs={:.1f} maxMs={:.1f} pushfpsMs={:.1f}",
                    sum.answered, sum.trials, sum.meanMs, sum.medianMs, sum.minMs, sum.maxMs, t.pushfpsSamples ? t.pushfpsMs / t.pushfpsSamples : 0.0));
                // Every press, for the A/B analysis (tools/remoteplay_latency_bench/analyze.py): a
                // median of 8-20 presses cannot resolve a difference of a few milliseconds.
                std::string values;
                for (const auto v : t.results) values += std::format("{}{:.1f}", values.empty() ? "" : ",", double(v));
                veyra::log::info("push-latency", std::format("trials answerMs=[{}]", values));
                if (x.on) {
                    x.summary = ls::summarizeTest(x.presses, t.trials, x.button);
                    if (statsSwitch()) veyra::log::info("slipstream-latency-test", ls::testLine(*x.summary));
                }
            } else {
                probe.beginWindow();
                if (x.on && x.random) x.baselineMs = qint64(std::llround(ls::pressGapMs(*x.random, double(kBaselineMs), x.intervalMs)));
                t.phase = 0; t.phaseStart = now;
            }
        }
        break;
    }
    }
}

QVariantMap QmlPlayerBridge::pushLatencyTestStatus() const {
    const auto& t = impl_->latency;
    const auto& x = v2();
    const auto sum = engine::latency::summarize(t.results, uint32_t(t.trials));
    const double pushfps = t.pushfpsSamples ? t.pushfpsMs / t.pushfpsSamples : 0.0;
    QVariantList list;
    for (double ms : t.results) list << ms;
    QVariantMap out{{"state", t.state}, {"error", t.error}, {"trial", t.trial}, {"trials", t.trials},
                    {"answered", int(sum.answered)}, {"results", list}, {"meanMs", sum.meanMs}, {"medianMs", sum.medianMs},
                    {"minMs", sum.minMs}, {"maxMs", sum.maxMs}, {"pushfpsMs", pushfps},
                    {"remoteMs", sum.answered ? std::max(0.0, sum.medianMs - pushfps) : 0.0},
                    {"trialsMax", ls::kTrialsMax}, {"button", QString::fromLatin1(ls::buttonName(x.button))}, {"split", x.on}};
    // v2, at the end: the split medians and what this run can resolve (-1 = not known).
    if (x.on && x.summary && t.state == QLatin1String("done")) {
        const auto value = [](double v) { return std::isfinite(v) ? v : -1.0; };
        out["consoleMs"] = value(x.summary->console.p50);
        out["pcMs"] = value(x.summary->pc.p50);
        out["pcBasis"] = QString::fromLatin1(ls::pcBasisName(x.summary->basis));
        out["totalCiLowMs"] = value(x.summary->total.medianCi.low);
        out["totalCiHighMs"] = value(x.summary->total.medianCi.high);
        out["detectableMs"] = value(x.summary->total.detectableMs);
        out["pcDetectableMs"] = value(x.summary->pc.detectableMs);
    }
    return out;
}

} // namespace veyra::ui
