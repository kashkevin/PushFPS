// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "predict": see include/veyra/pipeline/PushPredictPass.h and docs/drafts/PREDICT_GPU.md.
#include "veyra/pipeline/PushPredictPass.h"
#include "veyra/guidance/GpuDisOpticalFlow.h"
#include "veyra/guidance/AmdOpticalFlow.h"
#include "veyra/pipeline/PushPredictCheck.h"
#include "veyra/engine/PushLowDelay.h"
#include "veyra/RuntimePaths.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>

namespace veyra::pipeline {
namespace {
constexpr UINT kPerSlot = 5;               // predict t2, t3, u0; downsample u0 (A), u0 (B)
constexpr UINT kCounterBytes = 16;         // four uint32 per slot
bool predictFormat(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM; }
float bitsOf(uint32_t v) { return std::bit_cast<float>(v); }
ComPtr<ID3D12Resource> makeBufferOf(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
void uavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r; list->ResourceBarrier(1, &b);
}
// Package 16, item 4 (d): one synchronous run of a flow on a synthetic pair, on a queue of its own (graph build only, never
// per frame). B is A moved by (+3, +1) pixels, so the B -> A vectors are (-3, -1). Returns the flow, its trust and the GPU
// time between two timestamps around the flow's dispatch (one sample on this GPU at start-up).
// Crash containment (2026-11-25): progress lines name the step reached; nothing is executed when the flow could not be
// recorded; a fault while recording is caught (sehFlowDispatch); a submission that does not finish keeps every object it
// uses alive for the rest of the process (released objects the GPU may still use would be a second fault); a removed
// device is reported and nothing more is done with it.
struct FlowSelfRun {
    std::vector<uint8_t> motion, trust;
    uint32_t motionPitch = 0, trustPitch = 0;
    double gpuMs = -1.0;
    bool ok = false;
    bool executed = false;   // submitted to the GPU
    bool hung = false, removed = false, faulted = false;
    bool inFlight = false;   // REL16 delta MUST 2: stopped by the budget with a frame still on the GPU
};
uint8_t selfCheckPixel(int x, int y) {
    const double v = 128.0 + 55.0 * std::sin(x * 0.23 + y * 0.05) + 35.0 * std::sin(y * 0.19 - x * 0.07) + 20.0 * std::sin((x + 2 * y) * 0.61);
    return uint8_t(std::clamp(v, 0.0, 255.0));
}
bool sehFlowDispatch(guidance::GpuDisOpticalFlow* flow, ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Fence* fence,
                     ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* motion, ID3D12Resource* trust, bool* faulted) {
    __try {
        return flow->dispatch(list, queue, fence, 1, 0, a, b, motion, trust, 1, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}
// Objects a submission that never finished may still use: never released (a leak of a few MB, once, instead of a fault).
std::vector<ComPtr<IUnknown>>& keptForever() { static std::vector<ComPtr<IUnknown>> kept; return kept; }

FlowSelfRun runFlowSelfCheck(ID3D12Device* device, guidance::GpuDisOpticalFlow& flow, uint32_t w, uint32_t h, const char* name) {
    FlowSelfRun r;
    const auto step = [&](const char* what) { log::info("push-predict", std::format("flow self-check {} {}x{}: {}", name, w, h, what)); };
    step("recording");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return r;
    ComPtr<ID3D12Resource> pictures[2] = {makeTexture(device, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, true), makeTexture(device, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, true)};
    ComPtr<ID3D12Resource> motion = makeTexture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, true), trust = makeTexture(device, w, h, DXGI_FORMAT_R8_UNORM, true);
    if (!pictures[0] || !pictures[1] || !motion || !trust) return r;
    const uint32_t pitch = (w * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> upload = makeBufferOf(device, uint64_t(pitch) * h * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    if (!upload) return r;
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped) return r;
    for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = i == 0 ? selfCheckPixel(int(x), int(y)) : selfCheckPixel(int(x) - 3, int(y) - 1);
                uint8_t* p = mapped + (size_t(i) * h + y) * pitch + size_t(x) * 4;
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
    upload->Unmap(0, nullptr);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFp{}, trustFp{};
    UINT64 motionBytes = 0, trustBytes = 0;
    const auto md = motion->GetDesc(), td = trust->GetDesc();
    device->GetCopyableFootprints(&md, 0, 1, 0, &motionFp, nullptr, nullptr, &motionBytes);
    device->GetCopyableFootprints(&td, 0, 1, 0, &trustFp, nullptr, nullptr, &trustBytes);
    ComPtr<ID3D12Resource> motionRb = makeBufferOf(device, motionBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    ComPtr<ID3D12Resource> trustRb = makeBufferOf(device, trustBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    ComPtr<ID3D12Resource> stampRb = makeBufferOf(device, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2;
    ComPtr<ID3D12QueryHeap> stamps;
    if (!motionRb || !trustRb || !stampRb || FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&stamps)))) return r;
    for (uint32_t i = 0; i < 2; ++i) {
        transition(list.Get(), pictures[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = uint64_t(i) * pitch * h; from.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, pitch};
        to.pResource = pictures[i].Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), pictures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    list->EndQuery(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    // current = B (pictures[1]), previous = A: B -> A vectors, as recordMotion asks for them.
    const bool dispatched = sehFlowDispatch(&flow, list.Get(), queue.Get(), fence.Get(), pictures[0].Get(), pictures[1].Get(), motion.Get(), trust.Get(), &r.faulted);
    if (!dispatched) {
        // Nothing reaches the GPU: the list is dropped unexecuted.
        log::warn("push-predict", std::format("flow self-check {} {}x{}: not recorded{}", name, w, h, r.faulted ? " (a fault inside the flow's recording was caught)" : ""));
        return r;
    }
    list->EndQuery(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->ResolveQueryData(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stampRb.Get(), 0);
    transition(list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), trust.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = motion.Get(); dst.pResource = motionRb.Get(); dst.PlacedFootprint = motionFp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    src.pResource = trust.Get(); dst.pResource = trustRb.Get(); dst.PlacedFootprint = trustFp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(list.Get(), trust.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list->Close())) return r;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    r.executed = true;
    const bool signalled = SUCCEEDED(queue->Signal(fence.Get(), 1));
    step("submitted");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished = signalled && done && SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    r.removed = FAILED(device->GetDeviceRemovedReason());
    if (!finished || r.removed) {
        r.hung = !finished;
        // The GPU may still use these: keep them (and, through keptForever in the caller, the flow) for the rest of the process.
        for (IUnknown* o : std::initializer_list<IUnknown*>{queue.Get(), allocator.Get(), list.Get(), fence.Get(), pictures[0].Get(), pictures[1].Get(),
                                                            motion.Get(), trust.Get(), upload.Get(), motionRb.Get(), trustRb.Get(), stampRb.Get(), stamps.Get()})
            keptForever().emplace_back(o);
        log::error("push-predict", std::format("flow self-check {} {}x{}: {} (device state 0x{:08X}); the predicted frame is unavailable in this run",
                                               name, w, h, r.removed ? "the device was removed" : "did not finish within 10 s",
                                               unsigned(device->GetDeviceRemovedReason())));
        return r;
    }
    step("finished on the GPU");
    UINT64 frequency = 0;
    uint64_t ticks[2] = {};
    void* data = nullptr;
    const D3D12_RANGE none{0, 0};
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && SUCCEEDED(stampRb->Map(0, nullptr, &data)) && data) {
        std::memcpy(ticks, data, sizeof(ticks));
        stampRb->Unmap(0, &none);
        if (frequency && ticks[1] >= ticks[0]) r.gpuMs = double(ticks[1] - ticks[0]) * 1000.0 / double(frequency);
    }
    data = nullptr;
    if (FAILED(motionRb->Map(0, nullptr, &data)) || !data) return r;
    r.motion.assign(static_cast<const uint8_t*>(data) + motionFp.Offset, static_cast<const uint8_t*>(data) + motionFp.Offset + size_t(motionFp.Footprint.RowPitch) * h);
    motionRb->Unmap(0, &none);
    data = nullptr;
    if (FAILED(trustRb->Map(0, nullptr, &data)) || !data) return r;
    r.trust.assign(static_cast<const uint8_t*>(data) + trustFp.Offset, static_cast<const uint8_t*>(data) + trustFp.Offset + size_t(trustFp.Footprint.RowPitch) * h);
    trustRb->Unmap(0, &none);
    r.motionPitch = motionFp.Footprint.RowPitch; r.trustPitch = trustFp.Footprint.RowPitch;
    r.ok = true;
    step("read back");
    return r;
}

// SEH around the FidelityFX calls of the end-to-end self-check (PushPredictPass::runFidelityCheck): a fault inside the
// SDK while recording is caught and fails the check instead of PushFPS.
bool sehFidelityInitialize(guidance::AmdOpticalFlow* flow, ID3D12Device* device, uint32_t w, uint32_t h, bool* faulted) {
    __try {
        return flow->initialize(device, w, h);   // the SDK may throw (seen on WARP below its minimum size): caught here
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}
bool sehRecordFlowFrame(PushPredictPass* pass, ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* current, uint32_t pitch, uint64_t frame,
                        bool reset, bool* faulted) {
    __try {
        return pass->recordFlowFrame(list, slot, current, pitch, frame, reset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}

// The marker of a running self-check (PushFlowCheckBlock): in the data folder, naming this program file.
std::filesystem::path flowCheckMarkerPath() { return runtime::localDataDirectory() / L"lowdelay-flow-check.txt"; }
std::string readFlowCheckMarker() {
    std::ifstream in(flowCheckMarkerPath(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.substr(0, 512);
}
void writeFlowCheckMarker(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(flowCheckMarkerPath().parent_path(), ec);
    std::ofstream out(flowCheckMarkerPath(), std::ios::binary | std::ios::trunc);
    out << text << "\n";
    out.flush();   // before the GPU work: a crash inside it must leave the file behind
}
void clearFlowCheckMarker() { std::error_code ec; std::filesystem::remove(flowCheckMarkerPath(), ec); }

// One self-check per process for each adapter and flow size: later graph builds reuse its result.
struct FlowCheckResult {
    PushFlowUse use = PushFlowUse::None;
    PushFlowCheckPlan plan;
    PushFlowAgreement agreement;   // the flow against the known motion (pushFlowFoundMotion)
    bool deviceDoubles = false, ok = false, found = false, fidelityFound = false;
    double gpuMs = -1.0, costMs = -1.0;   // costMs: one real frame and P (SHOULD 4's entry guard)
    std::string fidelityText;   // the end-to-end check's numbers (PushPredictPass::runFidelityCheck)
};
std::mutex& flowCheckMutex() { static std::mutex m; return m; }
std::map<std::tuple<uint32_t, int32_t, uint32_t, uint32_t, int>, FlowCheckResult>& flowCheckCache() {
    static std::map<std::tuple<uint32_t, int32_t, uint32_t, uint32_t, int>, FlowCheckResult> cache;
    return cache;
}
bool& flowCheckFailedThisProcess() { static bool failed = false; return failed; }
}  // namespace

PushPredictPass::PushPredictPass() = default;
PushPredictPass::~PushPredictPass() = default;

D3D12_CPU_DESCRIPTOR_HANDLE PushPredictPass::cpu(UINT index) const {
    return {heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * increment_};
}
D3D12_GPU_DESCRIPTOR_HANDLE PushPredictPass::gpu(UINT index) const {
    return {heap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * increment_};
}

// predict: b0 (16 constants), t0 / t1 raw buffers (root SRVs), table {t2, t3, u0}, u1 raw buffer (root UAV).
// downsample: b0 (8 constants), t0 raw buffer (root SRV), table {u0}.
bool PushPredictPass::createPipeline(ID3D12Device* device, const char* shader, bool predict, ComPtr<ID3D12RootSignature>& root,
                                     ComPtr<ID3D12PipelineState>& pso) {
    std::vector<uint8_t> cs;
    if (!loadShaderBytes(shader, cs)) return false;
    std::array<D3D12_DESCRIPTOR_RANGE1, 2> ranges{};
    UINT rangeCount = 0;
    if (predict) {
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 2;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
        rangeCount = 2;
    } else {
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        rangeCount = 1;
    }
    std::array<D3D12_ROOT_PARAMETER1, 5> parameters{};
    UINT count = 0;
    parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[count].Constants.Num32BitValues = predict ? kConstants : 8; ++count;
    parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[count].Descriptor.ShaderRegister = 0; ++count;
    if (predict) { parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[count].Descriptor.ShaderRegister = 1; ++count; }
    parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[count].DescriptorTable.NumDescriptorRanges = rangeCount; parameters[count].DescriptorTable.pDescriptorRanges = ranges.data(); ++count;
    if (predict) { parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[count].Descriptor.ShaderRegister = 1; ++count; }
    for (UINT i = 0; i < count; ++i) parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = count; desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        log::error("push-predict", std::format("root signature for {} not created", shader));
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = root.Get(); pipeline.CS = {cs.data(), cs.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)))) {
        log::error("push-predict", std::format("pipeline for {} not created", shader));
        return false;
    }
    return true;
}

// verdict (review 2 SHOULD 3): b0 (2 constants), u0 counters, u1 verdict (both root UAVs).
bool PushPredictPass::createVerdictPipeline(ID3D12Device* device) {
    std::vector<uint8_t> cs;
    if (!loadShaderBytes("PushPredictVerdict.dxil", cs)) return false;
    std::array<D3D12_ROOT_PARAMETER1, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants.Num32BitValues = 2;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[2].Descriptor.ShaderRegister = 1;
    for (auto& p : parameters) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = UINT(parameters.size()); desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&verdictRoot_)))) {
        log::error("push-predict", "root signature for PushPredictVerdict.dxil not created");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = verdictRoot_.Get(); pipeline.CS = {cs.data(), cs.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&verdictPso_)))) {
        log::error("push-predict", "pipeline for PushPredictVerdict.dxil not created");
        return false;
    }
    return true;
}

