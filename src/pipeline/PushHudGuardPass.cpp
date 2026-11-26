// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#include "veyra/pipeline/PushHudGuardPass.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

namespace veyra::pipeline {
namespace {
constexpr DXGI_FORMAT kChangeFormat = DXGI_FORMAT_R8G8_UNORM, kTileFormat = DXGI_FORMAT_R32_UINT, kStateFormat = DXGI_FORMAT_R32_UINT,
                      kMaskFormat = DXGI_FORMAT_R8_UINT;
// Descriptor layouts (SRVs first, then UAVs, per set):
//   change  per parity { t0 B, t1 A, u0 change, u1 tiles, u2 counters }
//   update  { t0 change, t1 tiles, t2 mask, u0 state }
//   mask    { t0 state, u0 mask, u1 counters }
//   clean   per parity { t0 B, t1 mask, u0 cleaned }
//   compose per (slot, source) { t0 A, t1 B, t2 mask, t3 change, t4 cleaned copy (B again without pre-clean, never read),
//   u0 generated }; source 0 / 1: B = real[source], 2: the held copy
constexpr UINT kChangeStride = 5, kCleanStride = 3, kComposeStride = 6, kSources = 3;
constexpr UINT kCounterBytes = 32;   // protect, clean, centre protect, centre clean, aim protect, aim clean, aim run max, spare
float bitsOf(uint32_t v) { return std::bit_cast<float>(v); }
ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heap, bool uav, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type = heap;
    D3D12_RESOURCE_DESC bd{};bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width = bytes;bd.Height = 1;bd.DepthOrArraySize = 1;bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r)))) r.Reset();
    return r;
}
void makeRawUav(ID3D12Device* device, ID3D12Resource* buffer, UINT words, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = words;uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(buffer, nullptr, &uav, handle);
}
int64_t steadyMsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

std::string PushHudGuardPass::describe() const {
    return std::format("HUD guard {}x{} mode={} preClean={} stillFrames={} rearmFrames={} nearFrames={} edgeFrames={} gate={} holdFrames={} "
                       "surroundShare={} reach={} grow={} fillReach={} still={:.4f} hold={:.4f} move={:.4f} edge={:.4f} anchorFrames={}",
                       width_, height_, outputMode_, preClean() ? 1 : 0, params_.stillFrames, params_.rearmFrames, params_.nearFrames, params_.edgeFrames,
                       params_.gate, params_.holdFrames, params_.surroundShare, params_.reach, params_.grow, params_.fillReach, params_.still, params_.hold,
                       params_.move, params_.edge, params_.anchorFrames);
}

