// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// GPU half of pipeline/StreamChange.h: how much a decoded picture differs from the one before it,
// measured on the decoder's own texture (experiment VEYRA_EXP_RP_STREAM_DEDUP, off by default).
//
// StreamChangePass records one dispatch of shaders/PushStreamChange.hlsl into a command list and
// reads the eight result words back from a mapped readback buffer, the pattern EnhanceGraph uses
// for the frame-generation status (fgDisableReadback_). It keeps the previous picture's block
// means in a small R32_FLOAT texture and the previous picture's luma plane in a full-size one
// (R32_FLOAT: the one format every adapter can read and write through a typed UAV; 8 MB at 1080p,
// 33 MB at 4K), both rewritten by the same dispatch, so one picture costs one dispatch and the
// decoder's previous surface is never held back for the comparison.
//
// StreamChangeRunner owns a command list of its own and runs the pass on the graph's queue before
// the engine decides whether to keep the frame. That decision needs the number, so the calling
// thread waits for it (StreamChangeRunner::measure, the only wait here on the live path): for the
// budget the caller hands in, which is at most 4 ms and never past the engine's next presentation
// (streamChangeWaitBudget). The bound is a high-resolution waitable timer waited for together with
// the fence's event, as engine/DeadlineWait.h does: a millisecond timeout alone returns on the
// scheduler tick, up to 15.6 ms later. Without such a timer there is no wait at all. When the GPU
// does not answer in time the picture has no number and is kept, as without the experiment; the
// late answer is handed over with the next picture so the classifier does not lose its place.
// Nothing here exists unless measure() is called.
//
// Written without a Windows compiler or dxc at hand: unverified on Windows.
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/StreamChange.h"
#include <algorithm>
#include <bit>
#include <chrono>

namespace veyra::pipeline {

class StreamChangePass {
public:
    static constexpr UINT kConstants = 8;
    static constexpr UINT kWords = kStreamChangeWordCount;
    static constexpr UINT64 kBytes = kWords * sizeof(uint32_t);

    ~StreamChangePass() { release(); }

