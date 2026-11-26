// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/engine/PushPipelineProbe.h"
#include "veyra/engine/PushFrameDump.h"
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace veyra::engine {
namespace {
void mergeMean(PushMetricMean& into, const PushMetricMean& from) {
    into.sum += from.sum;
    into.count += from.count;
}
void mergeStats(PushQualityStats& into, const PushQualityStats& from) {
    into.frames += from.frames;
    if (from.width && from.height) { into.width = from.width; into.height = from.height; }
    into.hdr = into.hdr || from.hdr;
    mergeMean(into.blockiness, from.blockiness);
    mergeMean(into.banding, from.banding);
    mergeMean(into.ringing, from.ringing);
    mergeMean(into.detail, from.detail);
    mergeMean(into.hudDelta, from.hudDelta);
    mergeMean(into.flicker, from.flicker);
    mergeMean(into.meanLuma, from.meanLuma);
}
// The per-frame numbers of one picture. `scale` = its width over the raw picture's width (1 for the raw one).
void addFrame(PushQualityStats& stats, const PushLumaImage& image, double scale, bool hdr) {
    ++stats.frames;
    stats.width = static_cast<uint32_t>(image.width);
    stats.height = static_cast<uint32_t>(image.height);
    stats.hdr = hdr;
    stats.blockiness.add(pushBlockiness(image, 8.0 * scale));
    stats.banding.add(pushBanding(image));
    stats.ringing.add(pushRinging(image, scale));
    stats.detail.add(pushDetail(image, scale));
    stats.meanLuma.add(pushMeanLuma(image));
}
} // namespace

bool PushPipelineProbe::layoutOf(DXGI_FORMAT format, Layout& layout) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        layout = Layout::Rgba8; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        layout = Layout::Bgra8; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        layout = Layout::Rgb10a2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        layout = Layout::Rgba16f; return true;
    default: return false;
    }
}

void PushPipelineProbe::start(const Options& options) {
    std::lock_guard lock(m_);
    options_ = options;
    options_.seconds = std::clamp<uint32_t>(options.seconds, 2, 600);
    options_.saveFrames = std::min<uint32_t>(options.saveFrames, 4);
    saved_ = 0;
    if (options_.hud.empty()) {
        // Where console HUDs sit: the four corners, a quarter of each side.
        options_.hud = {PushRectN{0.0f, 0.0f, 0.25f, 0.25f}, PushRectN{0.75f, 0.0f, 1.0f, 0.25f},
                        PushRectN{0.0f, 0.75f, 0.25f, 1.0f}, PushRectN{0.75f, 0.75f, 1.0f, 1.0f}};
    }
    if (options_.hud.size() > 8) options_.hud.resize(8);
    running_ = true; done_ = false; failed_ = false; reason_.clear();
    releaseWanted_ = false;
    target_ = pushPipelineProbePairs(options_.seconds);
    pairs_ = 0;
    filled_ = 0;   // a copy still in flight from an earlier run is overwritten by this run's first frame
    rawStats_ = {}; outputStats_ = {};
    const auto now = std::chrono::steady_clock::now();
    interval_ = std::chrono::milliseconds(static_cast<int64_t>(options_.seconds) * 1000 / static_cast<int64_t>(target_));
    nextDue_ = now;
    rawGrace_ = now + std::chrono::seconds(1);   // the graph keeps the raw picture from its next frame on
    deadline_ = now + std::chrono::seconds(static_cast<int64_t>(options_.seconds) + 5);
    ++revision_;
    veyra::log::info("slipstream-quality", std::format("event=start seconds={} pairs={} rectangles={}", options_.seconds, target_, options_.hud.size()));
}

PushPipelineProbe::~PushPipelineProbe() {
    if (work_.valid()) work_.wait();
}

void PushPipelineProbe::cancel() {
    std::lock_guard lock(m_);
    if (running_) veyra::log::info("slipstream-quality", std::format("event=cancelled pairs={}", pairs_));
    running_ = false;
    releaseWanted_ = true;
    releaseIfIdleLocked();
}

void PushPipelineProbe::releaseIfIdleLocked() {
    if (!releaseWanted_ || running_ || pending_ || reading_) return;
    // A submitted copy may still execute: wait (at the next call) until its fence has passed.
    for (uint32_t i = 0; i < filled_ && i < 2; ++i)
        if (slots_[i].fence && slots_[i].fence->GetCompletedValue() < slots_[i].fenceValue) return;
    for (auto& slot : slots_) { slot.output = {}; slot.raw = {}; slot.screen = {}; slot.fence.Reset(); }
    retired_.clear();
    device_.Reset();
    filled_ = 0;
    releaseWanted_ = false;
}