bool PushHudGuardPass::initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, unsigned outputMode, bool preCleanOn,
                                  PushHudGuardParams rules) {
    if (!device || !width || !height || format == DXGI_FORMAT_UNKNOWN) return false;
    if (rules.reach > kPushHudGuardTile || rules.stillFrames == 0 || rules.rearmFrames == 0) return false;   // the tile early-out needs reach <= 16
    width_ = width; height_ = height; format_ = format; outputMode_ = outputMode;
    params_ = pushHudGuardModeParams(rules, outputMode);   // HDR10: the thresholds x pqScale (review SHOULD 2)
    tilesX_ = (width_ + kPushHudGuardTile - 1) / kPushHudGuardTile; tilesY_ = (height_ + kPushHudGuardTile - 1) / kPushHudGuardTile;
    change_ = makeTexture(device, width_, height_, kChangeFormat, true);
    tiles_ = makeTexture(device, tilesX_, tilesY_, kTileFormat, true);
    state_ = makeTexture(device, width_, height_, kStateFormat, true);
    mask_ = makeTexture(device, width_, height_, kMaskFormat, true);
    counters_ = makeBuffer(device, kCounterBytes, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
    readback_ = makeBuffer(device, kCounterBytes * kReadbackSlots, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!change_ || !tiles_ || !state_ || !mask_ || !counters_ || !readback_) return false;
    change_->SetName(L"PushFPS HUD Guard change"); tiles_->SetName(L"PushFPS HUD Guard tiles"); state_->SetName(L"PushFPS HUD Guard state");
    mask_->SetName(L"PushFPS HUD Guard mask"); counters_->SetName(L"PushFPS HUD Guard counters"); readback_->SetName(L"PushFPS HUD Guard readback");
    if (preCleanOn) {
        cleaned_ = makeTexture(device, width_, height_, format_, true);
        if (!cleaned_) veyra::log::warn("hud-guard", "the cleaned copy could not be created: compose only (no pre-clean)");
        else cleaned_->SetName(L"PushFPS HUD Guard cleaned copy");
    }
    std::vector<uint8_t> cs;
    if (!change_pass_.loadShader("PushHudGuardChange.dxil", cs) || !change_pass_.create(device, cs, 2 * kChangeStride, 2, 3, kConstants)) return false;
    if (!update_pass_.loadShader("PushHudGuardUpdate.dxil", cs) || !update_pass_.create(device, cs, 4, 3, 1, kConstants)) return false;
    if (!mask_pass_.loadShader("PushHudGuardMask.dxil", cs) || !mask_pass_.create(device, cs, 3, 1, 2, kConstants)) return false;
    if (cleaned_ && (!clean_pass_.loadShader("PushHudGuardClean.dxil", cs) || !clean_pass_.create(device, cs, 2 * kCleanStride, 2, 1, kConstants))) return false;
    if (!compose_pass_.loadShader("PushHudGuardCompose.dxil", cs) || !compose_pass_.create(device, cs, kMaxGenerated * kSources * kComposeStride, 5, 1, kConstants))
        return false;
    veyra::log::info("hud-guard", std::format("created {} textures={:.1f} MB", describe(), double(textureBytes(width_, height_, preClean())) / 1048576.0));
    return true;
}

void PushHudGuardPass::createViews(ID3D12Device* device, DescriptorStager& stager, ID3D12Resource* const real[2], ID3D12Resource* const* generated,
                                   unsigned generatedCount, ID3D12Resource* held) {
    real_[0] = real[0]; real_[1] = real[1]; real_[2] = held;
    generatedCount_ = std::min<unsigned>(generatedCount, kMaxGenerated);
    for (unsigned i = 0; i < generatedCount_; ++i) generated_[i] = generated[i];
    for (unsigned parity = 0; parity < 2; ++parity) {
        const UINT base = parity * kChangeStride;
        stager.stageSrv(real_[parity], nullptr, change_pass_.heap.Get(), base);
        stager.stageSrv(real_[1 - parity], nullptr, change_pass_.heap.Get(), base + 1);
        makeUav(device, change_.Get(), kChangeFormat, cpuHandleOf(change_pass_, base + 2));
        makeUav(device, tiles_.Get(), kTileFormat, cpuHandleOf(change_pass_, base + 3));
        makeRawUav(device, counters_.Get(), kCounterBytes / 4, cpuHandleOf(change_pass_, base + 4));
        if (cleaned_) {
            const UINT c = parity * kCleanStride;
            stager.stageSrv(real_[parity], nullptr, clean_pass_.heap.Get(), c);
            stager.stageSrv(mask_.Get(), nullptr, clean_pass_.heap.Get(), c + 1);
            makeUav(device, cleaned_.Get(), format_, cpuHandleOf(clean_pass_, c + 2));
        }
    }
    stager.stageSrv(change_.Get(), nullptr, update_pass_.heap.Get(), 0);
    stager.stageSrv(tiles_.Get(), nullptr, update_pass_.heap.Get(), 1);
    stager.stageSrv(mask_.Get(), nullptr, update_pass_.heap.Get(), 2);
    makeUav(device, state_.Get(), kStateFormat, cpuHandleOf(update_pass_, 3));
    stager.stageSrv(state_.Get(), nullptr, mask_pass_.heap.Get(), 0);
    makeUav(device, mask_.Get(), kMaskFormat, cpuHandleOf(mask_pass_, 1));
    makeRawUav(device, counters_.Get(), kCounterBytes / 4, cpuHandleOf(mask_pass_, 2));
    for (unsigned slot = 0; slot < generatedCount_; ++slot)
        for (unsigned source = 0; source < kSources; ++source) {
            ID3D12Resource* b = real_[source];
            ID3D12Resource* a = source == kHeld ? real_[kHeld] : real_[1 - source];
            if (!a || !b || !generated_[slot]) continue;
            const UINT base = (slot * kSources + source) * kComposeStride;
            stager.stageSrv(a, nullptr, compose_pass_.heap.Get(), base);
            stager.stageSrv(b, nullptr, compose_pass_.heap.Get(), base + 1);
            stager.stageSrv(mask_.Get(), nullptr, compose_pass_.heap.Get(), base + 2);
            stager.stageSrv(change_.Get(), nullptr, compose_pass_.heap.Get(), base + 3);
            stager.stageSrv(cleaned_ ? cleaned_.Get() : b, nullptr, compose_pass_.heap.Get(), base + 4);
            makeUav(device, generated_[slot], format_, cpuHandleOf(compose_pass_, base + 5));
        }
}

bool PushHudGuardPass::update(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool reset) {
    if (!list || parity > 1 || !real_[0] || !real_[1]) return false;
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, real_[1 - parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, change_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tracker.transition(list, tiles_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tracker.transition(list, counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {   // 1: change, contrast, moving pixels per tile; clears the counters.
        float c[kConstants]{};
        c[0] = bitsOf(width_); c[1] = bitsOf(height_); c[2] = bitsOf(reset ? 1u : 0u); c[3] = bitsOf(outputMode_);
        c[4] = params_.move; c[5] = params_.still;
        const UINT base = parity * kChangeStride;
        change_pass_.bind(list, c, gpuHandleOf(change_pass_, base).ptr, gpuHandleOf(change_pass_, base + 2).ptr);
        list->Dispatch(tilesX_, tilesY_, 1);   // one group per 16 x 16 tile
    }
    tracker.uavBarrier(list, change_.Get());
    tracker.uavBarrier(list, tiles_.Get());
    tracker.uavBarrier(list, counters_.Get());
    tracker.transition(list, change_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, tiles_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {   // 2: the state, from the previous frame's mask.
        float c[kConstants]{};
        c[0] = bitsOf(width_); c[1] = bitsOf(height_); c[2] = bitsOf(reset ? 1u : 0u); c[3] = bitsOf(tilesX_);
        c[4] = bitsOf(tilesY_); c[5] = bitsOf(params_.reach);
        c[6] = bitsOf(uint32_t(std::floor(double(params_.surroundShare) * 1000.0 + 0.5))); c[7] = bitsOf(params_.holdFrames);
        c[8] = params_.still; c[9] = params_.hold; c[10] = params_.move; c[11] = params_.edge;
        c[12] = bitsOf(params_.recentFrames); c[13] = bitsOf(params_.anchorMargin);
        update_pass_.bind(list, c, gpuHandleOf(update_pass_, 0).ptr, gpuHandleOf(update_pass_, 3).ptr);
        list->Dispatch(tilesX_, tilesY_, 1);   // group = tile: the early-out reads the group's own tile and its neighbours
    }
    tracker.uavBarrier(list, state_.Get());
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {   // 3: the mask and the evidence counters.
        float c[kConstants]{};
        c[0] = bitsOf(width_); c[1] = bitsOf(height_); c[2] = bitsOf(params_.stillFrames); c[3] = bitsOf(params_.rearmFrames);
        c[4] = bitsOf(params_.nearFrames); c[5] = bitsOf(params_.edgeFrames); c[6] = bitsOf(params_.gate); c[7] = bitsOf(params_.anchorFrames); c[8] = bitsOf(params_.anchorFlatMax);
        mask_pass_.bind(list, c, gpuHandleOf(mask_pass_, 0).ptr, gpuHandleOf(mask_pass_, 1).ptr);
        list->Dispatch(tilesX_, tilesY_, 1);
    }
    tracker.uavBarrier(list, mask_.Get());
    tracker.uavBarrier(list, counters_.Get());
    // The counters into this frame's readback slot (read a few frames later, once its fence completed).
    const unsigned slot = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % kReadbackSlots;
    tracker.transition(list, counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback_.Get(), UINT64(slot) * kCounterBytes, counters_.Get(), 0, kCounterBytes);
    slotFence_[slot] = 0;   // not readable until submitted() names its fence
    pendingSlot_ = slot;
    tracker.transition(list, counters_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, state_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, change_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, tiles_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[1 - parity], D3D12_RESOURCE_STATE_COMMON);
    ++updates_;
    return true;
}

bool PushHudGuardPass::clean(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity) {
    if (!list || parity > 1 || !cleaned_ || !real_[parity]) return false;
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, cleaned_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    float c[kConstants]{};
    c[0] = bitsOf(width_); c[1] = bitsOf(height_); c[2] = bitsOf(params_.grow); c[3] = bitsOf(params_.fillReach);
    const UINT base = parity * kCleanStride;
    clean_pass_.bind(list, c, gpuHandleOf(clean_pass_, base).ptr, gpuHandleOf(clean_pass_, base + 2).ptr);
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    tracker.uavBarrier(list, cleaned_.Get());
    tracker.transition(list, cleaned_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, real_[parity], D3D12_RESOURCE_STATE_COMMON);
    ++cleans_;
    return true;
}

bool PushHudGuardPass::compose(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned parity, float t, unsigned source,
                               bool cleanedPair) {
    const unsigned s = source == kHeld ? kHeld : parity;
    if (!list || slot >= generatedCount_ || parity > 1 || !generated_[slot] || !real_[s] || (s != kHeld && !real_[1 - s])) return false;
    ID3D12Resource* b = real_[s];
    ID3D12Resource* a = s == kHeld ? real_[kHeld] : real_[1 - s];
    tracker.transition(list, a, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, change_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The cleaned copy is of B only for a pair whose generator input it was (the graph says so), never for the held copy.
    const bool withCleaned = cleanedPair && cleaned_ && s != kHeld;
    if (withCleaned) tracker.transition(list, cleaned_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, generated_[slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    float c[kConstants]{};
    c[0] = bitsOf(width_); c[1] = bitsOf(height_); c[2] = bitsOf(params_.grow); c[3] = bitsOf(outputMode_);
    c[4] = s == kHeld ? 1.0f : std::clamp(t, 0.0f, 1.0f); c[5] = params_.move; c[6] = params_.backgroundMatch;
    c[8] = bitsOf(withCleaned ? 1u : 0u);
    const UINT base = (slot * kSources + s) * kComposeStride;
    compose_pass_.bind(list, c, gpuHandleOf(compose_pass_, base).ptr, gpuHandleOf(compose_pass_, base + 5).ptr);
    list->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    tracker.uavBarrier(list, generated_[slot]);
    tracker.transition(list, generated_[slot], D3D12_RESOURCE_STATE_COMMON);
    if (withCleaned) tracker.transition(list, cleaned_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, change_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, mask_.Get(), D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, b, D3D12_RESOURCE_STATE_COMMON);
    if (a != b) tracker.transition(list, a, D3D12_RESOURCE_STATE_COMMON);
    ++composes_;
    return true;
}

void PushHudGuardPass::submitted(uint64_t fence) {
    if (pendingSlot_ < kReadbackSlots) slotFence_[pendingSlot_] = fence;
    pendingSlot_ = kReadbackSlots;
}

void PushHudGuardPass::report(uint64_t completedFence, double gpuMs, const char* preCleanState) {
    // The newest completed slot not read yet.
    unsigned best = kReadbackSlots;
    for (unsigned i = 0; i < kReadbackSlots; ++i)
        if (slotFence_[i] && slotFence_[i] <= completedFence && slotFence_[i] > newestReadFence_ && (best == kReadbackSlots || slotFence_[i] > slotFence_[best])) best = i;
    if (best < kReadbackSlots) {
        void* data = nullptr;
        const D3D12_RANGE range{SIZE_T(best) * kCounterBytes, SIZE_T(best + 1) * kCounterBytes};
        if (SUCCEEDED(readback_->Map(0, &range, &data)) && data) {
            uint32_t v[8]{};
            std::memcpy(v, static_cast<const uint8_t*>(data) + SIZE_T(best) * kCounterBytes, sizeof(v));
            const D3D12_RANGE none{0, 0};
            readback_->Unmap(0, &none);
            lastCounts_.protect = v[0]; lastCounts_.clean = v[1]; lastCounts_.centreProtect = v[2]; lastCounts_.centreClean = v[3];
            lastCounts_.aimProtect = v[4]; lastCounts_.aimClean = v[5]; lastCounts_.aimRunMax = v[6];
            newestReadFence_ = slotFence_[best];
        }
    }
    const int64_t now = steadyMsNow();
    if (lastReportMs_ && now - lastReportMs_ < 1000) return;
    const bool first = lastReportMs_ == 0;
    lastReportMs_ = now;
    if (first) { updates_ = cleans_ = composes_ = 0; return; }   // the first call only starts the clock
    const double pixels = double(width_) * double(height_);
    veyra::log::info("hud-guard", std::format(
        "state={} updates={} cleans={} composes={} preClean={} protectPx={} cleanPx={} protectPerMille={:.3f} cleanPerMille={:.3f} centreProtectPx={} "
        "centreCleanPx={} aimProtectPx={} aimCleanPx={} aimRunMax={} gpuHudGuardMs={} (counts: newest completed real frame, before the 1-px growth; "
        "centre = middle tenth each way, aim = 128 x 128 px at the exact centre; gpuHudGuardMs: update + pre-clean in the real frame's base list, which "
        "every frame of the pair waits for: every frame is shown up to that much later; each generated frame's compose is inside gpu_fg1/fg2)",
        updates_ ? (composes_ ? "executing" : "updating-no-generated-frames") : "idle", updates_, cleans_, composes_, preCleanState ? preCleanState : "?",
        lastCounts_.protect, lastCounts_.clean, pushHudGuardPerMille(lastCounts_.protect, uint64_t(pixels)), pushHudGuardPerMille(lastCounts_.clean, uint64_t(pixels)),
        lastCounts_.centreProtect, lastCounts_.centreClean, lastCounts_.aimProtect, lastCounts_.aimClean, lastCounts_.aimRunMax,
        gpuMs >= 0 ? std::format("{:.3f}", gpuMs) : std::string("-1")));
    updates_ = cleans_ = composes_ = 0;
}

}  // namespace veyra::pipeline
