// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "present" R1: the GPU half of the in-picture interface items (engine/PushVideoOverlay.h). VideoPresenter owns one;
// it runs on the presentation queue's own command lists, after the picture's present blit (shaders/PushVideoOverlay.hlsl).
//   - upload(): a NEW picture only (a few times a second at most): one CopyTextureRegion from a persistent upload buffer.
//     The CPU writes that buffer only after the previous upload's list has finished (its fence); otherwise the new picture
//     waits for a later present and the previous one keeps being drawn. The texture grows (never shrinks) only after the
//     last list that read it has finished, so nothing in flight is ever overwritten.
//   - draw(): viewport and scissor are the picture's rectangle in buffer pixels: nothing outside it is written.
// Nothing is recorded when no picture is published (VideoPresenter checks the board's version first).
// Windows only (D3D12). Unverified on Windows hardware; WARP tests: tests/integration/PushGpuTests.cpp "video-overlay*".
#include "veyra/engine/PushVideoOverlay.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace veyra::engine {

class PushPresentOverlay {
public:
    enum class Mode : unsigned { Sdr = 0, ScRgb = 1, Pq = 2 };
    static Mode modeFor(bool hdrOutput, bool hdr10Output) { return hdr10Output ? Mode::Pq : hdrOutput ? Mode::ScRgb : Mode::Sdr; }
    // Shaders and the blended pipeline for render targets of `format`. Idempotent for the same device, format and mode.
    bool create(ID3D12Device* device, DXGI_FORMAT format, Mode mode);
    bool created() const { return pso_ != nullptr; }
    void reset();
    // Records the upload of `image` when it can be done without touching anything in flight (see above). True when the copy was recorded (the caller then calls
    // submitted() with the list's fence value); false when the picture must wait (nothing recorded).
    bool upload(ID3D12GraphicsCommandList* list, const PushVideoOverlayImage& image);
    // Records the blend of the current picture onto `rtv` (a buffer of bufferW x bufferH in RENDER_TARGET state) for a window
    // of windowW x windowH client pixels. False when there is nothing to draw (no picture, outside the buffer).
    bool draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned windowW, unsigned windowH, unsigned bufferW, unsigned bufferH);
    // After the list that recorded upload() and/or draw() was submitted and will signal `value` on `fence` (the presenter's).
    void submitted(ID3D12Fence* fence, uint64_t value, bool uploaded, bool drew) {
        if (!uploaded && !drew) return;
        fence_ = fence;
        if (uploaded) uploadValue_ = value;
        if (drew || uploaded) useValue_ = value;
    }
    bool hasPicture() const { return pictureW_ > 0 && pictureH_ > 0; }
    void clearPicture() { pictureW_ = pictureH_ = 0; }
    uint64_t pictureVersion() const { return pictureVersion_; }
    void setPictureVersion(uint64_t v) { pictureVersion_ = v; }

private:
    bool complete(ID3D12Fence* fence, uint64_t value) const { return !value || (fence && fence->GetCompletedValue() >= value); }
    // A creation that failed (a missing shader file) is not tried again for the same device, format and mode: the
    // presenter would otherwise read the files from disk on every present.
    ID3D12Device* failedDevice_ = nullptr;
    DXGI_FORMAT failedFormat_ = DXGI_FORMAT_UNKNOWN;
    Mode failedMode_ = Mode::Sdr;
    ID3D12Device* device_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    Mode mode_ = Mode::Sdr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_, upload_;
    D3D12_RESOURCE_STATES textureState_ = D3D12_RESOURCE_STATE_COMMON;
    unsigned textureW_ = 0, textureH_ = 0;
    uint64_t uploadBytes_ = 0;
    ID3D12Fence* fence_ = nullptr;
    uint64_t uploadValue_ = 0, useValue_ = 0;
    int pictureX_ = 0, pictureY_ = 0;
    unsigned pictureW_ = 0, pictureH_ = 0;
    uint64_t pictureVersion_ = 0;
};

}  // namespace veyra::engine
