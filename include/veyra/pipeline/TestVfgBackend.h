#pragma once
// TEST ONLY: a stand-in for NVIDIA VFG on machines without CUDA or NVIDIA's runtime (CI's software renderer).
//
// Selected only by the environment switch VEYRA_TEST_FAKE_VFG=1 (exactly "1", read when VfgBackend::initialize runs;
// never from slipstream.ini, never by any setting or preference). VfgBackend forwards every public call to it then, so
// EnhanceGraph and EngineController take VFG's own code paths unchanged: frameGenerationBackend == Vfg, multipliers 2-4,
// the FrameIndex-2-alone request of the "Lower delay" schedule, back-to-back (VEYRA_EXP_RP_VFG_BACK_TO_BACK, the same
// pushVfgChainedWait), the bounded worker, the GPU shot-change test (VfgShotChange.dxil) with predicated copies, and the
// refusal path (VEYRA_TEST_VFG_REJECT_ALONE=1, the same switch as the real backend).
//
// What it is not: no neural model, no CUDA interop, no real GPU timing. A generated frame is a per-channel linear blend
// of the pair's encoded inputs A and B at timestep FrameIndex / FrameMultiplier (shaders/TestVfgBlend.hlsl), made on its
// own D3D12 compute queue after an artificial cost per request, serial like the CUDA stream: VEYRA_TEST_FAKE_VFG_COST_MS =
// "first,later" ms for the first and every later request of a pair (default 2.0,2.2: the owner's VFG at 4K HDR, his log of
// 24 Nov 2026), VEYRA_TEST_FAKE_VFG_ALONE_COST_MS for an index-2-alone request (default: the first cost). The start-up
// log names it ("vfg" lines with TEST-ONLY, with the size and DXGI format).
#include "veyra/pipeline/VfgBackend.h"
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace veyra::gfx { class D3D12DeviceContext; }
namespace veyra::pipeline {
class TestVfgBackend {
public:
    // VEYRA_TEST_FAKE_VFG is exactly "1".
    static bool requested();
    TestVfgBackend();
    ~TestVfgBackend();
    TestVfgBackend(const TestVfgBackend&)=delete;
    TestVfgBackend& operator=(const TestVfgBackend&)=delete;
    bool initialize(gfx::D3D12DeviceContext&,unsigned width,unsigned height,DXGI_FORMAT,unsigned multiplier,unsigned quality,bool asynchronousSubmission);
    bool created() const;
    bool capture(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned parity,bool comparePrevious);
    bool generate(unsigned parity,unsigned subframe,unsigned multiplier,bool reset,bool fast,bool alone);
    bool takePredictRefused();
    VfgBackend::SubmissionState submissionState(unsigned slot) const;
    bool copyOutput(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned slot);
    bool copyInput(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned parity,bool onlyOnCut);
    ID3D12Resource* shotChangeResults() const;
    ID3D12Resource* inputBuffer(unsigned parity) const;
    uint32_t inputRowPitch() const;
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
