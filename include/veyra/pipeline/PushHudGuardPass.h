#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// HUD Guard on the GPU (stream "s12-hud"; rules and CPU reference: pipeline/PushHudGuard.h; design and the interface
// for the own frame generator: docs/drafts/HUD_GUARD.md). Part of Live HUD Lock (EnhanceGraph::initHudLock creates it
// beside PushHudLock; restoring switches VEYRA_TEST_RP_LEGACY_HUD_GUARD / _HUD_GUARD_CLEAN in engine/PushLegacy.h).
//   update  (each real frame, base list, before frame generation's input copy): change + contrast + tiles, the state,
//           the mask (3 dispatches).
//   clean   (VFG pairs only): the cleaned copy of the real frame that VfgBackend::capture copies instead of the real one.
//   compose (each generated frame): protected pixels from the real frames, A + t (B - A), written in place.
// Nothing here waits for a later frame; only past real frames are read. But update and clean sit in the real frame's
// base list, whose fence every frame of the pair waits for: every frame shown is later by their GPU time (stage
// HudGuard in [fg-health]). Evidence: one [hud-guard] line a second.
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/PushHudGuard.h"
#include <array>
#include <cstdint>
#include <string>

namespace veyra::pipeline {

class PushHudGuardPass {
public:
    static constexpr UINT kConstants = 16;
    static constexpr unsigned kMaxGenerated = 32;
    static constexpr unsigned kHeld = 2;   // compose source "both from the predicted frame's held copy of B"
    // Bytes the guard's textures take at a size (change 2 + state 4 + mask 1, cleaned copy 4 with the pre-clean): the
    // graph's memory admission counts them (EnhanceGraph::checkMemory).
    static uint64_t textureBytes(uint32_t width, uint32_t height, bool preClean) {
        return uint64_t(width) * height * (7u + (preClean ? 4u : 0u));
    }
    // outputMode as EffectExclusion / PushHudLock (0 sRGB, 1 scRGB, 2 PQ). preClean: the clean pass and its texture.
    bool initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, unsigned outputMode, bool preClean,
                    PushHudGuardParams rules = {});
    // real[0..1]: the graph's real-frame textures by parity; generated: its generated-frame slots; held: PushHudLock's
    // copy of B for predicted frames (may be null).
    void createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* const real[2], ID3D12Resource* const* generated,
                     unsigned generatedCount, ID3D12Resource* held);
    // After real frame `parity` is final: B = real[parity] against A = real[1 - parity]. reset: start over.
    bool update(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool reset);
    // The cleaned copy of real[parity] into cleaned() (left in COMMON). Only after update() of the same frame.
    bool clean(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity);
    ID3D12Resource* cleaned() const { return cleaned_.Get(); }
    bool preClean() const { return cleaned_ != nullptr; }
    // Generated slot `slot` at time t between A = real[1 - parity] and B = real[parity]; source kHeld: both from the held
    // copy (t ignored). Writes only protected pixels. cleanedPair: cleaned() holds this pair's B as the generator read it
    // (clean() ran for it and no later clean() was recorded): background pixels of the clean area keep the generator's output.
    bool compose(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned parity, float t, unsigned source = 0,
                 bool cleanedPair = false);
    // The fence value that completes the list holding the last update() (for the counters' readback).
    void submitted(uint64_t fence);
    // Reads the newest completed counters (no wait) and, at most once a second, logs the [hud-guard] line. gpuMs: the
    // HudGuard stage of the newest timed frame (< 0 unknown); preCleanState: whether the last pair's generator input
    // was cleaned, and why not.
    void report(uint64_t completedFence, double gpuMs, const char* preCleanState);
    const PushHudGuardParams& params() const { return params_; }   // as used for this output mode
    ID3D12Resource* state() const { return state_.Get(); }
    ID3D12Resource* mask() const { return mask_.Get(); }
    std::string describe() const;
    // What the last completed readback said (tests and the line).
    PushHudGuardCounts lastCounts() const { return lastCounts_; }

private:
    uint32_t width_ = 0, height_ = 0, tilesX_ = 0, tilesY_ = 0;
    unsigned outputMode_ = 0, generatedCount_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    PushHudGuardParams params_{};
    ComputePass change_pass_, update_pass_, mask_pass_, clean_pass_, compose_pass_;
    ComPtr<ID3D12Resource> change_, tiles_, state_, mask_, cleaned_, counters_, readback_;
    ID3D12Resource* real_[3]{};
    ID3D12Resource* generated_[kMaxGenerated]{};
    static constexpr unsigned kReadbackSlots = 4;
    std::array<uint64_t, kReadbackSlots> slotFence_{};
    unsigned nextSlot_ = 0, pendingSlot_ = kReadbackSlots;
    uint64_t newestReadFence_ = 0;
    PushHudGuardCounts lastCounts_{};
    uint64_t updates_ = 0, cleans_ = 0, composes_ = 0;
    int64_t lastReportMs_ = 0;
};

}  // namespace veyra::pipeline
