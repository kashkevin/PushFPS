// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test: reads presented frames around a press (PushInputLatency.h).
#pragma once
#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/PushInputLatency.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>

namespace veyra::engine {

class PushInputLatencyProbe final : public PresentCapture {
public:
    // Starts reading presented frames (a window around one press).
    void beginWindow();
    void endWindow();
    // The clock used for both ends (steady clock, 100 ns).
    static int64_t now100ns();
    // The press went out at sentHost100ns (now100ns()).
    void markSent(int64_t sentHost100ns);
    // Polled by the UI: reads finished copies. Returns press-to-picture in ms once seen, else -1.
    double poll();
    bool windowOpen() const { std::lock_guard lock(m_); return open_; }
    // Latency test v2: how the answer is recognised (latency::Detector::Mode); kept across windows.
    void setDetectorMode(latency::Detector::Mode mode);
    // Latency test v2 (Anywhere): a change sooner than this after the press is not an answer (one stream frame).
    void setMinimumAnswerMs(double ms);
    // Latency test v2 (Anywhere): this press cannot be measured, the picture was moving (latency::Detector::Verdict).
    // Polled with poll(); reset by beginWindow() and markSent().
    bool pictureMoving() const { std::lock_guard lock(m_); return pictureMoving_; }
    // Sham presses (Anywhere): the picture moving came from a change sooner than one stream frame after a still
    // picture (latency::Detector::changedEarly): for a sham, the picture changed by itself.
    bool changedEarly() const { std::lock_guard lock(m_); return pictureMoving_ && detector_.changedEarly(); }
    // Latency test v2: the engine thread reports each real frame's network arrival (its presentation timestamp
    // as the presenter sees it and the host time the frame arrived). Only kept while a window is open: a load
    // of one atomic otherwise.
    void noteArrival(int64_t pts100ns, int64_t arrivalHost100ns);
    // The answered press, split: sent (markSent), network arrival of the frame that showed the answer (for a
    // generated frame, the real frame after it), that frame's present (just before Present), the newest arrival
    // before the press. 0 = unknown.
    struct PressTiming { int64_t sent = 0, arrival = 0, present = 0, lastArrivalBeforeSent = 0; };
    PressTiming timing() const;

    bool record(ID3D12GraphicsCommandList* list, const Frame& frame) override;
    bool capturing() const override { return collecting_.load(std::memory_order_relaxed); }
    void submitted(ID3D12Fence* fence, uint64_t value) override;
    void discarded() override;

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t fenceValue = 0, bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint32_t width = 0, height = 0;
        quality::OutputLayout layout{};
        int64_t host100ns = 0, pts100ns = 0;
        uint64_t order = 0;
        enum class State { Free, Pending, InFlight } state = State::Free;
    };
    mutable std::mutex m_;
    bool open_ = false;
    int64_t sent_ = 0;
    double answerMs_ = -1;
    bool pictureMoving_ = false;
    int64_t answerPresent_ = 0, answerPts_ = 0;
    struct Arrival { int64_t pts = 0, host = 0; };
    std::array<Arrival, 128> arrivals_{};   // about 2 s of a 60 fps stream
    size_t arrivalNext_ = 0;
    std::atomic<bool> collecting_{false};
    uint64_t order_ = 0, lastRead_ = 0;
    latency::Detector detector_;
    // The last frames judged after the press (host time, timestamp), so an answer is timed at its onset frame
    // (latency::Detector::onsetFramesBack; always the newest in Bottom mode).
    struct Judged { int64_t host = 0, pts = 0; };
    std::array<Judged, latency::Detector::kOnsetFrames> judged_{};
    size_t judgedNext_ = 0;
    std::array<Slot, 4> slots_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
};

} // namespace veyra::engine
