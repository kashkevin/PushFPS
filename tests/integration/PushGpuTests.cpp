// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS GPU pieces on a real D3D12 device (hardware, or WARP on build
// machines without a graphics card). VEYRA_PUSH_GPU_DEBUG=1 enables the D3D12
// debug layer (when installed) and fails on its errors.
//  - RecordingCompare (before/after comparison clips): a real frame is held,
//    a generated frame gets the held real frame in its left half and keeps its
//    own right half, the divider and both labels land where expected in luma
//    and neutral chroma, a later real frame replaces the hold; an HDR clip
//    splits without overlays.
//  - PushHudSampler (automatic HUD detection): copies only real frames, one at
//    a time, at the interval; finds a static HUD box on a moving picture end to
//    end; refuses a format it cannot read.
//  - PushHudLock (Live HUD Lock): after a second of a still HUD over a moving
//    picture, a generated frame takes the HUD pixels from the held real frame
//    and keeps its own everywhere else; a HUD that disappears is released on
//    that frame; a reset starts over.
//  - StreamChangePass / StreamChangeRunner (experiment VEYRA_EXP_RP_STREAM_DEDUP): the block-mean
//    and per-sample change of a luma plane against the previous picture equals the CPU reference
//    (pipeline/StreamChange.h) for 8-bit and 16-bit planes, includes the smaller blocks at the
//    edges, sees an object that moves inside one block, reports "not comparable" for the first
//    picture, after a restart and after a size change; the runner with its own command list gives
//    the same numbers, also when an answer arrives late or is not waited for.
//  - PushFlow (PushFPS's own frame generation, RIFE on DirectML): when VEYRA_PUSHFLOW_TEST_RUNTIME names a
//    folder holding pushflow\ (the release's runtime), two identical frames give the same picture back,
//    and a square that moves 16 px lands near the middle at t = 0.5, far closer than a blend of the two
//    frames, in both tiers' models. Without the variable the test is skipped (and says so).
//
// Output for CI without a readable log: every failed check is one line
//   FAIL <test>: <what> got=<..> want=<..>
// (the published CI repeats lines starting with FAIL as annotations), every test prints
// `TIME <test> <ms> ms`, the adapter line says hardware or software, and each run ends with
// `SUMMARY run <i>/<n>: ...` naming the failed tests. stdout is unbuffered, so a crash or a hang
// still shows the last `RUN <test>`; an unhandled exception prints `FAIL <test>: crashed`, and a
// test still running after --test-timeout seconds (default 1800) prints `FAIL <test>: timed out`
// and ends the program.
//   veyra_push_gpu_tests                         every test once (as before)
//   veyra_push_gpu_tests --repeat 20             the whole list 20 times (intermittent failures)
//   veyra_push_gpu_tests --only hud-sampler,stream-change --repeat 50
//   veyra_push_gpu_tests --list                  the test names
#include "veyra/pushflow/PushFlowBackend.h"
#include "veyra/engine/PushHudSampler.h"
#include "veyra/engine/PushPipelineProbe.h"
#include "veyra/pipeline/PushHudLock.h"
#include "veyra/pipeline/StreamChangePass.h"
#include "veyra/engine/RecordingCompare.h"
#include "veyra/engine/PushStreamOverlay.h"
#include "veyra/engine/PushCanvas.h"
#include "veyra/pipeline/GpuPassUtils.h"

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <format>
#include <functional>
#include <vector>

using namespace veyra;
using Microsoft::WRL::ComPtr;

namespace {
int failures = 0;
// The test running now (read by the crash and time-out reporters, so a plain pointer to a literal).
std::atomic<const char*> currentTest{"setup"};
std::vector<std::string> failedThisRun;
void noteFailure() {
    ++failures;
    const std::string name = currentTest.load();
    if (std::find(failedThisRun.begin(), failedThisRun.end(), name) == failedThisRun.end()) failedThisRun.push_back(name);
}
// One line per check; a failure names the test, what was checked, what was seen and what was wanted.
void check(bool ok, const std::string& label, const std::string& got = "false", const std::string& want = "true") {
    if (ok) { std::printf("PASS %s\n", label.c_str()); return; }
    std::printf("FAIL %s: %s got=%s want=%s\n", currentTest.load(), label.c_str(), got.c_str(), want.c_str());
    noteFailure();
}
template <class T> std::string shown(const T& v) {
    if constexpr (std::is_floating_point_v<T>) return std::format("{:.4f}", double(v));
    else return std::format("{}", v);
}
// check() with the values: ok must be the comparison of `got` against `want` the caller made.
template <class G, class W> void checkValue(bool ok, const std::string& label, const G& got, const W& want) { check(ok, label, shown(got), shown(want)); }

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t value = 0;
    bool debug = false;
    bool software = false;   // WARP or another software adapter: slow, and timing checks have little margin
    std::string adapter;

    bool create() {
        if (GetEnvironmentVariableW(L"VEYRA_PUSH_GPU_DEBUG", nullptr, 0) > 0) {
            ComPtr<ID3D12Debug> d;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) { d->EnableDebugLayer(); debug = true; }
            else std::printf("NOTE D3D12 debug layer not installed; running without it\n");
        }
        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        for (UINT i = 0; !device; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) adapter = narrow(d.Description);
        }
        if (!device) {
            ComPtr<IDXGIAdapter> warp;
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
                FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
            adapter = "WARP";
            software = true;
        }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
               (event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
    }
    static std::string narrow(const wchar_t* w) {
        std::string s; for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?'); return s;
    }
    bool begin() { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); }
    bool submit() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        return SUCCEEDED(queue->Signal(fence.Get(), ++value));
    }
    bool wait() {
        DWORD waited = WAIT_OBJECT_0;
        if (fence->GetCompletedValue() < value) { fence->SetEventOnCompletion(value, event); waited = WaitForSingleObject(event, 60000); }
        const uint64_t completed = fence->GetCompletedValue();
        const HRESULT removed = device->GetDeviceRemovedReason();
        // Say why a submission did not finish: the check that follows only says that it did not.
        if (completed < value || FAILED(removed))
            std::printf("NOTE %s: GPU wait ended with fence %llu of %llu (wait result %lu), device state 0x%08X%s\n", currentTest.load(),
                        static_cast<unsigned long long>(completed), static_cast<unsigned long long>(value), static_cast<unsigned long>(waited),
                        static_cast<unsigned>(removed), waited == WAIT_TIMEOUT ? " (timed out after 60 s)" : "");
        return completed >= value && SUCCEEDED(removed);
    }
    bool submitAndWait() { return submit() && wait(); }
    bool debugErrors() {
        if (!debug) return false;
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device.As(&info))) return false;
        bool errors = false;
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
            std::vector<char> buffer(size); auto* m = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(info->GetMessage(i, m, &size)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("NOTE %s: D3D12 debug layer error: %s\n", currentTest.load(), m->pDescription); errors = true;
            }
        }
        info->ClearStoredMessages();
        return errors;
    }
};

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

// A texture plane: bytes per pixel `bpp`, filled or read through a buffer.
struct Plane {
    ComPtr<ID3D12Resource> texture;
    DXGI_FORMAT format;
    uint32_t w, h, bpp;
    UINT pitch() const { return (w * bpp + 255) & ~255u; }
};

// Records an upload of `pixels` (tight rows) into the plane; the texture ends in `after`.
ComPtr<ID3D12Resource> recordFill(Gpu& gpu, const Plane& p, const std::vector<uint8_t>& pixels, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    auto upload = makeBuffer(gpu.device.Get(), uint64_t(p.pitch()) * p.h, D3D12_HEAP_TYPE_UPLOAD);
    if (!upload) return nullptr;
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t y = 0; y < p.h; ++y) std::memcpy(mapped + size_t(y) * p.pitch(), pixels.data() + size_t(y) * p.w * p.bpp, size_t(p.w) * p.bpp);
    upload->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = p.texture.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = before; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; gpu.list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {p.format, p.w, p.h, 1, p.pitch()};
    to.pResource = p.texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    if (after != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = after; gpu.list->ResourceBarrier(1, &b);
    }
    return upload;
}

// Records a readback of the plane (currently in `state`, left there) into a new buffer.
ComPtr<ID3D12Resource> recordRead(Gpu& gpu, const Plane& p, D3D12_RESOURCE_STATES state) {
    auto readback = makeBuffer(gpu.device.Get(), uint64_t(p.pitch()) * p.h, D3D12_HEAP_TYPE_READBACK);
    if (!readback) { std::printf("NOTE %s: readback buffer of %u x %u not created\n", currentTest.load(), p.w, p.h); return nullptr; }
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = p.texture.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = state; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; gpu.list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = p.texture.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = {p.format, p.w, p.h, 1, p.pitch()};
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); gpu.list->ResourceBarrier(1, &b);
    }
    return readback;
}

std::vector<uint8_t> mapRead(const Plane& p, ID3D12Resource* readback) {
    if (!readback) return {};
    std::vector<uint8_t> out(size_t(p.w) * p.h * p.bpp);
    uint8_t* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped) return {};
    for (uint32_t y = 0; y < p.h; ++y) std::memcpy(out.data() + size_t(y) * p.w * p.bpp, mapped + size_t(y) * p.pitch(), size_t(p.w) * p.bpp);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return out;
}

void testCompare(Gpu& gpu) {
    constexpr uint32_t W = 256, H = 144;
    auto* device = gpu.device.Get();
    Plane y{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R8_UNORM, true), DXGI_FORMAT_R8_UNORM, W, H, 1};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, DXGI_FORMAT_R8G8_UNORM, true), DXGI_FORMAT_R8G8_UNORM, W / 2, H / 2, 2};
    check(y.texture && uv.texture, "compare: planes created");
    engine::RecordingLabel left{8, 4, std::vector<uint8_t>(32, 255)}, right{9, 5, std::vector<uint8_t>(45, 0)};
    engine::RecordingCompare compare;
    std::wstring reason;
    check(compare.create(device, W, H, false, left, right, reason), "compare: created");
    check(compare.labelled() && compare.patches().size() == 3, "compare: divider and two labels");
    check(compare.splitX() == 128, "compare: split in the middle");

    pipeline::StateTracker states;
    auto frame = [&](uint8_t luma, uint8_t chroma, bool generated, std::vector<uint8_t>* yOut, std::vector<uint8_t>* uvOut) {
        gpu.begin();
        // The recorder's planes are UAVs written by the conversion; fill them like it.
        auto a = recordFill(gpu, y, std::vector<uint8_t>(W * H, luma), states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto b = recordFill(gpu, uv, std::vector<uint8_t>(W / 2 * H / 2 * 2, chroma), states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compare.compose(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), generated);
        ComPtr<ID3D12Resource> ry, ruv;
        if (yOut) { ry = recordRead(gpu, y, states.get(y.texture.Get())); ruv = recordRead(gpu, uv, states.get(uv.texture.Get())); }
        const bool ok = gpu.submitAndWait();
        if (yOut && ok) { *yOut = mapRead(y, ry.Get()); *uvOut = mapRead(uv, ruv.Get()); }
        return ok;
    };
    std::vector<uint8_t> py, puv;
    check(frame(50, 100, false, &py, &puv), "compare: real frame runs");
    check(py.size() == W * H && py[60 * W + 2] == 50 && py[60 * W + 200] == 50, "compare: a real frame stays whole");
    check(frame(200, 150, true, &py, &puv), "compare: generated frame runs");
    const auto Y = [&](uint32_t x, uint32_t yy) { return int(py[size_t(yy) * W + x]); };
    const auto U = [&](uint32_t x, uint32_t yy) { return int(puv[(size_t(yy) * (W / 2) + x) * 2]); };
    check(Y(2, 60) == 50 && Y(100, 120) == 50, "compare: left half holds the real frame");
    check(Y(200, 60) == 200 && Y(250, 140) == 200, "compare: right half is the generated frame");
    check(Y(126, 60) == 235 && Y(129, 60) == 235 && Y(125, 60) == 50 && Y(130, 60) == 200, "compare: divider is four white pixels at the split");
    check(Y(10, 10) == 235 && Y(8, 8) == 235 && Y(15, 11) == 235 && Y(16, 10) == 50, "compare: left label (white) at the top left");
    check(Y(240, 8) == 16 && Y(247, 11) == 16 && Y(248, 10) == 200 && Y(239, 10) == 200, "compare: right label (black, odd size trimmed) at the top right");
    check(U(1, 30) == 100 && U(100, 30) == 150 && U(5, 5) == 128 && U(64, 30) == 128, "compare: chroma follows and overlays are neutral");
    check(compare.generatedFrames() == 1 && compare.realFrames() == 1, "compare: counts frames");
    check(frame(90, 120, false, &py, &puv), "compare: next real frame runs");
    check(Y(2, 60) == 90 && Y(200, 60) == 90, "compare: a new real frame replaces the hold");
    check(frame(10, 10, true, &py, &puv) && Y(2, 60) == 90 && Y(200, 60) == 10, "compare: the next generated frame uses the new hold");
    check(!gpu.debugErrors(), "compare: no D3D12 debug errors");

    engine::RecordingLabel huge{200, 100, std::vector<uint8_t>(20000, 255)};
    engine::RecordingCompare tooBig;
    check(tooBig.create(device, W, H, false, huge, huge, reason) && tooBig.patches().size() == 3, "compare: labels too large are scaled down, not left out");
    if (tooBig.patches().size() == 3) {
        const auto& l = tooBig.patches()[1];
        check(l.width <= 128 - 2 * 8 - 4 && l.height <= H / 3 - 16 && l.width >= 8, "compare: the scaled label fits its half");
        check(tooBig.patches()[2].x + tooBig.patches()[2].width <= W - 8, "compare: the scaled right label stays inside the picture");
    }
    engine::RecordingLabel nothing{};
    engine::RecordingCompare unlabelled;
    check(unlabelled.create(device, W, H, false, nothing, nothing, reason) && unlabelled.patches().size() == 1, "compare: no label text keeps only the divider");
}

