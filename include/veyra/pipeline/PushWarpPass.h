// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg": the GPU passes of PushWarp's interpolated frame I(t) (pipeline/PushWarp.h, shaders/PushWarp*.hlsl).
//
//   recordInterpolate  A and B (pitch-linear buffers of 32-bit words, as VFG's input buffers or PushWarpFrames hold the
//                      real frames) and the B -> A field of the pair at the flow size (R16G16_FLOAT v x trust and its
//                      trust: PushPredictPass::motion / trust, shared with the predicted frame) -> I(t) at the work size
//                      in the working format (R8G8B8A8_UNORM or R10G10B10A2_UNORM, PQ codes for HDR10). Trust, splat,
//                      fill, compose, then the whole-frame verdict and its predicated copy of the nearer real frame.
//
// SAFETY (the project's rule after an estimator that had never run removed the device on its first execution): the pass
// is ready() only after its START-UP SELF-CHECK passed on this device: a small synthetic pair (moving background, a faster
// square, a static HUD box, failed flow cells) through exactly the play path on a queue of its own, read back and
// compared with the CPU reference (pushWarpReference, pushWarpCheckPass). Containment as the predicted frame's flow
// check (PushPredictPass): a marker file in the data folder while it runs (a start that finds it, for this program file,
// never runs the check again: the pass stays unavailable), SEH around the recording, a 10 s wait, a removed device or an
// unfinished submission keeps every object it used alive and blocks every later check of this process; one check per
// process for each adapter, format and size. Nothing is created before initialize(); nothing in the graph calls it unless the
// PushFPS frame source is in use (EnhanceGraph, PushOwnFrame.h).
// Review 1 of stream ownfg:
//   SHOULD 6: after the small check, ONE contained run AT THE PLAY SIZE (the graph's work and flow size, synthetic frames)
//             inside the same containment, so the first 4K compose and full fill pyramid do not first execute in play;
//             its GPU time (playGpuMs, one sample) feeds the entry guard. The whole check has ONE time budget (budgetMs,
//             what the row promises together with the motion check): over it, PushWarp is unavailable for this run with
//             that reason; the 10 s wait is only the hang guard.
//   SHOULD 5: every recorded frame runs the same GPU scene-cut test as VFG's path (shaders/VfgShotChange.hlsl, the
//             thresholds of core::SceneCadenceAnalyzer) on its A and B; on a cut the frame is B (a predicated copy),
//             whatever the field says.
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/PushPredict.h"
#include "veyra/pipeline/PushWarp.h"
#include <filesystem>
#include <string>

namespace veyra::pipeline {

class PushWarpPass {
public:
    static constexpr UINT kSlots = 8;   // 4 per parity: I uses parity x 4; a causal pair's P1 / P2 and the generator's frames parity x 4 + 0..2
    static constexpr UINT kConstants = 28;
    PushWarpPass();
    ~PushWarpPass();
    PushWarpPass(const PushWarpPass&) = delete;
    PushWarpPass& operator=(const PushWarpPass&) = delete;

    // width/height: the work size; format: R8G8B8A8_UNORM or R10G10B10A2_UNORM; flowWidth/flowHeight: the field's size.
    // Creates everything, then runs (or reuses this process's result of) the start-up self-check.
    bool initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t flowWidth, uint32_t flowHeight,
                    double budgetMs = 3000.0);
    bool ready() const { return ready_; }
    // The self-check's numbers (one line) and the GPU time of its one run (one sample at start-up; -1 unknown).
    const std::string& checkText() const { return checkText_; }
    double checkGpuMs() const { return checkGpuMs_; }
    double playGpuMs() const { return playGpuMs_; }   // the play-size run's GPU time (one sample at start-up; -1 unknown)
    static std::filesystem::path checkMarker();
    // Review 2 SHOULD 3: PushWarp's own fault-in-play memory (REL16 delta SHOULD 1's rule, engine::pushPlayFaultAction, on
    // this pass's marker): a session that ends with the device removed while PushWarp did GPU work in the last 5 s writes
    // "suspect"; the second in a row "failed" (blocked until the program file changes); a normal end clears a "suspect".
    static bool priorSuspect();
    static std::int64_t checkBootMinute();   // review 2 SHOULD 4: the boot a "running" / "failed" marker is scoped to (PushOwnFrame.h)
    static void noteSessionEnd(int action, const char* why);

