#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// The in-app pipeline probe (S5_CONTRACT.md interface item 7; sprint 5, stream "pipeline"): test-only, started
// by the autorun step `probe:<seconds>` (PushAutorunPlan.h, PushAutorun.cpp). While it runs it takes PAIRS of
// consecutive real (not generated) presented frames, the way PushHudSampler takes single ones
// (PresentCapture.h): the graph's output picture and the raw console picture of the same frame are copied
// into read-back buffers inside the presentation command list. At most one pair is held at a time, nothing
// waits for a copy, and nothing is recorded while no probe runs. status() (the interface thread, polled by
// the autorun runner) only checks the fences: a finished pair is handed to a worker thread (std::async), which
// maps the read-back buffers, reduces them to luma, computes the no-reference metrics of PushImageMetrics.h
// and, after the last pair, logs
//   [slipstream-quality] frames= size= blockiness= banding= ringing= detail= hudDelta= flicker= meanLuma= hdr= stage=raw
//   [slipstream-quality] ... stage=output
// What "output" is: the graph's output at its work size (after clean-up, neural layer, upscaler, colour), as
// the recorder sees it; the presenter's own resize to the window and its dither are NOT in it.
// While the probe runs the graph keeps the raw picture (two extra full-frame copies per real frame, as for a
// comparison clip): never read timing numbers from a window in which a probe ran.
// Cost (reasoned, not measured): per pair two output and two raw pictures are converted and measured, a few
// passes over each; at a 3840x2160 HDR10 output that is about 33 million pixel conversions (PQ decode with
// pow) per pair, tens to a few hundred milliseconds of ONE CPU core, on the worker thread, never on the
// interface thread (mouse and keyboard input is polled there) and never on the engine thread. At most one
// pair is reduced at a time, one pair per 2 s. Read-back memory: 4 buffers of one picture each (about 33 MB
// each at 4K FP16), released when the probe finishes, fails or is cancelled and the GPU is done with them.
// An R10G10B10A2 output is decoded as BT.2020 PQ: the graph makes that format only for HDR10 output
// (EnhanceGraph::outputFormat); a 10-bit picture without hdrMode 2 fails the probe with reason "format".
// Blind Windows code, written after PushHudSampler: unverified on Windows.
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/PushImageMetrics.h"

namespace veyra::engine {

class PushPipelineProbe final : public PresentCapture {
public:
    struct Options {
        uint32_t seconds = 10;            // 2..600
        std::vector<PushRectN> hud;       // rectangles for hudDelta; empty = the four corners (a quarter of each side)
        // Stream "clarity" (VEYRA_TEST_RP_PROBE_FRAMES, 0 = off): the first frame of each of the first N pairs is also
        // written losslessly (raw, output and the presented back buffer; PushFrameDump.h) into
        // <local data folder>\clarity-frames. The back buffer is copied only while frames are still to be written.
        uint32_t saveFrames = 0;
    };
    struct Status {
        bool running = false, done = false, failed = false;
        std::string reason;               // one word when failed
        uint32_t pairs = 0, target = 0;
        uint64_t revision = 0;            // +1 per start
        PushQualityStats raw, output;     // valid when done: what the two [slipstream-quality] lines say
    };
    void start(const Options& options);
    void cancel();
    Status status();
    ~PushPipelineProbe() override;

    bool wantsRawFrames() const override;
    bool record(ID3D12GraphicsCommandList* list, const Frame& frame) override;
    void submitted(ID3D12Fence* fence, uint64_t value) override;
    void discarded() override;

private:
    enum class Layout : uint8_t { Rgba8, Bgra8, Rgb10a2, Rgba16f };
    // One read-back copy: the output picture or the raw picture of one frame.
    struct Copy {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint32_t width = 0, height = 0;
        Layout layout = Layout::Rgba8;
        bool valid = false;
    };
    struct Slot {
        Copy output, raw;
        Copy screen;                      // the swap chain's back buffer after the blit (only for a frame to be saved)
        uint32_t hdrMode = 0;
        bool rawSdrInHdr = false;
        bool save = false;                // this pair's first frame is written to files (PushFrameDump.h)
        uint32_t saveIndex = 0;
        int64_t pts100ns = 0;
        float ditherStep = 0;
        int presentFine = -1;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t fenceValue = 0;
    };
    static bool layoutOf(DXGI_FORMAT format, Layout& layout);
    bool copyInto(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* texture, uint32_t width, uint32_t height, Copy& copy);
    static bool reduce(const Copy& copy, uint32_t hdrMode, bool raw, bool rawSdrInHdr, PushLumaImage& out);
    // The worker's part: reduce a finished pair, then merge it under the lock.
    void reducePair(Slot first, Slot second, std::vector<PushRectN> hud, uint64_t revision);
    // Writes the three pictures of a slot and their JSON (worker thread; no lock held). Returns the files written.
    static unsigned saveSlot(const Slot& slot, uint64_t revision);
    void failLocked(const char* reason);
    void finishLocked();
    // Releases the read-back buffers and the device once nothing may still write to or read from them.
    void releaseIfIdleLocked();

    mutable std::mutex m_;
    bool running_ = false, done_ = false, failed_ = false, pending_ = false, reading_ = false;
    std::string reason_;
    Options options_;
    uint64_t revision_ = 0;
    uint32_t target_ = 0, pairs_ = 0, filled_ = 0;
    uint32_t saved_ = 0;   // frames scheduled for saving in this run (options_.saveFrames)   // filled_: slots of the current pair already submitted (0..2)
    std::chrono::steady_clock::time_point nextDue_{}, deadline_{}, rawGrace_{};
    std::chrono::milliseconds interval_{2000};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Slot slots_[2];
    // Buffers replaced by larger ones while a list that writes to them may still execute.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retired_;
    PushQualityStats rawStats_, outputStats_;
    bool releaseWanted_ = false;
    // Declared last: destroyed first, so a running reduction ends before the members it uses.
    std::future<void> work_;
};

} // namespace veyra::engine
