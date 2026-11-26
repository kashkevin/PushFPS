// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/pipeline/PushCleanPass.h"
#include <format>

namespace veyra::pipeline {
namespace {
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
static_assert(sizeof(engine::PushCleanConstants) == PushCleanPass::kConstants * sizeof(float));
// Descriptor layout: encode { 0: t0 picture, 1: u0 encoded }, filter { 2: t0 encoded, 3: u0 picture }.
constexpr UINT kEncodeSrv = 0, kEncodeUav = 1, kFilterSrv = 2, kFilterUav = 3, kSlots = 4;
// Read once, as the graph's other statistics lines (EnhanceGraph.cpp srStatsSwitch).
bool cleanStatsSwitch() {
    static const bool on = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_EXP_RP_STATS", value, 8);
        return length == 1 && value[0] == L'1';
    }();
    return on;
}
}

std::string PushCleanPass::describe() const {
    return std::format("restoration {}x{}{}", width_, height_, hdr_ ? " hdr=pq" : " sdr");
}

bool PushCleanPass::initialize(ID3D12Device* device, uint32_t width, uint32_t height, bool hdr) {
    if (!device || !width || !height) return false;
    width_ = width; height_ = height; hdr_ = hdr;
    encoded_ = makeTexture(device, width_, height_, kColorFormat, true);
    if (!encoded_) return false;
    std::vector<uint8_t> cs;
    if (!pass_.loadShader(hdr_ ? "PushCleanPq.dxil" : "PushCleanSdr.dxil", cs) || !pass_.create(device, cs, kSlots, 1, 1, kConstants)) {
        veyra::log::error("clean", "restoration shader could not be created");
        return false;
    }
    veyra::log::info("clean", std::format("created {}", describe()));
    return true;
}

void PushCleanPass::createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* picture) {
    picture_ = picture;
    stager.stageSrv(picture, nullptr, pass_.heap.Get(), kEncodeSrv);
    makeUav(device, encoded_.Get(), kColorFormat, cpuHandleOf(pass_, kEncodeUav));
    stager.stageSrv(encoded_.Get(), nullptr, pass_.heap.Get(), kFilterSrv);
    makeUav(device, picture, kColorFormat, cpuHandleOf(pass_, kFilterUav));
}

bool PushCleanPass::dispatch(ID3D12GraphicsCommandList* list, StateTracker& tracker, engine::PushCleanSettings settings) {
    if (!list || !picture_ || !encoded_) return false;
    settings.pq = hdr_;
    settings.outputBits = hdr_ ? 10 : 8;
    const engine::PushCleanParams params = engine::pushCleanParams(settings);
    lastStrength_ = settings.strength; lastHud_ = settings.hudProtect; lastParts_ = settings.parts;
    if (!params.active) return false;
    engine::PushCleanConstants constants = engine::pushCleanConstants(params);
    constants.width = width_; constants.height = height_;
    // 1. encode
    constants.stage = 0;
    tracker.transition(list, picture_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, encoded_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pass_.bind(list, reinterpret_cast<const float*>(&constants), gpuHandleOf(pass_, kEncodeSrv).ptr, gpuHandleOf(pass_, kEncodeUav).ptr);
    list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    tracker.uavBarrier(list, encoded_.Get());
    // 2. filter: writes the picture only where a pixel changed
    constants.stage = 1;
    tracker.transition(list, encoded_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, picture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pass_.bind(list, reinterpret_cast<const float*>(&constants), gpuHandleOf(pass_, kFilterSrv).ptr, gpuHandleOf(pass_, kFilterUav).ptr);
    list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    tracker.uavBarrier(list, picture_);
    tracker.transition(list, picture_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

void PushCleanPass::noteTimings(const std::vector<diagnostics::GpuFrameTiming>& timings) {
    if (!cleanStatsSwitch()) return;
    const auto now = std::chrono::steady_clock::now();
    if (windowStart_ == std::chrono::steady_clock::time_point{}) windowStart_ = now;
    for (const auto& sample : timings) {
        const auto& clean = sample.gpu[size_t(diagnostics::GpuStage::Clean)];
        if (clean.state == diagnostics::SampleState::Measured && clean.milliseconds) window_.add(*clean.milliseconds);
    }
    if (now - windowStart_ < std::chrono::seconds(5)) return;
    if (window_.frames())
        veyra::log::info("slipstream-clean", engine::pushCleanStatsLine(window_.frames(), lastStrength_, width_, height_,
            window_.percentile(50.0), window_.percentile(95.0), lastHud_, hdr_, lastParts_));
    window_.clear();
    windowStart_ = now;
}
}
