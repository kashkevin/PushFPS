// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "predict": the GPU passes of the predicted frame P (pipeline/PushPredict.h, docs/drafts/PREDICT_GPU.md).
//
//   recordMotion   A and B (VFG's pitch-linear input buffers) -> two small BGRA8 pictures at the flow size
//                  (shaders/PushPredictDownsample.hlsl) -> GpuDisOpticalFlow (bidirectional DIS, forward/backward and
//                  photometric trust) -> the B -> A flow and its trust at the flow size.
//   recordPredict  P = B advanced by `step` of the A -> B motion at the working size (shaders/PushPredict.hlsl),
//                  still pixels bit-identical to B, untrusted pixels = B; four counters per slot.
//
// Nothing is created before initialize(); the graph creates this object only when VEYRA_EXP_RP_FG_SCHEDULE=onepredicted
// is set, VFG is the generator and the format is one VFG takes. Two slots (the pair's parity): a slot's descriptors,
// flow pictures and counters are rewritten only after the graph has retired that parity's previous lists (the same
// rule as the graph's own per-parity resources). Every resource is left in COMMON.
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/PushPredict.h"
#include <memory>

namespace veyra::guidance { class GpuDisOpticalFlow; }

namespace veyra::pipeline {

class PushPredictPass {
public:
    static constexpr UINT kSlots = 2;
    static constexpr UINT kConstants = 16;
    PushPredictPass();
    ~PushPredictPass();
    PushPredictPass(const PushPredictPass&) = delete;
    PushPredictPass& operator=(const PushPredictPass&) = delete;

    // width/height: the working size; format: R8G8B8A8_UNORM or R10G10B10A2_UNORM (the generated frames' format).
    bool initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);
    // The flow (only the graph needs it; the tests give their own flow to recordPredict).
    bool initializeFlow(ID3D12Device* device, uint32_t flowWidth, uint32_t flowHeight);
    bool ready() const { return ready_; }
    bool flowReady() const { return flow_ != nullptr; }

    // A (previous) and B (current) as VFG keeps them: buffers of `rowPitch`-byte rows of 32-bit words, in COMMON.
    // The completion value is the graph-fence value the caller signals right after submitting `list` (GpuDis's rule).
    bool recordMotion(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t completion,
                      UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                      uint64_t previousId, uint64_t currentId);
    ID3D12Resource* motion(UINT slot) const { return slot < kSlots ? motion_[slot].Get() : nullptr; }
    ID3D12Resource* trust(UINT slot) const { return slot < kSlots ? trust_[slot].Get() : nullptr; }

    // P into `output` (a working-size texture of the working format with UAV access, in COMMON). `motion`: R16G16_FLOAT
    // B -> A flow in its own pixels, `trust`: R8_UNORM or R16_FLOAT / R32_FLOAT 0..1, both of one size, in COMMON.
    // When more of the moving pixels fell back than were moved, `current` (B) is copied over P as a whole on the GPU
    // (a predicated copy; `rowPitch` must be a multiple of 256, as VFG's buffers are). The counters stay the warp's.
    bool recordPredict(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current,
                       uint32_t rowPitch, ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* output,
                       float step, const PushPredictThresholds& thresholds = {});
    // The counters of the last recordPredict of `slot`; call only after its list's fence has completed.
    bool counters(UINT slot, PushPredictCounters& out) const;
    // The number of the last recorded P (0: none) and the counters of P number `sequence`: false when that readback slot
    // was rewritten since (PushPredictReadbackRing) or the list has not been recorded. Call after its fence completed.
    uint64_t lastSequence() const { return readback_.next; }
    bool countersOf(uint64_t sequence, PushPredictCounters& out) const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_FORMAT format() const { return format_; }

private:
    bool createPipeline(ID3D12Device* device, const char* shader, bool predict, ComPtr<ID3D12RootSignature>& root,
                        ComPtr<ID3D12PipelineState>& pso);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) const;

    ID3D12Device* device_ = nullptr;
    uint32_t width_ = 0, height_ = 0, flowWidth_ = 0, flowHeight_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool ready_ = false;
    ComPtr<ID3D12RootSignature> predictRoot_, downsampleRoot_, verdictRoot_;
    ComPtr<ID3D12PipelineState> predictPso_, downsamplePso_, verdictPso_;
    ComPtr<ID3D12Resource> verdict_;   // review 2 SHOULD 3: 8 bytes per slot, the whole-frame predicate (COMMON between uses)
    bool createVerdictPipeline(ID3D12Device* device);
    // Per slot: predict t2, t3, u0 (3) then downsample u0 for A and B (2): 5 descriptors per slot.
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT increment_ = 0;
    DescriptorStager stager_;
    ComPtr<ID3D12Resource> counters_, countersZero_, countersReadback_;
    PushPredictReadbackRing readback_;
    uint64_t lastOfSlot_[kSlots] = {};
    ComPtr<ID3D12Resource> flowPicture_[kSlots][2], motion_[kSlots], trust_[kSlots];
    std::unique_ptr<guidance::GpuDisOpticalFlow> flow_;
};

}  // namespace veyra::pipeline