// One comparison frame on planes of any format: fill y and uv with the given bytes,
// compose, read both back (tight rows).
bool composeFrame(Gpu& gpu, engine::RecordingCompare& compare, pipeline::StateTracker& states, const Plane& y, const Plane& uv,
                  const std::vector<uint8_t>& yData, const std::vector<uint8_t>& uvData, bool generated,
                  std::vector<uint8_t>& yOut, std::vector<uint8_t>& uvOut) {
    if (!gpu.begin()) return false;
    auto a = recordFill(gpu, y, yData, states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto b = recordFill(gpu, uv, uvData, states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    compare.compose(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), generated);
    auto ry = recordRead(gpu, y, states.get(y.texture.Get()));
    auto ruv = recordRead(gpu, uv, states.get(uv.texture.Get()));
    if (!gpu.submitAndWait()) return false;
    yOut = mapRead(y, ry.Get()); uvOut = mapRead(uv, ruv.Get());
    return yOut.size() == size_t(y.w) * y.h * y.bpp;
}
std::vector<uint8_t> words(const std::vector<uint16_t>& v) {
    std::vector<uint8_t> out(v.size() * 2);
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}
uint16_t word(const std::vector<uint8_t>& bytes, size_t index) { uint16_t v; std::memcpy(&v, bytes.data() + index * 2, 2); return v; }

void testCompareHdr(Gpu& gpu) {
    // P010: 10-bit codes in the top bits. White text and the divider at code 573 (203 nits).
    constexpr uint32_t W = 256, H = 144;
    auto* device = gpu.device.Get();
    Plane y{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R16_UNORM, true), DXGI_FORMAT_R16_UNORM, W, H, 2};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, DXGI_FORMAT_R16G16_UNORM, true), DXGI_FORMAT_R16G16_UNORM, W / 2, H / 2, 4};
    check(y.texture && uv.texture, "compare HDR: planes created");
    engine::RecordingLabel left{8, 4, std::vector<uint8_t>(32, 255)}, right{8, 4, std::vector<uint8_t>(32, 0)};
    engine::RecordingCompare compare;
    std::wstring reason;
    check(compare.create(device, W, H, true, left, right, reason), "compare HDR: created");
    check(compare.labelled() && compare.patches().size() == 3, "compare HDR: divider and both labels (they used to be left out)");
    pipeline::StateTracker states;
    std::vector<uint8_t> py, puv;
    const uint16_t real = 300 << 6, gen = 700 << 6, chroma = 600 << 6;
    check(composeFrame(gpu, compare, states, y, uv, words(std::vector<uint16_t>(W * H, real)), words(std::vector<uint16_t>(W / 2 * H / 2 * 2, chroma)), false, py, puv), "compare HDR: real frame runs");
    check(composeFrame(gpu, compare, states, y, uv, words(std::vector<uint16_t>(W * H, gen)), words(std::vector<uint16_t>(W / 2 * H / 2 * 2, chroma)), true, py, puv), "compare HDR: generated frame runs");
    const auto Y = [&](uint32_t x, uint32_t yy) { return int(word(py, size_t(yy) * W + x)); };
    check(Y(2, 60) == real && Y(200, 60) == gen, "compare HDR: split holds the real frame on the left");
    check(Y(127, 60) == (573 << 6), "compare HDR: divider at 203 nits");
    check(Y(10, 10) == (573 << 6), "compare HDR: white label text at 203 nits");
    check(Y(240, 9) == (64 << 6), "compare HDR: black label box at code 64");
    check(int(word(puv, size_t(5) * (W / 2) * 2 + 5 * 2)) == (512 << 6), "compare HDR: overlay chroma is neutral");
    check(!gpu.debugErrors(), "compare HDR: no D3D12 debug errors");
}

void testCompareSideBySide(Gpu& gpu, bool hdr) {
    constexpr uint32_t W = 256, H = 144;
    const char* tag = hdr ? "side by side HDR" : "side by side";
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    engine::RecordingLabel left{8, 4, std::vector<uint8_t>(32, 255)}, right{8, 4, std::vector<uint8_t>(32, 255)};
    engine::RecordingCompare compare;
    std::wstring reason;
    const bool created = compare.create(device, W, H, hdr, left, right, reason, engine::CompareLayout::SideBySide);
    if (!created) std::printf("%s: create failed: %ls\n", tag, reason.c_str());
    check(created, std::string(tag) + ": created (shader loaded)");
    if (!created) return;
    check(compare.pictureWidth() == 128 && compare.pictureHeight() == 72 && compare.pictureTop() == 36 && compare.rightX() == 128, std::string(tag) + ": two half-size pictures, centred");
    check(compare.patches().size() == 3, std::string(tag) + ": divider and two labels");
    if (compare.patches().size() == 3) {
        const auto& d = compare.patches()[0]; const auto& l = compare.patches()[1]; const auto& r = compare.patches()[2];
        check(d.y == 36 && d.height == 72, std::string(tag) + ": divider only between the pictures");
        check(l.y + l.height <= 36 && r.y + r.height <= 36, std::string(tag) + ": labels in the black bar above the pictures");
        check(l.x == 60 && r.x == 188, std::string(tag) + ": labels centred over their picture");
    }
    // Values: SDR in 8-bit codes, HDR in 10-bit codes (stored << 6).
    const auto code = [&](uint32_t v8, uint32_t v10) { return hdr ? v10 << 6 : v8; };
    const auto fill = [&](auto f, uint32_t w, uint32_t h, uint32_t channels) {
        std::vector<uint8_t> out(size_t(w) * h * channels * bpp);
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t x = 0; x < w; ++x)
                for (uint32_t c = 0; c < channels; ++c) {
                    const uint32_t v = f(x, yy);
                    const size_t at = (size_t(yy) * w + x) * channels + c;
                    if (hdr) { const uint16_t w16 = uint16_t(v); std::memcpy(out.data() + at * 2, &w16, 2); }
                    else out[at] = uint8_t(v);
                }
        return out;
    };
    const auto read = [&](const std::vector<uint8_t>& bytes, size_t index) { return hdr ? int(word(bytes, index)) : int(bytes[index]); };
    pipeline::StateTracker states;
    std::vector<uint8_t> py, puv;
    // Real frame: luma pairs of equal columns (so the 2x2 average is exact), chroma 100.
    const auto realY = fill([&](uint32_t x, uint32_t) { return code(x & ~1u, (x & ~1u) + 64); }, W, H, 1);
    const auto realUv = fill([&](uint32_t, uint32_t) { return code(100, 400); }, W / 2, H / 2, 2);
    check(composeFrame(gpu, compare, states, y, uv, realY, realUv, false, py, puv), std::string(tag) + ": real frame runs");
    const auto Y = [&](uint32_t x, uint32_t yy) { return read(py, size_t(yy) * W + x); };
    const auto U = [&](uint32_t x, uint32_t yy) { return read(puv, (size_t(yy) * (W / 2) + x) * 2); };
    check(Y(10, 41) == int(code(20, 84)), std::string(tag) + ": left picture is the frame at half size (2x2 average)");
    check(Y(138, 41) == int(code(20, 84)), std::string(tag) + ": a real frame shows on both sides");
    check(Y(5, 5) == int(code(16, 64)) && Y(5, 140) == int(code(16, 64)), std::string(tag) + ": black bars above and below");
    check(U(2, 2) == int(code(128, 512)) && U(10, 25) == int(code(100, 400)), std::string(tag) + ": chroma neutral in the bars, kept in the picture");
    check(Y(62, 17) == int(code(235, 573)), std::string(tag) + ": label text in the bar");
    check(Y(127, 60) == int(code(235, 573)) && Y(127, 20) == int(code(16, 64)), std::string(tag) + ": divider between the pictures only");
    // Generated frame: the right side changes, the left keeps the held real frame.
    const auto genY = fill([&](uint32_t, uint32_t) { return code(200, 800); }, W, H, 1);
    const auto genUv = fill([&](uint32_t, uint32_t) { return code(150, 600); }, W / 2, H / 2, 2);
    check(composeFrame(gpu, compare, states, y, uv, genY, genUv, true, py, puv), std::string(tag) + ": generated frame runs");
    check(Y(10, 41) == int(code(20, 84)), std::string(tag) + ": left keeps the real frame");
    check(Y(138, 41) == int(code(200, 800)) && U(70, 25) == int(code(150, 600)), std::string(tag) + ": right shows the generated frame");
    check(U(10, 25) == int(code(100, 400)), std::string(tag) + ": left chroma stays the real frame's");
    check(compare.generatedFrames() == 1 && compare.realFrames() == 1, std::string(tag) + ": counts frames");
    check(!gpu.debugErrors(), std::string(tag) + ": no D3D12 debug errors");
}

