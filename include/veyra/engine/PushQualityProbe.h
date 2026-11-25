// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS ground-truth frame-generation quality test, the engine part (maths in PushQuality.h).
// While running, the engine thread calls withhold() for every source frame headed for the graph:
// every second one is kept back (frame generation then makes that moment itself) and a luma copy of
// it is stored, with its neighbours for the blend baseline. As a PresentCapture beside the recorders
// it reads back the generated frame shown at a withheld frame's time (and some real frames, for the
// tone fit) and tracks whether generated frames come on time. status(), polled by the UI, finishes
// the readbacks and returns the report. Needs pictures in CPU memory (capture cards, frame input);
// hardware-decoded streams are reported as unsupported.
#pragma once
#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/PushQuality.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct AVFrame;

namespace veyra::engine {

class PushQualityProbe final : public PresentCapture {
public:
    struct Options {
        uint32_t seconds = 6;                 // how long frames are withheld (2..30)
        uint32_t targetSamples = 24;          // stops earlier once this many generated frames are compared
        std::vector<quality::Box> hud;        // HUD boxes for the HUD PSNR (may be empty)
    };
    struct Status {
        bool running = false, done = false, failed = false;
        std::wstring error;
        uint32_t samples = 0, target = 0;
        uint64_t withheld = 0;
        quality::Report report;               // valid when done
        uint64_t revision = 0;
    };
    void start(const Options& options);
    void cancel();
    Status status();
    bool running() const { return running_.load(std::memory_order_relaxed); }

    // Engine thread, for each source frame going to the graph. True: keep this one back.
    // packedCarrier: the frame's pixels are a packed capture format in a BGRA carrier (not RGB).
    bool withhold(const AVFrame* frame, int64_t pts100ns, bool packedCarrier);

    bool record(ID3D12GraphicsCommandList* list, const Frame& frame) override;
    bool capturing() const override { return running(); }
    void submitted(ID3D12Fence* fence, uint64_t value) override;
    void discarded() override;

private:
    struct Ref { int64_t pts = 0; quality::Luma truth, previous, next; bool haveNext = false, used = false; };
    struct Kept { int64_t pts = 0; quality::Luma luma; bool used = false; };
    struct Pending { bool generated = false; int64_t pts = 0; };
    void stopLocked(const wchar_t* error);
    void finishLocked();
    bool layoutFor(DXGI_FORMAT format, quality::OutputLayout& layout) const;

    std::mutex m_;
    std::atomic<bool> running_{false};
    bool done_ = false, failed_ = false, pending_ = false, inFlight_ = false, reading_ = false, toggle_ = false;
    std::wstring error_;
    Options options_;
    uint64_t revision_ = 0, withheld_ = 0;
    std::chrono::steady_clock::time_point started_{}, nextReal_{};
    std::deque<Ref> refs_;
    std::deque<Kept> kept_;
    quality::Luma lastKept_;
    // Generated frames compared with their originals; real frames paired with their source luma.
    struct GenPair { quality::Luma generated; Ref ref; };
    struct RealPair { quality::Luma presented, source; };
    std::vector<GenPair> generated_;
    std::vector<RealPair> real_;
    quality::Report report_;
    // Timing.
    int64_t lastPts_ = 0;
    std::chrono::steady_clock::time_point lastHost_{};
    uint64_t shown_ = 0, onTime_ = 0;
    // Readback.
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t fenceValue_ = 0, readbackBytes_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    uint32_t width_ = 0, height_ = 0;
    quality::OutputLayout layout_{};
    Pending pendingMeta_, flightMeta_;
};

} // namespace veyra::engine