bool PushPipelineProbe::wantsRawFrames() const {
    std::lock_guard lock(m_);
    return running_;
}

void PushPipelineProbe::failLocked(const char* reason) {
    running_ = false; failed_ = true; reason_ = reason;
    veyra::log::warn("slipstream-quality", std::format("event=failed reason={} pairs={}", reason, pairs_));
    releaseWanted_ = true;
    releaseIfIdleLocked();
}

void PushPipelineProbe::finishLocked() {
    running_ = false; done_ = true;
    veyra::log::info("slipstream-quality", pushQualityLine(rawStats_, "raw"));
    veyra::log::info("slipstream-quality", pushQualityLine(outputStats_, "output"));
    // Give the read-back memory and the device back as soon as nothing is in flight (now, or at a later call).
    releaseWanted_ = true;
    releaseIfIdleLocked();
}

bool PushPipelineProbe::copyInto(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* texture, uint32_t width, uint32_t height, Copy& copy) {
    copy.valid = false;
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    if (!total) return false;
    if (!copy.buffer || copy.bytes < total) {
        // A buffer an earlier, still executing list may write to stays alive until a later pair has been read.
        if (copy.buffer) retired_.push_back(copy.buffer);
        copy.buffer.Reset();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy.buffer)))) {
            copy.bytes = 0;
            return false;
        }
        copy.bytes = total;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = texture; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    to.pResource = copy.buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    copy.footprint = footprint;
    copy.width = std::min<uint32_t>(width ? width : uint32_t(desc.Width), uint32_t(desc.Width));
    copy.height = std::min<uint32_t>(height ? height : desc.Height, desc.Height);
    copy.valid = true;
    return true;
}

