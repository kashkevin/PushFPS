#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS restoration ("clean-up") stage (EnhancementSettings::clean, docs/RESTORATION.md): removes
// compression blocks, ringing and banding from the decoded console picture at the stream's size, before
// any upscaler or neural layer. Single frame, no history, plain HLSL (every DirectX 12 GPU). The maths is
// engine/PushClean.h; shaders/PushClean.hlsl mirrors it. Two compute dispatches on the graph's linear
// FP16 picture, in place:
//   1. encode  picture (linear) -> encoded_ (sRGB, or BT.2020 PQ for HDR), the stage's only texture
//   2. filter  encoded_ -> picture, written only where the filter changed the pixel (everything it
//              protects keeps its own texel, bit for bit)
// With a strength of 0 or no part switched on nothing is dispatched. Unverified on Windows.
#include "veyra/diagnostics/FrameMetrics.h"
#include "veyra/engine/PushClean.h"
#include "veyra/engine/PushSrPlan.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <chrono>
#include <string>
#include <vector>

namespace veyra::pipeline {

class PushCleanPass {
public:
    static constexpr UINT kConstants = 20;   // engine::PushCleanConstants
    // hdr: the graph works in scRGB (HDR) rather than SDR linear BT.709.
    bool initialize(ID3D12Device* device, uint32_t width, uint32_t height, bool hdr);
    // picture: the graph's linear FP16 picture at the stream's size; it is cleaned in place.
    void createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* picture);
    // strength, hudProtect, parts and seed of `settings` are used; pq and outputBits follow hdr.
    // Returns false when nothing was dispatched (off): the picture is exactly as it was. Leaves the
    // picture in NON_PIXEL_SHADER_RESOURCE when it ran.
    bool dispatch(ID3D12GraphicsCommandList* list, StateTracker& tracker, engine::PushCleanSettings settings);
    // `[slipstream-clean]` (VEYRA_EXP_RP_STATS=1): the stage's GPU time from the graph's own timestamps
    // (diagnostics::GpuStage::Clean), one line per 5 s.
    void noteTimings(const std::vector<diagnostics::GpuFrameTiming>& timings);
    std::string describe() const;
    bool hdr() const { return hdr_; }

private:
    uint32_t width_ = 0, height_ = 0;
    bool hdr_ = false;
    ComputePass pass_;
    ComPtr<ID3D12Resource> encoded_;
    ID3D12Resource* picture_ = nullptr;
    // Statistics (engine thread only).
    engine::PushSrWindow window_;
    std::chrono::steady_clock::time_point windowStart_{};
    int lastStrength_ = 0;
    bool lastHud_ = true;
};
}
