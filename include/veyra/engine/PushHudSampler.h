#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Takes the frames for automatic HUD detection (PushHud.h) from the presented
// picture, the same way live recording reads it (PresentCapture.h): while a
// detection runs, one real (not generated) frame every `intervalMs` is copied
// into a readback buffer inside the presentation command list. Nothing is
// recorded while no detection runs, and at most one copy is in flight, so the
// presenter never waits for it. status() (any thread) reduces finished copies
// to the luma grid outside the lock and runs detect() after the last one.
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/PushHud.h"

namespace veyra::engine {

class PushHudSampler final : public PresentCapture {
public:
    struct Status {
        bool running = false, done = false, failed = false;
        uint32_t taken = 0, target = 0;
        std::wstring error;
        hud::Detection detection;           // valid when done
        std::vector<uint8_t> preview;       // RGB, hud::kPreviewWidth x kPreviewHeight, of the last frame taken
        uint64_t revision = 0;              // +1 per start
    };
    // Starts a detection over `samples` frames (at least hud::kMinFrames).
    void start(uint32_t samples = 12, uint32_t intervalMs = 350);
    void cancel();
    Status status();

    bool record(ID3D12GraphicsCommandList* list, const Frame& frame) override;
    bool capturing() const override { std::lock_guard lock(m_); return running_ || pending_ || inFlight_ || reading_; }
    void submitted(ID3D12Fence* fence, uint64_t value) override;
    void discarded() override;

private:
    void finishLocked();
    mutable std::mutex m_;
    bool running_ = false, done_ = false, failed_ = false, pending_ = false, inFlight_ = false, reading_ = false;
    uint32_t target_ = 0, intervalMs_ = 350;
    uint64_t revision_ = 0;
    std::wstring error_;
    std::chrono::steady_clock::time_point nextDue_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    uint64_t readbackBytes_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    uint32_t width_ = 0, height_ = 0;
    hud::PixelLayout layout_ = hud::PixelLayout::Rgba8;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t fenceValue_ = 0;
    std::vector<hud::LumaFrame> frames_;
    hud::Detection detection_;
    std::vector<uint8_t> preview_;
};

} // namespace veyra::engine