bool PushPredictPass::initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    ready_ = false;
    if (!device || width < 2 || height < 2 || !predictFormat(format)) {
        log::error("push-predict", std::format("unsupported size {}x{} or format {}", width, height, unsigned(format)));
        return false;
    }
    device_ = device; width_ = width; height_ = height; format_ = format;
    if (!createPipeline(device, "PushPredict.dxil", true, predictRoot_, predictPso_) ||
        !createPipeline(device, "PushPredictDownsample.dxil", false, downsampleRoot_, downsamplePso_) || !createVerdictPipeline(device)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSlots * kPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))) || !stager_.initialize(device, 8)) return false;
    increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    counters_ = makeBufferOf(device, kSlots * kCounterBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    countersZero_ = makeBufferOf(device, kSlots * kCounterBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    countersReadback_ = makeBufferOf(device, PushPredictReadbackRing::kSlots * kCounterBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    verdict_ = makeBufferOf(device, kSlots * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    if (!verdict_) return false;
    if (!counters_ || !countersZero_ || !countersReadback_) return false;
    void* zero = nullptr;
    if (FAILED(countersZero_->Map(0, nullptr, &zero)) || !zero) return false;
    std::memset(zero, 0, kSlots * kCounterBytes);
    countersZero_->Unmap(0, nullptr);
    // Package 16, item 4: the stage timestamps (optional).
    D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = PushPredictReadbackRing::kSlots * kPushPredictStamps;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&stamps_)))) stamps_.Reset();
    stampsReadback_ = stamps_ ? makeBufferOf(device, uint64_t(qd.Count) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false) : nullptr;
    if (!stampsReadback_) stamps_.Reset();
    ready_ = true;
    log::info("push-predict", std::format("initialized {}x{} format={} (experiment VEYRA_EXP_RP_FG_SCHEDULE=onepredicted)", width, height, unsigned(format)));
    return true;
}

