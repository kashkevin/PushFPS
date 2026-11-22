// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: hardware test automation and the configuration line the test kit compares arms with
// (docs/LATENCY_MEASUREMENT.md, docs/PS5_REMOTE_PLAY_EXPERIMENTS.md "Sprint 4: measurement and automation").
//   - VEYRA_TEST_RP_AUTOCONNECT=1: once at start, the PS5 connection the user would start by pressing Connect
//     on the most recently used console (ps5Load + ps5Connect, as resumeLastSource does). No credentials here.
//   - VEYRA_TEST_RP_CAPTURE=1 (stream capture): the capture card remembered in this settings folder instead of the PS5
//     (never both); with VEYRA_EXP_RP_STATS=1 a capture session also logs `[slipstream-capture] event=card` once.
//   - VEYRA_TEST_RP_AUTORUN=<plan>: the plan (engine/PushAutorunPlan.h) driven by the bridge's poll timer:
//     settle, latency (the built-in test, PushLatencyTest.cpp), wait, hook steps sr / fg / quality registered by
//     other parts through engine::autorun::registerStep, exit through the window's normal close.
//   - [slipstream-config] ends with the sprint 5 keys nrRuntime= nrSize= srTarget= srModel= fgBackend= clean= profile=.
//   - [slipstream-config]: while a session runs, at its start and when a value changes (not fgActive, srActive or
//     the window size alone), at most once per 5 s; always.
//   - [log-summary] errors= warnings=: every 60 s with VEYRA_EXP_RP_STATS=1, and at exit.
// Nothing here changes a setting or a preference. Only the switches above (set in the environment when PushFPS
// started) make anything happen beyond the config line. Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushAutorunPlan.h"
#include "veyra/engine/PushCaptureStats.h"
#include "veyra/pipeline/ResolutionPlan.h"
#include "veyra/engine/PushBenchmarkProbe.h"
#include "veyra/engine/PushCompareSteps.h"
#include "veyra/engine/PushLatencyStats.h"
#include "veyra/Log.h"
#ifdef VEYRA_ENABLE_REMOTEPLAY
#include "veyra/remoteplay/ConnectionHealth.h"
#endif
#include "veyra/remoteplay/ClarityOverride.h"

#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QWindow>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>

