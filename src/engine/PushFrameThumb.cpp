// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/engine/PushFrameThumb.h"
#include "veyra/Log.h"
#include <format>
#include <vector>

namespace veyra::engine {
bool PushFrameThumb::readableFormat(DXGI_FORMAT f, bool* floatOut) {
    bool floatFormat = false;
    const bool ok = [&] {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
        floatFormat = false; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
        floatFormat = true; return true;
    default: return false;
    }
    }();
    if (floatOut) *floatOut = floatFormat;
    return ok;
}
namespace {
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}
}  // namespace

bool PushFrameThumb::create(ID3D12Device* device) {
    if (created_) return true;
    if (failed_ || !device) return false;
    failed_ = true;   // until everything below succeeded
    std::vector<uint8_t> cs;
    // Heap: one SRV per entry (an entry's view is rewritten only while it is free), then the UAV.
    if (!pipeline::loadShaderBytes("PushFrameThumb.dxil", cs) || !pass_.create(device, cs, kEntries + 1, 1, 1, 4)) {
        veyra::log::warn("frame-probe", "thumbnail pass unavailable (shader or pipeline); the frame probe is off");
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&thumb_)))) return false;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Width = kBytes * kEntries;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) return false;
    // A readback heap may stay mapped; each entry is read only after its fence completed.
    if (FAILED(readback_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_))) || !mapped_) return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = UINT(PushFrameProbe::kCells);
    device->CreateUnorderedAccessView(thumb_.Get(), nullptr, &uav,
        {pass_.heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(kEntries) * pass_.increment});
    failed_ = false;
    created_ = true;
    veyra::log::info("frame-probe", std::format("thumbnail pass ready: {}x{} luma, 16 point taps per cell, {} readback entries of {} bytes, after Present, never waited for",
                                         PushFrameProbe::kWidth, PushFrameProbe::kHeight, kEntries, kBytes));
    return true;
}

bool PushFrameThumb::record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* source, DXGI_FORMAT format, std::uint64_t sequence, bool generated) {
    bool floatFormat = false;
    if (!created_ || !list || !source || !readableFormat(format, &floatFormat) || count_ >= kEntries || recorded_ != kEntries) return false;
    const unsigned index = (head_ + count_) % kEntries;
    auto& e = entries_[index];
    if (e.state != State::Free) return false;
    const auto cpuBase = pass_.heap->GetCPUDescriptorHandleForHeapStart().ptr, gpuBase = pass_.heap->GetGPUDescriptorHandleForHeapStart().ptr;
    pipeline::makeSrv(device, source, format, {cpuBase + SIZE_T(index) * pass_.increment});
    transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, thumb_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT constants[4] = {floatFormat ? 1u : 0u, 0u, 0u, 0u};
    pass_.bind(list, reinterpret_cast<const float*>(constants), gpuBase + UINT64(index) * pass_.increment, gpuBase + UINT64(kEntries) * pass_.increment);
    list->Dispatch((PushFrameProbe::kWidth + 7) / 8, (PushFrameProbe::kHeight + 7) / 8, 1);
    transition(list, thumb_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback_.Get(), UINT64(index) * kBytes, thumb_.Get(), 0, kBytes);
    transition(list, thumb_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    e.state = State::Recorded;
    e.sequence = sequence;
    e.generated = generated;
    recorded_ = index;
    return true;
}

void PushFrameThumb::submitted(ID3D12Fence* fence, std::uint64_t value) {
    if (recorded_ == kEntries) return;
    auto& e = entries_[recorded_];
    e.state = State::InFlight;
    e.fence = fence;
    e.value = value;
    ++count_;
    recorded_ = kEntries;
}

void PushFrameThumb::discarded() {
    if (recorded_ == kEntries) return;
    entries_[recorded_].state = State::Free;
    recorded_ = kEntries;
}

void PushFrameThumb::reset() {
    for (auto& e : entries_) e = Entry{};
    head_ = count_ = 0;
    recorded_ = kEntries;
}

PushFrameThumb::~PushFrameThumb() {
    if (readback_ && mapped_) {
        const D3D12_RANGE none{};
        readback_->Unmap(0, &none);
    }
}

}  // namespace veyra::engine
