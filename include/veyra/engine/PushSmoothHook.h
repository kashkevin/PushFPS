// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Windows-side glue of stream "smooth" (decisions in PushSmoothness.h, which is portable and tested). Included by
// src/engine/EngineController.cpp only; everything runs on the engine thread. What it does:
//   - reads the switches once per process: VEYRA_EXP_RP_SMOOTH_SR_DISPLAY, VEYRA_EXP_RP_SMOOTH_CAP_FIT,
//     VEYRA_EXP_RP_SMOOTH_STEADY (each on only when exactly "1"), VEYRA_EXP_RP_SMOOTH_TARGET (60/90/120/180) and
//     VEYRA_EXP_RP_STATS (the `[slipstream-smooth]` window);
//   - choose(): once per processed pair, AFTER the existing multiplier rules (adaptive, output-cap reduction): with
//     the cap-fit switch the cap reduction's choice is replaced by the tolerant one behind a 0.5 s gate; with
//     Smoothness Mode on (the interface's pref pushSmoothTarget through pushSmoothShared(), or the switch above)
//     its multiplier replaces both. Every switch unset and the mode off: the multiplier is returned unchanged;
//   - the GPU stage times (sample) and the generated-frame counts (ready) for the window.
// Blind Windows code: unverified on Windows.
#include "veyra/engine/PushSmoothness.h"
#include "veyra/engine/PushFgLatency.h"
#include "veyra/engine/PushAdaptiveFg.h"
#include "veyra/engine/DisplayFgBudget.h"
#include "veyra/engine/EnhancementSettings.h"
#include "veyra/diagnostics/FrameMetrics.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/ResolutionPlan.h"
#include "veyra/Log.h"
#include <format>
#include <memory>
#include <string>