    // I(t) into `output` (work size, working format, UAV access, COMMON). previous = A, current = B: buffers of `rowPitch`
    // bytes per row (a multiple of 256), in COMMON (or upload heaps). motion / trust: the B -> A field (flow size, COMMON).
    // timestampQueue: the queue the list runs on, for the stage times (null: not timed). Every resource ends in COMMON.
    bool recordInterpolate(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                           ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* output, const PushWarpParams& params = {},
                           ID3D12CommandQueue* timestampQueue = nullptr, uint64_t cellsToken = 0);
    // cellsToken (non-zero: the pair, e.g. its real frame index): the pair's later frames reuse the cells its first frame
    // computed (the t-independent trust pass: one SAD per flow cell per pair). cellsReused(): records that did.
    uint64_t cellsReused() const { return cellsReused_; }
    // The motion histogram of the frame numbered `sequence` (trusted vectors, full-size px per real frame, PushWarp.h);
    // zero for a frame that reused its pair's cells (the pair is counted once).
    bool motionHistogramOf(uint64_t sequence, PushWarpMotionHistogram& out) const;
    // Step 4: the PREDICTED frame P(s) = B + s (params.t = 1 + s), forward-projected from B alone, into `output`. previousMotion
    // / previousTrust: the previous pair's field (Z -> A on A's grid, same size as `motion`), for the acceleration
    // (params.acceleration); null: none. Same resources and states as recordInterpolate.
    bool recordPredicted(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                         ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* previousMotion, ID3D12Resource* previousTrust,
                         ID3D12Resource* output, const PushWarpParams& params, ID3D12CommandQueue* timestampQueue = nullptr, uint64_t cellsToken = 0);
    // The number of the last recorded I (0: none), its counters and stage times; call after its list's fence completed.
    uint64_t lastSequence() const { return readback_.next; }
    bool countersOf(uint64_t sequence, PushWarpCounters& out) const;
    // The static-overlay mask of the frame numbered `sequence` (params.overlayRules; PushWarpOverlay.hlsl): its overlay
    // pixels in all and within the middle eighth of the width and height (the crosshair's place); zero for a frame that
    // reused its pair's mask.
    bool overlayCountsOf(uint64_t sequence, uint32_t& total, uint32_t& centre) const;
    // Test only: the mask (bits 1 overlay, 2 sources; one byte per pixel, rows of ceil(width / 4) words), COMMON.
    ID3D12Resource* overlayMaskForTests() const { return masks_[2].Get(); }
    bool stageTimesOf(uint64_t sequence, PushWarpStageMs& out) const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_FORMAT format() const { return format_; }

private:
    struct Record {
        ID3D12GraphicsCommandList* list = nullptr;
        UINT slot = 0;
        ID3D12Resource* previous = nullptr;
        ID3D12Resource* current = nullptr;
        uint32_t rowPitch = 0;
        ID3D12Resource* motion = nullptr;
        ID3D12Resource* trust = nullptr;
        ID3D12Resource* previousMotion = nullptr;   // step 4: the acceleration's field (optional)
        ID3D12Resource* previousTrust = nullptr;
        ID3D12Resource* output = nullptr;
        const PushWarpParams* params = nullptr;
        ID3D12CommandQueue* timestampQueue = nullptr;
        uint64_t cellsToken = 0;
    };
    struct CellsKey {
        uint64_t token = 0;
        const void* motion = nullptr; const void* trust = nullptr; const void* previous = nullptr; const void* current = nullptr;
        uint32_t gw = 0, gh = 0, w = 0, h = 0;
        float minConfidence = 0, maxMotionShare = 0, sadLow = 0, sadHigh = 0, beta = 0;
        bool operator==(const CellsKey&) const = default;
    };
    CellsKey lastCells_{};
    uint64_t cellsReused_ = 0;
    bool recordCore(const Record& r);
    static bool sehRecord(PushWarpPass* pass, const Record* r, bool* faulted);
    bool createPipeline(ID3D12Device* device, const char* shader, ComPtr<ID3D12PipelineState>& pso);
    bool runSelfCheck(ID3D12Device* device, std::string& text, double& gpuMs, double& playMs, bool& failed, double budgetMs);
    bool createCutTest(ID3D12Device* device);
    void recordCutTest(ID3D12GraphicsCommandList* list, const Record& r, uint32_t W, uint32_t H);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) const;

    ID3D12Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0, flowWidth_ = 0, flowHeight_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool created_ = false, ready_ = false;
    std::string checkText_;
    double checkGpuMs_ = -1.0, playGpuMs_ = -1.0;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> trustPso_, splatPso_, fillPso_, composePso_, verdictPso_, overlayPso_;
    ComPtr<ID3D12Resource> masks_[3], overlayCells_;   // the static-overlay mask's buffers (PushWarpOverlay.hlsl)
    CellsKey maskOf_{};                                // the pair whose mask masks_[2] holds
    bool maskValid_ = false;
    ComPtr<ID3D12DescriptorHeap> heap_;   // per slot: flow SRV, trust SRV, output UAV
    UINT increment_ = 0;
    DescriptorStager stager_;
    ComPtr<ID3D12Resource> scratch_, counters_, countersZero_, countersReadback_, verdict_;
    ComPtr<ID3D12RootSignature> cutRoot_;      // SHOULD 5: VfgShotChange.dxil's layout (b0 12 constants, t0, t1, u0)
    ComPtr<ID3D12PipelineState> cutPso_;
    ComPtr<ID3D12Resource> cut_;               // 64 bytes per slot: the shader's flags and scores at parity 0
    uint64_t scratchBytes_ = 0;
    PushPredictReadbackRing readback_;
    ComPtr<ID3D12QueryHeap> stamps_;
    ComPtr<ID3D12Resource> stampsReadback_;
    uint64_t stampFrequency_ = 0;
    uint64_t timedOf_[PushPredictReadbackRing::kSlots] = {};
};

}  // namespace veyra::pipeline