bool PushPipelineProbe::record(ID3D12GraphicsCommandList* list, const Frame& frame) {
    std::lock_guard lock(m_);
    if (!running_) { releaseIfIdleLocked(); return false; }
    if (pending_ || reading_ || filled_ >= 2 || !list || !frame.texture || frame.generated) return false;
    // The first frame of a pair waits for its time; the second is the next real frame after it.
    const auto now = std::chrono::steady_clock::now();
    if (filled_ == 0 && now < nextDue_) return false;
    Layout layout = Layout::Rgba8;
    if (!layoutOf(frame.format, layout)) { failLocked("format"); return false; }
    // Decoded as BT.2020 PQ: only HDR10 output uses this format (EnhanceGraph::outputFormat).
    if (layout == Layout::Rgb10a2 && frame.hdrMode != 2) { failLocked("format"); return false; }
    // A pair starts with the raw picture beside it; only when the graph has none to give after a second
    // (a colour grade or an exclusion keeps its own reference) is the output measured alone.
    if (filled_ == 0 && !frame.raw && now < rawGrace_) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(frame.texture->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (device_.Get() != device.Get()) {
        // Another device (a new session): its predecessor's buffers and half pair are of no use.
        for (auto& slot : slots_) { slot.output = {}; slot.raw = {}; slot.screen = {}; slot.fence.Reset(); }
        retired_.clear();
        device_ = device;
        filled_ = 0;
    }
    Slot& slot = slots_[filled_];
    slot.output.layout = layout;
    if (!copyInto(list, device.Get(), frame.texture, frame.width, frame.height, slot.output)) { failLocked("memory"); return false; }
    slot.raw.valid = false;
    if (frame.raw) {
        Layout rawLayout = Layout::Rgba8;
        if (layoutOf(frame.raw->GetDesc().Format, rawLayout) && rawLayout == Layout::Rgba16f) {
            slot.raw.layout = rawLayout;
            // Without memory for it the pair is still measured, as output only.
            static_cast<void>(copyInto(list, device.Get(), frame.raw, 0, 0, slot.raw));
        }
    }
    slot.hdrMode = frame.hdrMode;
    slot.rawSdrInHdr = frame.rawSdrInHdr;
    slot.pts100ns = frame.pts100ns;
    slot.ditherStep = frame.ditherStep;
    slot.presentFine = frame.presentFine;
    // Stream "clarity": the first frame of a pair, while frames are still to be saved, also keeps the back buffer.
    slot.save = false;
    slot.screen.valid = false;
    if (filled_ == 0 && saved_ < options_.saveFrames && slot.raw.valid) {
        slot.save = true;
        slot.saveIndex = saved_++;
        Layout screenLayout = Layout::Rgba8;
        if (frame.screen && layoutOf(frame.screen->GetDesc().Format, screenLayout)) {
            slot.screen.layout = screenLayout;
            // Without memory for it the frame is still saved, without the screen picture.
            static_cast<void>(copyInto(list, device.Get(), frame.screen, 0, 0, slot.screen));
        }
    }
    pending_ = true;
    return true;
}

void PushPipelineProbe::submitted(ID3D12Fence* fence, uint64_t value) {
    std::lock_guard lock(m_);
    if (!pending_) return;
    pending_ = false;
    if (!fence || filled_ >= 2) return;   // no fence: when the copy is done cannot be known; the frame is taken again
    slots_[filled_].fence = fence;
    slots_[filled_].fenceValue = value;
    ++filled_;
}

void PushPipelineProbe::discarded() {
    std::lock_guard lock(m_);
    pending_ = false;
}

bool PushPipelineProbe::reduce(const Copy& copy, uint32_t hdrMode, bool raw, bool rawSdrInHdr, PushLumaImage& out) {
    if (!copy.valid || !copy.buffer || !copy.width || !copy.height) return false;
    void* mapped = nullptr;
    // The last row has no padding in the buffer: never name a range past its end.
    const D3D12_RANGE range{0, SIZE_T(std::min<uint64_t>(copy.bytes, copy.footprint.Offset + uint64_t(copy.footprint.Footprint.RowPitch) * copy.height))};
    if (FAILED(copy.buffer->Map(0, &range, &mapped)) || !mapped) return false;
    const auto* data = static_cast<const uint8_t*>(mapped) + copy.footprint.Offset;
    const size_t pitch = copy.footprint.Footprint.RowPitch;
    const int width = static_cast<int>(copy.width), height = static_cast<int>(copy.height);
    bool ok = false;
    switch (copy.layout) {
    case Layout::Rgba8: ok = pushLumaFromRgba8(data, pitch, width, height, false, out); break;
    case Layout::Bgra8: ok = pushLumaFromRgba8(data, pitch, width, height, true, out); break;
    case Layout::Rgb10a2: ok = pushLumaFromRgb10a2(data, pitch, width, height, out); break;
    case Layout::Rgba16f:
        // scRGB units (1 = 80 nits). An SDR source shown in HDR sits at the 203-nit white, as the comparison view shows it.
        ok = pushLumaFromRgba16f(data, pitch, width, height, hdrMode != 0, raw && rawSdrInHdr ? 203.0f / 80.0f : 1.0f, out);
        break;
    }
    const D3D12_RANGE none{0, 0};
    copy.buffer->Unmap(0, &none);
    return ok;
}

void PushPipelineProbe::reducePair(Slot first, Slot second, std::vector<PushRectN> hud, uint64_t revision) {
    const Slot pair[2] = {std::move(first), std::move(second)};
    PushLumaImage output[2], raw[2];
    bool haveOutput[2] = {false, false}, haveRaw[2] = {false, false};
    PushQualityStats rawAdd, outputAdd;
    for (int i = 0; i < 2; ++i) {
        const bool hdr = pair[i].hdrMode != 0;
        haveOutput[i] = reduce(pair[i].output, pair[i].hdrMode, false, false, output[i]);
        haveRaw[i] = reduce(pair[i].raw, pair[i].hdrMode, true, pair[i].rawSdrInHdr, raw[i]);
        if (!haveOutput[i]) continue;
        // Without the raw picture the enlargement is unknown: the output is then measured as it is.
        const double scale = haveRaw[i] && raw[i].width > 0 ? double(output[i].width) / double(raw[i].width) : 1.0;
        addFrame(outputAdd, output[i], scale, hdr);
        if (haveRaw[i]) {
            addFrame(rawAdd, raw[i], 1.0, hdr);
            outputAdd.hudDelta.add(pushHudDelta(raw[i], output[i], hud.data(), hud.size()));
        }
    }
    if (haveOutput[0] && haveOutput[1]) outputAdd.flicker.add(pushFlicker(output[0], output[1]));
    if (pair[0].save) {
        const unsigned files = saveSlot(pair[0], revision);
        veyra::log::info("slipstream-quality", std::format("event=frames-saved probe={} index={} files={} pts100ns={} (clarity-frames in the data folder)",
            revision, pair[0].saveIndex, files, pair[0].pts100ns));
    }
    if (haveRaw[0] && haveRaw[1]) rawAdd.flicker.add(pushFlicker(raw[0], raw[1]));
    std::lock_guard lock(m_);
    reading_ = false;
    if (running_ && revision == revision_) {
        if (!haveOutput[0] || !haveOutput[1]) {
            failLocked("readback");
        } else {
            mergeStats(rawStats_, rawAdd);
            mergeStats(outputStats_, outputAdd);
            ++pairs_;
            filled_ = 0;
            retired_.clear();   // both copies of this pair are done, so is everything recorded before them
            nextDue_ = std::chrono::steady_clock::now() + interval_;
            if (pairs_ >= target_) finishLocked();
        }
    } else {
        filled_ = 0;
        releaseIfIdleLocked();
    }
}

namespace {
PushDumpPixels dumpPixelsOf(int layout) {
    return layout == 1 ? PushDumpPixels::Bgra8 : layout == 2 ? PushDumpPixels::Rgb10a2 : layout == 3 ? PushDumpPixels::Rgba16f : PushDumpPixels::Rgba8;
}
} // namespace

unsigned PushPipelineProbe::saveSlot(const Slot& slot, uint64_t revision) {
    std::error_code error;
    const auto folder = veyra::runtime::localDataDirectory() / L"clarity-frames";
    std::filesystem::create_directories(folder, error);
    if (error) return 0;
    PushFrameDumpInfo info;
    info.probe = revision; info.pair = slot.saveIndex; info.pts100ns = slot.pts100ns; info.hdrMode = slot.hdrMode; info.ditherStep = slot.ditherStep;
    info.presentFine = slot.presentFine;
    // one stamp per process (its first save), so the files of two app starts can never be taken for one run
    static const std::string stamp = pushDumpStamp(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    info.stamp = stamp;
    const std::string stem = std::format("{}-p{}-f{}", stamp, revision, slot.saveIndex);
    unsigned files = 0;
    const auto write = [&](const Copy& copy, const char* name, bool raw, PushFrameDumpPicture& picture) {
        if (!copy.valid || !copy.buffer || !copy.width || !copy.height) return;
        const PushDumpPixels pixels = dumpPixelsOf(static_cast<int>(copy.layout));
        void* mapped = nullptr;
        const D3D12_RANGE range{0, SIZE_T(std::min<uint64_t>(copy.bytes, copy.footprint.Offset + uint64_t(copy.footprint.Footprint.RowPitch) * copy.height))};
        if (FAILED(copy.buffer->Map(0, &range, &mapped)) || !mapped) return;
        std::vector<uint8_t> packed;
        const bool ok = pushPackRgb(static_cast<const uint8_t*>(mapped) + copy.footprint.Offset, copy.footprint.Footprint.RowPitch,
                                    copy.width, copy.height, pixels, packed);
        const D3D12_RANGE none{0, 0};
        copy.buffer->Unmap(0, &none);
        if (!ok) return;
        const std::string file = stem + "-" + name + ".npy";
        const std::string header = pushNpyHeader(pushNpyDescr(pixels), copy.height, copy.width);
        std::ofstream out(folder / file, std::ios::binary | std::ios::trunc);
        if (!out || header.empty()) return;
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
        if (!out) return;
        picture.file = file; picture.width = copy.width; picture.height = copy.height;
        picture.dtype = pushNpyDescr(pixels); picture.encoding = pushDumpEncoding(pixels, raw); picture.present = true;
        ++files;
    };
    write(slot.raw, "raw", true, info.raw);
    write(slot.output, "output", false, info.output);
    write(slot.screen, "screen", false, info.screen);
    if (files == 0) return 0;
    std::ofstream json(folder / (stem + ".json"), std::ios::binary | std::ios::trunc);
    const std::string text = pushFrameDumpJson(info);
    json.write(text.data(), static_cast<std::streamsize>(text.size()));
    return files;
}

PushPipelineProbe::Status PushPipelineProbe::status() {
    std::lock_guard lock(m_);
    const bool pairReady = running_ && !reading_ && filled_ == 2 && slots_[0].fence && slots_[1].fence &&
                           slots_[0].fence->GetCompletedValue() >= slots_[0].fenceValue &&
                           slots_[1].fence->GetCompletedValue() >= slots_[1].fenceValue;
    if (pairReady) {
        // The worker maps and measures the pair; the copies of the pointers keep the buffers alive meanwhile.
        // The previous reduction has ended (reading_ was false), so waiting for its future returns at once.
        if (work_.valid()) work_.wait();
        reading_ = true;
        try {
            work_ = std::async(std::launch::async, [this, first = slots_[0], second = slots_[1], hud = options_.hud, revision = revision_]() mutable {
                reducePair(std::move(first), std::move(second), std::move(hud), revision);
            });
        } catch (const std::system_error&) {
            reading_ = false;
            failLocked("thread");
        }
    } else if (running_ && !reading_ && std::chrono::steady_clock::now() >= deadline_) {
        // Out of time: what was measured is the result; nothing measured is a failure.
        if (pairs_ > 0) finishLocked();
        else failLocked("no-frames");
    } else {
        releaseIfIdleLocked();
    }
    Status out;
    out.running = running_; out.done = done_; out.failed = failed_; out.reason = reason_;
    out.pairs = pairs_; out.target = target_; out.revision = revision_;
    if (done_) { out.raw = rawStats_; out.output = outputStats_; }
    return out;
}

} // namespace veyra::engine