namespace veyra::engine {
static_assert(unsigned(pipeline::SrTarget::Qhd) == 0 && unsigned(pipeline::SrTarget::Uhd4K) == 1 && unsigned(pipeline::SrTarget::Uhd8K) == 2 &&
              unsigned(pipeline::SrTarget::Uhd5K) == 3 && unsigned(pipeline::SrTarget::Uhd6K) == 4 && unsigned(pipeline::SrTarget::Uhd7K) == 5,
              "kPushSmoothSrBoxes follows pipeline::SrTarget's values");

struct PushSmoothSwitches {
    bool srDisplay = false, capFit = false, steady = false, stats = false;
    int envTarget = 0;
    // Stream "delay90": VEYRA_EXP_RP_FIT_DISPLAY=1 (PushSizes.h), a DIAGNOSTIC switch only (a kit reference arm: what the
    // working size itself costs; 4K stays the owner's setting): the SR target follows the display exactly as
    // VEYRA_EXP_RP_SMOOTH_SR_DISPLAY does (the same code path); either switch turns it on.
    bool fitDisplay = false;
    bool followDisplay() const { return srDisplay || fitDisplay; }
};
inline const PushSmoothSwitches& pushSmoothSwitches() {
    static const PushSmoothSwitches switches = [] {
        const auto one = [](const wchar_t* name) {
            wchar_t value[4]{};
            const DWORD length = GetEnvironmentVariableW(name, value, 4);
            return length == 1 && value[0] == L'1';
        };
        PushSmoothSwitches s;
        s.srDisplay = one(L"VEYRA_EXP_RP_SMOOTH_SR_DISPLAY");
        s.fitDisplay = one(L"VEYRA_EXP_RP_FIT_DISPLAY");
        if (s.fitDisplay)
            veyra::log::info("slipstream-sizes", std::format("event=switch fitDisplay=1 srDisplay={} (VEYRA_EXP_RP_FIT_DISPLAY, a diagnostic: live playback evaluates "
                "super resolution into the smallest target that covers the display, to show what the working size itself costs)", s.srDisplay ? 1 : 0));
        s.capFit = one(L"VEYRA_EXP_RP_SMOOTH_CAP_FIT");
        s.steady = one(L"VEYRA_EXP_RP_SMOOTH_STEADY");
        s.stats = one(L"VEYRA_EXP_RP_STATS");
        wchar_t target[8]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_RP_SMOOTH_TARGET", target, 8);
        if (length > 0 && length < 8) {
            const int fps = _wtoi(target);
            if (pushSmoothTargetValid(fps)) s.envTarget = fps;
        }
        if (s.srDisplay || s.capFit || s.steady || s.envTarget)
            veyra::log::info("slipstream-smooth", std::format("switches srDisplay={} capFit={} steady={} target={} (VEYRA_EXP_RP_SMOOTH_*)",
                s.srDisplay ? 1 : 0, s.capFit ? 1 : 0, s.steady ? 1 : 0, s.envTarget));
        return s;
    }();
    return switches;
}
// The size of the monitor the video window is on (0 x 0 when unknown). MonitorFromWindow / GetMonitorInfoW send no
// window message, so the engine thread may ask.
inline void pushSmoothMonitorSize(HWND hwnd, std::uint32_t& width, std::uint32_t& height) {
    width = height = 0;
    if (!hwnd) return;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return;
    const LONG w = info.rcMonitor.right - info.rcMonitor.left, h = info.rcMonitor.bottom - info.rcMonitor.top;
    if (w > 0 && h > 0) { width = static_cast<std::uint32_t>(w); height = static_cast<std::uint32_t>(h); }
}
// The display size the prewarm graph should be built for under VEYRA_EXP_RP_SMOOTH_SR_DISPLAY: what live playback last
// used, else the primary monitor. 0 x 0 without the switch (the prewarm keeps the saved SR target, as before).
inline void pushSmoothPrewarmDisplay(std::uint32_t& width, std::uint32_t& height) {
    width = height = 0;
    if (!pushSmoothSwitches().followDisplay()) return;
    width = pushSmoothShared().displayW.load(std::memory_order_relaxed);
    height = pushSmoothShared().displayH.load(std::memory_order_relaxed);
    if (width && height) return;
    const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (w > 0 && h > 0) { width = static_cast<std::uint32_t>(w); height = static_cast<std::uint32_t>(h); }
    else width = height = 0;
}
inline PushFgMask pushSmoothMask(FrameGenerationBackend backend) {
    PushFgMask mask = 0;
    for (unsigned m = 2; m < 32; ++m)
        if (displayFgMultiplierSelectable(backend, m)) mask |= 1u << m;
    return mask;
}

class PushSmoothHook {
public:
    PushSmoothHook() : sw_(pushSmoothSwitches()) {
        if (sw_.stats) window_ = std::make_unique<PushSmoothWindow>();  // about 70 KB, only with the statistics switch
        if (sw_.stats) latency_ = std::make_unique<PushFgLatencyWindow>();  // stream "fglat": about 40 KB, same switch
        clearShared();   // a stream that ended must not leave its "now 3x" for the next one's row
    }
    ~PushSmoothHook() { clearShared(); }
    PushSmoothHook(const PushSmoothHook&) = delete;
    PushSmoothHook& operator=(const PushSmoothHook&) = delete;
    // Whether choose() has anything to do for a pair: a switch that decides, a target, the statistics, or a mode that
    // must still be turned off. Otherwise the caller skips the per-pair inputs entirely (the default path).
    bool wantsPairs() const { return sw_.capFit || window_ || wantsCosts() || mode_.applied() != 0; }
    // The monitor size describeStages may lower the SR target to (VEYRA_EXP_RP_SMOOTH_SR_DISPLAY), 0 x 0 without the
    // switch: the last one this hook read (see display()).
    std::uint32_t followWidth() const { return sw_.followDisplay() ? displayW_ : 0u; }
    std::uint32_t followHeight() const { return sw_.followDisplay() ? displayH_ : 0u; }
    const PushSmoothSwitches& switches() const { return sw_; }
    // Smoothness Mode may run (its target is set): only then are the budget's P95 costs worth computing per pair.
    bool wantsCosts() const {
        return sw_.envTarget != 0 || ullHeadroom_ || pushSmoothTargetValid(pushSmoothShared().targetFps.load(std::memory_order_relaxed));
    }
    // Stream "fglat": VEYRA_EXP_RP_FG_ULL part "headroom" (PushFgLatency.h): without a target from the switch or the
    // interface, Smoothness Mode runs with pushUllHeadroomTarget (below the VRR ceiling). Off: unchanged.
    void setUllHeadroom(bool on) { ullHeadroom_ = on; }
    // Fills a stage request's display size when the SR-follows-display switch is on (describeStages lowers the SR
    // target with it). Remembered for the window line.
    void display(HWND hwnd, std::uint32_t& width, std::uint32_t& height) {
        width = height = 0;
        if (!sw_.followDisplay() && !window_) return;
        std::uint32_t w = 0, h = 0;
        pushSmoothMonitorSize(hwnd, w, h);
        displayW_ = w;
        displayH_ = h;
        if (sw_.followDisplay()) {
            width = w;
            height = h;
            // For the prewarm graph (PreviewGpuSession), which has no window: the size live playback last used.
            pushSmoothShared().displayW.store(w, std::memory_order_relaxed);
            pushSmoothShared().displayH.store(h, std::memory_order_relaxed);
        }
    }
    // One completed GPU record (graph records carry the stages, the presenter's only its blit).
    void sample(const diagnostics::GpuFrameTiming& timing) {
        if (!window_) return;
        const double blit = measured(timing, diagnostics::GpuStage::Blit);
        if (blit >= 0) window_->stage(PushSmoothStage::Blit, blit);
        const auto span = diagnostics::graphExecutionSpanMs(timing);
        if (!span) return;
        window_->stage(PushSmoothStage::Span, *span);
        window_->stage(PushSmoothStage::Color, measured(timing, diagnostics::GpuStage::Color));
        window_->stage(PushSmoothStage::Clean, measured(timing, diagnostics::GpuStage::Clean));
        window_->stage(PushSmoothStage::Sr, measured(timing, diagnostics::GpuStage::Sr));
        window_->stage(PushSmoothStage::Nr, neural(timing));
        window_->stage(PushSmoothStage::Flow, measured(timing, diagnostics::GpuStage::Flow));
        window_->stage(PushSmoothStage::Fg, measured(timing, diagnostics::GpuStage::FgBatch));
        window_->stage(PushSmoothStage::Fg1, measured(timing, diagnostics::GpuStage::Fg1));   // stream "delay90"
    }
    // A completed full generation group: its FG stage time and how many frames it generated (Smoothness Mode's cost
    // per level, PushSmoothCostModel: a fixed part per pair, the flow, plus a part per generated frame).
    void groupCost(double fgMs, unsigned generated) { costs_.record(generated, fgMs); }
    void ready(unsigned valid, unsigned invalid) { generated_ += valid; invalid_ += invalid; }
    // Stream "fglat": the latency fields (PushFgLatency.h PushFgLatencyWindow), statistics switch only. The engine
    // checks wantsLatency() before it computes anything for them.
    bool wantsLatency() const { return latency_ != nullptr; }
    // Stream "delay90" (PushFgLatencyWindow::parts): from decode or callback, pickupMs = engine pickup, readyMs = the real
    // frame seen ready, callMs = its Present call; interpMs = (M-1)/M x T of its pair; -1 unknown.
    void presentedReal(double ageMs, double holdMs, double phaseMs, double firstMs, double lateMs, bool onPhase, unsigned generatedShown, bool fallback,
                       double pickupMs = -1.0, double readyMs = -1.0, double interpMs = -1.0, double callMs = -1.0) {
        if (!latency_) return;
        latency_->real(ageMs, holdMs, phaseMs, firstMs, onPhase, generatedShown, fallback);
        latency_->parts(onPhase, pickupMs, readyMs, firstMs, interpMs, phaseMs, lateMs, callMs);
        latency_->presented(lateMs);
    }
    void presentedGenerated(double lateMs) {
        if (latency_) latency_->presented(lateMs);
    }

