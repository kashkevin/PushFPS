// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS automatic Remote Play latency test: readback of presented frames around a press.
#include "veyra/engine/PushInputLatencyProbe.h"
#include <chrono>

#include <algorithm>

namespace veyra::engine {
namespace {
bool layoutOf(DXGI_FORMAT format, quality::OutputLayout& layout) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: layout = quality::OutputLayout::Rgba8; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: layout = quality::OutputLayout::Bgra8; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: layout = quality::OutputLayout::Rgb10a2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: layout = quality::OutputLayout::Rgba16fLinear; return true;
    default: return false;
    }
}
} // namespace

int64_t PushInputLatencyProbe::now100ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}

void PushInputLatencyProbe::beginWindow() {
    std::lock_guard lock(m_);
    open_ = true; sent_ = 0; answerMs_ = -1; answerPresent_ = 0; answerPts_ = 0; pictureMoving_ = false; detector_.reset();
    judged_ = {}; judgedNext_ = 0;
    collecting_.store(true, std::memory_order_relaxed);
}
void PushInputLatencyProbe::endWindow() {
    std::lock_guard lock(m_);
    open_ = false;
    collecting_.store(false, std::memory_order_relaxed);
}
void PushInputLatencyProbe::markSent(int64_t sentHost100ns) {
    std::lock_guard lock(m_);
    sent_ = sentHost100ns; answerMs_ = -1; answerPresent_ = 0; answerPts_ = 0; pictureMoving_ = false;
    judged_ = {}; judgedNext_ = 0;
}
void PushInputLatencyProbe::setDetectorMode(latency::Detector::Mode mode) {
    std::lock_guard lock(m_);
    detector_.setMode(mode);
}
void PushInputLatencyProbe::setMinimumAnswerMs(double ms) {
    std::lock_guard lock(m_);
    detector_.setMinimumAnswerMs(ms);
}
void PushInputLatencyProbe::noteArrival(int64_t pts100ns, int64_t arrivalHost100ns) {
    if (!collecting_.load(std::memory_order_relaxed) || pts100ns <= 0 || arrivalHost100ns <= 0) return;
    std::lock_guard lock(m_);
    arrivals_[arrivalNext_ % arrivals_.size()] = Arrival{pts100ns, arrivalHost100ns};
    ++arrivalNext_;
}
PushInputLatencyProbe::PressTiming PushInputLatencyProbe::timing() const {
    std::lock_guard lock(m_);
    PressTiming t;
    t.sent = sent_;
    if (answerMs_ >= 0) t.present = answerPresent_;
    int64_t bestPts = 0;
    for (const auto& a : arrivals_) {
        if (a.host <= 0) continue;
        // The frame that showed the answer: its own arrival for a real frame (same timestamp), for a generated
        // one the real frame it was made towards (the smallest timestamp not before it).
        if (answerMs_ >= 0 && answerPts_ > 0 && a.pts >= answerPts_ && (!bestPts || a.pts < bestPts)) { bestPts = a.pts; t.arrival = a.host; }
        if (sent_ > 0 && a.host <= sent_ && a.host > t.lastArrivalBeforeSent) t.lastArrivalBeforeSent = a.host;
    }
    return t;
}