bool PushPredictPass::priorSuspect() {
    std::string m = readFlowCheckMarker();
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    return m == "suspect " + flowCheckIdentity();
}
void PushPredictPass::noteSessionEnd(int action, const char* why) {
    const auto a = static_cast<engine::PushPlayFaultAction>(action);
    if (a == engine::PushPlayFaultAction::None) return;
    std::lock_guard lock(flowCheckMutex());
    if (a == engine::PushPlayFaultAction::ClearSuspect) {
        clearFlowCheckMarker();
        log::info("push-predict", "a session with the row on ended normally: the earlier \"suspect\" mark of Lower delay is cleared");
        return;
    }
    if (a == engine::PushPlayFaultAction::WriteSuspect) {
        writeFlowCheckMarker("suspect " + flowCheckIdentity());
        log::warn("push-predict", std::format("the session ended ({}) while the predicted frames were running: marked \"suspect\" (a second time in a row "
                                              "blocks Lower delay until the next program update; {})", why, flowCheckMarkerPath().string()));
        return;
    }
    flowCheckFailedThisProcess() = true;
    flowCheckCache().clear();
    writeFlowCheckMarker("failed " + flowCheckIdentity());
    log::error("push-predict", std::format("the session ended ({}) while the predicted frames were running, the second time in a row: \"Lower delay\" is "
                                           "blocked until the next program update (or until {} is deleted)", why, flowCheckMarkerPath().string()));
}

std::string PushPredictPass::flowCheckIdentity() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (n == 0 || n >= MAX_PATH || !GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return "unknown";
    return std::format("{:08x}{:08x}-{:08x}{:08x}", a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime, a.nFileSizeHigh, a.nFileSizeLow);
}
std::filesystem::path PushPredictPass::flowCheckMarker() { return flowCheckMarkerPath(); }

