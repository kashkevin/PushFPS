// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "predict": see include/veyra/pipeline/PushPredictPass.h and docs/drafts/PREDICT_GPU.md.
#include "veyra/pipeline/PushPredictPass.h"
#include "veyra/guidance/GpuDisOpticalFlow.h"

#include <array>
#include <bit>
#include <cstring>

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
    ready_ = true;
    log::info("push-predict", std::format("initialized {}x{} format={} (experiment VEYRA_EXP_RP_FG_SCHEDULE=onepredicted)", width, height, unsigned(format)));
    return true;
}

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
    auto flow = std::make_unique<guidance::GpuDisOpticalFlow>();
    if (!flow->initialize(device, flowWidth, flowHeight)) return false;
    flow_ = std::move(flow);
    log::info("push-predict", std::format("flow {}x{} (GPU DIS, bidirectional, on the enhanced real frames)", flowWidth, flowHeight));
    return true;
}

bool PushPredictPass::recordMotion(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t completion,
                                   UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                                   uint64_t previousId, uint64_t currentId) {
    if (!ready_ || !flow_ || !list || slot >= kSlots || !previous || !current || rowPitch < width_ * 4) return false;
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
    // current = B, previous = A: the provider's current-to-previous vectors are B -> A.
    return flow_->dispatch(list, queue, fence, completion, slot, flowPicture_[slot][0].Get(), flowPicture_[slot][1].Get(),
                           motion_[slot].Get(), trust_[slot].Get(), previousId, currentId);
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

}  // namespace veyra::pipeline