// Vertical (TikTok, Shorts, Reels): a 1080 x 1920 picture of its own, the console (held real frame)
// in the top half and the current frame in the bottom half, each the middle of the 1920 x 1080
// frame (1214 x 1080 from column 352) scaled to 1080 x 960; labels next to the divider.
void testCompareVertical(Gpu& gpu, bool hdr) {
    constexpr uint32_t W = 1920, H = 1080;
    const std::string tag = hdr ? "vertical HDR" : "vertical";
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    uint32_t ow = 0, oh = 0;
    engine::compareOutputSize(engine::CompareLayout::Vertical, W, H, ow, oh);
    check(ow == 1080 && oh == 1920, tag + ": 1080 x 1920 file");
    const auto crop = engine::compareVerticalCrop(W, H, 1080, 960);
    check(crop.width == 1214 && crop.height == 1080 && crop.x == 352 && crop.y == 0, tag + ": the middle of the frame, 9:8");
    const auto tall = engine::compareVerticalCrop(1080, 1920, 1080, 960);
    check(tall.width == 1080 && tall.height == 960 && tall.x == 0 && tall.y == 480, tag + ": a tall frame keeps its middle rows");
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    engine::RecordingLabel left{8, 4, std::vector<uint8_t>(32, 255)}, right{8, 4, std::vector<uint8_t>(32, 255)};
    engine::RecordingCompare compare;
    std::wstring reason;
    const bool created = compare.create(device, W, H, hdr, left, right, reason, engine::CompareLayout::Vertical);
    if (!created) std::printf("%s: create failed: %ls\n", tag.c_str(), reason.c_str());
    check(created, tag + ": created (shader loaded)");
    if (!created) return;
    check(compare.outputY() && compare.outputWidth() == 1080 && compare.outputHeight() == 1920 && compare.pictureHeight() == 960, tag + ": own output planes");
    check(compare.patches().size() == 3, tag + ": divider and two labels");
    if (compare.patches().size() == 3) {
        const auto& d = compare.patches()[0]; const auto& l = compare.patches()[1]; const auto& r = compare.patches()[2];
        check(d.x == 0 && d.y == 958 && d.width == 1080 && d.height == 4, tag + ": divider across the middle");
        check(l.y + l.height <= 958 && l.y + l.height + 80 > 958, tag + ": console label just above the divider");
        check(r.y >= 962 && r.y < 962 + 80, tag + ": PushFPS label just below the divider");
        check(l.x == 536 && r.x == 536, tag + ": labels centred");
    }
    const auto code = [&](uint32_t v8, uint32_t v10) { return hdr ? v10 << 6 : v8; };
    const auto fill = [&](auto f, uint32_t w, uint32_t h, uint32_t channels) {
        std::vector<uint8_t> out(size_t(w) * h * channels * bpp);
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t x = 0; x < w; ++x)
                for (uint32_t c = 0; c < channels; ++c) {
                    const uint32_t v = f(x, yy);
                    const size_t at = (size_t(yy) * w + x) * channels + c;
                    if (hdr) { const uint16_t w16 = uint16_t(v); std::memcpy(out.data() + at * 2, &w16, 2); }
                    else out[at] = uint8_t(v);
                }
        return out;
    };
    Plane oy{compare.outputY(), yFormat, 1080, 1920, bpp};
    Plane ouv{compare.outputUv(), uvFormat, 540, 960, bpp * 2};
    pipeline::StateTracker states;
    std::vector<uint8_t> py, puv;
    const auto frame = [&](const std::vector<uint8_t>& yData, const std::vector<uint8_t>& uvData, bool generated) {
        if (!gpu.begin()) return false;
        auto a = recordFill(gpu, y, yData, states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto b = recordFill(gpu, uv, uvData, states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compare.compose(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), generated);
        auto ry = recordRead(gpu, oy, states.get(oy.texture.Get()));
        auto ruv = recordRead(gpu, ouv, states.get(ouv.texture.Get()));
        if (!gpu.submitAndWait()) return false;
        py = mapRead(oy, ry.Get()); puv = mapRead(ouv, ruv.Get());
        return py.size() == size_t(1080) * 1920 * bpp;
    };
    const auto read = [&](const std::vector<uint8_t>& bytes, size_t index) { return hdr ? int(word(bytes, index)) : int(bytes[index]); };
    const auto Y = [&](uint32_t x, uint32_t yy) { return read(py, size_t(yy) * 1080 + x); };
    const auto U = [&](uint32_t x, uint32_t yy) { return read(puv, (size_t(yy) * 540 + x) * 2); };
    // Real frame: 120 inside the crop, 16 outside it (so a wrong crop shows), chroma 100.
    const auto realY = fill([&](uint32_t x, uint32_t) { return x >= 352 && x < 352 + 1214 ? code(120, 500) : code(16, 64); }, W, H, 1);
    const auto realUv = fill([&](uint32_t, uint32_t) { return code(100, 400); }, W / 2, H / 2, 2);
    check(frame(realY, realUv, false), tag + ": real frame runs");
    check(Y(2, 100) == int(code(120, 500)) && Y(1077, 100) == int(code(120, 500)), tag + ": the edges show the crop's edges, not the frame outside it");
    check(Y(540, 400) == int(code(120, 500)) && Y(540, 1500) == int(code(120, 500)), tag + ": a real frame shows in both halves");
    check(U(270, 200) == int(code(100, 400)) && U(270, 750) == int(code(100, 400)), tag + ": chroma kept");
    check(Y(10, 959) == int(code(235, 573)) && Y(10, 956) == int(code(120, 500)), tag + ": divider rows 958-961 only");
    // Generated frame: the bottom changes, the top keeps the held real frame.
    const auto genY = fill([&](uint32_t, uint32_t) { return code(200, 800); }, W, H, 1);
    const auto genUv = fill([&](uint32_t, uint32_t) { return code(150, 600); }, W / 2, H / 2, 2);
    check(frame(genY, genUv, true), tag + ": generated frame runs");
    check(Y(540, 400) == int(code(120, 500)) && U(270, 200) == int(code(100, 400)), tag + ": top keeps the console frame");
    check(Y(540, 1500) == int(code(200, 800)) && U(270, 750) == int(code(150, 600)), tag + ": bottom shows the generated frame");
    check(compare.generatedFrames() == 1 && compare.realFrames() == 1, tag + ": counts frames");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

// Streaming mode scenes (PushStreamOverlay.h): a transparent overlay with an opaque white corner box
// keeps the picture elsewhere; an opaque card replaces the whole picture.
void testStreamOverlay(Gpu& gpu, bool hdr) {
    constexpr uint32_t W = 128, H = 64;
    const std::string tag = hdr ? "scene overlay HDR" : "scene overlay";
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    const auto code = [&](uint32_t v8, uint32_t v10) { return hdr ? v10 << 6 : v8; };
    const auto plane = [&](uint32_t value, uint32_t w, uint32_t h, uint32_t channels) {
        std::vector<uint8_t> out(size_t(w) * h * channels * bpp);
        for (size_t i = 0; i < size_t(w) * h * channels; ++i) {
            if (hdr) { const uint16_t v = uint16_t(value); std::memcpy(out.data() + i * 2, &v, 2); } else out[i] = uint8_t(value);
        }
        return out;
    };
    const auto read = [&](const std::vector<uint8_t>& b, size_t i) { return hdr ? int(word(b, i)) : int(b[i]); };
    // Overlay: transparent, with an opaque white 16 x 16 box at the top left. Drawn at half size to
    // check that it is scaled to the frame.
    engine::PushOverlayImage halfSize{W / 2, H / 2, {}, std::vector<uint8_t>(size_t(W / 2) * (H / 2) * 4, 0)};
    for (uint32_t yy = 0; yy < 8; ++yy)
        for (uint32_t x = 0; x < 8; ++x) { uint8_t* p = &halfSize.rgba[(size_t(yy) * (W / 2) + x) * 4]; p[0] = p[1] = p[2] = p[3] = 255; }
    engine::FrameOverlay overlay;
    check(overlay.create(device, W, H, hdr, halfSize), tag + ": created (scaled to the frame)");
    pipeline::StateTracker states;
    const auto run = [&](engine::FrameOverlay& o, std::vector<uint8_t>& py, std::vector<uint8_t>& puv) {
        if (!gpu.begin()) return false;
        auto a = recordFill(gpu, y, plane(code(100, 400), W, H, 1), states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto b = recordFill(gpu, uv, plane(code(90, 300), W / 2, H / 2, 2), states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        o.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get());
        auto ry = recordRead(gpu, y, states.get(y.texture.Get()));
        auto ruv = recordRead(gpu, uv, states.get(uv.texture.Get()));
        if (!gpu.submitAndWait()) return false;
        py = mapRead(y, ry.Get()); puv = mapRead(uv, ruv.Get());
        return py.size() == size_t(W) * H * bpp;
    };
    std::vector<uint8_t> py, puv;
    check(run(overlay, py, puv), tag + ": runs");
    check(read(py, size_t(4) * W + 4) == int(code(235, 573)), tag + ": the box is white (203 nits in HDR)");
    check(read(py, size_t(40) * W + 90) == int(code(100, 400)), tag + ": the picture is kept where the overlay is clear");
    check(read(puv, (size_t(20) * (W / 2) + 40) * 2) == int(code(90, 300)), tag + ": chroma kept where clear");
    check(read(puv, (size_t(2) * (W / 2) + 2) * 2) == int(code(128, 512)), tag + ": white box chroma neutral");
    // A card: opaque black everywhere.
    engine::PushOverlayImage card{W, H, {}, std::vector<uint8_t>(size_t(W) * H * 4, 0)};
    for (size_t i = 0; i < size_t(W) * H; ++i) card.rgba[i * 4 + 3] = 255;
    engine::FrameOverlay cardOverlay;
    check(cardOverlay.create(device, W, H, hdr, card), tag + ": card created");
    check(run(cardOverlay, py, puv), tag + ": card runs");
    check(read(py, size_t(40) * W + 90) == int(code(16, 64)) && read(py, 0) == int(code(16, 64)), tag + ": a card replaces the whole picture");
    // Process-wide publication.
    const auto before = engine::pushStreamOverlayVersion();
    engine::setPushStreamOverlay(std::make_shared<engine::PushOverlayImage>(card));
    uint64_t version = 0;
    check(engine::pushStreamOverlay(version) != nullptr && version == before + 1, tag + ": published with a new version");
    engine::setPushStreamOverlay(nullptr);
    check(engine::pushStreamOverlay(version) == nullptr, tag + ": cleared");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

// PushFPS outputs and canvas (engine/PushCanvas.h, shaders/PushCanvasConvert.hlsl): the presented
// picture sampled into an output of another size and layout, as the recorder does for each output.
void testCanvasConvert(Gpu& gpu) {
    const std::string tag = "canvas convert";
    auto* device = gpu.device.Get();
    constexpr uint32_t SW = 128, SH = 72;
    Plane src{pipeline::makeTexture(device, SW, SH, DXGI_FORMAT_R32G32B32A32_FLOAT, false), DXGI_FORMAT_R32G32B32A32_FLOAT, SW, SH, 16};
    // White picture with a black left quarter (so a crop is visible).
    std::vector<uint8_t> pixels(size_t(SW) * SH * 16);
    for (uint32_t y = 0; y < SH; ++y)
        for (uint32_t x = 0; x < SW; ++x) {
            const float v = x < SW / 4 ? 0.0f : 1.0f;
            const float px[4] = {v, v, v, 1.0f};
            std::memcpy(pixels.data() + (size_t(y) * SW + x) * 16, px, 16);
        }
    if (!gpu.begin()) return;
    auto srcUpload = recordFill(gpu, src, pixels, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    check(gpu.submitAndWait(), tag + ": picture uploaded");
    pipeline::ComputePass pass;
    pipeline::DescriptorStager stager;
    std::vector<uint8_t> shader;
    check(pass.loadShader("PushCanvasConvert.dxil", shader) && pass.create(device, shader, 3, 1, 2, 16) && stager.initialize(device, 2), tag + ": shader loaded");
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
    stager.stageSrv(src.texture.Get(), &view, pass.heap.Get(), 0);
    // Runs one layout into a W x H output; returns the Y plane.
    const auto run = [&](const engine::PushCanvas& canvas, uint32_t W, uint32_t H, std::vector<uint8_t>& py) {
        Plane y{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R8_UNORM, true), DXGI_FORMAT_R8_UNORM, W, H, 1};
        Plane uv{pipeline::makeTexture(device, W / 2, H / 2, DXGI_FORMAT_R8G8_UNORM, true), DXGI_FORMAT_R8G8_UNORM, W / 2, H / 2, 2};
        if (!y.texture || !uv.texture) return false;
        pipeline::makeUav(device, y.texture.Get(), DXGI_FORMAT_R8_UNORM, pipeline::cpuHandleOf(pass, 1));
        pipeline::makeUav(device, uv.texture.Get(), DXGI_FORMAT_R8G8_UNORM, pipeline::cpuHandleOf(pass, 2));
        const auto map = engine::mapPushCanvas(canvas, SW, SH, W, H);
        const uint32_t taps = engine::pushCanvasTaps(map.srcW, map.dstW) | (engine::pushCanvasTaps(map.srcH, map.dstH) << 8);
        const float constants[16] = {std::bit_cast<float>(W), std::bit_cast<float>(H), 0.0f, 0.0f,
            float(map.dstX), float(map.dstY), float(map.dstW), float(map.dstH), float(map.srcX), float(map.srcY), float(map.srcW), float(map.srcH),
            std::bit_cast<float>(SW), std::bit_cast<float>(SH), std::bit_cast<float>(taps), std::bit_cast<float>(canvas.background)};
        if (!gpu.begin()) return false;
        D3D12_RESOURCE_BARRIER b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[i].Transition.pResource = i ? uv.texture.Get() : y.texture.Get();
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        gpu.list->ResourceBarrier(2, b);
        pass.bind(gpu.list.Get(), constants, pipeline::gpuHandleOf(pass, 0).ptr, pipeline::gpuHandleOf(pass, 1).ptr);
        gpu.list->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
        auto ry = recordRead(gpu, y, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!gpu.submitAndWait()) return false;
        py = mapRead(y, ry.Get());
        return py.size() == size_t(W) * H;
    };
    std::vector<uint8_t> py;
    // Same shape, half size (box filtered): white right of the black quarter, black in it.
    engine::PushCanvas full;
    check(run(full, 64, 36, py), tag + ": half size runs");
    check(py[size_t(18) * 64 + 40] == 235 && py[size_t(18) * 64 + 4] == 16, tag + ": half size keeps the picture");
    // 16:9 into 9:16, fit: picture in the middle band, black bars above and below.
    engine::PushCanvas fit; fit.fit = engine::PushCanvasFit::Fit;
    check(run(fit, 36, 64, py), tag + ": vertical fit runs");
    check(py[size_t(32) * 36 + 30] == 235 && py[size_t(2) * 36 + 30] == 16 && py[size_t(61) * 36 + 30] == 16, tag + ": fit leaves bars above and below");
    // Fill: the middle of the picture covers the output (the black quarter is cropped away).
    engine::PushCanvas fill; fill.fit = engine::PushCanvasFit::Fill;
    check(run(fill, 36, 64, py), tag + ": vertical fill runs");
    check(py[size_t(2) * 36 + 2] == 235 && py[size_t(61) * 36 + 33] == 235, tag + ": fill covers the whole output");
    // A crop of the black quarter and a green background around a box.
    engine::PushCanvas box; box.x = 0.5f; box.y = 0.5f; box.w = 0.5f; box.h = 0.5f; box.fit = engine::PushCanvasFit::Stretch;
    box.cropLeft = 0.25f; box.background = 0x00FF00;
    check(run(box, 64, 36, py), tag + ": box runs");
    check(py[size_t(27) * 64 + 34] == 235, tag + ": the cropped picture (no black quarter) in its box");
    checkValue(std::abs(int(py[size_t(4) * 64 + 4]) - 173) <= 1, tag + ": green background around it", int(py[size_t(4) * 64 + 4]), "173 +-1");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

// PushFPS streaming mode: scene transitions (the scene picture at part opacity) and live layers (a
// webcam, window or video frame scaled into a rectangle, engine/PushStreamOverlay.h LiveLayerBlend).
void testLiveLayers(Gpu& gpu, bool hdr) {
    constexpr uint32_t W = 128, H = 64;
    const std::string tag = hdr ? "live layers HDR" : "live layers";
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    const auto code = [&](uint32_t v8, uint32_t v10) { return hdr ? v10 << 6 : v8; };
    const auto plane = [&](uint32_t value, uint32_t w, uint32_t h, uint32_t channels) {
        std::vector<uint8_t> out(size_t(w) * h * channels * bpp);
        for (size_t i = 0; i < size_t(w) * h * channels; ++i) {
            if (hdr) { const uint16_t v = uint16_t(value); std::memcpy(out.data() + i * 2, &v, 2); } else out[i] = uint8_t(value);
        }
        return out;
    };
    const auto read = [&](const std::vector<uint8_t>& b, size_t i) { return hdr ? int(word(b, i)) >> 6 : int(b[i]); };
    const auto within = [](int a, int b, int tolerance) { return std::abs(a - b) <= tolerance; };

    // Placement maths.
    uint32_t x0 = 0, y0 = 0, w = 0, h = 0;
    engine::PushLayerPlace place{0.5f, 0.25f, 0.25f, 0.0f, 0, true};
    check(engine::pushLayerRect(engine::resolvePushLayerPlace(place, 32, 16, W, H), W, H, x0, y0, w, h) && x0 == 64 && y0 == 16 && w == 32 && h == 16,
          tag + ": automatic height keeps the frame's shape");
    engine::PushLayerPlace corner{1.0f, 1.0f, 0.25f, 0.0f, 3, true};
    check(engine::pushLayerRect(engine::resolvePushLayerPlace(corner, 32, 16, W, H), W, H, x0, y0, w, h) && x0 == 96 && y0 == 48 && w == 32 && h == 16,
          tag + ": bottom-right anchor");
    engine::PushLayerPlace off{1.2f, 0.0f, 0.2f, 0.2f, 0, true};
    check(!engine::pushLayerRect(off, W, H, x0, y0, w, h), tag + ": a place off the picture draws nothing");
    check(engine::pushTransitionProgress(0, 300) == 0.0f && engine::pushTransitionProgress(300, 300) == 1.0f &&
          std::abs(engine::pushTransitionProgress(150, 300) - 0.5f) < 1e-4f && engine::pushTransitionProgress(5, 0) == 1.0f,
          tag + ": transition progress eases from 0 to 1 (0 ms: a cut)");

    // An opaque white 32 x 16 frame at (64, 16).
    auto frame = std::make_shared<engine::PushOverlayImage>(engine::PushOverlayImage{32, 16, {}, std::vector<uint8_t>(32 * 16 * 4, 255)});
    std::array<engine::PushLiveLayerState, engine::kPushLiveLayers> layers{};
    layers[0].frame = frame; layers[0].frameVersion = 1; layers[0].place = place; layers[0].opacity = 1.0f;
    engine::LiveLayerBlend blend;
    check(blend.create(device, W, H, hdr, 2), tag + ": created");
    pipeline::StateTracker states;
    const auto run = [&](const std::function<void()>& draw, std::vector<uint8_t>& py, std::vector<uint8_t>& puv) {
        if (!gpu.begin()) return false;
        auto a = recordFill(gpu, y, plane(code(100, 400), W, H, 1), states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto b = recordFill(gpu, uv, plane(code(90, 300), W / 2, H / 2, 2), states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        draw();
        auto ry = recordRead(gpu, y, states.get(y.texture.Get()));
        auto ruv = recordRead(gpu, uv, states.get(uv.texture.Get()));
        if (!gpu.submitAndWait()) return false;
        py = mapRead(y, ry.Get()); puv = mapRead(uv, ruv.Get());
        return py.size() == size_t(W) * H * bpp;
    };
    std::vector<uint8_t> py, puv;
    check(run([&] { blend.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), layers); }, py, puv), tag + ": runs");
    const int white = hdr ? 573 : 235, base = hdr ? 400 : 100;
    check(read(py, size_t(20) * W + 70) == white && read(py, size_t(31) * W + 95) == white, tag + ": the frame fills its rectangle");
    check(read(py, size_t(20) * W + 97) == base && read(py, size_t(10) * W + 10) == base && read(py, size_t(33) * W + 70) == base,
          tag + ": the picture is kept around it");
    check(read(puv, (size_t(10) * (W / 2) + 36) * 2) == (hdr ? 512 : 128), tag + ": white is neutral in chroma");
    // Half opacity: halfway between the picture and white (the same uploaded frame).
    layers[0].opacity = 0.5f;
    check(run([&] { blend.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), layers); }, py, puv), tag + ": half opacity runs");
    checkValue(within(read(py, size_t(20) * W + 70), (white + base) / 2, 2), tag + ": half opacity blends halfway", read(py, size_t(20) * W + 70), std::format("{} +-2", (white + base) / 2));
    // A larger frame drawn smaller (box-filtered): still white inside, nothing outside.
    auto big = std::make_shared<engine::PushOverlayImage>(engine::PushOverlayImage{128, 64, {}, std::vector<uint8_t>(128 * 64 * 4, 255)});
    layers[0].frame = big; layers[0].frameVersion = 2; layers[0].opacity = 1.0f;
    for (int i = 0; i < 6; ++i) { // past the guard that delays a second size change
        std::vector<uint8_t> ty, tuv;
        run([&] { blend.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), layers); }, ty, tuv);
        py = ty; puv = tuv;
    }
    check(read(py, size_t(20) * W + 70) == white && read(py, size_t(20) * W + 97) == base, tag + ": a 4x larger frame is scaled into the same place");
    // Transitions: a black card at half opacity over the picture.
    engine::PushOverlayImage card{W, H, {}, std::vector<uint8_t>(size_t(W) * H * 4, 0)};
    for (size_t i = 0; i < size_t(W) * H; ++i) card.rgba[i * 4 + 3] = 255;
    engine::FrameOverlay overlay;
    check(overlay.create(device, W, H, hdr, card), tag + ": card created");
    check(run([&] { overlay.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), 0.5f); }, py, puv), tag + ": faded card runs");
    const int black = hdr ? 64 : 16;
    check(within(read(py, size_t(40) * W + 90), (black + base) / 2, 2), tag + ": a card at half opacity shows half the picture (scene fade)");
    check(run([&] { overlay.apply(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), 0.0f); }, py, puv) &&
          read(py, size_t(40) * W + 90) == base, tag + ": opacity 0 leaves the picture alone");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