bool PushPredictPass::initializeFlow(ID3D12Device* device, uint32_t flowWidth, uint32_t flowHeight) {
    if (!ready_ || !device) return false;
    flowWidth_ = flowWidth; flowHeight_ = flowHeight;
    for (UINT s = 0; s < kSlots; ++s) {
        for (UINT i = 0; i < 2; ++i) {
            flowPicture_[s][i] = makeTexture(device, flowWidth, flowHeight, DXGI_FORMAT_B8G8R8A8_UNORM, true);
            if (!flowPicture_[s][i]) return false;
            makeUav(device, flowPicture_[s][i].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, cpu(s * kPerSlot + 3 + i));
        }
        motion_[s] = makeTexture(device, flowWidth, flowHeight, DXGI_FORMAT_R16G16_FLOAT, true);
        trust_[s] = makeTexture(device, flowWidth, flowHeight, DXGI_FORMAT_R8_UNORM, true);
        if (!motion_[s] || !trust_[s]) return false;
    }
    // P's motion source (PushPredictFlowChoice): FidelityFX by default; GPU DIS strict or float only when chosen for a
    // comparison (VEYRA_TEST_PREDICT_FLOW). Crash containment (PushFlowCheckPlan): one self-check per process for each
    // adapter, size and choice, behind a marker file; the strict build only with FP64 shader operations; a flow is used
    // only after its own self-run completed and found the known motion.
    static const PushPredictFlowChoice choice = [] {
        wchar_t value[16]{};
        const DWORD n = GetEnvironmentVariableW(L"VEYRA_TEST_PREDICT_FLOW", value, 16);
        return n > 0 && n < 16 ? pushPredictFlowChoiceFrom(std::wstring_view(value, n)) : PushPredictFlowChoice::Auto;
    }();
#ifdef VEYRA_HAS_AMD_OF
    constexpr bool fidelityBuilt = true;
#else
    constexpr bool fidelityBuilt = false;
#endif
    // FidelityFX's own minimum (pushPredictFidelityExtentOk) and its wave size (pushPredictFidelityWaveOk): without them
    // the context is never created. VEYRA_TEST_PREDICT_FIDELITY_MECHANICS=1 (tests only, e.g. on WARP's 4-lane waves)
    // creates and uses it anyway, after a self-check that only has to complete: what runs is tested, not its motion.
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &waves, sizeof(waves)))) waves = {};
    const bool mechanicsOnly = GetEnvironmentVariableW(L"VEYRA_TEST_PREDICT_FIDELITY_MECHANICS", nullptr, 0) > 0;
    const bool fidelitySizeOk = pushPredictFidelityExtentOk(PushExtent{flowWidth, flowHeight});
    const bool fidelityWaveOk = waves.WaveOps && pushPredictFidelityWaveOk(waves.WaveLaneCountMin);
    const bool fidelityUsable = fidelityBuilt && fidelitySizeOk && (fidelityWaveOk || mechanicsOnly);
    bool fidelityFaulted = false;
    const auto makeFidelity = [&](std::unique_ptr<guidance::AmdOpticalFlow>& out) {
        std::vector<uint8_t> cs;
        auto of = std::make_unique<guidance::AmdOpticalFlow>();
        if (!sehFidelityInitialize(of.get(), device, flowWidth, flowHeight, &fidelityFaulted)) {
            if (fidelityFaulted) { (void)of.release(); log::error("push-predict", "FidelityFX optical flow faulted while being created (caught); not used"); }
            return false;
        }
        if (!loadShaderBytes("NvofDensify.dxil", cs) || !densify_.create(device, cs, 3 * 7, 5, 2)) return false;
        out = std::move(of);
        return true;
    };
    if (choice == PushPredictFlowChoice::Zero) {
        const bool made = initializeZeroFlow(device);
        log::warn("push-predict", std::format("flow source={} {}x{} for work {}x{}: TEST-ONLY VEYRA_TEST_PREDICT_FLOW=zero: no motion source, P's flow is zero with "
                                              "full trust (P is B where anything moves); for the engine test of the pair's plumbing, schedule and timing",
                                              made ? "test-zero" : "none", flowWidth, flowHeight, width_, height_));
        return made;
    }
    const LUID luid = device->GetAdapterLuid();
    const auto key = std::make_tuple(uint32_t(luid.LowPart), int32_t(luid.HighPart), flowWidth, flowHeight, int(choice) | (mechanicsOnly ? 0x100 : 0));
    std::lock_guard lock(flowCheckMutex());
    FlowCheckResult result;
    std::unique_ptr<guidance::GpuDisOpticalFlow> testedDis;     // the instance whose self-run passed, used as it is
    std::unique_ptr<guidance::AmdOpticalFlow> testedFidelity;
    const auto cached = flowCheckCache().find(key);
    if (cached != flowCheckCache().end()) {
        result = cached->second;
    } else {
        const std::string identity = flowCheckIdentity();
        const PushFlowCheckBlock block = flowCheckFailedThisProcess() ? PushFlowCheckBlock::FailedThisProcess : pushFlowCheckMarker(readFlowCheckMarker(), identity);
        if (block == PushFlowCheckBlock::StoppedInsideLastTime) writeFlowCheckMarker("failed " + identity);   // stays until the program file changes
        result.deviceDoubles = guidance::GpuDisOpticalFlow::deviceHasDoubles(device);
        result.plan = pushFlowCheckPlan(choice, result.deviceDoubles, fidelityUsable, block);
        const auto& plan = result.plan;
        FlowSelfRun run;
        bool created = false;
        // A "suspect" mark (one device loss in play) survives a passing check (delta SHOULD 1: two strikes in a row).
        const bool wasSuspect = [&] { std::string m = readFlowCheckMarker(); while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back(); return m == "suspect " + identity; }();
        if (plan.fidelity || plan.strict || plan.flt) writeFlowCheckMarker("running " + identity);
        if (plan.fidelity) {
            created = makeFidelity(testedFidelity);
            if (created) {
                fidelity_ = std::move(testedFidelity);   // the check runs the pass's own play path
                const FidelityCheck fc = runFidelityCheck(device);
                testedFidelity = std::move(fidelity_);
                lastFlowFrame_ = 0; flowFrameOf_[0] = flowFrameOf_[1] = 0; warmOf_[0] = warmOf_[1] = false;
                run.ok = fc.ok; run.executed = fc.executed; run.hung = fc.hung; run.removed = fc.removed; run.faulted = fc.faulted;
                run.inFlight = fc.inFlight;
                run.gpuMs = fc.flowGpuMs;
                result.costMs = fc.pairGpuMs;
                result.fidelityFound = fc.found;
                result.fidelityText = fc.text;
            }
        } else if (plan.strict || plan.flt) {
            testedDis = std::make_unique<guidance::GpuDisOpticalFlow>();
            created = testedDis->initialize(device, flowWidth, flowHeight, plan.flt);
            if (created) run = runFlowSelfCheck(device, *testedDis, flowWidth, flowHeight, plan.flt ? "float" : "strict-fp64");
        }
        // REL16 delta MUST 2: a frame still on the GPU when the budget ran out is a failure too (marker "failed", nothing
        // cached), so a card that hangs in the check is not tried again at every start.
        const bool failed = pushFlowCheckFailed(run.hung, run.removed, run.faulted || fidelityFaulted, run.inFlight);
        if (failed) {
            // The GPU may still use an unfinished flow's objects, and the pass's own (its heap, pipelines, motion and trust
            // textures, counters, stamps: the graph keeps the pass, gpuMayStillUse): never destroyed.
            if (pushFlowCheckGpuMayStillUse(run.hung, run.removed, run.inFlight)) { (void)testedDis.release(); (void)testedFidelity.release(); gpuMayStillUse_ = true; }
            testedDis.reset(); testedFidelity.reset();
            flowCheckFailedThisProcess() = true;
            writeFlowCheckMarker("failed " + identity);
        } else if (plan.fidelity || plan.strict || plan.flt) {
            if (wasSuspect) writeFlowCheckMarker("suspect " + identity);
            else clearFlowCheckMarker();
        }
        result.ok = run.ok && !failed;
        result.gpuMs = run.gpuMs;
        if (!plan.fidelity) result.costMs = run.gpuMs;   // GPU DIS: its one run is P's flow
        if (result.ok && plan.fidelity) {
            result.found = result.fidelityFound;
        } else if (result.ok) {
            const PushFlowView view{run.motion.data(), run.motionPitch, run.trust.data(), run.trustPitch};
            result.agreement = pushFlowAgreement(view, view, flowWidth, flowHeight, -3.0f, -1.0f);
            result.found = pushFlowFoundMotion(result.agreement);
        }
        result.use = pushFlowUse(plan, result.ok, result.found || (mechanicsOnly && plan.fidelity));
        if (mechanicsOnly && plan.fidelity && result.ok && !result.found)
            log::warn("push-predict", "VEYRA_TEST_PREDICT_FIDELITY_MECHANICS: FidelityFX used although it did not find the known motion (tests only)");
        if (result.use != PushFlowUse::FidelityFx) testedFidelity.reset();
        if (result.use != PushFlowUse::Strict && result.use != PushFlowUse::Float) testedDis.reset();
        if (!failed) flowCheckCache()[key] = result;   // a failure blocks every later check of this process (flowCheckFailedThisProcess)
        if (!created && (plan.fidelity || plan.strict || plan.flt)) log::warn("push-predict", "the chosen flow could not be created (see the lines above)");
    }
    const auto& plan = result.plan;
    const char* blockText = plan.block == PushFlowCheckBlock::StoppedInsideLastTime ? " blocked: PushFPS stopped inside this self-check in an earlier run of this program file"
                          : plan.block == PushFlowCheckBlock::FailedThisProcess ? " blocked: an earlier self-check of this run failed" : "";
    log::info("push-predict", std::format("flow source={} {}x{} for work {}x{} choice={} selfCheck:{}{} checked={} fp64={} fidelityBuilt={} fidelitySizeOk={} waveLanes={}..{} fidelityWaveOk={} ok={} found={} trusted={:.3f} errorPx={:.3f} "
        "gpuMs={:.3f}{}{} (synthetic frames at graph build, once per device and size, one sample each on this GPU: not a measurement of play)",
        pushFlowUseName(result.use), flowWidth, flowHeight, width_, height_, pushPredictFlowChoiceName(choice), cached != flowCheckCache().end() ? " (result of this run's earlier check)" : "",
        blockText, plan.fidelity ? "fidelityfx" : plan.strict ? "strict-fp64" : plan.flt ? "float" : "none", result.deviceDoubles, fidelityBuilt, fidelitySizeOk, waves.WaveLaneCountMin, waves.WaveLaneCountMax, fidelityWaveOk, result.ok, result.found,
        result.agreement.strictTrusted, result.agreement.floatErrorPx, result.gpuMs, result.fidelityText.empty() ? "" : " ", result.fidelityText));
    if (result.use == PushFlowUse::None) {
        log::warn("push-predict", "no flow passed its self-check: the predicted frame is unavailable (the Lower delay row says so); nothing more is run for it");
        return false;
    }
    checkCostMs_ = result.costMs;
    if (result.use == PushFlowUse::FidelityFx) {
        if (!testedFidelity && !makeFidelity(testedFidelity)) return false;   // cached: a fresh context of what this run already checked
        fidelity_ = std::move(testedFidelity);
        lastFlowFrame_ = 0; flowFrameOf_[0] = flowFrameOf_[1] = 0; warmOf_[0] = warmOf_[1] = false;
        return true;
    }
    if (!testedDis) {
        testedDis = std::make_unique<guidance::GpuDisOpticalFlow>();
        if (!testedDis->initialize(device, flowWidth, flowHeight, result.use == PushFlowUse::Float)) return false;
    }
    flow_ = std::move(testedDis);
    floatFlow_ = result.use == PushFlowUse::Float;
    return true;
}

