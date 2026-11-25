#pragma once
#include <d3d12.h>
#include "veyra/pipeline/PushPredict.h"
#include <memory>
#include <string>

namespace veyra::gfx { class D3D12DeviceContext; }
namespace veyra::pipeline {
// Public NvVFX 1.3 API. SDK metadata is allocated by NVCVImage.dll; no SDK
// headers, models or implementation are vendored into the source repository.
// Borrowed encoded textures stay in the graph's normal FrameLease pool.
class VfgBackend {
public:
    enum class SubmissionState { Pending, Succeeded, Failed };
    VfgBackend();
    ~VfgBackend();
    VfgBackend(const VfgBackend&)=delete;
    VfgBackend& operator=(const VfgBackend&)=delete;
    static std::wstring runtimeDirectory(const std::wstring& runtimeRoot);
    static bool runtimeAvailable(const std::wstring& runtimeRoot);
    // quality 0-5 as engine::EnhancementSettings::vfgQuality; a mixed tier
    // loads a second effect with the second model on the same CUDA stream.
    bool initialize(gfx::D3D12DeviceContext&,const std::wstring& runtimeRoot,
                    unsigned width,unsigned height,DXGI_FORMAT,unsigned multiplier,unsigned quality,
                    bool asynchronousSubmission=false,bool fastFallback=false);
    bool created() const;
    // Caller transitions encoded texture to COPY_SOURCE and back to COMMON.
    // Submit this list before generate(). Normal frames never CPU-wait here;
    // teardown and exceptional SDK rejection drain work before releasing it.
    // The same list also runs the GPU shot-change test of this pair against
    // the other parity's input (comparePrevious=false when the pair has no
    // valid A, e.g. after a reset); copyOutput() of the pair's subframes is
    // predicated on its result and shows the current input on a cut.
    bool capture(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned parity,bool comparePrevious=true);
    // Signals an OWNED producer fence and queues a GPU wait before copyOutput.
    // Preview offloads the serial CUDA wait/run/signal to a bounded worker;
    // synchronous/export callers retain the immediate SDK rejection verdict.
    // fast: this pair uses the second effect, see fastReady().
    // alone (Package 15): FrameIndex `subframe` without the pair's earlier ones (the one-predicted schedule asks for 2 of 3
    // alone). If the SDK refuses or fails it, the backend stays usable (failed() is not raised) and takePredictRefused()
    // reports it once; VEYRA_TEST_VFG_REJECT_ALONE=1 refuses every such request (a test of that path).
    bool generate(unsigned parity,unsigned subframe,unsigned multiplier,bool reset,bool fast=false,bool alone=false);
    bool takePredictRefused();
    // Review 1 SHOULD 6 / 7: start the FrameIndex-2-only ladder over at the next such request (from any thread), and the
    // mode the ladder is on now (direct, timestep, index1-then-2, refused).
    void resetAloneLadder();
    PushVfgAloneMode aloneMode() const;
    // A second effect is loaded: a mixed tier's second model, or with fastFallback
    // the next faster model, for pairs the chosen one would finish too late for.
    bool fastReady() const;
    // Poll after the corresponding consumer fence. A submitted copy is not
    // proof that the SDK accepted the generated frame.
    SubmissionState submissionState(unsigned slot) const;
    bool copyOutput(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned slot);
    // Package 15 (one-predicted schedule): the pair's current input B (inputBuffer(parity)) into `texture` (the
    // working size and format, in COPY_DEST; the caller transitions it). onlyOnCut: predicated on the GPU shot-change
    // result of this pair as copyOutput is (skipped unless a cut; nothing at all with the SDK's own detection), so a
    // predicted frame across a cut is B; otherwise unconditional (a predicted frame that could not be recorded).
    bool copyInput(ID3D12GraphicsCommandList*,ID3D12Resource*,unsigned parity,bool onlyOnCut);
    // Test diagnostics only: the shot-change result buffer (null when the SDK's
    // automatic detection is used instead). Between captures it is in
    // D3D12_RESOURCE_STATE_PREDICATION; uint32 layout: cut flag per parity at
    // 0/2, last SAD/histogram/compare per parity at 4/8, cut count at 12.
    ID3D12Resource* shotChangeResultsForTest() const;
    // Stream "predict": the pair's input buffers as capture() leaves them (COMMON, pitch-linear 32-bit words of the
    // encoded format, inputRowPitch() bytes per row): B = inputBuffer(parity), A = inputBuffer(1 - parity). Read-only
    // for the caller, and only after this pair's last copyOutput (CUDA has finished with them then).
    ID3D12Resource* inputBuffer(unsigned parity) const;
    uint32_t inputRowPitch() const;
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
