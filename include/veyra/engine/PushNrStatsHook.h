// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Windows-side glue for the `[slipstream-nr]` line (S5_CONTRACT.md item 4; maths in PushNrStats.h, which is
// portable and tested). Included by src/engine/EngineController.cpp only; everything runs on the engine
// thread inside collectTimings, which already walks the GPU timestamps. With VEYRA_EXP_RP_STATS unset (read
// once) sample() and flush() return at once: nothing is stored and nothing is logged.
// Blind Windows code: unverified on Windows.
#include "veyra/engine/PushNrStats.h"
#include "veyra/engine/EnhancementSettings.h"
#include "veyra/diagnostics/FrameMetrics.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/Log.h"
#include <cstring>
#include <memory>

namespace veyra::engine {
class PushNrStatsHook {
public:
    PushNrStatsHook() {
        wchar_t value[4]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_RP_STATS", value, 4);
        on_ = length == 1 && value[0] == L'1';
        if (on_) window_ = std::make_unique<PushNrWindow>();  // about 50 KB, only when the statistics are asked for
    }
    bool on() const { return on_; }
    // One completed GPU record. The presenter's records (its blit only) carry no graph stage and are skipped.
    void sample(const diagnostics::GpuFrameTiming& timing) {
        if (!on_ || !window_) return;
        const auto graphMs = diagnostics::graphExecutionSpanMs(timing);
        if (!graphMs) return;
        PushNrFrame frame;
        frame.graphMs = *graphMs;
        frame.nrMs = neuralLayers(timing);
        frame.srMs = measured(timing, diagnostics::GpuStage::Sr);
        frame.fgMs = measured(timing, diagnostics::GpuStage::FgBatch);
        window_->add(frame);
    }
    // After the records of one collection. Costs one clock comparison until the window is 5 s old; only then is
    // the configuration read (once per window) and the line written. A window in which the configuration changed
    // is dropped without a line, so a line never mixes two configurations ("at most one line per 5 s").
    void flush(const pipeline::EnhanceGraph& graph, const EnhancementSettings& settings, const gfx::D3D12DeviceContext& context,
               std::int64_t now100ns) {
        if (!on_ || !window_) return;
        if (!haveInfo_) {
            info_ = describe(graph, settings);
            haveInfo_ = true;
        }
        if (!window_->due(now100ns)) return;
        const PushNrStatsInfo now = describe(graph, settings);
        if (info_.sameConfiguration(now) && window_->frames()) {
            std::uint64_t budget = 0, usage = 0;
            if (context.videoMemoryInfo(budget, usage)) {
                info_.vramMiB = static_cast<std::int64_t>(usage >> 20);
                info_.budgetMiB = static_cast<std::int64_t>(budget >> 20);
            } else {
                info_.vramMiB = -1;
                info_.budgetMiB = -1;
            }
            veyra::log::info("slipstream-nr", pushNrStatsLine(*window_, info_));
        }
        info_ = now;
        window_->restart(now100ns);
    }

private:
    static double measured(const diagnostics::GpuFrameTiming& timing, diagnostics::GpuStage stage) {
        const auto& s = timing.gpu[static_cast<std::size_t>(stage)];
        return s.state == diagnostics::SampleState::Measured && s.milliseconds ? *s.milliseconds : -1.0;
    }
    // Every layer marks the shared Nr stage, which therefore holds only the LAST layer's evaluate
    // (diagnostics/EnhancementProcessingTime.h). The layers' own spans (evaluate and decode) are added up
    // instead; the shared stage is the answer only when no layer span was measured.
    static double neuralLayers(const diagnostics::GpuFrameTiming& timing) {
        double sum = 0.0;
        bool any = false;
        for (unsigned layer = 0; layer < diagnostics::kTimedNrLayers; ++layer) {
            const double ms = measured(timing, diagnostics::nrLayerStage(layer));
            if (ms >= 0.0) { sum += ms; any = true; }
        }
        return any ? sum : measured(timing, diagnostics::GpuStage::Nr);
    }
    // The version resource of the loaded NVIDIA runtime file, read the way DlssNrRuntimeAdapter.cpp reads it
    // (no version.lib). Numbers only: no path and no name leave this function. 0/0 when it is not loaded.
    static void runtimeVersion(std::uint32_t& versionMs, std::uint32_t& versionLs) {
        versionMs = 0;
        versionLs = 0;
        const HMODULE module = GetModuleHandleW(L"nvngx_dlssnr.dll");
        if (!module) return;
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
        if (!resource) return;
        const DWORD size = SizeofResource(module, resource);
        const HGLOBAL loaded = LoadResource(module, resource);
        const auto* bytes = loaded ? static_cast<const std::uint8_t*>(LockResource(loaded)) : nullptr;
        for (DWORD at = 0; bytes && at + 52 <= size; at += 4) {
            std::uint32_t signature = 0;
            std::memcpy(&signature, bytes + at, 4);
            if (signature != 0xFEEF04BDu) continue;
            std::memcpy(&versionMs, bytes + at + 8, 4);
            std::memcpy(&versionLs, bytes + at + 12, 4);
            return;
        }
    }
    static PushNrStatsInfo describe(const pipeline::EnhanceGraph& graph, const EnhancementSettings& settings) {
        PushNrStatsInfo info;
        const bool layerRuns = graph.nrEnabled() && graph.nrCreated() && graph.nrLayerCount() > 0;
        info.layers = layerRuns ? graph.nrLayerCount() : 0u;
        info.internalW = layerRuns ? graph.nrWidth() : 0u;
        info.internalH = layerRuns ? graph.nrHeight() : 0u;
        info.outputW = graph.workWidth();
        info.outputH = graph.workHeight();
        const int runtimeId = static_cast<int>(settings.nrRuntime);
        info.runtime = pushNrStatsRuntimeName(runtimeId, layerRuns);
        if (layerRuns && runtimeId != static_cast<int>(NrRuntime::LmxxfAmd)) runtimeVersion(info.versionMs, info.versionLs);
        PushNrCopyConfig copies;
        copies.nrLayers = info.layers;
        // A layer is downsampled when its inference size is not its input's size (source before the upscaler,
        // work size after it). Every layer is counted like the first: the graph reports one inference size.
        const std::uint32_t inputW = graph.nrBeforeSr() ? graph.sourceWidth() : graph.workWidth();
        const std::uint32_t inputH = graph.nrBeforeSr() ? graph.sourceHeight() : graph.workHeight();
        copies.nrLayersResized = layerRuns && (info.internalW != inputW || info.internalH != inputH) ? info.layers : 0u;
        const bool enlarges = graph.workWidth() != graph.sourceWidth() || graph.workHeight() != graph.sourceHeight();
        copies.sr = pushNrSrKind(graph.srEnabled(), settings.videoSrQuality, enlarges);
        copies.hdr = graph.hdrInput() && graph.hdrOutput();
        info.copyCount = pushNrCopyCount(copies);
        return info;
    }

    bool on_ = false;
    bool haveInfo_ = false;
    PushNrStatsInfo info_{};
    std::unique_ptr<PushNrWindow> window_;
};
}  // namespace veyra::engine