    struct Pair {
        bool applies = false;                  // graph frame generation on, not an image, not a present-sink provider
        bool fgOn = false;                     // frame generation on (a present-sink provider included)
        bool adaptive = false;                 // the adaptive rule chose (pushAdaptive)
        FrameGenerationBackend backend = FrameGenerationBackend::Vfg;
        unsigned ceiling = 1;                  // options.fgMultiplier (1 = generation off)
        double contentFps = 0, capFps = 0, refreshHz = 0;
        double baseMs = -1, blitMs = 0;
        std::uint64_t admitted = 0, rejected = 0, fast = 0;
        std::uint64_t dropCap = 0, dropDeadline = 0, dropCapacity = 0, resets = 0, received = 0;
        std::int64_t holdUntil = 0;
        int vrr = -1;
        bool sr = false;
        std::uint64_t revision = 0;
        const pipeline::EnhanceGraph* graph = nullptr;
        const EnhancementSettings* settings = nullptr;
        std::int64_t now100ns = 0;
        bool phaseLearned = false;             // the pair phase is learned (LivePairLatency), not the fixed interval + base
    };
    // The multiplier for this pair: `multiplier` (the existing rules' choice) unless a switch or the mode decides.
    unsigned choose(unsigned multiplier, const Pair& p) {
        unsigned m = multiplier;
        const bool capRule = p.applies && !p.adaptive && p.ceiling > 2 && p.capFps > 0;
        std::string why = p.adaptive ? "adaptive" : capRule ? "cap" : "user";
        if (p.revision != revision_) { revision_ = p.revision; costs_.reset(); capGate_.reset(); }
        if (p.applies) {
            const PushFgMask mask = pushSmoothMask(p.backend);
            if (sw_.capFit && capRule && p.contentFps > 0.01) {
                const unsigned fit = pushSmoothCapMultiplier(p.ceiling, p.contentFps, p.capFps, mask);
                m = pushSmoothCapFitSwitch(m, capGate_.update(fit, p.now100ns));
                why = "cap-fit";
            }
            const int target = targetFor(p);
            if (pushSmoothTargetValid(target) && p.ceiling >= 3) {
                PushSmoothInput in;
                in.targetFps = target;
                in.contentFps = p.contentFps;
                in.capFps = p.capFps;
                in.refreshHz = p.refreshHz;
                in.ceiling = p.ceiling;
                in.offered = mask;
                in.baseMs = p.baseMs;
                in.costs = &costs_;
                in.blitMs = p.blitMs;
                in.admitted = p.admitted;
                in.rejected = p.rejected;
                in.now100ns = p.now100ns;
                const unsigned before = mode_.applied();
                const PushSmoothDecision d = mode_.update(in);
                if (d.multiplier >= 2) { m = std::min(d.multiplier, p.ceiling); why = "smooth"; }
                auto& shared = pushSmoothShared();
                shared.chosen.store(d.multiplier, std::memory_order_relaxed);
                shared.preferred.store(d.preferred, std::memory_order_relaxed);
                shared.reason.store(static_cast<std::uint8_t>(d.reason), std::memory_order_relaxed);
                shared.stepDowns.store(mode_.stepDowns(), std::memory_order_relaxed);
                shared.loadPermille.store(std::isfinite(d.load) && d.load >= 0 && d.load < 1e6 ? static_cast<std::int64_t>(d.load * 1000.0) : -1, std::memory_order_relaxed);
                if (d.changed)
                    veyra::log::info("slipstream-smooth", std::format("event=change from={}X to={}X preferred={}X reason={} targetFps={} contentFps={:.2f} capFps={:.2f} "
                        "refreshHz={:.3f} load={:.2f} groupMs={:.3f} baseMs={:.3f} stepDowns={} stepUps={} (Smoothness Mode; load and groupMs are a model)",
                        before, d.multiplier, d.preferred, pushSmoothReasonName(d.reason), target, p.contentFps, p.capFps, p.refreshHz, d.load,
                        d.multiplier >= 2 ? costs_.predict(d.multiplier - 1) : -1.0, p.baseMs, mode_.stepDowns(), mode_.stepUps()));
            } else if (mode_.applied()) {
                mode_.reset();
                clearShared();
            }
        } else if (mode_.applied()) {
            mode_.reset();
            clearShared();
        }
        ++unique_;
        // With frame generation off the window records 1 (Package 9 logs printed chosen=2X with backend=none).
        if (window_) windowPair(p.applies ? m : 1u, p, p.applies ? why : std::string(p.fgOn ? "present-sink" : "off"));
        return m;
    }

private:
    // The switch's target, else the interface's, else (VEYRA_EXP_RP_FG_ULL part "headroom") the one below the VRR ceiling.
    int targetFor(const Pair& p) const {
        const int target = sw_.envTarget ? sw_.envTarget : pushSmoothShared().targetFps.load(std::memory_order_relaxed);
        if (pushSmoothTargetValid(target) || !ullHeadroom_) return target;
        return pushUllHeadroomTarget(p.refreshHz, p.vrr);
    }
    static double measured(const diagnostics::GpuFrameTiming& timing, diagnostics::GpuStage stage) {
        const auto& s = timing.gpu[static_cast<std::size_t>(stage)];
        return s.state == diagnostics::SampleState::Measured && s.milliseconds ? *s.milliseconds : -1.0;
    }
    // The layers' own spans added up (as PushNrStatsHook does); the shared stage when no layer span was measured.
    static double neural(const diagnostics::GpuFrameTiming& timing) {
        double sum = 0.0;
        bool any = false;
        for (unsigned layer = 0; layer < diagnostics::kTimedNrLayers; ++layer) {
            const double ms = measured(timing, diagnostics::nrLayerStage(layer));
            if (ms >= 0.0) { sum += ms; any = true; }
        }
        return any ? sum : measured(timing, diagnostics::GpuStage::Nr);
    }
    static void clearShared() {
        auto& shared = pushSmoothShared();
        shared.chosen.store(0, std::memory_order_relaxed);
        shared.preferred.store(0, std::memory_order_relaxed);
        shared.reason.store(0, std::memory_order_relaxed);
        shared.stepDowns.store(0, std::memory_order_relaxed);
        shared.loadPermille.store(-1, std::memory_order_relaxed);
    }
    PushSmoothCounters counters(const Pair& p) const {
        PushSmoothCounters c;
        c.unique = unique_;
        c.received = p.received ? p.received : unique_;
        c.evaluated = p.admitted;
        c.rejected = p.rejected;
        c.fast = p.fast;
        c.generated = generated_;
        c.invalid = invalid_;
        c.dropCap = p.dropCap;
        c.dropDeadline = p.dropDeadline;
        c.dropCapacity = p.dropCapacity;
        c.resets = p.resets;
        c.holds = holds_;
        return c;
    }
    PushSmoothConfigInfo describe(const Pair& p) const {
        PushSmoothConfigInfo info;
        info.backend = p.applies ? std::string(frameGenerationBackendName(p.backend)) : std::string("none");
        info.ceiling = p.applies ? p.ceiling : 1u;
        if (p.graph) {
            info.sourceW = p.graph->sourceWidth();
            info.sourceH = p.graph->sourceHeight();
            info.workW = p.graph->workWidth();
            info.workH = p.graph->workHeight();
            info.flowW = p.graph->flowWidth();
            info.flowH = p.graph->flowHeight();
        }
        info.displayW = displayW_;
        info.displayH = displayH_;
        if (p.sr && p.settings) {
            info.srTargetUser = static_cast<int>(p.settings->srTarget);
            info.srTarget = sw_.followDisplay() ? static_cast<int>(pushSmoothSrTarget(static_cast<std::uint32_t>(p.settings->srTarget), displayW_, displayH_))
                                          : info.srTargetUser;
        }
        info.srFollow = sw_.followDisplay();   // either switch lowers the target (stream "delay90": FIT_DISPLAY too)
        info.capFit = sw_.capFit;
        info.steady = sw_.steady;
        return info;
    }
    void windowPair(unsigned m, const Pair& p, const std::string& why) {
        if (p.holdUntil != lastHold_) {
            if (p.holdUntil > p.now100ns) ++holds_;
            lastHold_ = p.holdUntil;
        }
        if (!window_->started()) {
            info_ = describe(p);
            window_->restart(p.now100ns, counters(p));
            if (latency_) latency_->clear();
            uploadCopiesBase_ = uploadCopies(p);
        }
        window_->pair(m, p.contentFps);
        phaseLearned_ = p.phaseLearned;
        line_.capFps = p.capFps;
        line_.refreshHz = p.refreshHz;
        line_.vrr = p.vrr;
        line_.why = why;
        if (!window_->due(p.now100ns)) return;
        const PushSmoothConfigInfo now = describe(p);
        const PushSmoothCounters c = counters(p);
        // A window in which the configuration changed is dropped without a line (never two configurations mixed).
        if (info_.sameConfiguration(now)) {
            const int target = targetFor(p);
            line_.smoothTarget = pushSmoothTargetValid(target) ? target : 0;
            line_.smoothReason = mode_.applied() ? mode_.reason() : PushSmoothReason::Off;
            line_.smoothDowns = mode_.stepDowns();
            line_.smoothUps = mode_.stepUps();
            const double seconds = double(p.now100ns - window_->start()) / 1e7;
            line_.extra = latency_ ? latency_->fields(seconds, phaseLearned_) : std::string();
            // Stream "delay90": capture frames this graph copied again because their upload buffer was still being read.
            char upload[48];
            const std::uint64_t copies = uploadCopies(p);
            const int n = std::snprintf(upload, sizeof upload, " uploadCpuCopyFps=%.2f",
                seconds > 0.001 && copies >= uploadCopiesBase_ ? double(copies - uploadCopiesBase_) / seconds : -1.0);
            if (n > 0) line_.extra.append(upload, static_cast<std::size_t>(std::min(n, static_cast<int>(sizeof upload) - 1)));
            veyra::log::info("slipstream-smooth", pushSmoothLine(*window_, now, c, p.now100ns, line_));
        }
        info_ = now;
        window_->restart(p.now100ns, c);
        if (latency_) latency_->clear();
        uploadCopiesBase_ = uploadCopies(p);
    }
    static std::uint64_t uploadCopies(const Pair& p) { return p.graph ? p.graph->metrics().captureUploadCpuCopies : 0; }

    PushSmoothSwitches sw_;   // a copy of the process-wide switches (read once)
    std::unique_ptr<PushSmoothWindow> window_;
    std::unique_ptr<PushFgLatencyWindow> latency_;
    PushSmoothnessMode mode_;
    PushAdaptiveFgGate capGate_;
    PushSmoothConfigInfo info_;
    PushSmoothLineInputs line_;
    PushSmoothCostModel costs_;
    std::uint64_t unique_ = 0, generated_ = 0, invalid_ = 0, holds_ = 0, revision_ = 0;
    std::int64_t lastHold_ = 0;
    std::uint32_t displayW_ = 0, displayH_ = 0;
    bool ullHeadroom_ = false, phaseLearned_ = false;
    std::uint64_t uploadCopiesBase_ = 0;   // stream "delay90"
};
}  // namespace veyra::engine