// The console side from the raw picture (linear BT.709, float RGBA, here half the recorded size):
// no enhancement, only scaled and coded like the recorder's own conversion.
void testCompareRaw(Gpu& gpu, bool hdr, engine::CompareLayout layout) {
    constexpr uint32_t W = 256, H = 144;
    const bool sbs = layout == engine::CompareLayout::SideBySide;
    const std::string tag = std::string("raw console ") + (sbs ? "side by side" : "split") + (hdr ? " HDR" : "");
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    Plane raw{pipeline::makeTexture(device, W / 2, H / 2, DXGI_FORMAT_R32G32B32A32_FLOAT, false), DXGI_FORMAT_R32G32B32A32_FLOAT, W / 2, H / 2, 16};
    check(y.texture && uv.texture && raw.texture, tag + ": textures created");
    engine::RecordingLabel nothing{};
    engine::RecordingCompare compare;
    std::wstring reason;
    check(compare.create(device, W, H, hdr, nothing, nothing, reason, layout), tag + ": created");
    check(compare.rawAvailable(), tag + ": raw pass loaded");
    // Raw: linear grey. SDR 0.5 -> sRGB 0.7354 -> code 177. HDR 1.0 (80 nits) -> PQ 0.4859 -> code 490.
    const float grey = hdr ? 1.0f : 0.5f;
    std::vector<uint8_t> rawData(size_t(raw.w) * raw.h * 16);
    for (size_t i = 0; i < size_t(raw.w) * raw.h; ++i) {
        const float px[4] = {grey, grey, grey, 1.0f};
        std::memcpy(rawData.data() + i * 16, px, 16);
    }
    if (!gpu.begin()) return;
    auto rawUpload = recordFill(gpu, raw, rawData, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
    check(gpu.submitAndWait(), tag + ": raw picture uploaded");
    const auto words16 = [&](uint32_t value, size_t count) { return words(std::vector<uint16_t>(count, uint16_t(value))); };
    const auto plane = [&](uint32_t v8, uint32_t v10, size_t count) { return hdr ? words16(v10 << 6, count) : std::vector<uint8_t>(count, uint8_t(v8)); };
    const auto read = [&](const std::vector<uint8_t>& bytes, size_t index) { return hdr ? int(word(bytes, index)) : int(bytes[index]); };
    const int rawCode = hdr ? (490 << 6) : 177, tolerance = hdr ? 64 : 1;
    pipeline::StateTracker states;
    std::vector<uint8_t> py, puv;
    const engine::RecordingRawFrame frame{raw.texture.Get(), false};
    // A real frame with its raw picture: the enhanced picture is a flat 200 (8-bit) / 800 (10-bit).
    if (!gpu.begin()) return;
    auto a = recordFill(gpu, y, plane(200, 800, W * H), states.get(y.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    states.set(y.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto b = recordFill(gpu, uv, plane(150, 600, W / 2 * H / 2 * 2), states.get(uv.texture.Get()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    states.set(uv.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    compare.compose(gpu.list.Get(), states, y.texture.Get(), uv.texture.Get(), false, &frame, 3);
    auto ry = recordRead(gpu, y, states.get(y.texture.Get()));
    auto ruv = recordRead(gpu, uv, states.get(uv.texture.Get()));
    check(gpu.submitAndWait(), tag + ": real frame with raw picture runs");
    py = mapRead(y, ry.Get()); puv = mapRead(uv, ruv.Get());
    const auto Y = [&](uint32_t x, uint32_t yy) { return read(py, size_t(yy) * W + x); };
    const auto U = [&](uint32_t x, uint32_t yy) { return read(puv, (size_t(yy) * (W / 2) + x) * 2); };
    const uint32_t lx = 10, ly = sbs ? 60 : 60, rx = sbs ? 138 : 200;
    std::printf("INFO %s left=%d right=%d\n", tag.c_str(), Y(lx, ly), Y(rx, ly));
    checkValue(std::abs(Y(lx, ly) - rawCode) <= tolerance, tag + ": console side is the raw picture, also on a real frame", Y(lx, ly), std::format("{} +-{}", rawCode, tolerance));
    check(Y(rx, ly) == int(hdr ? (800 << 6) : 200), tag + ": PushFPS side is the enhanced picture");
    check(std::abs(U(lx / 2, ly / 2) - int(hdr ? (512 << 6) : 128)) <= tolerance, tag + ": raw grey has neutral chroma");
    check(compare.rawHold() && compare.rawFrames() == 1, tag + ": counts the raw frame");
    // A generated frame: the console side keeps the raw picture.
    check(composeFrame(gpu, compare, states, y, uv, plane(90, 300, W * H), plane(150, 600, W / 2 * H / 2 * 2), true, py, puv), tag + ": generated frame runs");
    check(std::abs(Y(lx, ly) - rawCode) <= tolerance, tag + ": console side holds the raw picture");
    check(Y(rx, ly) == int(hdr ? (300 << 6) : 90), tag + ": PushFPS side shows the generated frame");
    // A real frame without a raw picture falls back to the held real frame.
    check(composeFrame(gpu, compare, states, y, uv, plane(60, 200, W * H), plane(150, 600, W / 2 * H / 2 * 2), false, py, puv), tag + ": real frame without raw runs");
    check(Y(lx, ly) == int(hdr ? (200 << 6) : 60) && !compare.rawHold(), tag + ": without a raw picture the real frame is the console side");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

// Colour labels: straight-alpha RGBA blended onto the picture.
void testCompareBadges(Gpu& gpu, bool hdr) {
    constexpr uint32_t W = 256, H = 144;
    const std::string tag = std::string("badges") + (hdr ? " HDR" : "");
    auto* device = gpu.device.Get();
    const auto yFormat = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uvFormat = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const uint32_t bpp = hdr ? 2 : 1;
    Plane y{pipeline::makeTexture(device, W, H, yFormat, true), yFormat, W, H, bpp};
    Plane uv{pipeline::makeTexture(device, W / 2, H / 2, uvFormat, true), uvFormat, W / 2, H / 2, bpp * 2};
    // Left: 8x4, left half opaque white, right half transparent. Right: 8x4 black at 50 %.
    engine::RecordingLabel left{8, 4, {}, std::vector<uint8_t>(8 * 4 * 4, 0)}, right{8, 4, {}, std::vector<uint8_t>(8 * 4 * 4, 0)};
    for (uint32_t yy = 0; yy < 4; ++yy)
        for (uint32_t x = 0; x < 8; ++x) {
            uint8_t* l = &left.rgba[(yy * 8 + x) * 4];
            if (x < 4) l[0] = l[1] = l[2] = l[3] = 255;
            right.rgba[(yy * 8 + x) * 4 + 3] = 128;
        }
    engine::RecordingCompare compare;
    std::wstring reason;
    check(compare.create(device, W, H, hdr, left, right, reason), tag + ": created");
    check(compare.overlays().size() == 2 && compare.patches().size() == 1, tag + ": both badges blended, the divider copied");
    pipeline::StateTracker states;
    std::vector<uint8_t> py, puv;
    const auto plane = [&](uint32_t v8, uint32_t v10, size_t count) {
        return hdr ? words(std::vector<uint16_t>(count, uint16_t(v10 << 6))) : std::vector<uint8_t>(count, uint8_t(v8));
    };
    check(composeFrame(gpu, compare, states, y, uv, plane(50, 300, W * H), plane(100, 400, W / 2 * H / 2 * 2), false, py, puv), tag + ": frame runs");
    const auto read = [&](const std::vector<uint8_t>& bytes, size_t index) { return hdr ? int(word(bytes, index)) : int(bytes[index]); };
    const auto Y = [&](uint32_t x, uint32_t yy) { return read(py, size_t(yy) * W + x); };
    const auto U = [&](uint32_t x, uint32_t yy) { return read(puv, (size_t(yy) * (W / 2) + x) * 2); };
    const int tol = hdr ? 64 : 1;
    std::printf("INFO %s white=%d clear=%d half=%d\n", tag.c_str(), Y(9, 9), Y(13, 9), Y(242, 9));
    check(std::abs(Y(9, 9) - int(hdr ? (573 << 6) : 235)) <= tol, tag + ": opaque white badge pixel");
    check(Y(13, 9) == int(hdr ? (300 << 6) : 50), tag + ": transparent badge pixel leaves the picture");
    // 50 % black over the picture: halfway between the picture and black.
    const int halfway = hdr ? int(std::lround((300 - (300 - 64) * 128.0 / 255.0))) << 6 : int(std::lround(50 - (50 - 16) * 128.0 / 255.0));
    checkValue(std::abs(Y(242, 9) - halfway) <= tol, tag + ": half-transparent badge pixel is blended", Y(242, 9), std::format("{} +-{}", halfway, tol));
    check(std::abs(U(4, 4) - int(hdr ? (512 << 6) : 128)) <= tol, tag + ": white badge has neutral chroma");
    check(U(6, 4) == int(hdr ? (400 << 6) : 100), tag + ": chroma under a transparent badge pixel is kept");
    check(Y(30, 60) == int(hdr ? (300 << 6) : 50), tag + ": picture outside the badges unchanged");
    check(!gpu.debugErrors(), tag + ": no D3D12 debug errors");
}

void testCompareSkipRule() {
    // Real frames are never skipped by the rule (only when no slot is free at all).
    check(!engine::compareSkipsFrame(false, 0, true) && !engine::compareSkipsFrame(false, 1, false), "skip rule: real frames kept");
    // Generated frames leave the last free slot for the next real frame, and give way when the disk is behind.
    check(engine::compareSkipsFrame(true, 1, false) && engine::compareSkipsFrame(true, 0, false), "skip rule: generated frames leave one slot free");
    check(engine::compareSkipsFrame(true, 6, true), "skip rule: generated frames skipped while the disk is behind");
    check(!engine::compareSkipsFrame(true, 2, false), "skip rule: generated frames recorded while two slots are free");
}

void testLabelScale() {
    engine::RecordingLabel l{8, 4, std::vector<uint8_t>(32, 0)};
    for (int yy = 0; yy < 4; ++yy) for (int x = 4; x < 8; ++x) l.luma[size_t(yy) * 8 + x] = 255;
    const auto h = engine::scaleRecordingLabel(l, 4, 2);
    check(h.width == 4 && h.height == 2 && h.luma[0] == 0 && h.luma[3] == 255, "label scale: halves by area average");
    const auto t = engine::scaleRecordingLabel(l, 3, 3);
    check(t.luma.size() == 9 && t.luma[1] == 128, "label scale: partial pixels are weighted");
    check(engine::scaleRecordingLabel(l, 0, 3).luma.empty(), "label scale: empty size gives no label");
    engine::RecordingLabel c{2, 2, {}, {255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 0, 255, 0, 0, 255, 0}};
    const auto one = engine::scaleRecordingLabel(c, 1, 1);
    check(one.rgba.size() == 4 && one.rgba[0] == 255 && one.rgba[2] == 0 && one.rgba[3] == 128, "label scale: colour weighted by alpha");
}

void testHudSampler(Gpu& gpu) {
    constexpr uint32_t W = 640, H = 360;
    auto* device = gpu.device.Get();
    Plane frameTex{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 4};
    check(frameTex.texture != nullptr, "hud: frame texture created");
    engine::PushHudSampler sampler;
    // Not running: nothing recorded.
    gpu.begin();
    const engine::PresentCapture::Frame idle{frameTex.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 0, 0, false};
    check(!sampler.record(gpu.list.Get(), idle), "hud: nothing is copied while no detection runs");
    gpu.list->Close();

    sampler.start(6, 60);
    // HUD box: x 40..200, y 20..60 (static text-like strokes); the rest moves.
    auto picture = [&](int shift) {
        std::vector<uint8_t> px(size_t(W) * H * 4);
        for (uint32_t yy = 0; yy < H; ++yy)
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t v;
                if (x >= 40 && x < 200 && yy >= 20 && yy < 60) v = ((x / 4 + yy / 5) % 3 == 0 || x % 9 == 0) ? 235 : 20;
                else {
                    const int sx = int(x) + shift, sy = int(yy) + shift / 2;
                    uint32_t h = uint32_t(sx / 2) * 374761393u + uint32_t(sy / 2) * 668265263u;
                    h = (h ^ (h >> 13)) * 1274126177u;
                    v = uint8_t(std::clamp(int(128 + 60 * std::sin(sx * 0.03) + 40 * std::cos(sy * 0.04)) + int(h >> 28) * 4 - 30, 0, 255));
                }
                uint8_t* p = px.data() + (size_t(yy) * W + x) * 4;
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
        return px;
    };
    int copies = 0, refusedGenerated = 0, refusedBusy = 0;
    engine::PushHudSampler::Status status;
    for (int step = 0; step < 200 && !status.done && !status.failed; ++step) {
        gpu.begin();
        // The picture moves by one step per frame the sampler takes, not per loop turn: which turns
        // fall on the sampler's 60 ms interval depends on how fast the adapter is (WARP on a busy
        // build machine or a GPU), and the detection must see the same six pictures on both.
        auto upload = recordFill(gpu, frameTex, picture(copies * 7), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        const engine::PresentCapture::Frame generated{frameTex.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 0, 0, true};
        if (sampler.record(gpu.list.Get(), generated)) { sampler.submitted(gpu.fence.Get(), gpu.value + 1); ++refusedGenerated; } // counts a wrong copy
        const engine::PresentCapture::Frame real{frameTex.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 0, 0, false};
        const bool recorded = sampler.record(gpu.list.Get(), real);
        if (recorded) {
            ++copies;
            if (sampler.record(gpu.list.Get(), real)) ++refusedBusy; // a second copy in the same list must be refused
        }
        if (!gpu.submit()) break;
        if (recorded) sampler.submitted(gpu.fence.Get(), gpu.value);
        gpu.wait();
        status = sampler.status();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(refusedGenerated == 0, "hud: generated frames are never copied");
    check(refusedBusy == 0, "hud: one copy at a time");
    check(status.done && !status.failed && copies == 6 && status.taken == 6, "hud: six frames taken, then done",
          std::format("done={} failed={} copies={} taken={}", status.done, status.failed, copies, status.taken),
          "done=true failed=false copies=6 taken=6");
    check(status.preview.size() == size_t(engine::hud::kPreviewWidth) * engine::hud::kPreviewHeight * 3, "hud: preview picture of the last frame");
    std::printf("INFO hud motion=%.2f regions=%zu\n", status.detection.motion, status.detection.regions.size());
    bool found = false;
    for (const auto& r : status.detection.regions)
        found = found || (r.left <= 40.0f / W + 0.01f && r.top <= 20.0f / H + 0.01f && r.right >= 200.0f / W - 0.01f && r.bottom >= 60.0f / H - 0.01f &&
                          r.right <= 230.0f / W && r.bottom <= 80.0f / H);
    check(found, "hud: the static box is found end to end", std::format("{} regions, motion {:.2f}", status.detection.regions.size(), status.detection.motion),
          "a region covering x 40..200, y 20..60");
    check(!gpu.debugErrors(), "hud: no D3D12 debug errors");

    // A discarded list leaves the sampler free for the next frame.
    sampler.start(6, 50);
    gpu.begin();
    const engine::PresentCapture::Frame real{frameTex.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 0, 0, false};
    check(sampler.record(gpu.list.Get(), real), "hud: copies after a restart");
    sampler.discarded();
    check(sampler.record(gpu.list.Get(), real), "hud: a discarded copy frees the slot");
    gpu.list->Close();
    sampler.discarded();
    sampler.cancel();
    check(!sampler.status().running, "hud: cancel stops it");

    // A format it cannot read fails with a message.
    Plane floatTex{pipeline::makeTexture(device, 64, 64, DXGI_FORMAT_R32_FLOAT, false), DXGI_FORMAT_R32_FLOAT, 64, 64, 4};
    sampler.start(6, 50);
    gpu.begin();
    const engine::PresentCapture::Frame odd{floatTex.texture.Get(), DXGI_FORMAT_R32_FLOAT, 64, 64, 0, 0, false};
    check(!sampler.record(gpu.list.Get(), odd), "hud: an unsupported format is not copied");
    gpu.list->Close();
    const auto failed = sampler.status();
    check(failed.failed && !failed.error.empty() && !failed.running, "hud: an unsupported format fails with a reason");
}
// The pipeline probe (engine/PushPipelineProbe.h, autorun step probe:<seconds>): pairs of consecutive real
// frames, the output picture and the raw console picture of each, read back and reduced to the picture
// metrics of engine/PushImageMetrics.h. Flat pictures with known values, so every number has one answer.
void testPipelineProbe(Gpu& gpu) {
    constexpr uint32_t W = 640, H = 360, RW = 480, RH = 270;
    auto* device = gpu.device.Get();
    Plane outTex{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 4};
    Plane rawTex{pipeline::makeTexture(device, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false), DXGI_FORMAT_R16G16B16A16_FLOAT, RW, RH, 8};
    check(outTex.texture != nullptr && rawTex.texture != nullptr, "probe: textures created");
    engine::PushPipelineProbe probe;
    engine::PresentCapture::Frame real{outTex.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 0, 0, false};
    real.raw = rawTex.texture.Get();
    engine::PresentCapture::Frame generated = real;
    generated.generated = true;
    // Not running: nothing recorded, no raw picture asked for.
    gpu.begin();
    check(!probe.record(gpu.list.Get(), real) && !probe.wantsRawFrames(), "probe: idle while no probe runs");
    gpu.list->Close();

    // Output: grey 128 (sRGB codes). Raw: linear 0.5 in half floats = sRGB code 187.5.
    const std::vector<uint8_t> grey(size_t(W) * H * 4, uint8_t{128});
    std::vector<uint8_t> half(size_t(RW) * RH * 8);
    for (size_t i = 0; i < size_t(RW) * RH; ++i) {
        const uint16_t px[4] = {0x3800, 0x3800, 0x3800, 0x3C00};
        std::memcpy(half.data() + i * 8, px, 8);
    }
    engine::PushPipelineProbe::Options options;
    options.seconds = 4;   // two pairs, 2 s apart
    probe.start(options);
    check(probe.wantsRawFrames(), "probe: asks for the raw picture while it runs");
    int copies = 0, copiedGenerated = 0, copiedTwice = 0;
    engine::PushPipelineProbe::Status status;
    for (int step = 0; step < 800 && !status.done && !status.failed; ++step) {
        gpu.begin();
        auto uploadOut = recordFill(gpu, outTex, grey, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        auto uploadRaw = recordFill(gpu, rawTex, half, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        if (probe.record(gpu.list.Get(), generated)) { probe.submitted(gpu.fence.Get(), gpu.value + 1); ++copiedGenerated; } // counts a wrong copy
        const bool recorded = probe.record(gpu.list.Get(), real);
        if (recorded) {
            ++copies;
            if (probe.record(gpu.list.Get(), real)) ++copiedTwice; // a second copy in the same list must be refused
        }
        if (!gpu.submit()) break;
        if (recorded) probe.submitted(gpu.fence.Get(), gpu.value);
        gpu.wait();
        status = probe.status();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(copiedGenerated == 0, "probe: generated frames are never copied");
    check(copiedTwice == 0, "probe: one copy per presented frame");
    check(status.done && !status.failed && copies == 4 && status.pairs == 2, "probe: two pairs of frames taken, then done",
          std::format("done={} failed={} reason={} copies={} pairs={}", status.done, status.failed, status.reason, copies, status.pairs),
          "done=true failed=false copies=4 pairs=2");
    check(status.output.frames == 4 && status.output.width == W && status.output.height == H && !status.output.hdr,
          "probe: the output picture of every frame was read",
          std::format("frames={} size={}x{}", status.output.frames, status.output.width, status.output.height), "frames=4 size=640x360");
    check(status.raw.frames == 4 && status.raw.width == RW && status.raw.height == RH, "probe: the raw picture of every frame was read",
          std::format("frames={} size={}x{}", status.raw.frames, status.raw.width, status.raw.height), "frames=4 size=480x270");
    const auto within = [](double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; };
    check(within(status.output.meanLuma.value(), 128.0, 0.5), "probe: output luma read back", shown(status.output.meanLuma.value()), "128");
    check(within(status.raw.meanLuma.value(), 187.5, 1.0), "probe: raw luma read back and sRGB encoded", shown(status.raw.meanLuma.value()), "187.5");
    check(within(status.output.hudDelta.value(), 59.5, 1.5), "probe: hudDelta is output against raw in the corners", shown(status.output.hudDelta.value()), "59.5");
    check(within(status.output.blockiness.value(), 0.0, 0.01) && within(status.output.banding.value(), 0.0, 0.01) &&
          within(status.output.detail.value(), 0.0, 0.01) && within(status.output.flicker.value(), 0.0, 0.01) &&
          within(status.output.ringing.value(), 0.0, 0.01), "probe: a flat picture has no defect and no detail",
          std::format("blockiness={:.3f} banding={:.3f} detail={:.3f} flicker={:.3f} ringing={:.3f}", status.output.blockiness.value(),
                      status.output.banding.value(), status.output.detail.value(), status.output.flicker.value(), status.output.ringing.value()),
          "all 0");
    check(!probe.wantsRawFrames(), "probe: stops asking for the raw picture when done");
    check(!gpu.debugErrors(), "probe: no D3D12 debug errors");

    // A discarded list leaves the probe free for the next frame; cancel stops it.
    probe.start(options);
    gpu.begin();
    check(probe.record(gpu.list.Get(), real), "probe: copies after a restart");
    probe.discarded();
    check(probe.record(gpu.list.Get(), real), "probe: a discarded copy frees the frame");
    gpu.list->Close();
    probe.discarded();
    probe.cancel();
    check(!probe.status().running && !probe.wantsRawFrames(), "probe: cancel stops it");

    // A format it cannot read fails with a reason.
    Plane floatTex{pipeline::makeTexture(device, 64, 64, DXGI_FORMAT_R32_FLOAT, false), DXGI_FORMAT_R32_FLOAT, 64, 64, 4};
    probe.start(options);
    gpu.begin();
    const engine::PresentCapture::Frame odd{floatTex.texture.Get(), DXGI_FORMAT_R32_FLOAT, 64, 64, 0, 0, false};
    check(!probe.record(gpu.list.Get(), odd), "probe: an unsupported format is not copied");
    gpu.list->Close();
    const auto failed = probe.status();
    check(failed.failed && failed.reason == "format" && !failed.running, "probe: an unsupported format fails with a reason", failed.reason, "format");
    // A 10-bit output is decoded as HDR10 (PQ) only: the graph makes no other. Without hdrMode 2 it is refused.
    Plane tenBit{pipeline::makeTexture(device, 64, 64, DXGI_FORMAT_R10G10B10A2_UNORM, false), DXGI_FORMAT_R10G10B10A2_UNORM, 64, 64, 4};
    probe.start(options);
    gpu.begin();
    const engine::PresentCapture::Frame sdrTenBit{tenBit.texture.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, 64, 64, 0, 0, false};
    check(!probe.record(gpu.list.Get(), sdrTenBit), "probe: a 10-bit picture that is not HDR10 is not copied");
    gpu.list->Close();
    const auto refused = probe.status();
    check(refused.failed && refused.reason == "format", "probe: a 10-bit SDR picture fails with a reason", refused.reason, "format");
}
// The change of a luma plane against the previous picture (pipeline/StreamChangePass.h) against the
// CPU reference (pipeline/StreamChange.h).
void testStreamChange(Gpu& gpu) {
    constexpr uint32_t W = 200, H = 120;   // 13 x 8 blocks; the last column of blocks is 8 wide, the last row 8 high
    auto* device = gpu.device.Get();
    pipeline::StreamChangePass pass;
    check(pass.initialize(device), "stream change: pass created");
    if (!pass.ready()) return;
    Plane luma8{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R8_UNORM, false), DXGI_FORMAT_R8_UNORM, W, H, 1};
    Plane luma16{pipeline::makeTexture(device, W, H, DXGI_FORMAT_R16_UNORM, false), DXGI_FORMAT_R16_UNORM, W, H, 2};
    Plane half{pipeline::makeTexture(device, 96, 64, DXGI_FORMAT_R8_UNORM, false), DXGI_FORMAT_R8_UNORM, 96, 64, 1};
    check(luma8.texture && luma16.texture && half.texture, "stream change: textures created");
    if (!luma8.texture || !luma16.texture || !half.texture) return;
    // 8-bit code values of a picture; `moved` shifts the pattern.
    auto codes = [&](uint32_t w, uint32_t h, int moved) {
        std::vector<uint8_t> v(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t k = uint32_t(int(x) + moved) * 374761393u + uint32_t(int(y) + moved / 2) * 668265263u;
                k = (k ^ (k >> 13)) * 1274126177u;
                v[size_t(y) * w + x] = uint8_t(30 + (k >> 25));   // 30..157
            }
        return v;
    };
    auto bytesOf = [](const std::vector<uint8_t>& v, uint32_t bpp) {
        if (bpp == 1) return v;
        std::vector<uint8_t> out(v.size() * 2);                   // the code value in the high byte, as P010 stores it
        for (size_t i = 0; i < v.size(); ++i) { out[2 * i] = 0; out[2 * i + 1] = v[i]; }
        return out;
    };
    pipeline::StateTracker states;
    auto measure = [&](const Plane& p, const std::vector<uint8_t>& px, pipeline::StreamChangeMeasurement& m) {
        gpu.begin();
        auto up = recordFill(gpu, p, px, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        states.set(p.texture.Get(), D3D12_RESOURCE_STATE_COMMON);
        const bool ok = up && pass.record(gpu.list.Get(), states, p.texture.Get(), 0, p.w, p.h);
        if (!ok) { gpu.list->Close(); return false; }
        if (!gpu.submitAndWait()) return false;
        m = pass.read();
        return true;
    };
    auto reference = [&](const Plane& p, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
        return pipeline::decodeStreamChange(pipeline::streamChangeReference(before.data(), after.data(), size_t(p.w) * p.bpp, p.w, p.h, p.bpp), p.w, p.h, true);
    };
    auto close = [](const pipeline::StreamChangeMeasurement& gpuValue, const pipeline::StreamChangeMeasurement& cpuValue) {
        // 1/64 of a step: float sums on the GPU against integer sums on the CPU, and the 8.8 rounding.
        // The count of moved samples has no rounding: no sample difference here lies near its limit.
        return gpuValue.valid && gpuValue.comparable && cpuValue.valid && std::fabs(gpuValue.maxBlock - cpuValue.maxBlock) < 1.0f / 64 &&
               std::fabs(gpuValue.meanBlock - cpuValue.meanBlock) < 1.0f / 64 && std::fabs(gpuValue.changedShare - cpuValue.changedShare) <= 1.0f / 104 + 1e-6f &&
               std::fabs(gpuValue.pixelMax - cpuValue.pixelMax) < 1.0f / 64 && gpuValue.pixelsOver == cpuValue.pixelsOver &&
               // The signed words: each block's sums are rounded to whole steps on both sides; a block's verdict may differ by one.
               std::fabs(gpuValue.signedMean - cpuValue.signedMean) < 1.0f / 64 && std::fabs(gpuValue.absMean - cpuValue.absMean) < 1.0f / 64 &&
               std::fabs(gpuValue.touchedShare - cpuValue.touchedShare) <= 1.0f / 104 + 1e-6f && std::fabs(gpuValue.coherentShare - cpuValue.coherentShare) <= 1.0f / 104 + 1e-6f;
    };
    auto show = [](const std::string& tag, const char* what, const pipeline::StreamChangeMeasurement& g, const pipeline::StreamChangeMeasurement& c) {
        std::printf("INFO %s: %s: GPU max %.4f mean %.5f share %.4f sample max %.3f moved %.0f signed %.4f abs %.4f touched %.4f coherent %.4f, CPU max %.4f mean %.5f share %.4f sample max %.3f moved %.0f signed %.4f abs %.4f touched %.4f coherent %.4f\n", tag.c_str(), what,
                    g.maxBlock, g.meanBlock, g.changedShare, g.pixelMax, g.pixelsOver, g.signedMean, g.absMean, g.touchedShare, g.coherentShare,
                    c.maxBlock, c.meanBlock, c.changedShare, c.pixelMax, c.pixelsOver, c.signedMean, c.absMean, c.touchedShare, c.coherentShare);
    };
    for (const Plane* plane : {&luma8, &luma16}) {
        const Plane& p = *plane;
        const std::string tag = p.bpp == 1 ? "stream change 8-bit" : "stream change 16-bit";
        const float scale = p.bpp == 2 ? 65280.0f / 65535.0f : 1.0f;   // a code value in the high byte of a 16-bit word, in 8-bit steps
        pass.restart();
        const auto base = codes(W, H, 0);
        const auto first = bytesOf(base, p.bpp);
        pipeline::StreamChangeMeasurement m;
        check(measure(p, first, m) && m.valid && !m.comparable, tag + ": the first picture is measured but has nothing to be compared with");
        // The same picture again: nothing changed, in no block and in no sample.
        check(measure(p, first, m) && m.valid && m.comparable && m.maxBlock < 1.0f / 64 && m.meanBlock < 1.0f / 64 && m.changedShare == 0.0f &&
              m.pixelMax == 0.0f && m.pixelsOver == 0.0f, tag + ": an unchanged picture gives zero");
        // One block brighter by 5 steps, one sample of another block changed by 64: a repeat-like picture.
        auto touched = base;
        for (uint32_t y = 32; y < 48; ++y) for (uint32_t x = 48; x < 64; ++x) touched[size_t(y) * W + x] = uint8_t(touched[size_t(y) * W + x] + 5);
        touched[size_t(5) * W + 3] = uint8_t(touched[size_t(5) * W + 3] ^ 0x40);
        const auto touchedBytes = bytesOf(touched, p.bpp);
        const auto wantTouched = reference(p, first, touchedBytes);
        check(measure(p, touchedBytes, m) && close(m, wantTouched), tag + ": one changed block matches the CPU reference");
        show(tag, "one block +5 and one sample", m, wantTouched);
        check(std::fabs(m.maxBlock - 5.0f * scale) < 0.05f && std::fabs(m.changedShare - 1.0f / 104) < 1e-4f && std::fabs(m.pixelMax - 64.0f * scale) < 0.05f && m.pixelsOver == 257.0f,
              tag + ": that is 5 steps in one of 104 blocks, 64 steps in one sample, 257 samples moved by more than 4");
        check(std::lround(m.touchedShare * 104.0f) == 2 && std::lround(m.coherentShare * 104.0f) == 2 && m.signedMean != 0.0f,
              tag + ": both changes went one way: two blocks touched, both coherent");
        // What an encoder's refinement looks like: samples nudged both ways, nothing net. Touched, not coherent.
        auto nudged = touched;
        for (uint32_t y = 80; y < 96; ++y) for (uint32_t x = 16; x < 32; ++x) { auto& v = nudged[size_t(y) * W + x]; v = uint8_t(((x + y) & 1) ? v + 3 : v - 3); }
        const auto nudgedBytes = bytesOf(nudged, p.bpp);
        const auto wantNudged = reference(p, touchedBytes, nudgedBytes);
        check(measure(p, nudgedBytes, m) && close(m, wantNudged) && std::lround(m.touchedShare * 104.0f) == 1 && m.coherentShare == 0.0f &&
              std::fabs(m.signedMean) < 1.0f / 64 && m.absMean > 0.0f, tag + ": samples nudged both ways: one block touched, none coherent");
        show(tag, "one block nudged +-3", m, wantNudged);
        check(measure(p, touchedBytes, m), tag + ": back to the picture before");
        // The last column and the last row belong to blocks too (8 wide, 8 high): a change there is seen.
        auto edge = touched;
        edge[size_t(60) * W + (W - 1)] = uint8_t(edge[size_t(60) * W + (W - 1)] ^ 0x40);
        edge[size_t(H - 1) * W + 20] = uint8_t(edge[size_t(H - 1) * W + 20] ^ 0x40);
        const auto edgeBytes = bytesOf(edge, p.bpp);
        const auto wantEdge = reference(p, touchedBytes, edgeBytes);
        check(measure(p, edgeBytes, m) && close(m, wantEdge) && m.pixelsOver == 2.0f && m.maxBlock > 0.4f, tag + ": samples in the edge blocks are measured");
        show(tag, "two edge samples", m, wantEdge);
        // What a block mean cannot see: a 6x6 object moves one sample to the right inside one block.
        auto objectA = base, objectB = base;
        for (uint32_t y = 68; y < 74; ++y) for (uint32_t x = 100; x < 106; ++x) { objectA[size_t(y) * W + x] = 250; objectB[size_t(y) * W + x + 1] = 250; }
        const auto objectABytes = bytesOf(objectA, p.bpp), objectBBytes = bytesOf(objectB, p.bpp);
        check(measure(p, objectABytes, m), tag + ": object picture");
        const auto wantObject = reference(p, objectABytes, objectBBytes);
        check(measure(p, objectBBytes, m) && close(m, wantObject) && m.pixelsOver == 12.0f && m.pixelMax > 90.0f * scale && m.maxBlock < 1.0f,
              tag + ": an object moving inside one block shows in the samples, not in the block means");
        show(tag, "6x6 object moved by one sample", m, wantObject);
        // Everything moves: a new picture.
        const auto movedBytes = bytesOf(codes(W, H, 7), p.bpp);
        const auto wantMoved = reference(p, objectBBytes, movedBytes);
        check(measure(p, movedBytes, m) && close(m, wantMoved) && m.changedShare > 0.5f && m.pixelsOver > float(W * H) / 2, tag + ": a moved picture matches the CPU reference");
        show(tag, "moved", m, wantMoved);
        // After a restart the stored picture is not used.
        pass.restart();
        check(measure(p, first, m) && m.valid && !m.comparable, tag + ": not comparable after a restart");
    }
    // Another picture size: other stores, nothing to compare with; then comparable again.
    pipeline::StreamChangeMeasurement m;
    const auto small0 = codes(96, 64, 0), small1 = codes(96, 64, 5);
    check(measure(luma8, bytesOf(codes(W, H, 0), 1), m) && m.valid, "stream change: back to the first size");
    check(measure(half, small0, m) && m.valid && !m.comparable, "stream change: a new picture size starts again");
    check(measure(half, small1, m) && close(m, reference(half, small0, small1)), "stream change: and is compared from the next picture on");
    // A texture it cannot read is refused.
    Plane odd{pipeline::makeTexture(device, 64, 64, DXGI_FORMAT_R32_FLOAT, false), DXGI_FORMAT_R32_FLOAT, 64, 64, 4};
    gpu.begin();
    check(odd.texture && !pass.record(gpu.list.Get(), states, odd.texture.Get(), 0, 64, 64), "stream change: an unsupported format is refused");
    check(!pass.record(gpu.list.Get(), states, luma8.texture.Get(), 0, 8, 8), "stream change: a picture smaller than one block is refused");
    check(!pass.record(gpu.list.Get(), states, luma8.texture.Get(), 0, 4112, 120), "stream change: a picture wider than the pass measures is refused");
    gpu.list->Close();

    // The decoder's own layout: an NV12 texture, read through a view of plane 0 (as EnhanceGraph::measureStreamChange
    // hands it over). Not every adapter can create one outside a decoder; then this part is skipped and said so.
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_NV12, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        const bool can = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                         (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD);
        // Plane 0 of an NV12 texture is subresource 0 and is filled and viewed as R8_UNORM; plane 1 is never read.
        Plane nv12{can ? pipeline::makeTexture(device, W, H, DXGI_FORMAT_NV12, false) : nullptr, DXGI_FORMAT_R8_UNORM, W, H, 1};
        if (!nv12.texture) std::printf("NOTE stream change: this adapter cannot create an NV12 texture; the NV12 plane view was NOT tested\n");
        else {
            const auto nvA = codes(W, H, 2), nvB = codes(W, H, 11);
            auto nvTouched = nvB;
            for (uint32_t y = 68; y < 74; ++y) for (uint32_t x = 100; x < 106; ++x) nvTouched[size_t(y) * W + x] = 250;
            pass.restart();
            check(measure(nv12, nvA, m) && m.valid && !m.comparable, "stream change NV12: the first picture is measured through the plane 0 view");
            check(measure(nv12, nvB, m) && close(m, reference(nv12, nvA, nvB)), "stream change NV12: a moved picture matches the CPU reference");
            const auto wantNv = reference(nv12, nvB, nvTouched);
            check(measure(nv12, nvTouched, m) && close(m, wantNv) && m.pixelsOver == 36.0f, "stream change NV12: a 6x6 object matches the CPU reference");
            show("stream change NV12", "6x6 object", m, wantNv);
            // Fourth review: the luma plane again unchanged, as when only the colour (plane 1) changed.
            // The pass does not read plane 1, so it must not claim chroma: the measurement is luma-exact
            // but "chroma not measured", which the exact-only classifier never calls a repeat.
            check(measure(nv12, nvTouched, m) && m.valid && m.comparable && m.pixelMax == 0.0f && m.pixelsOver == 0.0f && m.chromaChanged < 0.0f,
                  "stream change NV12: unchanged luma with unread chroma is never exact (chroma not measured)");
        }
    }

    // The runner: its own command list and fence on the same queue, a bounded wait; its stores
    // are created with it, for the picture size it is told.
    pipeline::StreamChangeRunner runner;
    check(runner.initialize(device, gpu.queue.Get(), W, H), "stream change: runner created with its stores");
    if (!runner.ready()) return;
    auto fill = [&](const std::vector<uint8_t>& px) {
        gpu.begin();
        auto up = recordFill(gpu, luma8, px, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
        return up && gpu.submitAndWait();
    };
    // Measures the picture now in luma8 with a deadline wait100ns from now. When the answer misses
    // the wait (a slow software adapter), the same picture is measured once more and the first
    // answer is taken from `late`. No wall-clock bound here: how long a call takes depends on the
    // adapter and the runner's load (a 50 ms slack failed once on WARP); the deadline mechanism is
    // checked below with a deliberately tiny deadline instead.
    auto run = [&](pipeline::StreamChangeMeasurement& out, int64_t wait100ns = pipeline::kStreamChangeWait100ns) {
        auto o = runner.measure(gpu.queue.Get(), luma8.texture.Get(), 0, W, H, nullptr, 0, pipeline::StreamChangeRunner::now100ns() + wait100ns);
        // The texture may be let go exactly when the list was seen to finish. The fence is read once
        // here: a list that was still running inside measure may finish before this read, so for a
        // queued one only "still reading means still pending" holds; one not queued never reads.
        const bool stillReading = runner.reading();
        check(o.reading == o.queued && (o.queued || !stillReading), "stream change runner: says whether a list may still read the texture");
        if (o.now.valid) { out = o.now; return true; }
        if (!o.queued) return false;
        for (int i = 0; i < 5000 && runner.pending(); ++i) Sleep(1);
        check(!runner.reading(), "stream change runner: the late list has finished, the texture is free");
        o = runner.measure(gpu.queue.Get(), luma8.texture.Get(), 0, W, H, nullptr, 0, pipeline::StreamChangeRunner::now100ns() + pipeline::kStreamChangeWait100ns);
        std::printf("NOTE stream change: a measurement missed its %.1f ms wait and was read late\n", double(wait100ns) / 10000.0);
        if (!o.hasLate || !o.late.valid) return false;
        out = o.late;
        return true;
    };
    const auto runA = codes(W, H, 0), runB = codes(W, H, 9);
    check(fill(runA) && run(m) && m.valid && !m.comparable, "stream change runner: the first picture has nothing to be compared with");
    check(fill(runB) && run(m) && close(m, reference(luma8, runA, runB)), "stream change runner: matches the CPU reference");
    check(fill(runB) && run(m) && m.comparable && m.maxBlock < 1.0f / 64 && m.pixelMax == 0.0f, "stream change runner: an unchanged picture gives zero");
    check(fill(runA) && run(m, 3000) && close(m, reference(luma8, runB, runA)), "stream change runner: a 0.3 ms wait still measures the picture");
    // The deadline mechanism, independent of the adapter's speed: a deliberately tiny deadline (one
    // microsecond) ends without an answer and says so (timed out: not dispatched, or queued and
    // handed over later), never with a wrong or silent result; the next measurement, with the
    // ordinary deadline, works and, where the first was queued, hands its answer over as `late`.
    {
        check(fill(runB), "stream change runner: picture for the tiny deadline");
        const auto tiny = runner.measure(gpu.queue.Get(), luma8.texture.Get(), 0, W, H, nullptr, 0, pipeline::StreamChangeRunner::now100ns() + 10);
        check(tiny.now.valid ? close(tiny.now, reference(luma8, runA, runB)) : tiny.timedOut, "stream change runner: a missed deadline is reported as timed out");
        check(tiny.now.valid || tiny.queued || !tiny.reading, "stream change runner: a list not dispatched does not read the texture");
        for (int i = 0; i < 5000 && runner.pending(); ++i) Sleep(1);
        check(!runner.pending(), "stream change runner: the late list finishes");
        check(fill(runA), "stream change runner: picture after the tiny deadline");
        const auto next = runner.measure(gpu.queue.Get(), luma8.texture.Get(), 0, W, H, nullptr, 0, pipeline::StreamChangeRunner::now100ns() + pipeline::kStreamChangeWait100ns);
        if (tiny.queued) check(next.hasLate && next.late.valid && close(next.late, reference(luma8, runA, runB)), "stream change runner: the late answer is handed over");
        check(next.now.valid || next.timedOut, "stream change runner: the next measurement works");
        if (next.now.valid && !tiny.queued && !tiny.now.valid) check(!next.now.comparable, "stream change runner: after a picture without a dispatch nothing is compared");
        if (next.queued) {   // this adapter missed the ordinary wait too: let it finish and take its answer
            for (int i = 0; i < 5000 && runner.pending(); ++i) Sleep(1);
            const auto drained = runner.noTime();
            check(drained.hasLate && drained.late.valid, "stream change runner: the next measurement's late answer is handed over");
        }
    }
    // No time at all (a presentation is due; the deadline has passed or is 0): nothing is dispatched,
    // no list reads the texture, and the next picture has nothing to be compared with.
    const auto before = runner.counters();
    for (const int64_t deadline : {int64_t(0), pipeline::StreamChangeRunner::now100ns() - 1}) {
        check(fill(runB), "stream change runner: picture");
        const auto none = runner.measure(gpu.queue.Get(), luma8.texture.Get(), 0, W, H, nullptr, 0, deadline);
        check(!none.now.valid && none.timedOut && !none.queued && !none.reading && !none.hasLate && !runner.reading() && !runner.pending(),
              "stream change runner: without time nothing is dispatched");
    }
    check(runner.counters().notDispatched == before.notDispatched + 2 && runner.counters().measured == before.measured && runner.counters().timeouts == before.timeouts,
          "stream change runner: counted as not dispatched");
    check(fill(runA) && run(m) && m.valid && !m.comparable, "stream change runner: the picture after a gap is not compared");
    check(fill(runB) && run(m) && close(m, reference(luma8, runA, runB)), "stream change runner: and the one after it is again");
    const auto counters = runner.counters();
    std::printf("INFO stream change runner: measured %llu, timeouts %llu, wait mean %.3f ms, max %.3f ms (this adapter; not the RTX 5070)\n",
                static_cast<unsigned long long>(counters.measured), static_cast<unsigned long long>(counters.timeouts),
                counters.measured + counters.timeouts ? counters.waitMsSum / double(counters.measured + counters.timeouts) : 0.0, counters.waitMsMax);
    check(counters.measured + counters.late >= 6 && counters.failures == 0, "stream change runner: every picture was measured");
    check(runner.shutdown(), "stream change runner: the last list had finished at shutdown");
    check(!runner.ready(), "stream change runner: shut down");
    check(!gpu.debugErrors(), "stream change: no D3D12 debug errors");
}
void testHudLock(Gpu& gpu) {
    constexpr uint32_t W = 256, H = 144;
    constexpr DXGI_FORMAT F = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto* device = gpu.device.Get();
    Plane real[2]{{pipeline::makeTexture(device, W, H, F, true), F, W, H, 4}, {pipeline::makeTexture(device, W, H, F, true), F, W, H, 4}};
    Plane generated{pipeline::makeTexture(device, W, H, F, true), F, W, H, 4};
    check(real[0].texture && real[1].texture && generated.texture, "hud lock: textures created");
    pipeline::PushHudLock lock;
    check(lock.initialize(device, W, H, F, 0), "hud lock: created");
    pipeline::DescriptorStager stager;
    check(stager.initialize(device, 64), "hud lock: staging heap");
    ID3D12Resource* reals[2] = {real[0].texture.Get(), real[1].texture.Get()};
    ID3D12Resource* gens[1] = {generated.texture.Get()};
    lock.createViews(device, stager, reals, gens, 1);
    // HUD: x 20..100, y 10..40, high-contrast stripes that never move. Everything else moves.
    auto hudAt = [](uint32_t x, uint32_t y) { return x >= 20 && x < 100 && y >= 10 && y < 40; };
    auto picture = [&](int shift, bool hud) {
        std::vector<uint8_t> px(size_t(W) * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t v;
                if (hud && hudAt(x, y)) v = ((x / 3) % 2) ? 230 : 25;
                else {
                    uint32_t h = uint32_t(int(x) + shift) * 374761393u + uint32_t(int(y) + shift / 3) * 668265263u;
                    h = (h ^ (h >> 13)) * 1274126177u;
                    v = uint8_t(40 + (h >> 25));
                }
                uint8_t* p = px.data() + (size_t(y) * W + x) * 4;
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
        return px;
    };
    pipeline::StateTracker states;
    std::vector<uint8_t> last;
    auto realFrame = [&](int i, bool hud, bool reset) {
        const unsigned parity = unsigned(i) & 1u;
        gpu.begin();
        last = picture(i * 5, hud);
        auto up = recordFill(gpu, real[parity], last, states.get(real[parity].texture.Get()), D3D12_RESOURCE_STATE_COMMON);
        states.set(real[parity].texture.Get(), D3D12_RESOURCE_STATE_COMMON);
        const bool ok = lock.update(gpu.list.Get(), states, parity, reset);
        return ok && gpu.submitAndWait();
    };
    auto generatedFrame = [&](unsigned held, std::vector<uint8_t>& out) {
        gpu.begin();
        auto up = recordFill(gpu, generated, std::vector<uint8_t>(size_t(W) * H * 4, 128), states.get(generated.texture.Get()), D3D12_RESOURCE_STATE_COMMON);
        states.set(generated.texture.Get(), D3D12_RESOURCE_STATE_COMMON);
        const bool ok = lock.compose(gpu.list.Get(), states, 0, held);
        auto rb = recordRead(gpu, generated, states.get(generated.texture.Get()));
        if (!ok || !gpu.submitAndWait()) return false;
        out = mapRead(generated, rb.Get());
        return true;
    };
    bool ran = true;
    int i = 0;
    for (; i < 60; ++i) ran = ran && realFrame(i, true, i == 0);
    check(ran, "hud lock: 60 real frames update the state");
    std::vector<uint8_t> out;
    check(generatedFrame(unsigned(i - 1) & 1u, out), "hud lock: a generated frame is composed");
    int hudHeld = 0, hudTotal = 0, bgKept = 0, bgTotal = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t k = (size_t(y) * W + x) * 4;
            if (hudAt(x, y)) { ++hudTotal; hudHeld += out[k] == last[k]; }
            else if (x >= 110 || y >= 50) { ++bgTotal; bgKept += out[k] == 128; }
        }
    std::printf("INFO hud lock: HUD pixels from the real frame %d/%d, moving pixels kept %d/%d\n", hudHeld, hudTotal, bgKept, bgTotal);
    checkValue(hudHeld >= hudTotal * 95 / 100, "hud lock: the HUD comes from the held real frame", hudHeld, std::format(">= {}", hudTotal * 95 / 100));
    checkValue(bgKept == bgTotal, "hud lock: the moving picture keeps the generated pixels", bgKept, bgTotal);
    // The HUD goes away (a menu closes): released on that very frame.
    check(realFrame(i, false, false), "hud lock: a frame without the HUD runs");
    check(generatedFrame(unsigned(i) & 1u, out), "hud lock: composed after the HUD went away");
    int released = 0;
    for (uint32_t y = 12; y < 38; ++y) for (uint32_t x = 22; x < 98; ++x) released += out[(size_t(y) * W + x) * 4] == 128;
    check(released >= 26 * 76 * 95 / 100, "hud lock: a HUD that moves or disappears is released at once (" + std::to_string(released) + "/1976)");
    // A reset starts over: nothing is locked right after it.
    ++i;
    check(realFrame(i, true, true), "hud lock: reset runs");
    check(generatedFrame(unsigned(i) & 1u, out), "hud lock: composed after a reset");
    int afterReset = 0;
    for (size_t k = 0; k < out.size(); k += 4) afterReset += out[k] != 128;
    checkValue(afterReset == 0, "hud lock: nothing is locked right after a reset", afterReset, 0);
    check(!gpu.debugErrors(), "hud lock: no D3D12 debug errors");
}
} // namespace

// PushFlow: RGBA8 frames with a textured square on a soft gradient.
std::vector<uint8_t> flowFrame(uint32_t w, uint32_t h, double squareX) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            const int base = 60 + int(40.0 * x / w) + int(30.0 * y / h);
            int r = base, g = base + 10, b = base + 20;
            const double dx = x - squareX, dy = double(y) - h * 0.35;
            if (dx >= 0 && dx < 40 && dy >= 0 && dy < 40) {
                const bool light = ((int(dx) / 8) + (int(dy) / 8)) % 2 == 0;
                r = light ? 235 : 40; g = light ? 200 : 30; b = light ? 60 : 20;
            }
            p[0] = uint8_t(r); p[1] = uint8_t(g); p[2] = uint8_t(b); p[3] = 255;
        }
    return px;
}
// Fine texture (stripes, 2-pixel checker, thin lines) panned sideways by `shift` pixels.
// Fine, non-repeating texture (hashed noise and 1-pixel lines) panning sideways, as in
// prepare_runtime.py's CPU check. A periodic pattern made the motion ambiguous (a shift of a whole
// period looks like no motion), so integer shifts and a pattern that never repeats.
std::vector<uint8_t> detailFrame(uint32_t w, uint32_t h, int shift) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const int64_t u = int64_t(x) - shift;
            const int64_t hsh = (u * 73856093) ^ (int64_t(y) * 19349663);
            const double noise = double((uint64_t(hsh ^ (hsh >> 13)) * 1274126177ull) & 0xFFFF) / 65535.0;   // unsigned: no overflow
            const bool line = ((u % 37) + 37) % 37 == 0 || y % 29 == 0;
            const double t = std::clamp(line ? 0.95 : 0.25 + 0.5 * noise, 0.0, 1.0);
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(t * 255.0 + 0.5); p[1] = uint8_t((t * 0.9 + 0.05) * 255.0 + 0.5); p[2] = uint8_t((t * 0.8 + 0.1) * 255.0 + 0.5); p[3] = 255;
        }
    return px;
}
double flowPsnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) { if (i % 4 == 3) continue; const double d = double(a[i]) - b[i]; se += d * d; ++n; }
    const double mse = n ? se / n : 0;
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}
// Reads `count` floats from a UAV buffer (left in UNORDERED_ACCESS).
std::vector<float> readFloats(Gpu& gpu, ID3D12Resource* buffer, uint64_t count) {
    std::vector<float> out;
    auto rb = makeBuffer(gpu.device.Get(), count * 4, D3D12_HEAP_TYPE_READBACK);
    if (!rb || !gpu.begin()) return out;
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = buffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gpu.list->ResourceBarrier(1, &b);
    gpu.list->CopyBufferRegion(rb.Get(), 0, buffer, 0, count * 4);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &b);
    if (!gpu.submitAndWait()) return out;
    float* mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return out;
    out.assign(mapped, mapped + count);
    const D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
    return out;
}
void testPushFlowRuntime(Gpu& gpu, const wchar_t* dir, bool strict);
// Network size per tier (no runtime needed): 4K never runs RIFE at 4K, small pictures stay full size
// (Fast stays at half), and every scale keeps at least the tier's pixel budget order.
void testPushFlowSizes() {
    const auto same = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    check(same(pushflow::networkScale(0, 640, 360), 0.5), "pushflow sizes: Fast at 640x360 is half size");
    check(same(pushflow::networkScale(1, 640, 360), 1.0) && same(pushflow::networkScale(2, 640, 360), 1.0), "pushflow sizes: small pictures run at full size");
    check(same(pushflow::networkScale(0, 3840, 2160), 0.25), "pushflow sizes: Fast at 4K is 960x540");
    check(same(pushflow::networkScale(1, 3840, 2160), 1.0 / 3.0), "pushflow sizes: Balanced at 4K is 1280x720");
    check(same(pushflow::networkScale(2, 3840, 2160), 0.5), "pushflow sizes: Quality at 4K is 1920x1080");
    check(same(pushflow::networkScale(2, 1920, 1080), 1.0) && same(pushflow::networkScale(1, 1920, 1080), 2.0 / 3.0), "pushflow sizes: 1080p");
}
void testPushFlow(Gpu& gpu) {
    wchar_t dir[1024]{}, full[1024]{};
    if (GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_TEST_RUNTIME_FP32", full, 1024)) { std::printf("INFO pushflow: full-precision models (diagnostic)\n"); testPushFlowRuntime(gpu, full, false); }
    if (!GetEnvironmentVariableW(L"VEYRA_PUSHFLOW_TEST_RUNTIME", dir, 1024)) { std::printf("SKIP pushflow: VEYRA_PUSHFLOW_TEST_RUNTIME not set\n"); return; }
    testPushFlowRuntime(gpu, dir, true);
}
void testPushFlowRuntime(Gpu& gpu, const wchar_t* dir, bool strict) {
    const auto checkOr = [&](bool ok, const std::string& label) { if (strict) check(ok, label); else std::printf("INFO %s %s\n", ok ? "ok" : "not ok", label.c_str()); };
    std::wstring why;
    checkOr(pushflow::runtimeInstalled(dir, &why), "pushflow: runtime installed");
    if (!pushflow::runtimeInstalled(dir)) return;
    constexpr uint32_t W = 256, H = 160;
    auto* device = gpu.device.Get();
    const auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    Plane a{pipeline::makeTexture(device, W, H, fmt, false), fmt, W, H, 4};
    Plane b{pipeline::makeTexture(device, W, H, fmt, false), fmt, W, H, 4};
    Plane out{pipeline::makeTexture(device, W, H, fmt, false), fmt, W, H, 4};
    checkOr(a.texture && b.texture && out.texture, "pushflow: textures created");
    const auto run = [&](const std::vector<uint8_t>& pa, const std::vector<uint8_t>& pb, float t, pushflow::PushFlowBackend& flow, double* ms) {
        std::vector<uint8_t> result;
        if (!gpu.begin()) return result;
        auto ua = recordFill(gpu, a, pa, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto ub = recordFill(gpu, b, pb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Wait for the pack list before resetting the allocator for the next one.
        if (!flow.pack(gpu.list.Get(), a.texture.Get(), b.texture.Get(), t) || !gpu.submitAndWait()) return result;
        const auto start = std::chrono::steady_clock::now();
        if (!flow.infer()) return result;
        if (!gpu.begin()) return result;
        D3D12_RESOURCE_BARRIER bar{}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar.Transition.pResource = out.texture.Get();
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        gpu.list->ResourceBarrier(1, &bar);
        if (!flow.resolve(gpu.list.Get(), a.texture.Get(), b.texture.Get(), out.texture.Get())) return result;
        auto rb = recordRead(gpu, out, D3D12_RESOURCE_STATE_COPY_DEST);
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        gpu.list->ResourceBarrier(1, &bar);
        // Back to COMMON for the next fill.
        D3D12_RESOURCE_BARRIER back[2]{};
        for (int i = 0; i < 2; ++i) {
            back[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; back[i].Transition.pResource = (i ? b : a).texture.Get();
            back[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            back[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; back[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        }
        gpu.list->ResourceBarrier(2, back);
        if (!gpu.submitAndWait()) return result;
        if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return mapRead(out, rb.Get());
    };
    for (uint32_t tier : {0u, 2u}) {
        pushflow::PushFlowBackend flow;
        const bool ready = flow.initialize(device, gpu.queue.Get(), dir, W, H, fmt, pushflow::Encoding::Encoded, tier);
        checkOr(ready, "pushflow: tier " + std::to_string(tier) + " initializes (" + flow.status().model + ")");
        if (!ready) continue;
        const auto first = flowFrame(W, H, 100), second = flowFrame(W, H, 116), middle = flowFrame(W, H, 108);
        double ms = 0;
        const auto same = run(first, first, 0.5f, flow, &ms);
        {
            const auto st = flow.status();
            const uint64_t plane = uint64_t(st.padW) * st.padH;
            const auto in = readFloats(gpu, flow.tensorBuffer(true), plane * flow.inputChannels());
            const auto outT = readFloats(gpu, flow.tensorBuffer(false), plane * 3);
            const uint32_t mx = uint32_t(110.0 * st.modelW / W), my = uint32_t((H * 0.35 + 10) * st.modelH / H);
            const uint64_t at = uint64_t(my) * st.padW + mx;
            if (!in.empty() && !outT.empty())
                std::printf("INFO pushflow tier %u tensor at square: in %.3f %.3f %.3f | %.3f %.3f %.3f | t %.3f; out %.3f %.3f %.3f\n", tier,
                    in[at], in[plane + at], in[2 * plane + at], in[3 * plane + at], in[4 * plane + at], in[5 * plane + at], in[6 * plane + at],
                    outT[at], outT[plane + at], outT[2 * plane + at]);
        }
        if (!same.empty()) {
            // Diagnostics: channel means and two sample pixels, output vs input.
            double mo[3]{}, mi[3]{};
            for (size_t i = 0; i < same.size(); i += 4) for (int k = 0; k < 3; ++k) { mo[k] += same[i + k]; mi[k] += first[i + k]; }
            const double n = double(same.size() / 4);
            const size_t sq = (size_t(H * 0.35 + 10) * W + 110) * 4, bg = (size_t(10) * W + 10) * 4;
            std::printf("INFO pushflow tier %u means out %.0f %.0f %.0f in %.0f %.0f %.0f; square out %d %d %d in %d %d %d; corner out %d %d %d in %d %d %d\n", tier,
                mo[0] / n, mo[1] / n, mo[2] / n, mi[0] / n, mi[1] / n, mi[2] / n, same[sq], same[sq + 1], same[sq + 2], first[sq], first[sq + 1], first[sq + 2],
                same[bg], same[bg + 1], same[bg + 2], first[bg], first[bg + 1], first[bg + 2]);
        }
        checkOr(!same.empty() && flowPsnr(same, first) > 30.0, std::format("pushflow: tier {} identical frames come back ({:.1f} dB, {:.0f} ms on {})", tier,
            same.empty() ? 0.0 : flowPsnr(same, first), ms, gpu.adapter));
        const auto mid = run(first, second, 0.5f, flow, &ms);
        std::vector<uint8_t> blend(first.size());
        for (size_t i = 0; i < blend.size(); ++i) blend[i] = uint8_t((int(first[i]) + second[i] + 1) / 2);
        const double flowDb = mid.empty() ? 0.0 : flowPsnr(mid, middle), blendDb = flowPsnr(blend, middle);
        checkOr(!mid.empty() && flowDb > blendDb + 2.0, std::format("pushflow: tier {} moving square lands in the middle ({:.1f} dB vs blend {:.1f} dB)", tier, flowDb, blendDb));
        const auto early = run(first, second, 0.25f, flow, nullptr);
        const auto quarter = flowFrame(W, H, 104);
        checkOr(!early.empty() && flowPsnr(early, quarter) > flowPsnr(early, middle), "pushflow: tier " + std::to_string(tier) + " t = 0.25 is closer to the quarter position");
        // Fine texture panning sideways: the Fast tier rebuilds it from the full-size frames.
        const auto d0 = detailFrame(W, H, 0), d1 = detailFrame(W, H, 8), dMid = detailFrame(W, H, 4);
        std::vector<uint8_t> dBlend(d0.size());
        for (size_t i = 0; i < dBlend.size(); ++i) dBlend[i] = uint8_t((int(d0[i]) + d1[i] + 1) / 2);
        const auto detail = run(d0, d1, 0.5f, flow, nullptr);
        const double detailDb = detail.empty() ? 0.0 : flowPsnr(detail, dMid), detailBlendDb = flowPsnr(dBlend, dMid);
        const std::string detailText = std::format("pushflow: tier {} fine texture in motion ({:.1f} dB vs blend {:.1f} dB, rebuild={}, precision={}, self-check {:.1f} dB)",
            tier, detailDb, detailBlendDb, flow.status().rebuild ? 1 : 0, flow.status().precision, flow.status().selfCheckDb);
        // What this guards is the Fast tier's full-resolution rebuild (network at half size). At full
        // size the network's own picture is used unchanged, and on per-pixel noise panning 8 pixels
        // RIFE 4.26 does not track it (14.6 dB vs blend 15.2 on WARP, 2026-11-17; real pictures pass
        // the moving-square check above). Reported, not required, until real-content tests replace it.
        if (tier == 0) checkOr(!detail.empty() && detailDb > detailBlendDb + 2.0, detailText);
        else std::printf("INFO %s\n", detailText.c_str());
        if (tier == 0) checkOr(flow.status().rebuild, "pushflow: the Fast tier rebuilds at full size");
        const auto tel = flow.telemetry();
        checkOr(tel.inferences > 0 && tel.inferP50Ms > 0, std::format("pushflow: telemetry ({} inferences, p50 {:.1f} ms)", tel.inferences, tel.inferP50Ms));
        checkOr(!gpu.debugErrors(), "pushflow: no debug layer errors");
    }
    checkOr(!pushflow::crashedLastTime(), "pushflow: the crash marker is gone after a normal close");
}

namespace {
// Crash and hang reporters: without them a crash leaves only a truncated log.
LONG WINAPI reportCrash(EXCEPTION_POINTERS* info) {
    const unsigned code = info && info->ExceptionRecord ? unsigned(info->ExceptionRecord->ExceptionCode) : 0u;
    std::printf("FAIL %s: crashed got=exception 0x%08X want=no exception\n", currentTest.load(), code);
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
std::atomic<int64_t> testStartedMs{0};
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct TestCase { const char* name; std::function<void(Gpu&)> run; };
} // namespace

int main(int argc, char** argv) {
    // Unbuffered: a crash or a killed process keeps every line printed so far.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(reportCrash);
    const std::vector<TestCase> tests = {
        {"compare", [](Gpu& g) { testCompare(g); }},
        {"compare-hdr", [](Gpu& g) { testCompareHdr(g); }},
        {"side-by-side", [](Gpu& g) { testCompareSideBySide(g, false); }},
        {"side-by-side-hdr", [](Gpu& g) { testCompareSideBySide(g, true); }},
        {"vertical", [](Gpu& g) { testCompareVertical(g, false); }},
        {"vertical-hdr", [](Gpu& g) { testCompareVertical(g, true); }},
        {"scene-overlay", [](Gpu& g) { testStreamOverlay(g, false); }},
        {"scene-overlay-hdr", [](Gpu& g) { testStreamOverlay(g, true); }},
        {"canvas-convert", [](Gpu& g) { testCanvasConvert(g); }},
        {"live-layers", [](Gpu& g) { testLiveLayers(g, false); }},
        {"live-layers-hdr", [](Gpu& g) { testLiveLayers(g, true); }},
        {"label-scale", [](Gpu&) { testLabelScale(); }},
        {"skip-rule", [](Gpu&) { testCompareSkipRule(); }},
        {"raw-split", [](Gpu& g) { testCompareRaw(g, false, engine::CompareLayout::Split); }},
        {"raw-split-hdr", [](Gpu& g) { testCompareRaw(g, true, engine::CompareLayout::Split); }},
        {"raw-side-by-side", [](Gpu& g) { testCompareRaw(g, false, engine::CompareLayout::SideBySide); }},
        {"badges", [](Gpu& g) { testCompareBadges(g, false); }},
        {"badges-hdr", [](Gpu& g) { testCompareBadges(g, true); }},
        {"hud-sampler", [](Gpu& g) { testHudSampler(g); }},
        {"pipeline-probe", [](Gpu& g) { testPipelineProbe(g); }},
        {"hud-lock", [](Gpu& g) { testHudLock(g); }},
        {"stream-change", [](Gpu& g) { testStreamChange(g); }},
        {"pushflow-sizes", [](Gpu&) { testPushFlowSizes(); }},
        {"pushflow", [](Gpu& g) { testPushFlow(g); }},
    };
    int repeat = 1;
    long long timeoutS = 1800;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const std::string next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--repeat" && !next.empty()) { repeat = std::clamp(std::atoi(next.c_str()), 1, 100000); ++i; }
        else if (a == "--test-timeout" && !next.empty()) { timeoutS = std::max(1LL, std::atoll(next.c_str())); ++i; }
        else if (a == "--only" && !next.empty()) {
            for (size_t from = 0; from <= next.size();) {
                const size_t comma = std::min(next.find(',', from), next.size());
                if (comma > from) only.push_back(next.substr(from, comma - from));
                from = comma + 1;
            }
            ++i;
        } else if (a == "--list") {
            for (const auto& t : tests) std::printf("%s\n", t.name);
            return 0;
        } else {
            std::printf("usage: veyra_push_gpu_tests [--repeat N] [--only name[,name...]] [--test-timeout seconds] [--list]\n");
            return 2;
        }
    }
    for (const auto& name : only) {
        if (std::none_of(tests.begin(), tests.end(), [&](const TestCase& t) { return name == t.name; })) {
            std::printf("FAIL setup: unknown test name got=%s want=one of --list\n", name.c_str());
            return 2;
        }
    }
    Gpu gpu;
    if (!gpu.create()) { std::printf("FAIL setup: no D3D12 device got=none want=a hardware or WARP device\n"); return 1; }
    std::printf("INFO adapter %s class=%s debug=%d\n", gpu.adapter.c_str(), gpu.software ? "software" : "hardware", gpu.debug ? 1 : 0);
    // Hang reporter: one test running longer than the limit ends the program with its name.
    std::thread([timeoutS] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t started = testStartedMs.load();
            if (started && nowMs() - started > timeoutS * 1000) {
                std::printf("FAIL %s: timed out got=still running after %lld s want=finished\n", currentTest.load(), timeoutS);
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }).detach();
    std::vector<std::pair<std::string, int>> failedRuns;   // test name, runs it failed in
    int failedRunCount = 0;
    for (int run = 1; run <= repeat; ++run) {
        failedThisRun.clear();
        const int failuresBefore = failures;
        const int64_t runStart = nowMs();
        for (const auto& t : tests) {
            if (!only.empty() && std::find(only.begin(), only.end(), std::string(t.name)) == only.end()) continue;
            currentTest = t.name;
            std::printf("RUN %s\n", t.name);
            const int64_t start = nowMs();
            testStartedMs = start;
            t.run(gpu);
            testStartedMs = 0;
            std::printf("TIME %s %lld ms\n", t.name, static_cast<long long>(nowMs() - start));
        }
        currentTest = "summary";
        std::string names;
        for (const auto& name : failedThisRun) {
            names += (names.empty() ? "" : ", ") + name;
            auto it = std::find_if(failedRuns.begin(), failedRuns.end(), [&](const auto& f) { return f.first == name; });
            if (it == failedRuns.end()) failedRuns.emplace_back(name, 1); else ++it->second;
        }
        if (!failedThisRun.empty()) ++failedRunCount;
        std::printf("SUMMARY run %d/%d: %d failed checks in %zu tests%s%s (%lld ms, %s adapter)\n", run, repeat, failures - failuresBefore, failedThisRun.size(),
                    names.empty() ? "" : ": ", names.c_str(), static_cast<long long>(nowMs() - runStart), gpu.software ? "software" : "hardware");
    }
    std::string all;
    for (const auto& [name, runs] : failedRuns) all += std::format("{}{} ({}/{} runs)", all.empty() ? "" : ", ", name, runs, repeat);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    if (repeat > 1 || failures) std::printf("SUMMARY %d of %d runs failed%s%s\n", failedRunCount, repeat, all.empty() ? "" : "; failed tests: ", all.c_str());
    return failures ? 1 : 0;
}