namespace veyra::ui {
namespace {
namespace ar = engine::autorun;
using SteadyClock = std::chrono::steady_clock;

// "1" in the environment when PushFPS started; read once.
bool switchIsOne(const wchar_t* name) {
    wchar_t value[4]{};
    const DWORD length = GetEnvironmentVariableW(name, value, 4);
    return length == 1 && value[0] == L'1';
}
bool statsOn() { static const bool on = switchIsOne(L"VEYRA_EXP_RP_STATS"); return on; }
bool autoConnectOn() { static const bool on = switchIsOne(L"VEYRA_TEST_RP_AUTOCONNECT"); return on; }
// VEYRA_TEST_RP_CLIP=<absolute path> (stream clarity): the kit's recorded clip is opened instead of connecting to
// the PS5, so every arm of a picture comparison processes the same frames. set: the variable exists; path: empty when
// it is not an absolute path to a file (remoteplay::clarityClipPathUsable). This hook logs no path (openPath itself
// writes none; openUrl's [ui-drop] line is avoided on purpose).
struct ClarityClip { bool set = false; QString path; };
const ClarityClip& clarityClip() {
    static const ClarityClip clip = [] {
        ClarityClip c;
        wchar_t value[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_TEST_RP_CLIP", value, 1024);
        if (length == 0) return c;
        c.set = true;
        if (length >= 1024 || !remoteplay::clarityClipPathUsable(std::wstring_view(value, length))) return c;
        const QString path = QString::fromWCharArray(value, static_cast<qsizetype>(length));
        if (QFileInfo(path).isFile()) c.path = path;
        return c;
    }();
    return clip;
}
// VEYRA_TEST_RP_CAPTURE=1 (stream capture, the kit's capture session): the capture card remembered in this settings
// folder is started (resumeCaptureSession, as "continue last capture" does) instead of a PS5 connection. Remote Play
// is never connected in this mode, also when VEYRA_TEST_RP_AUTOCONNECT=1 is set as well: the controller is plugged
// into the PS5 and the picture comes over HDMI. Read once.
bool captureAutostartOn() { static const bool on = switchIsOne(L"VEYRA_TEST_RP_CAPTURE"); return on; }
// VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1 (VideoPresenter.cpp) presents without the low queue and NVIDIA low latency
// whatever the saved preference says: [slipstream-config] must show what is in force, or the test kit would
// compare this run as if the queue were on.
bool presentLowQueueOff() { static const bool on = switchIsOne(L"VEYRA_EXP_PRESENT_LOW_QUEUE_OFF"); return on; }
// VEYRA_EXP_RP_LATENCY_BUTTONS=1: latency steps may use OPTIONS / the D-pad (engine/PushLatencyStats.h buttonAllowed).
bool latencyButtonsOn() { static const bool on = switchIsOne(L"VEYRA_EXP_RP_LATENCY_BUTTONS"); return on; }
// The plan as ASCII; nullopt when unset. Longer than the parser accepts: kept long so it fails as plan-too-long.
std::optional<std::string> autorunText() {
    static const std::optional<std::string> text = []() -> std::optional<std::string> {
        wchar_t value[2048]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_TEST_RP_AUTORUN", value, 2048);
        if (length == 0) return std::nullopt;
        if (length >= 2048) return std::string(ar::kMaxPlanBytes + 1, '?');
        std::string out;
        for (DWORD i = 0; i < length; ++i) out.push_back(value[i] > 0 && value[i] < 128 ? static_cast<char>(value[i]) : '?');
        return out;
    }();
    return text;
}
// Bridge thread only (one bridge per process).
struct AutorunState {
    bool started = false;               // the first tick ran (autoconnect, plan parsed)
    bool quitHooked = false, exiting = false;
    std::optional<ar::Runner> runner;
    SteadyClock::time_point lastTick{}, lastSummary{}, lastConfigCheck{};
    std::int64_t originMs = -1, steadySinceMs = -1;
    ar::ConfigLineGate configGate;
    std::string route = "unknown";
    uint64_t routeSession = ~uint64_t(0);
    uint64_t cardSession = ~uint64_t(0);   // [slipstream-capture] event=card, once per capture session
};
AutorunState& autorunState() { static AutorunState state; return state; }

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}
void logLine(const std::string& text) { veyra::log::info("slipstream-autorun", text); }
void logSummary(const char* when) {
    veyra::log::info("log-summary", std::format("errors={} warnings={} when={}", veyra::log::problemLines(true), veyra::log::problemLines(false), when));
}
// Only [a-z0-9-] (a preference value goes into a log line).
std::string word(const QString& text) {
    std::string w;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (w.size() >= 32) break;
        if ((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'-') w.push_back(static_cast<char>(u));
        else if (u >= u'A' && u <= u'Z') w.push_back(static_cast<char>(u - u'A' + u'a'));
    }
    return w.empty() ? std::string("unknown") : w;
}
// The program's main window: the largest visible top-level window without a transient parent.
QWindow* mainWindow() {
    QWindow* best = nullptr;
    for (QWindow* w : QGuiApplication::topLevelWindows())
        if (w && w->isVisible() && !w->transientParent() && (!best || w->width() * w->height() > best->width() * best->height())) best = w;
    return best;
}
// Closes the program the way a user closing the window does (as --exit-after does in main.cpp); quits the
// event loop after 20 s if the window could not close (a confirmation, a hung page).
void closeProgram() {
    QTimer::singleShot(300, qApp, [] {
        if (QWindow* w = mainWindow()) { veyra::log::info("slipstream-autorun", "exit: closing the window"); w->close(); }
        else QCoreApplication::quit();
    });
    QTimer::singleShot(20000, qApp, [] { veyra::log::warn("slipstream-autorun", "exit: the window did not close in 20 s; quitting"); QCoreApplication::quit(); });
}
const char* codecName(remoteplay::Codec codec) {
    return codec == remoteplay::Codec::H264 ? "h264" : codec == remoteplay::Codec::H265 ? "hevc" : codec == remoteplay::Codec::H265Hdr ? "hevc-hdr" : "unknown";
}
// 0 off; 1 DLSS SR, 2 RTX Video SR, 3 FSR, 4 NVIDIA Image Scaling (the choice behind videoSrQuality).
int srChoice(bool enabled, uint32_t quality) {
    if (!enabled) return 0;
    if (quality == 0) return 1;
    if (quality == engine::kVideoSrFsr) return 3;
    if (quality == engine::kVideoSrNis) return 4;
    return 2;
}
} // namespace

void QmlPlayerBridge::pushAutorunTick() {
    pushPipelineTestTick();   // VEYRA_TEST_RP_PIPELINE, test-only; nothing without it (PushPipelineHook.cpp)
    // The sr / fg / quality hook steps (PushCompare.cpp) join the registry once; with no switch set this only stores three handlers.
    static const bool compareStepsRegistered = (registerPushCompareAutorunSteps(*this), true);
    static_cast<void>(compareStepsRegistered);
    // The probe:<seconds> hook step (engine/PushPipelineProbe.h): pairs of presented frames are read back and the
    // probe itself logs the two [slipstream-quality] lines. Stored once, like the handlers above.
    static const bool probeStepRegistered = (ar::registerStep("probe", [this](ar::HookEvent event, int seconds) -> ar::HookResult {
        if (event == ar::HookEvent::Cancel) { impl_->engine.cancelPipelineProbe(); return {ar::HookState::Failed, "cancelled"}; }
        if (event == ar::HookEvent::Start) {
            engine::PushPipelineProbe::Options options;
            options.seconds = static_cast<uint32_t>(seconds > 0 ? seconds : 10);
            impl_->engine.startPipelineProbe(options);
            return {};
        }
        const auto probe = impl_->engine.pipelineProbe();
        if (probe.done) return {ar::HookState::Done, {}};
        if (probe.failed || !probe.running) return {ar::HookState::Failed, probe.reason.empty() ? std::string("failed") : probe.reason};
        return {};
    }), true);
    static_cast<void>(probeStepRegistered);
    pushNeuralTick();   // [nr-runtime-status] once per start (PushNeural.cpp)
    auto& a = autorunState();
    const auto now = SteadyClock::now();
    if (a.started && now - a.lastTick < std::chrono::milliseconds(50)) return;
    a.lastTick = now;
    const std::int64_t nowMs = steadyMs();
    const auto& s = impl_->snapshot;

    if (!a.started) {
        a.started = true;
        a.lastSummary = now;
        if (!a.quitHooked) {
            a.quitHooked = true;
            connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
                auto& st = autorunState();
                if (st.runner && !st.runner->finished()) {
                    const auto out = st.runner->abandon("closing");
                    for (const auto& line : out.lines) logLine(line);
                    if (out.action == ar::Action::CancelLatency) pushLatencyTestCancel();   // release the button
                }
                logSummary("exit");
            });
        }
        // The plan first: a plan that does not parse runs nothing (the runner fails it on its first tick), and
        // then nothing connects either.
        std::optional<ar::Plan> plan;
        if (const auto text = autorunText()) {
            plan = ar::parsePlan(*text);
            logLine(std::format("plan steps={} valid={} exit={}", plan->steps.size(), plan->valid() ? 1 : 0, plan->hasExit() ? 1 : 0));
        }
        // No console registered (contract: log it and do nothing else): no connection, no plan, no exit.
        bool noConsole = false;
        const char* noConsoleReason = "no-console";
        const auto& clip = clarityClip();
        if (clip.set && clip.path.isEmpty()) { noConsole = true; noConsoleReason = "no-clip"; }
        // A capture run needs the card remembered in this settings folder; it never loads or connects a console.
        const bool captureRun = captureAutostartOn() && !clip.set;
        if (captureRun) {
            if (ui::CapturePreferenceStore(impl_->dataDir).load().videoPath.empty()) { noConsole = true; noConsoleReason = "no-capture"; }
            if (autoConnectOn()) logLine("step=0 name=connect state=skipped reason=capture");
        }
        if ((autoConnectOn() || plan) && !clip.set && !captureRun) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
            ps5Load();
            noConsole = impl_->ps5ProfileList.isEmpty();
#else
            noConsole = true; noConsoleReason = "no-remoteplay";
#endif
        }
        if (clip.set) {
            if (clip.path.isEmpty()) logLine("step=0 name=clip state=failed reason=no-clip");
            else if (plan && !plan->valid()) logLine("step=0 name=clip state=failed reason=invalid-plan");
            else {
                logLine("step=0 name=clip state=start reason=none");
                // openPath directly (openUrl would write the full path as [ui-drop]); then no resume of a position
                // remembered by an earlier arm in the same settings folder, so every arm starts the clip at 0.
                openPath(QDir::toNativeSeparators(clip.path));
                impl_->resumeSession = 0;
                logLine("step=0 name=clip state=done reason=ok start=0");
            }
        } else if (captureRun) {
            if (noConsole) logLine("step=0 name=capture state=failed reason=no-capture");
            else if (plan && !plan->valid()) logLine("step=0 name=capture state=failed reason=invalid-plan");
            else {
                logLine("step=0 name=capture state=start reason=none");
                resumeCaptureSession();   // the remembered device, format, audio and colour, re-checked against the machine
                logLine("step=0 name=capture state=done reason=ok");   // the start was requested: resumeCaptureSession is asynchronous, the card delivers later
            }
        } else if (autoConnectOn()) {
            if (noConsole) logLine(std::format("step=0 name=connect state=failed reason={}", noConsoleReason));
            else if (plan && !plan->valid()) logLine("step=0 name=connect state=failed reason=invalid-plan");
            else {
#ifdef VEYRA_ENABLE_REMOTEPLAY
                logLine("step=0 name=connect state=start reason=none");
                if (ps5Connect()) logLine("step=0 name=connect state=done reason=ok");
                else logLine("step=0 name=connect state=failed reason=refused");
#endif
            }
        }
        if (plan) {
            if (plan->steps.empty()) logLine("finished ok=0 steps=0");
            else if (noConsole) logLine(std::format("finished ok=0 steps={} reason={}", plan->steps.size(), noConsoleReason));
            else {
                a.runner.emplace(std::move(*plan));
                a.runner->setButtonsAvailable(latencyButtonsOn());
            }
        }
    }

    // [slipstream-capture] event=card (engine/PushCaptureStats.h): once per capture-card session with VEYRA_EXP_RP_STATS=1:
    // the card's friendly name as one word and what its driver lists (the format list the capture dialog read).
    if (statsOn() && s.running && impl_->liveKind == QLatin1String("capture") && a.cardSession != s.sessionId) {
        a.cardSession = s.sessionId;
        veyra::log::info("slipstream-capture", engine::pushCaptureCardLine(impl_->liveLabel.toStdString(), engine::pushCaptureFormatSummary(impl_->captureFormatList)));
    }
    // [slipstream-config], at most once a second, while a session runs.
    if (s.running && now - a.lastConfigCheck >= std::chrono::seconds(1)) {
        a.lastConfigCheck = now;
        const auto set = settings();
        const auto& profile = s.remoteStream.requestedProfile;
        if (a.routeSession != s.sessionId) {
            // The route class of the console's address (wired / Wi-Fi), never the address itself. Once per session.
            a.routeSession = s.sessionId;
            a.route = "unknown";
#ifdef VEYRA_ENABLE_REMOTEPLAY
            const QString host = impl_->ps5Form.value(QStringLiteral("host")).toString().trimmed();
            const QString address = host.isEmpty() ? impl_->remotePlayHost.trimmed() : host;
            if (s.remotePlay && !address.isEmpty()) {
                const auto interfaces = remoteplay::localInterfaces();
                if (!interfaces.empty()) {
                    const auto r = remoteplay::assessRoute(address.toStdString(), interfaces);
                    a.route = r.link == remoteplay::LinkKind::Ethernet ? "wired" : r.link == remoteplay::LinkKind::WiFi ? "wifi" : "unknown";
                }
            }
#endif
        }
        std::string mode = "none", modeSource = "none";
        if (impl_->prefBool("pushModeSwitches", false)) {
            const QString m = impl_->prefs.value(QStringLiteral("pushMode")).toString();
            if (!m.isEmpty()) { mode = word(m); modeSource = "user"; }
        }
        if (switchIsOne(L"VEYRA_EXP_AUTO_MODE") && impl_->prefBool("pushModeAuto", false)) modeSource = "auto";
        int windowW = -1, windowH = -1, outputW = -1, outputH = -1;
        bool fullscreen = false;
        if (QWindow* w = mainWindow()) {
            const double dpr = w->devicePixelRatio();
            windowW = int(std::lround(w->width() * dpr)); windowH = int(std::lround(w->height() * dpr));
            fullscreen = w->visibility() == QWindow::FullScreen;
            if (QScreen* screen = w->screen()) {
                const double sdpr = screen->devicePixelRatio();
                outputW = int(std::lround(screen->geometry().width() * sdpr)); outputH = int(std::lround(screen->geometry().height() * sdpr));
            }
        }
        const bool remote = s.remotePlay;
        const uint32_t streamW = remote ? profile.width : s.sourceWidth, streamH = remote ? profile.height : s.sourceHeight;
        const double streamFps = remote ? double(profile.fps) : s.nominalSourceFps;
        const bool hdr = s.videoHdrActive || s.sourceHdr || (remote && profile.codec == remoteplay::Codec::H265Hdr);
        const std::string decoder = !remote ? "unknown" : !s.remoteStream.decodeConfirmed ? "unknown"
                                  : s.remoteStream.decodeFallback ? "software-fallback" : s.remoteStream.hardwareDecode ? "hardware" : "software";
        // The contract's unknown: -1 (the display's refresh is not known yet).
        const double hz = displayRefreshHz();
        const std::string refreshHz = std::isfinite(hz) && hz > 0 ? std::format("{:.2f}", hz) : std::string("-1");
        // Sprint 5 (contract item 2): the enhancement pipeline's choices, appended at the end; the keys before
        // them are untouched. nrSize is the first neural layer's size; clean and profile are the restoration and
        // profile preferences (0 / none while their parts are not in this build).
        std::string nrSize = "1080";
        for (uint32_t i = 0; i < impl_->chain.nodeCount; ++i) {
            const auto& node = impl_->chain.nodes[i];
            if (node.type != engine::EffectType::NrEnhance) continue;
            const auto policy = node.nr.sizePolicy;
            nrSize = policy == pipeline::NrSizePolicy::Native ? "native" : policy == pipeline::NrSizePolicy::P480 ? "480"
                   : policy == pipeline::NrSizePolicy::P720 ? "720" : policy == pipeline::NrSizePolicy::P900 ? "900"
                   : policy == pipeline::NrSizePolicy::P1440 ? "1440" : policy == pipeline::NrSizePolicy::Auto ? "auto"
                   : pipeline::manualNrSizePolicy(policy) ? "manual" : "1080";
            break;
        }
        const auto srExtent = pipeline::srTargetExtent(set.srTarget);
        const QString profilePref = impl_->prefs.value(QStringLiteral("pushProfile")).toString();
        const std::string pipelineKeys = std::format(" nrRuntime={} nrSize={} srTarget={} srModel={} fgBackend={} clean={} profile={}",
            int(engine::currentNrRuntime(set.nrRuntime)), nrSize,
            srEnabled() ? std::format("{}x{}", srExtent.width, srExtent.height) : std::string("off"),
            srEnabled() && set.videoSrQuality == 0 ? std::to_string(dlssSrModel()) : std::string("none"),
            fgEnabled() ? fgBackendName().toStdString() : std::string("none"),
            std::clamp(impl_->prefInt("pushClean", 0), 0, 100), profilePref.isEmpty() ? std::string("none") : word(profilePref));
        const std::string text = std::format(
            "mode={} fg={} fgMethod={} fgMultiplier={} fgTier={} sr={} srQuality={} enhance={} lowQueue={} nvLowLatency={} displaySync={} "
            "smoothPacing={} vrrRequested={} vrrLevel={} fullscreen={} window={}x{} output={}x{} refreshHz={} hdr={} stream={}x{}@{:.2f} "
            "codec={} bitrateKbps={} decoder={} route={} trialsMax={} modeSource={} fgActive={} srActive={} smartlink={}",
            mode, fgEnabled() ? 1 : 0, int(set.frameGenerationBackend), fgMultiplier(), vfgQuality(), srChoice(srEnabled(), set.videoSrQuality),
            int(set.videoSrQuality), nrEnabled() ? 1 : 0, fgLowQueue() && !presentLowQueueOff() ? 1 : 0, presentLowQueueOff() ? 0 : nvidiaLowLatency(), displaySync(),
            impl_->prefBool("pushSmoothPacing", false) ? 1 : 0, variableRefresh() ? 1 : 0, int(s.displayVrr), fullscreen ? 1 : 0,
            windowW, windowH, outputW, outputH, refreshHz, hdr ? 1 : 0, streamW, streamH, streamFps,
            remote ? codecName(profile.codec) : "unknown", remote ? int(profile.bitrateKbps) : -1, decoder, a.route,
            engine::latencystats::kTrialsMax, modeSource, s.fgActive ? 1 : 0, s.srActive ? 1 : 0, pushSmartLinkConfigMode()) + pipelineKeys;
        // At a session's start and on a change, at most once per 5 s; fgActive, srActive and the window size
        // alone never cause a line (they move by themselves), they are written with the next one.
        if (a.configGate.due(text, s.sessionId, nowMs)) veyra::log::info("slipstream-config", text);
    }

    if (statsOn() && now - a.lastSummary >= std::chrono::seconds(60)) { a.lastSummary = now; logSummary("periodic"); }

    if (!a.runner || a.runner->finished() || a.exiting) return;
    // Presenting: a PS5 stream runs and its frames are being presented. Steady: the stream delivers at least 90 %
    // of its nominal rate and real frames are presented (frame generation and duplicate removal change the
    // presented rate, so only that it is not zero).
    const auto& flow = s.metrics.flow;
    const bool presenting = s.running && s.remotePlay && flow.rateWindowReady && flow.realPresentFps > 0;
    const double nominal = s.remoteStream.requestedProfile.fps > 0 ? double(s.remoteStream.requestedProfile.fps) : 60.0;
    const bool steady = presenting && s.remoteRatesReady && s.remoteDecodedFps >= 0.9 * nominal && flow.realPresentFps >= 0.4 * nominal;
    if (!steady) a.steadySinceMs = -1;
    else if (a.steadySinceMs < 0) a.steadySinceMs = nowMs;
    ar::Inputs in;
    in.nowMs = nowMs;
    in.presenting = presenting;
    in.session = s.running && s.remotePlay ? s.sessionId : 0;
    in.steadySinceMs = a.steadySinceMs;
    in.measuring = engine::pushBenchmarkProbe().active() || impl_->engine.qualityTest().running;
    for (const auto* key : {"pushOptimizeRestore", "pushCalibrationRestore"}) {
        const QString v = impl_->prefs.value(QString::fromLatin1(key)).toString();
        if (!v.isEmpty() && v != QLatin1String("{}")) in.measuring = true;
    }
    const QString latency = impl_->latency.state;
    in.latency = latency == QLatin1String("running") ? ar::LatencyState::Running
               : latency == QLatin1String("done") ? ar::LatencyState::Done
               : latency == QLatin1String("failed") ? ar::LatencyState::Failed : ar::LatencyState::Idle;
    auto out = a.runner->tick(in);
    for (const auto& line : out.lines) logLine(line);
    switch (out.action) {
    case ar::Action::StartLatency: {
        const auto& step = a.runner->plan().steps[a.runner->index()];
        if (!pushLatencyTestBeginWith(step.trials, QString::fromLatin1(engine::latencystats::buttonName(step.button))))
            a.runner->latencyRefused(engine::latencystats::buttonAllowed(step.button, latencyButtonsOn())
                                         ? "refused" : engine::latencystats::kButtonsExperimentalReason);
        break;
    }
    case ar::Action::CancelLatency:
        pushLatencyTestCancel();
        if (a.runner->finished() && a.runner->exitAfterCancel()) { a.exiting = true; closeProgram(); }
        break;
    case ar::Action::Exit:
        a.exiting = true;
        closeProgram();
        break;
    case ar::Action::None:
        break;
    }
}

} // namespace veyra::ui