PushPredictPass::FidelityCheck PushPredictPass::runFidelityCheck(ID3D12Device* device) {
    FidelityCheck r;
    const auto started = std::chrono::steady_clock::now();
    const auto step = [&](const std::string& what) { log::info("push-predict", std::format("flow self-check fidelityfx {}x{} (work {}x{}): {}", flowWidth_, flowHeight_, width_, height_, what)); };
    const auto windows = pushFlowCheckWindows(width_, height_, flowWidth_, flowHeight_);
    const bool budgetOff = GetEnvironmentVariableW(L"VEYRA_TEST_PREDICT_FIDELITY_MECHANICS", nullptr, 0) > 0;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    if (!fidelity_ || FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || FAILED(list->Close())) return r;
    const uint32_t pitch = (width_ * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> frames[2] = {makeBufferOf(device, uint64_t(pitch) * height_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false),
                                        makeBufferOf(device, uint64_t(pitch) * height_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false)};
    ComPtr<ID3D12Resource> output = makeTexture(device, width_, height_, format_, true);
    if (!frames[0] || !frames[1] || !output) return r;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outFp{};
    UINT64 outBytes = 0;
    const auto od = output->GetDesc();
    device->GetCopyableFootprints(&od, 0, 1, 0, &outFp, nullptr, nullptr, &outBytes);
    ComPtr<ID3D12Resource> outRb = makeBufferOf(device, outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2 * kPushFlowCheckFrames;
    ComPtr<ID3D12QueryHeap> stamps;
    ComPtr<ID3D12Resource> stampRb = makeBufferOf(device, uint64_t(qh.Count) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    if (!outRb || !stampRb || FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&stamps)))) return r;
    uint8_t* mapped[2] = {};
    for (int i = 0; i < 2; ++i) if (FAILED(frames[i]->Map(0, nullptr, reinterpret_cast<void**>(&mapped[i]))) || !mapped[i]) return r;
    // The texture's separable parts over the widest shift the sequence reaches.
    int reach = 0;
    for (const auto& w : windows) reach = std::max({reach, std::abs(w.vx), std::abs(w.vy)});
    reach = reach * int(kPushFlowCheckFrames) + 2;
    std::vector<double> ax(size_t(width_) + 2 * size_t(reach)), cx(ax.size()), by(size_t(height_) + 2 * size_t(reach)), dy(by.size());
    for (size_t i = 0; i < ax.size(); ++i) { const double x = double(int(i) - reach); ax[i] = 45.0 * std::sin(0.21 * x); cx[i] = 25.0 * std::sin(0.53 * x); }
    for (size_t i = 0; i < by.size(); ++i) { const double y = double(int(i) - reach); by[i] = 35.0 * std::sin(0.17 * y); dy[i] = std::cos(0.29 * y); }
    const bool tenBit = format_ == DXGI_FORMAT_R10G10B10A2_UNORM;
    const auto wordOf = [&](double g) -> uint32_t {
        g = std::clamp(g, 0.0, 255.0);
        if (tenBit) { const uint32_t c = uint32_t(std::lround(g * 1023.0 / 255.0)); return c | (c << 10) | (c << 20) | (3u << 30); }
        const uint32_t c = uint32_t(std::lround(g)); return c | (c << 8) | (c << 16) | (255u << 24);
    };
    const auto fill = [&](uint8_t* to, int k) {
        for (const auto& w : windows)
            for (int y = w.y0; y < w.y1; ++y) {
                const size_t yi = size_t(y - w.vy * k + reach);
                uint32_t* row = reinterpret_cast<uint32_t*>(to + size_t(y) * pitch);
                for (int x = w.x0; x < w.x1; ++x) {
                    const size_t xi = size_t(x - w.vx * k + reach);
                    row[x] = wordOf(128.0 + ax[xi] + by[yi] + cx[xi] * dy[yi]);
                }
            }
    };
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) return r;
    const auto keepAll = [&] {
        for (IUnknown* o : std::initializer_list<IUnknown*>{queue.Get(), allocator.Get(), list.Get(), fence.Get(), frames[0].Get(), frames[1].Get(), output.Get(),
                                                            outRb.Get(), stamps.Get(), stampRb.Get()})
            keptForever().emplace_back(o);
    };
    const UINT last = UINT((kPushFlowCheckFrames - 1) % kSlots);
    step(std::format("{} frames, windows static and 1 / 4 / 12 / 30 flow px a frame (capped at 1/8 of a window)", kPushFlowCheckFrames));
    for (uint32_t k = 0; k < kPushFlowCheckFrames; ++k) {
        const bool lastFrame = k + 1 == kPushFlowCheckFrames;
        const UINT slot = UINT(k % kSlots);
        fill(mapped[slot], int(k));
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) { CloseHandle(done); return r; }
        list->EndQuery(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * k);
        bool recorded = sehRecordFlowFrame(this, list.Get(), slot, frames[slot].Get(), pitch, k + 1, k == 0, &r.faulted);
        if (recorded && lastFrame) {
            recorded = recordMotion(list.Get(), queue.Get(), fence.Get(), k + 1, slot, frames[1 - slot].Get(), frames[slot].Get(), pitch, k, k + 1) &&
                       recordPredict(list.Get(), slot, frames[1 - slot].Get(), frames[slot].Get(), pitch, motion(slot), trust(slot), output.Get(), pushPredictStep(3));
            if (recorded) {
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = output.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = outRb.Get(); dst.PlacedFootprint = outFp;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
        }
        list->EndQuery(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * k + 1);
        if (lastFrame) list->ResolveQueryData(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, qh.Count, stampRb.Get(), 0);
        if (!recorded) {
            list->Close();
            log::warn("push-predict", std::format("flow self-check fidelityfx: frame {} not recorded{}", k + 1, r.faulted ? " (a fault inside FidelityFX was caught)" : ""));
            CloseHandle(done);
            if (k > 0) keepAll();
            return r;
        }
        if (FAILED(list->Close())) { CloseHandle(done); if (k > 0) keepAll(); return r; }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        r.executed = true;
        const bool signalled = SUCCEEDED(queue->Signal(fence.Get(), k + 1));
        // REL16 MUST 3: the wait is what is left of the budget (at least the floor), so the check cannot run past it by
        // more than the floor.
        const double waitMs = pushFlowCheckWaitMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), budgetOff);
        const DWORD waited = signalled && SUCCEEDED(fence->SetEventOnCompletion(k + 1, done)) ? WaitForSingleObject(done, DWORD(waitMs)) : WAIT_FAILED;
        const bool finished = waited == WAIT_OBJECT_0;
        r.removed = FAILED(device->GetDeviceRemovedReason());
        if (!finished && !r.removed && !budgetOff && waited == WAIT_TIMEOUT) {
            r.inFlight = true;   // over the budget with this frame still on the GPU: its objects are kept; a failure (marker)
            keepAll();
            CloseHandle(done);
            r.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            r.text = std::format("stopped: over the {:.0f} ms budget with frame {} of {} still on the GPU ({:.0f} ms)", kPushFlowCheckBudgetMs, k + 1, kPushFlowCheckFrames, r.wallMs);
            log::warn("push-predict", "flow self-check fidelityfx: " + r.text + "; the predicted frame is unavailable until the next program update");
            return r;   // ok = false; initializeFlow counts it as a failure (pushFlowCheckFailed)
        }
        if (!finished || r.removed) {
            r.hung = !finished;
            keepAll();
            CloseHandle(done);
            log::error("push-predict", std::format("flow self-check fidelityfx: frame {} {} (device state 0x{:08X}); the predicted frame is unavailable in this run",
                                                   k + 1, r.removed ? "removed the device" : "did not finish in time", unsigned(device->GetDeviceRemovedReason())));
            return r;
        }
        // The hard budget (pushFlowCheckOverBudget), after every frame, the last included (REL16 MUST 3): nothing is in
        // flight here. Not applied under the test-only mechanics switch (WARP runs the same work about two orders of
        // magnitude slower).
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!budgetOff && pushFlowCheckOverBudget(elapsed)) {
            CloseHandle(done);
            r.wallMs = elapsed;
            r.text = std::format("stopped: over the {:.0f} ms budget after frame {} of {} ({:.0f} ms)", kPushFlowCheckBudgetMs, k + 1, kPushFlowCheckFrames, elapsed);
            log::warn("push-predict", "flow self-check fidelityfx: " + r.text + "; the predicted frame is unavailable in this run");
            return r;   // ok = false: not used, not a fault
        }
    }
    CloseHandle(done);
    for (int i = 0; i < 2; ++i) frames[i]->Unmap(0, nullptr);
    const D3D12_RANGE none{0, 0};
    UINT64 frequency = 0;
    void* data = nullptr;
    if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency && SUCCEEDED(stampRb->Map(0, nullptr, &data)) && data) {
        std::vector<uint64_t> t(qh.Count);
        std::memcpy(t.data(), data, t.size() * 8);
        stampRb->Unmap(0, &none);
        double flowSum = 0; unsigned flowN = 0;
        for (uint32_t k = 0; k + 1 < kPushFlowCheckFrames; ++k)
            if (t[2 * k + 1] >= t[2 * k]) { flowSum += double(t[2 * k + 1] - t[2 * k]) * 1000.0 / double(frequency); ++flowN; }
        if (flowN) r.flowGpuMs = flowSum / flowN;
        const uint32_t k = kPushFlowCheckFrames - 1;
        if (t[2 * k + 1] >= t[2 * k]) r.pairGpuMs = double(t[2 * k + 1] - t[2 * k]) * 1000.0 / double(frequency);
    }
    data = nullptr;
    if (FAILED(outRb->Map(0, nullptr, &data)) || !data) return r;
    const uint8_t* out = static_cast<const uint8_t*>(data) + outFp.Offset;
    PushPredictCounters counters;
    const bool haveCounters = this->counters(last, counters);
    // Score every second pixel of each window's inside against the truth at +1/3 (frame index 7 + 1/3).
    const double kp = double(kPushFlowCheckFrames - 1), kt = kp + 1.0 / 3.0;
    const double blockWork = 8.0 * std::max(double(width_) / double(flowWidth_), double(height_) / double(flowHeight_));
    const auto greyAt = [&](uint32_t x, uint32_t y) {
        uint32_t w = 0; std::memcpy(&w, out + size_t(y) * outFp.Footprint.RowPitch + size_t(x) * 4, 4);
        return tenBit ? double(w & 1023u) * 255.0 / 1023.0 : double(w & 255u);
    };
    bool all = true;
    std::string text;
    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        const int m = pushFlowCheckMargin(w, blockWork);
        double sp = 0, sb = 0, qp = 0, qb = 0; uint64_t n = 0;
        for (int y = w.y0 + m; y < w.y1 - m; y += 2)
            for (int x = w.x0 + m; x < w.x1 - m; x += 2) {
                const double truth = std::clamp(pushFlowCheckGrey(x - w.vx * kt, y - w.vy * kt), 0.0, 255.0);
                const double b = std::clamp(pushFlowCheckGrey(x - w.vx * kp, y - w.vy * kp), 0.0, 255.0);
                const double p = greyAt(uint32_t(x), uint32_t(y));
                sp += std::abs(p - truth); sb += std::abs(b - truth); qp += (p - truth) * (p - truth); qb += (b - truth) * (b - truth); ++n;
            }
        const double errP = n ? sp / double(n) : -1.0, errB = n ? sb / double(n) : -1.0;
        const bool pass = n > 0 && pushFlowWindowPass(w, errP, errB);
        all = all && pass;
        text += std::format("{}w{}=({},{})flow/({},{})work errP={:.2f} errB={:.2f} psnrP={:.1f} psnrB={:.1f} n={} {}", i ? " " : "", i, w.flowVx, w.flowVy, w.vx, w.vy,
                            errP, errB, n ? pushPsnr(qp / double(n)) : 0.0, n ? pushPsnr(qb / double(n)) : 0.0, n, pass ? "pass" : "FAIL");
    }
    outRb->Unmap(0, &none);
    r.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    r.found = all;
    r.ok = true;
    r.text = std::format("windows: {}; counters still={} held={} fallback={} warped={}{}; flowGpuMs={:.3f} (per real frame: downsample, FidelityFX, densify) "
                         "pairGpuMs={:.3f} (the last frame and P) checkWallMs={:.0f} ({} frames: FidelityFX needs 7 to warm up and 4 more without its first-frames scene-change flags)",
                         text, counters.still, counters.held, counters.fallback, counters.warped, haveCounters ? "" : " (not read)", r.flowGpuMs, r.pairGpuMs, r.wallMs,
                         kPushFlowCheckFrames);
    step(all ? "found the known motions" : "did not find the known motions");
    return r;
}

