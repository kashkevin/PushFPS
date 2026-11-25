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
#include <filesystem>
#include <memory>
#include <string>

namespace veyra::guidance { class GpuDisOpticalFlow; class AmdOpticalFlow; }

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
    // The self-check's marker file and the program identity it names (PushFlowCheckBlock; tests write the marker).
    static std::string flowCheckIdentity();
    static std::filesystem::path flowCheckMarker();
    // Release review REL16 SHOULD 2: the session ended on a device removal or an engine exception while this pass existed
    // and the row was on. Writes the marker "failed <identity>" (later runs of this program file block the motion source:
    // the row shows "Cannot run on this PC or with these settings") and blocks every later check of this process.
    static void noteFaultInPlay(const char* why);
    bool ready() const { return ready_; }
    bool flowReady() const { return flow_ != nullptr || fidelity_ != nullptr || zeroFlow_; }
    bool floatFlow() const { return floatFlow_; }   // Package 16: the float-only flow passed its start-up self-check
    // Review 1 SHOULD 4: the GPU time the motion source's start-up self-check measured for one real frame and P (FidelityFX:
    // the scored frame's list; GPU DIS: its one run); -1 unknown (test sources, no timestamps).
    double checkCostMs() const { return checkCostMs_; }
    // P's motion source (25 Nov 2026): AMD FidelityFX optical flow by default, fed every real frame while the mode is on
    // (it keeps its own history: recordFlowFrame); GPU DIS (strict or float) only when VEYRA_TEST_PREDICT_FLOW chose it.
    bool fidelityFlow() const { return fidelity_ != nullptr; }
    const char* flowName() const { return fidelity_ ? "fidelityfx" : flow_ ? (floatFlow_ ? "gpu-dis-float" : "gpu-dis-strict-fp64") : zeroFlow_ ? "test-zero" : "none"; }
    // FidelityFX only: one real frame (as VFG holds it: a buffer of `rowPitch`-byte rows, in COMMON) into the flow, in
    // its own list. Its B -> A motion and trust land in motion(slot) / trust(slot). `reset` (or the first frame) restarts
    // FidelityFX's history (7 frames before its vectors count); a frame that does not follow the last one (frameIndex not
    // last + 1) keeps it but is itself not usable (its vectors span more than one frame interval).
    bool recordFlowFrame(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* current, uint32_t rowPitch, uint64_t frameIndex, bool reset);
    // Whether motion(slot) holds usable B -> A motion for real frame `frameIndex` (FidelityFX: recorded for that frame
    // and past its warm-up; GPU DIS: computed per pair by recordMotion, so always).
    bool motionReady(UINT slot, uint64_t frameIndex) const;

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
    // Package 16, item 4: the GPU time of P number `sequence` by stage (PushPredictStage; -1 unknown). Written only when
    // recordMotion and recordPredict of that P ran in the graph (a pass fed its flow directly, as the WARP tests do, has
    // none). Call after its fence completed; false when the readback was reused since or nothing was timed.
    bool stageTimesOf(uint64_t sequence, PushPredictStageMs& out) const;

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
    // FidelityFX source: the context, NvofDensify.hlsl in its FidelityFX mode (vectors -> v x trust, trust), 7
    // descriptors per use (raw, cost, previous, current, scene changes; motion, trust): slots 0, 1 and the self-check.
    std::unique_ptr<guidance::AmdOpticalFlow> fidelity_;
    ComputePass densify_;
    uint64_t lastFlowFrame_ = 0, flowFrameOf_[kSlots] = {};
    bool zeroFlow_ = false;
    double checkCostMs_ = -1.0;   // VEYRA_TEST_PREDICT_FLOW=zero (tests only): motion_ zero and trust_ 1, written once
    bool initializeZeroFlow(ID3D12Device* device);
    bool warmOf_[kSlots] = {};
    // FidelityFX's start-up self-check, end to end through recordFlowFrame / recordMotion / recordPredict on synthetic
    // work-size frames (pushFlowCheckWindows), on a queue of its own; fidelity_ must be set. Containment as for GPU DIS.
    struct FidelityCheck {
        bool ok = false, executed = false, hung = false, removed = false, faulted = false, found = false;
        bool inFlight = false;   // REL16 MUST 3: stopped by the budget with a frame still on the GPU (objects kept)
        double flowGpuMs = -1.0, pairGpuMs = -1.0, wallMs = 0.0;
        std::string text;
    };
    FidelityCheck runFidelityCheck(ID3D12Device* device);
    bool recordDensify(guidance::AmdOpticalFlow& of, ID3D12GraphicsCommandList* list, UINT block, ID3D12Resource* previous,
                       ID3D12Resource* current, ID3D12Resource* motion, ID3D12Resource* trust, bool warm);
    // Package 16, item 4: kPushPredictStamps timestamps per readback slot (PushPredictReadbackRing's), resolved at the end
    // of recordPredict. No heap (creation failed): nothing is timed, nothing fails.
    ComPtr<ID3D12QueryHeap> stamps_;
    ComPtr<ID3D12Resource> stampsReadback_;
    uint64_t stampFrequency_ = 0;
    bool floatFlow_ = false;
    uint64_t stampedMotion_ = 0;                                       // the P number recordMotion stamped for
    uint64_t timedOf_[PushPredictReadbackRing::kSlots] = {};           // the P number each slot's stamps belong to
};

}  // namespace veyra::pipeline