    bool initialize(ID3D12Device* device) {
        release();
        if (!device) return false;
        device_ = device;
        std::vector<uint8_t> cs;
        // Descriptors: 0 = luma plane (t0), 1 = block means (u0), 2 = result words (u1), 3 = previous luma (u2).
        if (!pass_.loadShader("PushStreamChange.dxil", cs) || !pass_.create(device, cs, 4, 1, 3, kConstants)) return false;
        if (!stager_.initialize(device, 4)) return false;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result_));
        if (FAILED(hr)) { veyra::log::error("stream-change", std::format("allocate result hr=0x{:X}", unsigned(hr))); return false; }
        hp.Type = D3D12_HEAP_TYPE_READBACK; bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
        if (FAILED(hr)) { veyra::log::error("stream-change", std::format("allocate readback hr=0x{:X}", unsigned(hr))); return false; }
        zero_ = makeUploadBuffer(device, kBytes);
        if (!zero_) return false;
        void* initial = nullptr;
        if (FAILED(zero_->Map(0, nullptr, &initial)) || !initial) return false;
        *static_cast<StreamChangeWords*>(initial) = StreamChangeWords{};
        zero_->Unmap(0, nullptr);
        // The readback heap stays mapped; the words are read directly once the list has finished.
        D3D12_RANGE range{0, SIZE_T(kBytes)}; void* data = nullptr;
        hr = readback_->Map(0, &range, &data);
        if (FAILED(hr) || !data) { veyra::log::error("stream-change", std::format("Map hr=0x{:X}", unsigned(hr))); return false; }
        mapped_ = static_cast<const volatile uint32_t*>(data);
        D3D12_UNORDERED_ACCESS_VIEW_DESC words{};
        words.Format = DXGI_FORMAT_R32_UINT; words.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; words.Buffer.NumElements = kWords;
        device->CreateUnorderedAccessView(result_.Get(), nullptr, &words, cpuHandleOf(pass_, 2));
        ready_ = true;
        return true;
    }

    // Records the measurement of one picture. texture: an NV12 / P010 decoder texture (plane 0 is
    // read) or a single-plane R8 / R16 texture, in the state `tracker` knows (COMMON when it has
    // never seen it), left in COMMON. slice: the array slice of a decoder texture array.
    // width, height: the visible picture; every sample inside it is read, nothing outside it.
    // The list recorded before this one must have finished: the pass has one set of buffers.
    bool record(ID3D12GraphicsCommandList* list, StateTracker& tracker, ID3D12Resource* texture, UINT slice, uint32_t width, uint32_t height) {
        if (!ready_ || !list || !texture) return false;
        const StreamChangeGrid grid = streamChangeBlocks(width, height);
        const D3D12_RESOURCE_DESC source = texture->GetDesc();
        // Also refuses a picture beyond 4096x2304: the 32-bit sums would not hold it (StreamChange.h).
        if (!streamChangeMeasurable(width, height) || source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.Width < width || source.Height < height) return false;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        if (source.Format == DXGI_FORMAT_NV12 || source.Format == DXGI_FORMAT_R8_UNORM) view = DXGI_FORMAT_R8_UNORM;
        else if (source.Format == DXGI_FORMAT_P010 || source.Format == DXGI_FORMAT_P016 || source.Format == DXGI_FORMAT_R16_UNORM) view = DXGI_FORMAT_R16_UNORM;
        else return false;
        if (!means_ || !before_ || width != width_ || height != height_) {
            // Another picture size: new stores, and nothing to compare the first picture with.
            width_ = height_ = 0; haveMeans_ = false;
            means_ = makeTexture(device_, grid.x, grid.y, DXGI_FORMAT_R32_FLOAT, true);
            before_ = makeTexture(device_, width, height, DXGI_FORMAT_R32_FLOAT, true);
            if (!means_ || !before_) { means_.Reset(); before_.Reset(); return false; }
            makeUav(device_, means_.Get(), DXGI_FORMAT_R32_FLOAT, cpuHandleOf(pass_, 1));
            makeUav(device_, before_.Get(), DXGI_FORMAT_R32_FLOAT, cpuHandleOf(pass_, 3));
            width_ = width; height_ = height;
        }
        // The same view of plane 0 the graph's own import uses (EnhanceGraph::process).
        const bool isArray = source.DepthOrArraySize > 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC lumaSrv{};
        lumaSrv.Format = view;
        lumaSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (isArray) {
            lumaSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            lumaSrv.Texture2DArray.MostDetailedMip = 0;
            lumaSrv.Texture2DArray.MipLevels = 1;
            lumaSrv.Texture2DArray.FirstArraySlice = slice;
            lumaSrv.Texture2DArray.ArraySize = 1;
            lumaSrv.Texture2DArray.PlaneSlice = 0;
        } else {
            lumaSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            lumaSrv.Texture2D.MostDetailedMip = 0;
            lumaSrv.Texture2D.MipLevels = 1;
            lumaSrv.Texture2D.PlaneSlice = 0;
        }
        stager_.stageSrv(texture, &lumaSrv, pass_.heap.Get(), 0);
        float c[kConstants]{};
        c[0] = std::bit_cast<float>(uint32_t(grid.x)); c[1] = std::bit_cast<float>(uint32_t(grid.y));
        c[2] = std::bit_cast<float>(uint32_t(haveMeans_ ? 1 : 0)); c[3] = std::bit_cast<float>(uint32_t(width));
        c[4] = std::bit_cast<float>(uint32_t(height)); c[5] = kStreamChangePixelLimit;
        tracker.transition(list, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tracker.transition(list, result_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(result_.Get(), 0, zero_.Get(), 0, kBytes);
        tracker.transition(list, result_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        tracker.transition(list, means_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        tracker.transition(list, before_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pass_.bind(list, c, gpuHandleOf(pass_, 0).ptr, gpuHandleOf(pass_, 1).ptr);
        list->Dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, 1);
        tracker.uavBarrier(list, means_.Get());
        tracker.uavBarrier(list, before_.Get());
        tracker.uavBarrier(list, result_.Get());
        tracker.transition(list, result_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback_.Get(), 0, result_.Get(), 0, kBytes);
        tracker.transition(list, result_.Get(), D3D12_RESOURCE_STATE_COMMON);
        tracker.transition(list, means_.Get(), D3D12_RESOURCE_STATE_COMMON);
        tracker.transition(list, before_.Get(), D3D12_RESOURCE_STATE_COMMON);
        tracker.transition(list, texture, D3D12_RESOURCE_STATE_COMMON);
        recordedWidth_ = width; recordedHeight_ = height; recordedCompare_ = haveMeans_; haveMeans_ = true;
        return true;
    }

    // The measurement of the picture recorded last. Only after its list has finished on the GPU.
    StreamChangeMeasurement read() const {
        if (!mapped_ || !recordedWidth_ || !recordedHeight_) return {};
        StreamChangeWords words;
        words.maxFixed = mapped_[0]; words.sumFixed = mapped_[1]; words.changed = mapped_[2]; words.blocks = mapped_[3];
        words.pixelMaxFixed = mapped_[4]; words.pixelsOver = mapped_[5]; words.pixels = mapped_[6];
        return decodeStreamChange(words, recordedWidth_, recordedHeight_, recordedCompare_);
    }

    // The next picture is not compared with the stored means (a picture in between was not measured).
    void restart() { haveMeans_ = false; }
    bool ready() const { return ready_; }

    void release() {
        if (mapped_ && readback_) { D3D12_RANGE written{0, 0}; readback_->Unmap(0, &written); }
        mapped_ = nullptr; ready_ = false; haveMeans_ = false; recordedCompare_ = false; recordedWidth_ = recordedHeight_ = 0; width_ = height_ = 0;
        means_.Reset(); before_.Reset(); result_.Reset(); readback_.Reset(); zero_.Reset();
        pass_ = {}; stager_ = {}; device_ = nullptr;
    }

private:
    ID3D12Device* device_ = nullptr;
    ComputePass pass_;
    DescriptorStager stager_;
    ComPtr<ID3D12Resource> means_, before_, result_, readback_, zero_;
    const volatile uint32_t* mapped_ = nullptr;
    uint32_t width_ = 0, height_ = 0, recordedWidth_ = 0, recordedHeight_ = 0;
    bool ready_ = false, haveMeans_ = false, recordedCompare_ = false;
};

class StreamChangeRunner {
public:
    // The longest the calling thread waits for one picture's measurement, in milliseconds.
    static constexpr unsigned kWaitMs = unsigned(kStreamChangeWait100ns / 10000);
    struct Counters { uint64_t measured = 0, timeouts = 0, skipped = 0, late = 0, failures = 0; double waitMsMax = 0, waitMsSum = 0; };

    ~StreamChangeRunner() { shutdown(); }

    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
        shutdown();
        if (!device || !queue) return false;
        const auto type = queue->GetDesc().Type;
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
        if (!pass_.initialize(device)) return false;
        if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator_)))) return false;
        if (FAILED(device->CreateCommandList(0, type, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_)))) return false;
        // Command lists are created in the recording state; close it so measure() can reset it.
        if (FAILED(list_->Close())) return false;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) return false;
        // The same timer engine/DeadlineWait.h uses; no global timer resolution change. Without it
        // (Windows before 10 1803) a wait could only be bounded by the scheduler tick, so there is none.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) veyra::log::warn("stream-change", std::format("high resolution wait timer unavailable (error={}): measurements are not waited for and arrive one picture late", unsigned(GetLastError())));
        ready_ = true;
        veyra::log::info("stream-change", std::format("created: 16x16 block means and every sample of the decoded luma plane against the previous picture, 32-byte readback, wait <= {} ms per picture and never past the next presentation", kWaitMs));
        return true;
    }

    // True while the measurement submitted last has not finished (it missed its wait).
    bool pending() const { return pending_ && fence_ && fence() == StreamChangeFence::Running; }

    // True while a list that was submitted may still read the texture it was given: not seen to
    // finish yet (also after an error that leaves that unknown). The owner of the texture must
    // not release or reuse it before this is false.
    bool reading() const { return reading_ && fence_ && fence() != StreamChangeFence::Done; }

    // Measures `texture` on `queue` and waits for the result: at most wait100ns (100 ns units; the
    // engine passes streamChangeWaitBudget), never more than kWaitMs, not at all for 0.
    // decoded / decodedValue: the decoder's fence for this picture (the GPU waits for it, as in
    // EnhanceGraph::process); may be null.
    StreamChangeOutcome measure(ID3D12CommandQueue* queue, ID3D12Resource* texture, UINT slice, uint32_t width, uint32_t height,
                                ID3D12Fence* decoded, uint64_t decodedValue, int64_t wait100ns) {
        StreamChangeOutcome out;
        if (!ready_ || !queue || !texture) return out;
        if (pending_) {
            const StreamChangeFence earlier = fence();
            if (earlier == StreamChangeFence::Lost) return lost(out);
            if (earlier == StreamChangeFence::Running) {
                // Still running: this picture is not measured either, and the next one has nothing to be compared with.
                ++counters_.skipped; skippedOne_ = true; out.timedOut = true;
                return out;
            }
            pending_ = false; reading_ = false; ++counters_.late;
            out.hasLate = true; out.late = pass_.read();
        }
        if (skippedOne_) { pass_.restart(); skippedOne_ = false; }
        const auto start = std::chrono::steady_clock::now();
        if (decoded && FAILED(queue->Wait(decoded, decodedValue))) return failed(out, "decoder fence wait");
        if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) return failed(out, "command list reset");
        const bool recorded = pass_.record(list_.Get(), tracker_, texture, slice, width, height);
        if (FAILED(list_->Close())) return failed(out, "command list close");
        if (!recorded) { ++counters_.failures; pass_.restart(); return out; }
        ID3D12CommandList* lists[] = {list_.Get()};
        queue->ExecuteCommandLists(1, lists);
        // From here the GPU may read `texture` until the fence reaches value_; every way out below
        // says so (out.reading) unless the list was seen to finish.
        reading_ = true;
        if (FAILED(queue->Signal(fence_.Get(), ++value_))) { out.reading = true; return failed(out, "fence signal"); }
        // The engine cannot decide about this frame without the number: a bounded wait. The timer
        // is the bound; the event ends the wait as soon as the GPU is done. The millisecond timeout
        // is only a net under a timer that does not fire.
        const int64_t budget = streamChangeWaitClamp(wait100ns);
        const auto waitStart = std::chrono::steady_clock::now();
        if (budget > 0 && timer_ && fence() == StreamChangeFence::Running && SUCCEEDED(fence_->SetEventOnCompletion(value_, event_))) {
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                const int64_t left = budget - std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count() / 100;
                if (left <= 0) break;
                LARGE_INTEGER due; due.QuadPart = -static_cast<LONGLONG>(left);
                if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) break;
                HANDLE handles[2] = {event_, timer_};
                const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(left / 10000) + 2);
                // Not the event: the time is up. The event with the fence still running: it was left
                // set by an earlier, late completion; wait once more for this one.
                if (woke != WAIT_OBJECT_0 || fence() != StreamChangeFence::Running) break;
            }
        }
        out.waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        counters_.waitMsSum += out.waitMs; counters_.waitMsMax = (std::max)(counters_.waitMsMax, out.waitMs);
        const StreamChangeFence state = fence();
        // A removed device reports every fence as UINT64_MAX: not a completion, the words are not this picture's.
        if (state == StreamChangeFence::Lost) { out = lost(out); out.reading = true; return out; }
        if (state == StreamChangeFence::Running) { pending_ = true; ++counters_.timeouts; out.timedOut = true; out.queued = true; out.reading = true; return out; }
        reading_ = false;
        out.now = pass_.read();
        if (out.now.valid) ++counters_.measured; else { ++counters_.failures; pass_.restart(); }
        return out;
    }

    const Counters& counters() const { return counters_; }
    bool ready() const { return ready_; }

    // Waits (bounded) for the last list before its buffers go away.
    void shutdown() {
        if (fence_ && event_ && value_ && fence() == StreamChangeFence::Running && SUCCEEDED(fence_->SetEventOnCompletion(value_, event_)))
            (void)WaitForSingleObject(event_, 2000);
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
        list_.Reset(); allocator_.Reset(); fence_.Reset();
        pass_.release(); tracker_ = {};
        ready_ = false; pending_ = false; reading_ = false; skippedOne_ = false; value_ = 0;
    }

private:
    StreamChangeFence fence() const { return streamChangeFence(fence_->GetCompletedValue(), value_); }
    StreamChangeOutcome lost(StreamChangeOutcome out) {
        pending_ = false; out.hasLate = false; out.late = {}; out.timedOut = false; out.queued = false;
        return failed(out, "the device was removed (fence reports UINT64_MAX): measurement");
    }
    StreamChangeOutcome failed(StreamChangeOutcome out, const char* what) {
        // A device or list error does not go away: stop measuring, pictures are kept unnumbered.
        ++counters_.failures; ready_ = false; out.now = {};
        veyra::log::warn("stream-change", std::format("{} failed; the measurement is off until the graph is rebuilt", what));
        return out;
    }

    StreamChangePass pass_;
    StateTracker tracker_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr, timer_ = nullptr;
    uint64_t value_ = 0;
    Counters counters_;
    bool ready_ = false, pending_ = false, reading_ = false, skippedOne_ = false;
};

} // namespace veyra::pipeline
