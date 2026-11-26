// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/pipeline/PushHudLock.h"
#include <bit>
#include <format>

namespace veyra::pipeline {
namespace {
constexpr DXGI_FORMAT kStateFormat = DXGI_FORMAT_R16G16_FLOAT;  // x: stillness 0..1, y: held contrast
// Descriptor layouts. update: per parity { t0 current, t1 previous, u0 state }.
// compose: per (slot, held) { t0 generated, t1 held real, t2 state, u0 scratch }; held 0, 1: the real frames by parity,
// 2: the copy of the real frame a predicted frame follows (Package 16, only when enablePredictedHeld was called).
constexpr UINT kUpdateStride = 3, kComposeStride = 4, kHeldSources = 3;
}

std::string PushHudLock::describe() const {
    return std::format("HUD lock {}x{} mode={} generated={}", width_, height_, outputMode_, generatedCount_);
}

bool PushHudLock::initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, unsigned outputMode) {
    if (!device || !width || !height || format == DXGI_FORMAT_UNKNOWN) return false;
    width_ = width; height_ = height; format_ = format; outputMode_ = outputMode;
    state_ = makeTexture(device, width_, height_, kStateFormat, true);
    scratch_ = makeTexture(device, width_, height_, format_, true);
    if (!state_ || !scratch_) return false;
    std::vector<uint8_t> cs;
    if (!update_.loadShader("PushHudLockUpdate.dxil", cs) || !update_.create(device, cs, 2 * kUpdateStride, 2, 1, kConstants)) return false;
    if (!compose_.loadShader("PushHudLockCompose.dxil", cs) ||
        !compose_.create(device, cs, kMaxGenerated * kHeldSources * kComposeStride, 3, 1, kConstants)) return false;
    // Stream "s12-settings": the generated-frame slots are bound later (createViews), so this line no longer prints a
    // "generated=0" that read as "no generated frame is locked"; the views line below says how many.
    veyra::log::info("hud-lock", std::format("created HUD lock {}x{} mode={} (generated-frame slots are bound with the views)", width_, height_, outputMode_));
    return true;
}

static_assert(PushHudLock::kPredictedHeld + 1 == kHeldSources, "the held copy is the last held source");

bool PushHudLock::enablePredictedHeld(ID3D12Device* device) {
    if (!device || !width_ || !height_ || format_ == DXGI_FORMAT_UNKNOWN) return false;
    if (!heldCopy_) heldCopy_ = makeTexture(device, width_, height_, format_, false);
    if (heldCopy_) veyra::log::info("hud-lock", std::format("predicted-frame held copy {}x{} (Lower delay)", width_, height_));
    return heldCopy_ != nullptr;
}

void PushHudLock::createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* const real[2],
                              ID3D12Resource* const* generated, unsigned generatedCount) {
    real_[0] = real[0]; real_[1] = real[1]; real_[2] = heldCopy_.Get();
    generatedCount_ = std::min<unsigned>(generatedCount, kMaxGenerated);
    veyra::log::info("hud-lock", std::format("views {} (every generated frame of these slots gets the locked pixels)", describe()));
    for (unsigned i = 0; i < generatedCount_; ++i) generated_[i] = generated[i];
    for (unsigned parity = 0; parity < 2; ++parity) {
        const UINT base = parity * kUpdateStride;
        stager.stageSrv(real_[parity], nullptr, update_.heap.Get(), base);
        stager.stageSrv(real_[1 - parity], nullptr, update_.heap.Get(), base + 1);
        makeUav(device, state_.Get(), kStateFormat, cpuHandleOf(update_, base + 2));
    }
    for (unsigned slot = 0; slot < generatedCount_; ++slot)
        for (unsigned held = 0; held < kHeldSources; ++held) {
            if (!real_[held]) continue;
            const UINT base = (slot * kHeldSources + held) * kComposeStride;
            stager.stageSrv(generated_[slot], nullptr, compose_.heap.Get(), base);
            stager.stageSrv(real_[held], nullptr, compose_.heap.Get(), base + 1);
            stager.stageSrv(state_.Get(), nullptr, compose_.heap.Get(), base + 2);
            makeUav(device, scratch_.Get(), format_, cpuHandleOf(compose_, base + 3));
        }
}

bool PushHudLock::update(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool reset) {
    if (!list || parity > 1 || !real_[0] || !real_[1]) return false;
    float c[kConstants]{};
    c[0] = std::bit_cast<float>(width_); c[1] = std::bit_cast<float>(height_);
    c[2] = std::bit_cast<float>(uint32_t(reset ? 1 : 0)); c[3] = std::bit_cast<float>(uint32_t(outputMode_));
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, real_[1 - parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT base = parity * kUpdateStride;
    update_.bind(list, c, gpuHandleOf(update_, base).ptr, gpuHandleOf(update_, base + 2).ptr);
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    tracker.uavBarrier(list, state_.Get());
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[1 - parity], D3D12_RESOURCE_STATE_COMMON);
    return true;
}

bool PushHudLock::compose(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned heldParity) {
    if (!list || slot >= generatedCount_ || heldParity >= kHeldSources || !generated_[slot] || !real_[heldParity]) return false;
    auto* generated = generated_[slot];
    float c[kConstants]{};
    c[0] = std::bit_cast<float>(width_); c[1] = std::bit_cast<float>(height_);
    c[2] = std::bit_cast<float>(uint32_t(outputMode_));
    tracker.transition(list, generated, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, real_[heldParity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, scratch_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT base = (slot * kHeldSources + heldParity) * kComposeStride;
    compose_.bind(list, c, gpuHandleOf(compose_, base).ptr, gpuHandleOf(compose_, base + 3).ptr);
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    tracker.uavBarrier(list, scratch_.Get());
    tracker.transition(list, scratch_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    tracker.transition(list, generated, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(generated, scratch_.Get());
    tracker.transition(list, generated, D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, scratch_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[heldParity], D3D12_RESOURCE_STATE_COMMON);
    return true;
}
}