bool PushInputLatencyProbe::record(ID3D12GraphicsCommandList* list, const Frame& frame) {
    std::lock_guard lock(m_);
    if (!open_ || !list || !frame.texture || answerMs_ >= 0 || pictureMoving_) return false;
    quality::OutputLayout layout;
    if (!layoutOf(frame.format, layout)) return false;
    Slot* slot = nullptr;
    for (auto& s : slots_) if (s.state == Slot::State::Free) { slot = &s; break; }
    if (!slot) return false;   // all copies in flight: this frame is skipped
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(frame.texture->GetDevice(IID_PPV_ARGS(&device)))) return false;
    const D3D12_RESOURCE_DESC desc = frame.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    if (!total) return false;
    if (device_.Get() != device.Get()) { for (auto& s : slots_) if (s.state == Slot::State::Free) s.buffer.Reset(); device_ = device; }
    if (!slot->buffer || slot->bytes < total) {
        slot->buffer.Reset();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot->buffer)))) return false;
        slot->bytes = total;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = frame.texture; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    to.pResource = slot->buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    slot->footprint = footprint;
    slot->width = std::min<uint32_t>(frame.width ? frame.width : uint32_t(desc.Width), uint32_t(desc.Width));
    slot->height = std::min<uint32_t>(frame.height ? frame.height : desc.Height, desc.Height);
    slot->layout = layout;
    slot->host100ns = now100ns();   // just before this frame's Present: when it is shown
    slot->pts100ns = frame.pts100ns;
    slot->order = ++order_;
    slot->state = Slot::State::Pending;
    return true;
}

void PushInputLatencyProbe::submitted(ID3D12Fence* fence, uint64_t value) {
    std::lock_guard lock(m_);
    for (auto& s : slots_)
        if (s.state == Slot::State::Pending) {
            if (fence) { s.fence = fence; s.fenceValue = value; s.state = Slot::State::InFlight; }
            else s.state = Slot::State::Free;
        }
}

void PushInputLatencyProbe::discarded() {
    std::lock_guard lock(m_);
    for (auto& s : slots_) if (s.state == Slot::State::Pending) s.state = Slot::State::Free;
}

double PushInputLatencyProbe::poll() {
    for (;;) {
        // The oldest finished copy first, so frames are judged in order.
        Slot* next = nullptr;
        {
            std::lock_guard lock(m_);
            for (auto& s : slots_)
                if (s.state == Slot::State::InFlight && s.fence && s.fence->GetCompletedValue() >= s.fenceValue && (!next || s.order < next->order)) next = &s;
            if (!next) return answerMs_;
        }
        quality::Luma luma;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, SIZE_T(next->footprint.Offset + uint64_t(next->footprint.Footprint.RowPitch) * next->height)};
        bool ok = false;
        if (SUCCEEDED(next->buffer->Map(0, &range, &mapped)) && mapped) {
            ok = quality::lumaFromOutput(static_cast<const uint8_t*>(mapped) + next->footprint.Offset, next->footprint.Footprint.RowPitch,
                                         next->width, next->height, next->layout, luma);
            const D3D12_RANGE none{0, 0};
            next->buffer->Unmap(0, &none);
        }
        std::lock_guard lock(m_);
        next->state = Slot::State::Free; next->fence.Reset();
        if (!ok || next->order <= lastRead_) continue;
        lastRead_ = next->order;
        if (!open_ || answerMs_ >= 0 || pictureMoving_) continue;
        if (!sent_ || next->host100ns < sent_) { detector_.before(luma); continue; }
        // Bottom (the PS button): judge() is the original answered(); Anywhere (v2): it can also say PictureMoving.
        const double ms = double(next->host100ns - sent_) / 10000.0;
        judged_[judgedNext_ % judged_.size()] = Judged{next->host100ns, next->pts100ns};
        ++judgedNext_;
        const auto verdict = detector_.judge(luma, ms);
        if (verdict == latency::Detector::Verdict::Answered) {
            // The onset frame (the frame just judged unless the jump confirmed a change that began earlier).
            const size_t back = std::min(size_t(std::max(0, detector_.onsetFramesBack())), std::min(judgedNext_, judged_.size()) - 1);
            const Judged& onset = judged_[(judgedNext_ - 1 - back) % judged_.size()];
            answerMs_ = double(onset.host - sent_) / 10000.0; answerPresent_ = onset.host; answerPts_ = onset.pts;
        }
        else if (verdict == latency::Detector::Verdict::PictureMoving) pictureMoving_ = true;
    }
}

} // namespace veyra::engine
