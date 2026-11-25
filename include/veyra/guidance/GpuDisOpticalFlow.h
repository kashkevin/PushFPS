#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace veyra::guidance {
// GPU-only adapter. Two slots are retired by EnhanceGraph's upload fences.
// Caller supplies COMMON inputs/outputs and signals completion after submission.
class GpuDisOpticalFlow {
public:
    GpuDisOpticalFlow();
    ~GpuDisOpticalFlow();
    bool initialize(ID3D12Device*, uint32_t width, uint32_t height);
    // PushFPS Package 16 ("Lower delay", item 4 d): floatOnly = the same DIS over shaders compiled with `double` as `float`
    // (shaders/dis_float; no FP64 shader operations). Not CPU-exact: its caller compares it with the strict one first.
    bool initialize(ID3D12Device*, uint32_t width, uint32_t height, bool floatOnly);
    bool dispatch(ID3D12GraphicsCommandList*, ID3D12CommandQueue*, ID3D12Fence*,
                  uint64_t completion, uint32_t slot, ID3D12Resource* previous,
                  ID3D12Resource* current, ID3D12Resource* motion, ID3D12Resource* confidence,
                  uint64_t previousId, uint64_t currentId);
    // PushFPS Package 16 (crash triage of the flow self-check, 2026-11-25): whether the device runs FP64 shader operations
    // (D3D12_OPTIONS.DoublePrecisionFloatShaderOps), which the strict build needs.
    static bool deviceHasDoubles(ID3D12Device*);
    // Tests only: record at most `limit` of the dispatches of the next dispatch() calls, counted over all of them in
    // order (Veyra's two luma conversions, then GPU DIS's own, then the validation; -1 = all), and keep their names.
    void setTestDispatchLimit(int64_t limit);
    std::vector<std::string> testDispatchNames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