// NvofDensify.hlsl in its FidelityFX mode (flags 4, with 1 bounds and 2 photometric; 8 while FidelityFX warms up: all
// zero), descriptors block `block` of densify_'s heap: t0 vectors, t1 cost (unused by FidelityFX: a null view), t2
// previous, t3 current (BGRA8 pictures of the flow size), t4 scene changes; u0 v x trust (R16G16_FLOAT), u1 trust.
bool PushPredictPass::recordDensify(guidance::AmdOpticalFlow& of, ID3D12GraphicsCommandList* list, UINT block, ID3D12Resource* previous,
                                    ID3D12Resource* current, ID3D12Resource* motion, ID3D12Resource* trust, bool warm) {
    auto* vectors = of.vectors();
    auto* cuts = of.sceneChanges();
    if (!list || !vectors || !cuts || !previous || !current || !motion || !trust || block > 2 || !densify_.heap) return false;
    const UINT base = block * 7;
    const auto visible = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{densify_.heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(base + i) * densify_.increment}; };
    const auto srv = [&](UINT i, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = f; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        stager_.stageSrv(r, &d, densify_.heap.Get(), base + i);
    };
    srv(0, vectors, vectors->GetDesc().Format);
    srv(1, nullptr, DXGI_FORMAT_R8_UINT);              // FidelityFX has no cost: a null view (the shader does not read it)
    srv(2, previous, DXGI_FORMAT_B8G8R8A8_UNORM);
    srv(3, current, DXGI_FORMAT_B8G8R8A8_UNORM);
    srv(4, cuts, DXGI_FORMAT_R32_UINT);                 // as EnhanceGraph views it
    makeUav(device_, motion, DXGI_FORMAT_R16G16_FLOAT, visible(5));
    makeUav(device_, trust, DXGI_FORMAT_R8_UNORM, visible(6));
    for (auto* r : {vectors, cuts, previous, current}) transition(list, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (auto* r : {motion, trust}) transition(list, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto md = motion->GetDesc();
    const std::array<uint32_t, 8> c = {of.width(), of.height(), uint32_t(md.Width), md.Height, 8u, 0u, 32u, 1u | 2u | 4u | (warm ? 0u : 8u)};
    const UINT64 heapGpu = densify_.heap->GetGPUDescriptorHandleForHeapStart().ptr;
    densify_.bind(list, reinterpret_cast<const float*>(c.data()), heapGpu + UINT64(base) * densify_.increment, heapGpu + UINT64(base + 5) * densify_.increment);
    list->Dispatch((uint32_t(md.Width) + 15) / 16, (md.Height + 15) / 16, 1);
    uavBarrier(list, motion);
    uavBarrier(list, trust);
    for (auto* r : {motion, trust}) transition(list, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (auto* r : {vectors, cuts, previous, current}) transition(list, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

bool PushPredictPass::initializeZeroFlow(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    const uint32_t motionPitch = (flowWidth_ * 4 + 255) & ~255u, trustPitch = (flowWidth_ + 255) & ~255u;
    ComPtr<ID3D12Resource> upload = makeBufferOf(device, uint64_t(motionPitch + trustPitch) * flowHeight_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    uint8_t* mapped = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped) return false;
    std::memset(mapped, 0, size_t(motionPitch) * flowHeight_);                              // RG16F zero
    std::memset(mapped + size_t(motionPitch) * flowHeight_, 255, size_t(trustPitch) * flowHeight_);   // R8 trust 1
    upload->Unmap(0, nullptr);
    for (UINT s = 0; s < kSlots; ++s) {
        for (int k = 0; k < 2; ++k) {
            auto* target = k == 0 ? motion_[s].Get() : trust_[s].Get();
            transition(list.Get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Offset = k == 0 ? 0 : uint64_t(motionPitch) * flowHeight_;
            from.PlacedFootprint.Footprint = {k == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8_UNORM, flowWidth_, flowHeight_, 1, k == 0 ? motionPitch : trustPitch};
            to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished = done && SUCCEEDED(queue->Signal(fence.Get(), 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    if (!finished) {
        for (IUnknown* o : std::initializer_list<IUnknown*>{queue.Get(), allocator.Get(), list.Get(), fence.Get(), upload.Get()}) keptForever().emplace_back(o);
        return false;
    }
    zeroFlow_ = true;
    return true;
}

bool PushPredictPass::motionReady(UINT slot, uint64_t frameIndex) const {
    if (slot >= kSlots) return false;
    if (zeroFlow_) return true;
    if (fidelity_) return flowFrameOf_[slot] == frameIndex && frameIndex != 0 && warmOf_[slot];
    return flow_ != nullptr;
}

bool PushPredictPass::recordFlowFrame(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* current, uint32_t rowPitch, uint64_t frameIndex,
                                      bool reset) {
    if (!ready_ || !fidelity_ || !list || slot >= kSlots || !current || rowPitch < width_ * 4 || frameIndex == 0) return false;
    // A reset (or the first frame) restarts FidelityFX's history. A gap (frames without the flow: skipped pairs, the row
    // off) keeps it: only this frame's vectors span more than one interval, so this frame alone is not usable.
    reset = reset || lastFlowFrame_ == 0;
    const bool gap = !reset && frameIndex != lastFlowFrame_ + 1;
    lastFlowFrame_ = frameIndex;
    flowFrameOf_[slot] = 0;   // until this frame's motion is recorded
    // This frame's picture at the flow size into flowPicture_[slot][1] (the downsample of recordMotion).
    auto* target = flowPicture_[slot][1].Get();
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(downsampleRoot_.Get());
    list->SetPipelineState(downsamplePso_.Get());
    transition(list, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::array<uint32_t, 8> c = {width_, height_, rowPitch, format_ == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u, flowWidth_, flowHeight_, 0u, 0u};
    list->SetComputeRoot32BitConstants(0, 8, c.data(), 0);
    list->SetComputeRootShaderResourceView(1, current->GetGPUVirtualAddress());   // a buffer in COMMON: promoted
    list->SetComputeRootDescriptorTable(2, gpu(slot * kPerSlot + 4));
    list->Dispatch((flowWidth_ + 15) / 16, (flowHeight_ + 15) / 16, 1);
    uavBarrier(list, target);
    transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (!fidelity_->dispatch(list, target, reset)) return false;
    const bool warm = fidelity_->warmedUp();
    if (!recordDensify(*fidelity_, list, slot, flowPicture_[1 - slot][1].Get(), target, motion_[slot].Get(), trust_[slot].Get(), warm)) return false;
    flowFrameOf_[slot] = frameIndex;
    warmOf_[slot] = warm && !gap;
    return true;
}


bool PushPredictPass::recordMotion(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t completion,
                                   UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                                   uint64_t previousId, uint64_t currentId) {
    if (!ready_ || !list || slot >= kSlots || !previous || !current || rowPitch < width_ * 4) return false;
    // Package 16, item 4: timestamps of the P this list will record (its number once recordPredict records it).
    const uint64_t upcoming = readback_.next + 1;
    const UINT stamp = PushPredictReadbackRing::slotOf(upcoming) * kPushPredictStamps;
    stampedMotion_ = 0;
    if (stamps_ && queue && stampFrequency_ == 0) { UINT64 f = 0; if (SUCCEEDED(queue->GetTimestampFrequency(&f))) stampFrequency_ = f; }
    if (fidelity_ || zeroFlow_) {
        // FidelityFX: the motion was recorded with the real frame (recordFlowFrame, in its own list, timed there as the
        // graph's Flow stage): nothing of it is in P's list, so the downsample and flow stages read 0 here.
        (void)fence; (void)completion; (void)previousId;
        if (!motionReady(slot, currentId)) return false;
        if (stamps_) for (UINT i = 0; i < 3; ++i) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + i);
        if (stamps_) stampedMotion_ = upcoming;
        return true;
    }
    if (!flow_) return false;
    if (stamps_) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 0);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(downsampleRoot_.Get());
    list->SetPipelineState(downsamplePso_.Get());
    ID3D12Resource* sources[2] = {previous, current};
    for (UINT i = 0; i < 2; ++i) {
        auto* target = flowPicture_[slot][i].Get();
        transition(list, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const std::array<uint32_t, 8> c = {width_, height_, rowPitch, format_ == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u,
                                           flowWidth_, flowHeight_, 0u, 0u};
        list->SetComputeRoot32BitConstants(0, 8, c.data(), 0);
        list->SetComputeRootShaderResourceView(1, sources[i]->GetGPUVirtualAddress());   // a buffer in COMMON: promoted
        list->SetComputeRootDescriptorTable(2, gpu(slot * kPerSlot + 3 + i));
        list->Dispatch((flowWidth_ + 15) / 16, (flowHeight_ + 15) / 16, 1);
        uavBarrier(list, target);
        transition(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    if (stamps_) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 1);
    // current = B, previous = A: the provider's current-to-previous vectors are B -> A.
    const bool flowed = flow_->dispatch(list, queue, fence, completion, slot, flowPicture_[slot][0].Get(), flowPicture_[slot][1].Get(),
                                        motion_[slot].Get(), trust_[slot].Get(), previousId, currentId);
    if (flowed && stamps_) { list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 2); stampedMotion_ = upcoming; }
    return flowed;
}

bool PushPredictPass::recordPredict(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current,
                                    uint32_t rowPitch, ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* output,
                                    float step, const PushPredictThresholds& t) {
    if (!ready_ || !list || slot >= kSlots || !previous || !current || !motion || !trust || !output || rowPitch < width_ * 4 ||
        rowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) return false;
    const auto od = output->GetDesc(), md = motion->GetDesc(), td = trust->GetDesc();
    if (od.Width != width_ || od.Height != height_ || od.Format != format_ || md.Width != td.Width || md.Height != td.Height ||
        md.Format != DXGI_FORMAT_R16G16_FLOAT) {
        log::error("push-predict", "output, flow or trust of the wrong size or format");
        return false;
    }
    // Descriptors of this slot: the caller has retired the slot's previous lists.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = md.Format; stager_.stageSrv(motion, &srv, heap_.Get(), slot * kPerSlot + 0);
    srv.Format = td.Format; stager_.stageSrv(trust, &srv, heap_.Get(), slot * kPerSlot + 1);
    makeUav(device_, output, format_, cpu(slot * kPerSlot + 2));

    const uint64_t counterOffset = uint64_t(slot) * kCounterBytes;
    transition(list, counters_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(counters_.Get(), counterOffset, countersZero_.Get(), counterOffset, kCounterBytes);
    transition(list, counters_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, trust, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(predictRoot_.Get());
    list->SetPipelineState(predictPso_.Get());
    const float maxMotion = t.maxMotionShare * float(width_);
    const std::array<float, kConstants> c = {
        bitsOf(width_), bitsOf(height_), bitsOf(rowPitch), bitsOf(format_ == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u),
        bitsOf(uint32_t(md.Width)), bitsOf(md.Height), bitsOf(uint32_t(counterOffset)), 0.0f,
        step, t.stillDelta, t.minMotion, maxMotion, t.minConfidence, t.maxPhotoDelta, 0.0f, 0.0f};
    list->SetComputeRoot32BitConstants(0, kConstants, c.data(), 0);
    list->SetComputeRootShaderResourceView(1, previous->GetGPUVirtualAddress());   // buffers in COMMON (or upload heaps): promoted
    list->SetComputeRootShaderResourceView(2, current->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(3, gpu(slot * kPerSlot));
    list->SetComputeRootUnorderedAccessView(4, counters_->GetGPUVirtualAddress());
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);

    uavBarrier(list, output);
    uavBarrier(list, counters_.Get());
    const uint64_t upcoming = readback_.next + 1;
    const bool timed = stamps_ && stampedMotion_ == upcoming;
    const UINT stamp = PushPredictReadbackRing::slotOf(upcoming) * kPushPredictStamps;
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 3);   // after the warp
    transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(list, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(list, trust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    // Review 2, SHOULD 3: more of the moving pixels fell back than were moved -> B over P as a whole, decided on the GPU
    // (shaders/PushPredictVerdict.hlsl writes the predicate; the copy below is skipped unless it is 1). B is read as a
    // buffer in COMMON already promoted to a read state by the warp above: no explicit barrier (read states accumulate).
    const uint64_t verdictOffset = uint64_t(slot) * 8;
    transition(list, verdict_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(verdictRoot_.Get());
    list->SetPipelineState(verdictPso_.Get());
    const std::array<uint32_t, 2> vc = {uint32_t(counterOffset), uint32_t(verdictOffset)};
    list->SetComputeRoot32BitConstants(0, 2, vc.data(), 0);
    list->SetComputeRootUnorderedAccessView(1, counters_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, verdict_->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    uavBarrier(list, verdict_.Get());
    transition(list, verdict_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION);
    transition(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = current; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {format_, width_, height_, 1, rowPitch};
    list->SetPredication(verdict_.Get(), verdictOffset, D3D12_PREDICATION_OP_EQUAL_ZERO);   // skipped unless the predicate is 1
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    transition(list, verdict_.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COMMON);
    transition(list, counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint64_t sequence = readback_.record();
    lastOfSlot_[slot] = sequence;
    timedOf_[PushPredictReadbackRing::slotOf(sequence)] = 0;
    if (timed && sequence == upcoming) {   // after the verdict and its predicated copy
        list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 4);
        list->ResolveQueryData(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp, kPushPredictStamps, stampsReadback_.Get(), uint64_t(stamp) * 8);
        timedOf_[PushPredictReadbackRing::slotOf(sequence)] = sequence;
    }
    stampedMotion_ = 0;
    list->CopyBufferRegion(countersReadback_.Get(), uint64_t(PushPredictReadbackRing::slotOf(sequence)) * kCounterBytes, counters_.Get(), counterOffset, kCounterBytes);
    transition(list, counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

bool PushPredictPass::counters(UINT slot, PushPredictCounters& out) const {
    if (!ready_ || slot >= kSlots) return false;
    return countersOf(lastOfSlot_[slot], out);
}

bool PushPredictPass::countersOf(uint64_t sequence, PushPredictCounters& out) const {
    if (!ready_ || !readback_.valid(sequence)) return false;
    const SIZE_T at = SIZE_T(PushPredictReadbackRing::slotOf(sequence));
    const D3D12_RANGE range{at * kCounterBytes, (at + 1) * kCounterBytes};
    void* data = nullptr;
    if (FAILED(countersReadback_->Map(0, &range, &data)) || !data) return false;
    std::array<uint32_t, 4> values{};
    std::memcpy(values.data(), static_cast<const uint8_t*>(data) + range.Begin, sizeof(values));
    const D3D12_RANGE none{0, 0};
    countersReadback_->Unmap(0, &none);
    out.still = values[0]; out.held = values[1]; out.fallback = values[2]; out.warped = values[3];
    return true;
}

bool PushPredictPass::stageTimesOf(uint64_t sequence, PushPredictStageMs& out) const {
    out.fill(-1.0);
    if (!ready_ || !stamps_ || !readback_.valid(sequence)) return false;
    const unsigned at = PushPredictReadbackRing::slotOf(sequence);
    if (timedOf_[at] != sequence) return false;
    const D3D12_RANGE range{SIZE_T(at) * kPushPredictStamps * 8, SIZE_T(at + 1) * kPushPredictStamps * 8};
    void* data = nullptr;
    if (FAILED(stampsReadback_->Map(0, &range, &data)) || !data) return false;
    uint64_t ticks[kPushPredictStamps] = {};
    std::memcpy(ticks, static_cast<const uint8_t*>(data) + range.Begin, sizeof(ticks));
    const D3D12_RANGE none{0, 0};
    stampsReadback_->Unmap(0, &none);
    out = pushPredictStagesFromTicks(ticks, stampFrequency_);
    return true;
}

}  // namespace veyra::pipeline
