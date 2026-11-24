// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Windows-side glue of stream "delay90" (decisions and the line in PushSizes.h, which is portable and tested). Included by
// src/engine/EngineController.cpp only; everything runs on the engine thread. It writes `[slipstream-sizes]`: the live
// graph's real working sizes (source, SR input, SR result, base after any shrink, NR, flow, FG, output), the swap chain's
// back buffer, the video window's client area, the monitor and its display mode, and fit= / fitReason=. Always on, like
// [slipstream-config]: the engine asks due() once per processed frame (one comparison); at most once a second the facts
// are read (GetClientRect and MonitorFromWindow; GetMonitorInfoW and EnumDisplaySettingsW only when the monitor, the plan
// or the back buffer changed; none of them sends a window message) and the line is written only when its text changed. It decides nothing. Blind Windows code: unverified on Windows.
#include "veyra/engine/PushSizes.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/ResolutionPlan.h"
#include "veyra/Log.h"
#include <cstdint>
#include <string>

namespace veyra::engine {

class PushSizesHook {
public:
    // Whether the facts should be read now (no more than once a second).
    bool due(std::int64_t now100ns) { return gate_.due(now100ns); }
    struct Inputs {
        const pipeline::EnhanceGraph* graph = nullptr;
        const pipeline::ResolutionPlan* plan = nullptr;   // the live plan (EngineController: measured.resolution)
        bool sr = false, dualSr = false, hdrOutput = false;
        int srTargetAsked = -1, srTargetUsed = -1;
        std::uint32_t backBufferW = 0, backBufferH = 0;
        bool srDisplaySwitch = false, fitSwitch = false;
    };
    void observe(HWND window, const Inputs& in) {
        if (!in.graph || !in.plan || !in.graph->initialized()) return;
        const auto& g = *in.graph;
        PushSizesFacts f;
        f.source = {g.sourceWidth(), g.sourceHeight()};
        f.srInput = {in.plan->srInput.width, in.plan->srInput.height};
        f.srEvaluated = {in.plan->srOutput.width, in.plan->srOutput.height};
        f.base = {g.workWidth(), g.workHeight()};
        if (g.nrEnabled()) f.nr = {g.nrWidth(), g.nrHeight()};
        f.flow = {g.flowWidth(), g.flowHeight()};
        if (g.fgEnabled()) f.fg = f.base;   // frame generation works on the enhanced work texture (ResolutionPlan: fg = base)
        f.output = f.base;                  // what the presenter scales into the window
        f.backBuffer = {in.backBufferW, in.backBufferH};
        f.sr = in.sr;
        f.hdrOutput = in.hdrOutput;
        f.srTarget = in.sr ? in.srTargetUsed : -1;
        f.srTargetAsked = in.sr ? in.srTargetAsked : -1;
        f.srDisplaySwitch = in.srDisplaySwitch;
        f.fitSwitch = in.fitSwitch;
        if (window) {
            RECT client{};
            if (GetClientRect(window, &client) && client.right > client.left && client.bottom > client.top)
                f.client = {static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top)};
            // The monitor's size and display mode are read only when the monitor, the plan or the back buffer changed
            // (stream delay90 review): in the steady state no display query runs on the engine thread, only
            // MonitorFromWindow's handle lookup to notice a move to another monitor.
            const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            const bool same = cacheValid_ && monitor == cachedMonitor_ && in.plan->settingsRevision == cachedRevision_ &&
                              f.base == cachedBase_ && f.backBuffer == cachedBackBuffer_;
            if (!same) {
                cacheValid_ = true;
                cachedMonitor_ = monitor;
                cachedRevision_ = in.plan->settingsRevision;
                cachedBase_ = f.base;
                cachedBackBuffer_ = f.backBuffer;
                cachedDisplay_ = cachedMode_ = PushWxH{};
                cachedModeHz_ = 0;
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (monitor && GetMonitorInfoW(monitor, &info)) {
                    const LONG w = info.rcMonitor.right - info.rcMonitor.left, h = info.rcMonitor.bottom - info.rcMonitor.top;
                    if (w > 0 && h > 0) cachedDisplay_ = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
                    DEVMODEW mode{};
                    mode.dmSize = sizeof(mode);
                    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
                        cachedMode_ = {static_cast<std::uint32_t>(mode.dmPelsWidth), static_cast<std::uint32_t>(mode.dmPelsHeight)};
                        cachedModeHz_ = static_cast<unsigned>(mode.dmDisplayFrequency);
                    }
                }
            }
            f.display = cachedDisplay_;
            f.mode = cachedMode_;
            f.modeHz = cachedModeHz_;
        }
        PushFitInputs fit;
        fit.srInput = f.srInput;
        fit.srEvaluated = f.srEvaluated;
        fit.work = f.base;
        fit.display = f.display;
        fit.sr = in.sr;
        fit.dualSr = in.dualSr;
        fit.followDisplay = in.srDisplaySwitch || in.fitSwitch;
        fit.targetAsked = f.srTargetAsked;
        fit.targetUsed = f.srTarget;
        f.verdict = pushFitVerdict(fit);
        // Stream "laghunt": a new live graph (another graph object, or a settings revision) is written at once; a
        // line written after held-back changes says how many looks were held (PushSizesGate).
        if (in.graph != lastGraph_ || in.plan->settingsRevision != lastRevision_) gate_.forceNext();
        lastGraph_ = in.graph;
        lastRevision_ = in.plan->settingsRevision;
        const std::string line = pushSizesLine(f);
        if (gate_.changed(line)) {
            const std::uint64_t held = gate_.takeHeld();
            veyra::log::info("slipstream-sizes", held ? line + " held=" + std::to_string(held) : line);
        }
    }

private:
    PushSizesGate gate_;
    const pipeline::EnhanceGraph* lastGraph_ = nullptr;   // stream "laghunt": a rebuild is written at once
    std::uint64_t lastRevision_ = 0;
    // The last display facts read (see observe): reused until the monitor, the plan or the back buffer changes.
    HMONITOR cachedMonitor_ = nullptr;
    std::uint64_t cachedRevision_ = 0;
    PushWxH cachedBase_, cachedBackBuffer_, cachedDisplay_, cachedMode_;
    unsigned cachedModeHz_ = 0;
    bool cacheValid_ = false;
};

}  // namespace veyra::engine
