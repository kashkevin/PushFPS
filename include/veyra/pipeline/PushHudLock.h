#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS Live HUD Lock (EnhancementSettings::hudLock): the HUD found pixel by
// pixel on every real frame and kept out of frame generation.
//
// Frame generation interpolates every pixel between two real frames. Over a
// moving picture it pulls a still, sharp overlay (text, crosshair, health bar,
// minimap frame) along with the motion around it, so the overlay smears or
// wobbles in the generated frames. HUD protection fixes this with boxes drawn
// in advance; HUD Lock finds those pixels itself, every frame:
//   1. update (one dispatch per real frame): a per-pixel stillness level grows
//      while the pixel stays the same from frame to frame and drops to zero the
//      moment it changes; beside it, how much contrast the pixel has with its
//      neighbours (a held maximum that fades).
//   2. compose (one dispatch per generated frame): where a pixel has been still
//      for about a second and has an edge, the generated frame takes it from the
//      held real frame instead (feathered, grown by one pixel), so the overlay
//      stays exactly where the console drew it.
// A still pixel looks the same in the real frame, so locking one that is not
// HUD (a still wall) changes nothing visible; a pixel that starts to move is
// released on that same frame. Capture noise that never lets a pixel settle
// leaves it to frame generation, as without HUD Lock.
#include "veyra/pipeline/GpuPassUtils.h"
#include <algorithm>
#include <array>
#include <string>

namespace veyra::pipeline {

class PushHudLock {
public:
    static constexpr UINT kConstants = 8;
    static constexpr UINT kMaxGenerated = 32;
    // outputMode: how the frames are encoded, as EffectExclusion (0 sRGB, 1 scRGB, 2 PQ).
    bool initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, unsigned outputMode);
    // real[0..1]: the graph's two real-frame textures by parity; generated: the
    // generated-frame textures, in the graph's slot order.
    void createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* const real[2],
                     ID3D12Resource* const* generated, unsigned generatedCount);
    // After real frame `parity` is final: learn from real[1-parity] -> real[parity].
    // reset: no valid previous frame (start, seek, settings change): start over.
    bool update(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool reset);
    // Locked pixels of generated[slot] come from the held real frame real[heldParity]. heldParity kPredictedHeld (2):
    // from predictedHeld(), a copy of the real frame the predicted frame follows (Package 16, "Lower delay"), so the
    // predicted frame's compose never touches the real frame's own texture while it is being presented.
    bool compose(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned heldParity);
    static constexpr unsigned kPredictedHeld = 2;
    // Package 16: creates the copy texture (working size and format, about 33 MB at 3840x2160 in 32-bit formats). Call
    // before createViews; only the graph's predicted-frame pass asks for it. The caller copies B into it (COMMON before
    // and after) before compose(..., kPredictedHeld).
    bool enablePredictedHeld(ID3D12Device* device);
    ID3D12Resource* predictedHeld() const { return heldCopy_.Get(); }
    ID3D12Resource* state() const { return state_.Get(); }
    unsigned outputMode() const { return outputMode_; }
    std::string describe() const;

private:
    uint32_t width_ = 0, height_ = 0;
    unsigned outputMode_ = 0, generatedCount_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    ComputePass update_, compose_;
    ComPtr<ID3D12Resource> state_, scratch_, heldCopy_;
    ID3D12Resource* real_[3]{};   // [2]: heldCopy_ (Package 16)
    ID3D12Resource* generated_[kMaxGenerated]{};
};
}
