// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg": see include/veyra/pipeline/PushWarpPass.h and include/veyra/pipeline/PushWarp.h.
#include "veyra/pipeline/PushWarpPass.h"
#include "veyra/pipeline/PushPredictPass.h"
#include "veyra/pipeline/PushPredictCheck.h"
#include "veyra/pipeline/PushOwnFrame.h"
#include "veyra/RuntimePaths.h"
#include "veyra/core/SceneCadenceAnalyzer.h"
#include "veyra/engine/PushLowDelay.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace veyra::pipeline {
namespace {
constexpr UINT kPerSlot = 5;            // flow SRV, trust SRV, previous flow SRV, previous trust SRV, output UAV
constexpr UINT kCounterBytes = 16 + kPushWarpMotionBins * 4 + 8;   // four counters, the motion histogram (PushWarp.h), overlay pixels (all, centre)
constexpr UINT kCutBytes = 64;          // per slot: VfgShotChange.hlsl's flags (offset 0), scores (16) and count (48) at parity 0
constexpr UINT kCutConstants = 12;      // VfgShotChange.hlsl's cbuffer
bool warpFormat(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM; }
uint32_t bitsOf(float v) { return std::bit_cast<uint32_t>(v); }
ComPtr<ID3D12Resource> makeWarpBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
void warpTransition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
void warpUavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r; list->ResourceBarrier(1, &b);
}
// Objects a submission that never finished may still use: never released.
std::vector<ComPtr<IUnknown>>& warpKeptForever() { static std::vector<ComPtr<IUnknown>> kept; return kept; }

// Marker file of a running self-check (PushFlowCheckBlock's rules, its own file).
std::filesystem::path warpMarkerPath() { return runtime::localDataDirectory() / L"lowdelay-warp-check.txt"; }
std::string readWarpMarker() {
    std::ifstream in(warpMarkerPath(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.substr(0, 512);
}
void writeWarpMarker(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(warpMarkerPath().parent_path(), ec);
    std::ofstream out(warpMarkerPath(), std::ios::binary | std::ios::trunc);
    out << text << "\n";
    out.flush();   // before the GPU work: a crash inside it must leave the file behind
}
void clearWarpMarker() { std::error_code ec; std::filesystem::remove(warpMarkerPath(), ec); }

struct WarpCheckResult { bool ok = false; std::string text; double gpuMs = -1.0, playMs = -1.0; };
std::mutex& warpCheckMutex() { static std::mutex m; return m; }
std::map<std::tuple<uint32_t, int32_t, int, uint32_t, uint32_t, uint32_t, uint32_t>, WarpCheckResult>& warpCheckCache() {
    static std::map<std::tuple<uint32_t, int32_t, int, uint32_t, uint32_t, uint32_t, uint32_t>, WarpCheckResult> cache;
    return cache;
}
bool& warpCheckFailedThisProcess() { static bool failed = false; return failed; }
std::map<std::tuple<uint32_t, int32_t, int, uint32_t, uint32_t, uint32_t, uint32_t>, int>& warpBudgetMisses() {
    static std::map<std::tuple<uint32_t, int32_t, int, uint32_t, uint32_t, uint32_t, uint32_t>, int> misses;
    return misses;
}

// Any float that is 0 or normal in half precision, rounded to nearest (the self-check's field values are small integers).
uint16_t warpHalf(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
    if ((bits & 0x7FFFFFFFu) == 0 || exponent <= 0) return uint16_t(sign);
    if (exponent >= 31) return uint16_t(sign | 0x7BFFu);
    const uint32_t mantissa = bits & 0x7FFFFFu;
    uint32_t half = sign | (uint32_t(exponent) << 10) | (mantissa >> 13);
    if ((mantissa & 0x1FFFu) > 0x1000u || ((mantissa & 0x1FFFu) == 0x1000u && (half & 1u))) ++half;
    return uint16_t(half);
}
}  // namespace

PushWarpPass::PushWarpPass() = default;
PushWarpPass::~PushWarpPass() = default;

D3D12_CPU_DESCRIPTOR_HANDLE PushWarpPass::cpu(UINT index) const {
    return {heap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * increment_};
}
D3D12_GPU_DESCRIPTOR_HANDLE PushWarpPass::gpu(UINT index) const {
    return {heap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * increment_};
}
std::filesystem::path PushWarpPass::checkMarker() { return warpMarkerPath(); }

namespace {
// Minutes since 1601 at which Windows started (review 2 SHOULD 4: a failed marker blocks within one boot only).
std::int64_t warpBootMinute() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const std::int64_t now100ns = std::int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return now100ns / 600000000LL - std::int64_t(GetTickCount64() / 60000ULL);
}
std::string trimmedWarpMarker() {
    std::string m = readWarpMarker();
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    return m;
}
}  // namespace
std::int64_t PushWarpPass::checkBootMinute() { return warpBootMinute(); }
bool PushWarpPass::priorSuspect() { return trimmedWarpMarker() == "suspect " + PushPredictPass::flowCheckIdentity(); }
void PushWarpPass::noteSessionEnd(int action, const char* why) {
    const auto a = static_cast<engine::PushPlayFaultAction>(action);
    if (a == engine::PushPlayFaultAction::None) return;
    const std::string identity = PushPredictPass::flowCheckIdentity();
    std::lock_guard lock(warpCheckMutex());
    if (a == engine::PushPlayFaultAction::ClearSuspect) {
        clearWarpMarker();
        log::info("push-warp", "a session with PushWarp ended normally: the earlier \"suspect\" mark is cleared");
        return;
    }
    if (a == engine::PushPlayFaultAction::WriteSuspect) {
        writeWarpMarker("suspect " + identity);
        log::warn("push-warp", std::format("the session ended ({}) while PushWarp was running: marked \"suspect\" (a second time in a row blocks PushWarp "
                                           "until the next start of Windows or program update; {})", why, warpMarkerPath().string()));
        return;
    }
    warpCheckFailedThisProcess() = true;
    warpCheckCache().clear();
    writeWarpMarker(pushWarpBootMarker("failed", identity, warpBootMinute()));
    log::error("push-warp", std::format("the session ended ({}) while PushWarp was running, the second time in a row: PushWarp is blocked until the next "
                                        "start of Windows or program update (or until {} is deleted)", why, warpMarkerPath().string()));
}

bool PushWarpPass::createPipeline(ID3D12Device* device, const char* shader, ComPtr<ID3D12PipelineState>& pso) {
    std::vector<uint8_t> cs;
    if (!loadShaderBytes(shader, cs)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = root_.Get(); pipeline.CS = {cs.data(), cs.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)))) {
        log::error("push-warp", std::format("pipeline for {} not created", shader));
        return false;
    }
    return true;
}

// SHOULD 5: VFG's own GPU cut test (the same shader and thresholds), on PushWarp's A and B.
bool PushWarpPass::createCutTest(ID3D12Device* device) {
    std::vector<uint8_t> cs;
    if (!loadShaderBytes("VfgShotChange.dxil", cs)) return false;
    std::array<D3D12_ROOT_PARAMETER1, 4> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants.Num32BitValues = kCutConstants;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[2].Descriptor.ShaderRegister = 1;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[3].Descriptor.ShaderRegister = 0;
    for (auto& p : parameters) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = UINT(parameters.size()); desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&cutRoot_)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = cutRoot_.Get(); pipeline.CS = {cs.data(), cs.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&cutPso_)))) return false;
    cut_ = makeWarpBuffer(device, uint64_t(kSlots) * kCutBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    return cut_ != nullptr;
}
// Leaves cut_ in COMMON; A and B (buffers) are read through root SRVs (promoted from COMMON, as the passes below).
void PushWarpPass::recordCutTest(ID3D12GraphicsCommandList* list, const Record& r, uint32_t W, uint32_t H) {
    warpTransition(list, cut_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const core::SceneCadenceAnalyzer::Config scene{};
    const std::array<float, 6> thresholds = {float(scene.sceneCutSadThreshold), float(scene.sceneCutHistogramThreshold), float(scene.replacementSadThreshold),
                                             float(scene.replacementHistogramThreshold), float(scene.flashSadThreshold), float(scene.flashHistogramThreshold)};
    std::array<uint32_t, kCutConstants> c = {W, H, r.rowPitch, format_ == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u, 0u, 1u};
    std::memcpy(c.data() + 6, thresholds.data(), sizeof(thresholds));
    list->SetComputeRootSignature(cutRoot_.Get());
    list->SetPipelineState(cutPso_.Get());
    list->SetComputeRoot32BitConstants(0, kCutConstants, c.data(), 0);
    list->SetComputeRootShaderResourceView(1, r.previous->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, r.current->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, cut_->GetGPUVirtualAddress() + uint64_t(r.slot) * kCutBytes);
    list->Dispatch(1, 1, 1);
    warpUavBarrier(list, cut_.Get());
    warpTransition(list, cut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

bool PushWarpPass::initialize(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t flowWidth, uint32_t flowHeight,
                              double budgetMs) {
    ready_ = false; created_ = false;
    if (!device || width < 2 || height < 2 || !warpFormat(format) || flowWidth == 0 || flowHeight == 0 || flowWidth > width || flowHeight > height) {
        log::error("push-warp", std::format("unsupported size {}x{} (flow {}x{}) or format {}", width, height, flowWidth, flowHeight, unsigned(format)));
        return false;
    }
    device_ = device; width_ = width; height_ = height; format_ = format; flowWidth_ = flowWidth; flowHeight_ = flowHeight;
    // One root signature for every pass (shaders/PushWarp.hlsli): b0 constants, t0 / t1 root SRVs, table {t2, t3, u0},
    // u1 / u2 / u3 root UAVs; u4 / u5 / u6 / u7 the static-overlay mask's buffers (PushWarpOverlay.hlsl, the compose reads u6).
    std::array<D3D12_DESCRIPTOR_RANGE1, 2> ranges{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 4; ranges[0].BaseShaderRegister = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;   // t4 / t5 are null views without the acceleration
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;
    std::array<D3D12_ROOT_PARAMETER1, 11> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants.Num32BitValues = kConstants;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[2].Descriptor.ShaderRegister = 1;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = UINT(ranges.size()); parameters[3].DescriptorTable.pDescriptorRanges = ranges.data();
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[4].Descriptor.ShaderRegister = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[5].Descriptor.ShaderRegister = 2;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[6].Descriptor.ShaderRegister = 3;
    for (UINT i = 0; i < 4; ++i) { parameters[7 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[7 + i].Descriptor.ShaderRegister = 4 + i; }
    for (auto& p : parameters) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = UINT(parameters.size()); desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
        log::error("push-warp", "root signature not created");
        return false;
    }
    if (!createPipeline(device, "PushWarpTrust.dxil", trustPso_) || !createPipeline(device, "PushWarpSplat.dxil", splatPso_) ||
        !createPipeline(device, "PushWarpFill.dxil", fillPso_) || !createPipeline(device, "PushWarpCompose.dxil", composePso_) ||
        !createPipeline(device, "PushWarpVerdict.dxil", verdictPso_) || !createPipeline(device, "PushWarpOverlay.dxil", overlayPso_)) return false;
    if (!createCutTest(device)) { log::error("push-warp", "the scene-cut test (VfgShotChange.dxil) not created"); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSlots * kPerSlot;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))) || !stager_.initialize(device, 8)) return false;
    increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    scratchBytes_ = pushWarpScratchBytes(flowWidth, flowHeight);   // the self-check's flow fits too
    scratch_ = makeWarpBuffer(device, scratchBytes_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    counters_ = makeWarpBuffer(device, kSlots * kCounterBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    countersZero_ = makeWarpBuffer(device, kSlots * kCounterBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    countersReadback_ = makeWarpBuffer(device, PushPredictReadbackRing::kSlots * kCounterBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    verdict_ = makeWarpBuffer(device, kSlots * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    for (auto& m : masks_) m = makeWarpBuffer(device, pushWarpOverlayMaskBytes(width, height), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    overlayCells_ = makeWarpBuffer(device, pushWarpOverlayCellBytes(flowWidth, flowHeight), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    maskValid_ = false;
    if (!scratch_ || !counters_ || !countersZero_ || !countersReadback_ || !verdict_ || !masks_[0] || !masks_[1] || !masks_[2] || !overlayCells_) {
        log::error("push-warp", std::format("buffers not created (scratch {} bytes)", scratchBytes_));
        return false;
    }
    void* zero = nullptr;
    if (FAILED(countersZero_->Map(0, nullptr, &zero)) || !zero) return false;
    std::memset(zero, 0, kSlots * kCounterBytes);
    countersZero_->Unmap(0, nullptr);
    D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = PushPredictReadbackRing::kSlots * kPushWarpStamps;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&stamps_)))) stamps_.Reset();
    stampsReadback_ = stamps_ ? makeWarpBuffer(device, uint64_t(qd.Count) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false) : nullptr;
    if (!stampsReadback_) stamps_.Reset();
    created_ = true;

    // The start-up self-check (once per process for each adapter and format; behind the marker file).
    const LUID luid = device->GetAdapterLuid();
    const auto key = std::make_tuple(uint32_t(luid.LowPart), int32_t(luid.HighPart), int(format), width, height, flowWidth, flowHeight);
    std::lock_guard lock(warpCheckMutex());
    const auto cached = warpCheckCache().find(key);
    WarpCheckResult result;
    if (cached != warpCheckCache().end()) {
        result = cached->second;
    } else {
        const std::string identity = PushPredictPass::flowCheckIdentity();
        const std::int64_t boot = warpBootMinute();
        const PushFlowCheckBlock block = warpCheckFailedThisProcess()                         ? PushFlowCheckBlock::FailedThisProcess
                                         : pushWarpMarkerBlocks(readWarpMarker(), identity, boot) ? PushFlowCheckBlock::StoppedInsideLastTime
                                                                                                  : PushFlowCheckBlock::None;
        if (block == PushFlowCheckBlock::StoppedInsideLastTime) {
            writeWarpMarker(pushWarpBootMarker("failed", identity, boot));   // stays until Windows starts again or the program file changes
            result.text = "blocked: PushFPS stopped inside this self-check, or PushWarp faulted twice in play, earlier in this start of Windows with this program file";
        } else if (block == PushFlowCheckBlock::FailedThisProcess) {
            result.text = "blocked: an earlier self-check of this run failed";
        } else {
            const bool wasSuspect = trimmedWarpMarker() == "suspect " + identity;   // survives a passing check (two strikes in a row)
            writeWarpMarker(pushWarpBootMarker("running", identity, boot));
            bool failed = false;
            result.ok = runSelfCheck(device, result.text, result.gpuMs, result.playMs, failed, budgetMs);
            if (failed) {
                warpCheckFailedThisProcess() = true;
                writeWarpMarker(pushWarpBootMarker("failed", identity, boot));
            } else {
                if (wasSuspect) writeWarpMarker("suspect " + identity);
                else clearWarpMarker();
                // Review 2 SHOULD 4: a miss of the start-up budget alone (no fault, the picture right) is not kept the first
                // time: the next graph of this run checks once more (a busy start-up, shader compilation); the second miss stays.
                const bool budgetOnly = !result.ok && result.text.find("over the start-up budget") != std::string::npos;
                if (!budgetOnly || ++warpBudgetMisses()[key] >= 2) warpCheckCache()[key] = result;   // a failure blocks every later check of this process instead
                else result.text += " (checked once more by the next graph of this run)";
            }
        }
    }
    checkText_ = result.text; checkGpuMs_ = result.gpuMs; playGpuMs_ = result.playMs;
    ready_ = result.ok;
    log::info("push-warp", std::format("{} {}x{} flow {}x{} format={} selfCheck {}{}: {} (synthetic frames at graph build, one sample on this GPU: not a measurement of play)",
                                       ready_ ? "ready" : "UNAVAILABLE", width, height, flowWidth, flowHeight, unsigned(format), result.ok ? "passed" : "not passed",
                                       cached != warpCheckCache().end() ? " (this run's earlier result)" : "", result.text));
    return ready_;
}

bool PushWarpPass::sehRecord(PushWarpPass* pass, const Record* r, bool* faulted) {
    __try {
        return pass->recordCore(*r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
}

// The self-check: a synthetic pair at 256x144 (flow 64x36) through recordCore on a queue of its own, compared with the CPU
// reference. B = A moved by (+8, +4) work px, a square moving (+20, 0) in front, a static box (still pixels), a row of
// failed flow cells (trust 0) and one wrong vector (the block SAD). `failed`: a fault, a removed device or a submission
// that did not finish (the caller blocks every later check of this process); false with ok = false: the GPU's picture
// did not match the reference (the pass is not used; not a fault). Then the play-size run (SHOULD 6) on the same queue, and
// ONE budget for both: past budgetMs the pass is unavailable (not a fault: a later graph of this run reuses the result).
bool PushWarpPass::runSelfCheck(ID3D12Device* device, std::string& text, double& gpuMs, double& playMs, bool& failed, double budgetMs) {
    constexpr uint32_t W = kPushWarpCheckWidth, H = kPushWarpCheckHeight, FW = kPushWarpCheckFlowWidth, FH = kPushWarpCheckFlowHeight;
    const auto started = std::chrono::steady_clock::now();
    // Review 2 SHOULD 1: the CPU's synthesis of the play-size frames is not part of the budget (it is not PushWarp's cost).
    double synthMs = 0.0;
    const auto elapsedMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() - synthMs; };
    const bool tenBit = format_ == DXGI_FORMAT_R10G10B10A2_UNORM;
    const auto colour = [&](double x, double y, int layer) -> uint32_t {
        const double g = std::clamp(pushFlowCheckGrey(x, y) + (layer ? 40.0 : 0.0), 0.0, 255.0);
        const double r = std::clamp(255.0 - g * 0.8 + (layer ? 30.0 : 0.0), 0.0, 255.0), b = std::clamp(0.5 * g + 60.0, 0.0, 255.0);
        if (tenBit) {
            const auto q = [](double v) { return uint32_t(std::lround(v * 1023.0 / 255.0)); };
            return q(r) | (q(g) << 10) | (q(b) << 20) | (3u << 30);
        }
        return uint32_t(std::lround(r)) | (uint32_t(std::lround(g)) << 8) | (uint32_t(std::lround(b)) << 16) | (255u << 24);
    };
    const auto boxWord = tenBit ? (900u | (40u << 10) | (500u << 20) | (3u << 30)) : (230u | (10u << 8) | (125u << 16) | (255u << 24));
    const auto inBox = [](int x, int y) { return x >= 200 && x < 240 && y >= 8 && y < 24; };
    // A static 2-px white crosshair over the moving background (the static-overlay mask and its rules, directive 33).
    const auto inCross = [](int x, int y) { return (x >= 180 && x < 182 && y >= 34 && y < 76) || (y >= 54 && y < 56 && x >= 160 && x < 202); };
    const auto crossWord = tenBit ? (1023u | (1023u << 10) | (1023u << 20) | (3u << 30)) : 0xFFFFFFFFu;
    // s = 0: A, s = 1: B. Background moves (+8, +4) per interval, the square (+20, 0), the box and the crosshair stay.
    const auto at = [&](double s, int x, int y) -> uint32_t {
        if (inBox(x, y)) return boxWord;
        if (inCross(x, y)) return crossWord;
        const double sq = 60.0 + 20.0 * s;
        if (x >= sq && x < sq + 48 && y >= 56 && y < 104) return colour(x - sq, y - 56, 1);
        return colour(x - 8.0 * s, y - 4.0 * s, 0);
    };
    const uint32_t pitch = (W * 4 + 255) & ~255u;
    std::vector<uint32_t> a(size_t(W) * H), b(size_t(W) * H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) { a[size_t(y) * W + x] = at(0.0, int(x), int(y)); b[size_t(y) * W + x] = at(1.0, int(x), int(y)); }
    // The B -> A field on B's grid: background (-2, -1) flow px, the square (-5, 0); row 30 failed; cell (10, 4) wrong.
    std::vector<float> sx(size_t(FW) * FH), sy(sx.size()), tr(sx.size());
    std::vector<uint16_t> halves(size_t(FW) * FH * 2);
    std::vector<uint8_t> trustBytes(size_t(FW) * FH);
    for (uint32_t y = 0; y < FH; ++y)
        for (uint32_t x = 0; x < FW; ++x) {
            const int wx = int(x) * 4 + 2, wy = int(y) * 4 + 2;
            float vx = (wx >= 80 && wx < 128 && wy >= 56 && wy < 104) ? -5.0f : -2.0f, vy = vx == -5.0f ? 0.0f : -1.0f;
            uint8_t t = 255;
            if (y == 30) t = 0;
            if (x == 10 && y == 4) vx = 3.0f;
            const size_t i = size_t(y) * FW + x;
            trustBytes[i] = t;
            const float tf = float(t) / 255.0f;
            halves[2 * i] = warpHalf(vx * tf); halves[2 * i + 1] = warpHalf(vy * tf);
            sx[i] = pushHalfToFloat(halves[2 * i]); sy[i] = pushHalfToFloat(halves[2 * i + 1]); tr[i] = tf;
        }
    // The previous pair's field (Z -> A on A's grid) for the predicted run: the background moved (-1, -1) flow px before
    // and (-2, -1) now (an acceleration of 1 px: used), the square -5 both times (none).
    std::vector<float> zx(sx.size()), zy(sx.size()), zt(sx.size(), 1.0f);
    std::vector<uint16_t> zhalves(halves.size());
    for (uint32_t y = 0; y < FH; ++y)
        for (uint32_t x = 0; x < FW; ++x) {
            const int wx = int(x) * 4 + 2, wy = int(y) * 4 + 2;
            const bool square = wx >= 60 && wx < 108 && wy >= 56 && wy < 104;
            const size_t i = size_t(y) * FW + x;
            zhalves[2 * i] = warpHalf(square ? -5.0f : -1.0f); zhalves[2 * i + 1] = warpHalf(square ? 0.0f : -1.0f);
            zx[i] = pushHalfToFloat(zhalves[2 * i]); zy[i] = pushHalfToFloat(zhalves[2 * i + 1]);
        }
    PushWarpParams params;   // the product's parameters, t = 2/3, with the static-overlay rules (as the generator runs it)
    params.overlayRules = 3;
    PushWarpParams predicted; predicted.t = 5.0f / 3.0f; predicted.acceleration = true;   // step 4's P(2/3) with acceleration
    const PushWarpFrame fa{a.data(), W, H, W}, fb{b.data(), W, H, W};
    const PushWarpField zf{zx.data(), zy.data(), zt.data(), FW, FH};
    const auto reference = pushWarpReference(fa, fb, tenBit, PushWarpField{sx.data(), sy.data(), tr.data(), FW, FH}, params);
    const auto reference2 = pushWarpReference(fa, fb, tenBit, PushWarpField{sx.data(), sy.data(), tr.data(), FW, FH}, predicted, &zf);

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { text = "queue not created"; return false; }
    ComPtr<ID3D12Resource> frames[2] = {makeWarpBuffer(device, uint64_t(pitch) * H, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false),
                                        makeWarpBuffer(device, uint64_t(pitch) * H, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false)};
    ComPtr<ID3D12Resource> motion = makeTexture(device, FW, FH, DXGI_FORMAT_R16G16_FLOAT, false), trust = makeTexture(device, FW, FH, DXGI_FORMAT_R8_UNORM, false);
    ComPtr<ID3D12Resource> output = makeTexture(device, W, H, format_, true), output2 = makeTexture(device, W, H, format_, true);
    ComPtr<ID3D12Resource> motion2 = makeTexture(device, FW, FH, DXGI_FORMAT_R16G16_FLOAT, false), trust2 = makeTexture(device, FW, FH, DXGI_FORMAT_R8_UNORM, false);
    const uint32_t motionPitch = (FW * 4 + 255) & ~255u, trustPitch = (FW + 255) & ~255u;
    ComPtr<ID3D12Resource> fieldUpload = makeWarpBuffer(device, uint64_t(motionPitch + trustPitch) * FH * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    ComPtr<ID3D12Resource> outRb = makeWarpBuffer(device, uint64_t(pitch) * H * 2, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    if (!frames[0] || !frames[1] || !motion || !trust || !output || !output2 || !motion2 || !trust2 || !fieldUpload || !outRb) { text = "self-check resources not created"; return false; }
    {
        uint8_t* m = nullptr;
        for (int i = 0; i < 2; ++i) {
            if (FAILED(frames[i]->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) { text = "upload not mapped"; return false; }
            const auto& src = i == 0 ? a : b;
            for (uint32_t y = 0; y < H; ++y) std::memcpy(m + size_t(y) * pitch, src.data() + size_t(y) * W, size_t(W) * 4);
            frames[i]->Unmap(0, nullptr);
        }
        if (FAILED(fieldUpload->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) { text = "upload not mapped"; return false; }
        for (uint32_t y = 0; y < FH; ++y) {
            std::memcpy(m + size_t(y) * motionPitch, halves.data() + size_t(y) * FW * 2, size_t(FW) * 4);
            std::memcpy(m + size_t(motionPitch) * FH + size_t(y) * trustPitch, trustBytes.data() + size_t(y) * FW, FW);
            const size_t second = size_t(motionPitch + trustPitch) * FH;
            std::memcpy(m + second + size_t(y) * motionPitch, zhalves.data() + size_t(y) * FW * 2, size_t(FW) * 4);
            std::memset(m + second + size_t(motionPitch) * FH + size_t(y) * trustPitch, 255, FW);
        }
        fieldUpload->Unmap(0, nullptr);
    }
    for (int k = 0; k < 4; ++k) {
        auto* target = k == 0 ? motion.Get() : k == 1 ? trust.Get() : k == 2 ? motion2.Get() : trust2.Get();
        warpTransition(list.Get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = fieldUpload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = (k % 2 == 0 ? 0 : uint64_t(motionPitch) * FH) + (k >= 2 ? uint64_t(motionPitch + trustPitch) * FH : 0);
        from.PlacedFootprint.Footprint = {k % 2 == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8_UNORM, FW, FH, 1, k % 2 == 0 ? motionPitch : trustPitch};
        to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        warpTransition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    Record r;
    r.list = list.Get(); r.slot = 0; r.previous = frames[0].Get(); r.current = frames[1].Get(); r.rowPitch = pitch;
    r.motion = motion.Get(); r.trust = trust.Get(); r.output = output.Get(); r.params = &params; r.timestampQueue = queue.Get();
    log::info("push-warp", std::format("self-check {}x{} flow {}x{} format={}: recording", W, H, FW, FH, unsigned(format_)));
    bool faulted = false;
    bool recorded = sehRecord(this, &r, &faulted);
    const uint64_t firstSequence = readback_.next;
    Record r2 = r;   // step 4: the predicted frame with acceleration, in the same list (slot 1)
    r2.slot = 1; r2.output = output2.Get(); r2.params = &predicted; r2.previousMotion = motion2.Get(); r2.previousTrust = trust2.Get();
    if (recorded) recorded = sehRecord(this, &r2, &faulted);
    if (!recorded) {
        list->Close();   // nothing reaches the GPU
        failed = faulted;
        text = faulted ? "a fault inside the recording was caught; nothing was submitted" : "not recorded";
        log::warn("push-warp", "self-check: " + text);
        return false;
    }
    for (int k = 0; k < 2; ++k) {
        auto* out = k == 0 ? output.Get() : output2.Get();
        warpTransition(list.Get(), out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = out;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.pResource = outRb.Get(); dst.PlacedFootprint.Footprint = {format_, W, H, 1, pitch};
        dst.PlacedFootprint.Offset = uint64_t(k) * pitch * H;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        warpTransition(list.Get(), out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    if (FAILED(list->Close())) { text = "list not closed"; return false; }
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const bool signalled = SUCCEEDED(queue->Signal(fence.Get(), 1));
    log::info("push-warp", "self-check: submitted");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // REL16 delta MUST 2's rule here too: the wait is the start-up budget left (at least 250 ms), not a 10 s hang guard; a
    // frame still on the GPU after it is a failure for containment (marker "failed", every object kept for ever).
    const auto waitMs = [&] {
        const double left = budgetMs - elapsedMs();
        return DWORD(std::clamp(left, 250.0, 10000.0));
    };
    const bool finished = signalled && done && SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, waitMs()) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    const bool removed = FAILED(device->GetDeviceRemovedReason());
    if (!finished || removed) {
        for (IUnknown* o : std::initializer_list<IUnknown*>{queue.Get(), allocator.Get(), list.Get(), fence.Get(), frames[0].Get(), frames[1].Get(), motion.Get(),
                                                            trust.Get(), output.Get(), output2.Get(), motion2.Get(), trust2.Get(), fieldUpload.Get(), outRb.Get(), scratch_.Get(), counters_.Get(),
                                                            countersZero_.Get(), countersReadback_.Get(), verdict_.Get(), stamps_.Get(), stampsReadback_.Get(),
                                                            heap_.Get(), root_.Get(), trustPso_.Get(), splatPso_.Get(), fillPso_.Get(), composePso_.Get(), verdictPso_.Get(),
                                                            cut_.Get(), cutRoot_.Get(), cutPso_.Get(), overlayPso_.Get(), masks_[0].Get(), masks_[1].Get(), masks_[2].Get(), overlayCells_.Get()})
            if (o) warpKeptForever().emplace_back(o);
        failed = true;
        text = std::format("{} (device state 0x{:08X}); PushWarp is unavailable in this run", removed ? "the device was removed" : "still on the GPU after the start-up budget",
                           unsigned(device->GetDeviceRemovedReason()));
        log::error("push-warp", "self-check: " + text);
        return false;
    }
    log::info("push-warp", "self-check: finished on the GPU");
    PushWarpStageMs stages{};
    if (stageTimesOf(firstSequence, stages)) gpuMs = stages[static_cast<unsigned>(PushWarpStage::Total)];
    void* data = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(outRb->Map(0, nullptr, &data)) || !data) { text = "readback not mapped"; return false; }
    std::vector<uint32_t> got[2] = {std::vector<uint32_t>(size_t(W) * H), std::vector<uint32_t>(size_t(W) * H)};
    for (int k = 0; k < 2; ++k)
        for (uint32_t y = 0; y < H; ++y)
            std::memcpy(got[k].data() + size_t(y) * W, static_cast<const uint8_t*>(data) + size_t(k) * pitch * H + size_t(y) * pitch, size_t(W) * 4);
    outRb->Unmap(0, &none);
    const double wallMs = elapsedMs();
    bool pass = true;
    const char* names[2] = {"I(2/3)", "P(2/3) accelerated"};
    for (int k = 0; k < 2; ++k) {
        const auto& ref = k == 0 ? reference : reference2;
        PushWarpCounters counters;
        const bool haveCounters = countersOf(firstSequence + uint64_t(k), counters);
        const uint64_t differing = pushWarpDiffering(got[k], ref.words, tenBit, 1);
        const bool ok = haveCounters && pushWarpCheckPass(differing, uint64_t(W) * H, counters, ref.counters);
        pass = pass && ok;
        text += std::format("{}{} {}: differing>1code={} of {} counters gpu still={} blend={} conflict={} hole={} cpu still={} blend={} conflict={} hole={}{}",
                            k ? "; " : "", names[k], ok ? "matches the CPU reference" : "DOES NOT match the CPU reference", differing, uint64_t(W) * H,
                            counters.still, counters.blend, counters.conflict, counters.hole, ref.counters.still, ref.counters.blend, ref.counters.conflict,
                            ref.counters.hole, haveCounters ? "" : " (counters not read)");
    }
    text += std::format(" gpuMs={:.3f} (I) wallMs={:.0f}", gpuMs, wallMs);
    if (!pass) return false;
    if (wallMs > budgetMs) { text += std::format("; over the start-up budget of {:.0f} ms: PushWarp is unavailable in this run", budgetMs); return false; }

    // SHOULD 6: one run at the play size (work width_ x height_, field flowWidth_ x flowHeight_), synthetic frames: the
    // background moving (+8, +4) work px, a uniform field. Checked: it finishes within the budget, nothing faults, and the
    // counters account for every pixel; its GPU time is the entry guard's one sample. Review 2 SHOULD 1: the frames are
    // the check picture's 256x144 tile repeated (row copies, not 4 trig calls per pixel), and they are copied into DEFAULT
    // (video memory) buffers in the same list before the timed record, so PushWarp reads them as in play, not over PCIe.
    const uint32_t PW = width_, PH = height_, PFW = flowWidth_, PFH = flowHeight_, ppitch = (PW * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> big[2] = {makeWarpBuffer(device, uint64_t(ppitch) * PH, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false),
                                     makeWarpBuffer(device, uint64_t(ppitch) * PH, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false)};
    ComPtr<ID3D12Resource> bigOut = makeTexture(device, PW, PH, format_, true);
    ComPtr<ID3D12Resource> bigMotion = makeTexture(device, PFW, PFH, DXGI_FORMAT_R16G16_FLOAT, false), bigTrust = makeTexture(device, PFW, PFH, DXGI_FORMAT_R8_UNORM, false);
    const uint32_t bmPitch = (PFW * 4 + 255) & ~255u, btPitch = (PFW + 255) & ~255u;
    ComPtr<ID3D12Resource> bigField = makeWarpBuffer(device, uint64_t(bmPitch + btPitch) * PFH, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    ComPtr<ID3D12Resource> bigVram[2] = {makeWarpBuffer(device, uint64_t(ppitch) * PH, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false),
                                         makeWarpBuffer(device, uint64_t(ppitch) * PH, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false)};
    if (!big[0] || !big[1] || !bigVram[0] || !bigVram[1] || !bigOut || !bigMotion || !bigTrust || !bigField) { text += "; play-size resources not created"; return false; }
    {
        const auto synthStart = std::chrono::steady_clock::now();
        uint8_t* m = nullptr;
        std::vector<uint32_t> tile(size_t(W) * H);
        for (int i = 0; i < 2; ++i) {
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) tile[size_t(y) * W + x] = colour(double(x) - 8.0 * i, double(y) - 4.0 * i, 0);
            if (FAILED(big[i]->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) { text += "; upload not mapped"; return false; }
            for (uint32_t y = 0; y < PH; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(m + size_t(y) * ppitch);
                const uint32_t* source = tile.data() + size_t(y % H) * W;
                for (uint32_t x = 0; x < PW; x += W) std::memcpy(row + x, source, size_t(std::min(W, PW - x)) * 4);
            }
            big[i]->Unmap(0, nullptr);
        }
        synthMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - synthStart).count();
        if (FAILED(bigField->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) { text += "; upload not mapped"; return false; }
        const uint16_t hx = warpHalf(-8.0f * float(PFW) / float(PW)), hy = warpHalf(-4.0f * float(PFH) / float(PH));
        for (uint32_t y = 0; y < PFH; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(m + size_t(y) * bmPitch);
            for (uint32_t x = 0; x < PFW; ++x) { row[2 * x] = hx; row[2 * x + 1] = hy; }
            std::memset(m + size_t(bmPitch) * PFH + size_t(y) * btPitch, 255, PFW);
        }
        bigField->Unmap(0, nullptr);
    }
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) { text += "; list not reset"; return false; }
    for (int k = 0; k < 2; ++k) {
        auto* target = k == 0 ? bigMotion.Get() : bigTrust.Get();
        warpTransition(list.Get(), target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = bigField.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = k == 0 ? 0 : uint64_t(bmPitch) * PFH;
        from.PlacedFootprint.Footprint = {k == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8_UNORM, PFW, PFH, 1, k == 0 ? bmPitch : btPitch};
        to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        warpTransition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    for (int i = 0; i < 2; ++i) {   // into video memory before the timed record (the stamps bracket only the record)
        list->CopyBufferRegion(bigVram[i].Get(), 0, big[i].Get(), 0, uint64_t(ppitch) * PH);
        warpTransition(list.Get(), bigVram[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);   // then promoted as VFG's buffers in play
    }
    Record pr;
    pr.list = list.Get(); pr.slot = 2; pr.previous = bigVram[0].Get(); pr.current = bigVram[1].Get(); pr.rowPitch = ppitch;
    pr.motion = bigMotion.Get(); pr.trust = bigTrust.Get(); pr.output = bigOut.Get(); pr.params = &params; pr.timestampQueue = queue.Get();
    log::info("push-warp", std::format("self-check at the play size {}x{} flow {}x{}: recording", PW, PH, PFW, PFH));
    if (!sehRecord(this, &pr, &faulted)) {
        list->Close();
        failed = faulted;
        text += faulted ? "; play size: a fault inside the recording was caught; nothing was submitted" : "; play size: not recorded";
        return false;
    }
    const uint64_t playSequence = readback_.next;
    if (FAILED(list->Close())) { text += "; list not closed"; return false; }
    queue->ExecuteCommandLists(1, lists);
    const bool signalled2 = SUCCEEDED(queue->Signal(fence.Get(), 2));
    HANDLE done2 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished2 = signalled2 && done2 && SUCCEEDED(fence->SetEventOnCompletion(2, done2)) && WaitForSingleObject(done2, waitMs()) == WAIT_OBJECT_0;
    if (done2) CloseHandle(done2);
    const bool removed2 = FAILED(device->GetDeviceRemovedReason());
    if (!finished2 || removed2) {
        for (IUnknown* o : std::initializer_list<IUnknown*>{queue.Get(), allocator.Get(), list.Get(), fence.Get(), big[0].Get(), big[1].Get(), bigVram[0].Get(), bigVram[1].Get(), bigOut.Get(), bigMotion.Get(),
                                                            bigTrust.Get(), bigField.Get(), frames[0].Get(), frames[1].Get(), motion.Get(), trust.Get(), output.Get(), output2.Get(),
                                                            motion2.Get(), trust2.Get(), fieldUpload.Get(), outRb.Get(), scratch_.Get(), counters_.Get(), countersZero_.Get(),
                                                            countersReadback_.Get(), verdict_.Get(), stamps_.Get(), stampsReadback_.Get(), heap_.Get(), root_.Get(), trustPso_.Get(),
                                                            splatPso_.Get(), fillPso_.Get(), composePso_.Get(), verdictPso_.Get(), cut_.Get(), cutRoot_.Get(), cutPso_.Get(), overlayPso_.Get(), masks_[0].Get(), masks_[1].Get(), masks_[2].Get(), overlayCells_.Get()})
            if (o) warpKeptForever().emplace_back(o);
        failed = true;
        text += std::format("; play size: {} (device state 0x{:08X}); PushWarp is unavailable in this run", removed2 ? "the device was removed" : "still on the GPU after the start-up budget",
                            unsigned(device->GetDeviceRemovedReason()));
        log::error("push-warp", "self-check: " + text);
        return false;
    }
    PushWarpCounters pc;
    const bool havePlay = countersOf(playSequence, pc);
    PushWarpStageMs ps{};
    if (stageTimesOf(playSequence, ps)) playMs = ps[static_cast<unsigned>(PushWarpStage::Total)];
    const uint64_t accounted = uint64_t(pc.still) + pc.blend + pc.conflict + pc.hole;
    const double totalMs = elapsedMs();
    text += std::format("; play size {}x{} flow {}x{}: counters still={} blend={} conflict={} hole={} of {} gpuMs={:.3f} wallMs={:.0f} (budget {:.0f} ms)",
                        PW, PH, PFW, PFH, pc.still, pc.blend, pc.conflict, pc.hole, uint64_t(PW) * PH, playMs, totalMs, budgetMs);
    text += std::format(" (frames from video memory; their CPU synthesis {:.0f} ms not counted)", synthMs);
    if (!havePlay || accounted != uint64_t(PW) * PH) { text += " DOES NOT account for every pixel"; return false; }
    if (totalMs > budgetMs) { text += ": over the start-up budget: PushWarp is unavailable in this run"; return false; }
    return true;
}

bool PushWarpPass::recordInterpolate(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                                     ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* output, const PushWarpParams& params,
                                     ID3D12CommandQueue* timestampQueue, uint64_t cellsToken) {
    if (!ready_ || !output) return false;
    const auto od = output->GetDesc();
    if (od.Width != width_ || od.Height != height_) {
        log::error("push-warp", "output of the wrong size");
        return false;
    }
    Record r;
    r.list = list; r.slot = slot; r.previous = previous; r.current = current; r.rowPitch = rowPitch;
    r.motion = motion; r.trust = trust; r.output = output; r.params = &params; r.timestampQueue = timestampQueue; r.cellsToken = cellsToken;
    return recordCore(r);
}

bool PushWarpPass::recordPredicted(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* previous, ID3D12Resource* current, uint32_t rowPitch,
                                   ID3D12Resource* motion, ID3D12Resource* trust, ID3D12Resource* previousMotion, ID3D12Resource* previousTrust,
                                   ID3D12Resource* output, const PushWarpParams& params, ID3D12CommandQueue* timestampQueue, uint64_t cellsToken) {
    if (!ready_ || !output || !pushWarpPredicting(params)) return false;
    const auto od = output->GetDesc();
    if (od.Width != width_ || od.Height != height_) {
        log::error("push-warp", "output of the wrong size");
        return false;
    }
    if (previousMotion && motion && (previousMotion->GetDesc().Width != motion->GetDesc().Width || previousMotion->GetDesc().Height != motion->GetDesc().Height)) {
        previousMotion = nullptr; previousTrust = nullptr;   // a field of another size cannot be the previous pair's: no acceleration
    }
    Record r;
    r.list = list; r.slot = slot; r.previous = previous; r.current = current; r.rowPitch = rowPitch;
    r.motion = motion; r.trust = trust; r.previousMotion = previousMotion; r.previousTrust = previousTrust; r.output = output;
    r.params = &params; r.timestampQueue = timestampQueue; r.cellsToken = cellsToken;
    return recordCore(r);
}

bool PushWarpPass::recordCore(const Record& r) {
    if (!created_ || !r.list || r.slot >= kSlots || !r.previous || !r.current || !r.motion || !r.trust || !r.output || !r.params ||
        r.rowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) return false;
    const auto od = r.output->GetDesc(), md = r.motion->GetDesc(), td = r.trust->GetDesc();
    const auto W = uint32_t(od.Width), H = od.Height, gw = uint32_t(md.Width), gh = md.Height;
    if (od.Format != format_ || md.Format != DXGI_FORMAT_R16G16_FLOAT || td.Width != md.Width || td.Height != md.Height || r.rowPitch < W * 4 ||
        gw == 0 || gh == 0 || gw > W || gh > H || pushWarpScratch(gw, gh).bytes > scratchBytes_) {
        log::error("push-warp", "output, flow or trust of the wrong size or format");
        return false;
    }
    const auto& p = *r.params;
    auto* list = r.list;
    // Review 2 SHOULD 5: the shader-visible descriptors of this record are in a slot of their own, chosen by the record's
    // sequence (8 slots, each record in a list of its own, the graph's ring has 6 lists): a schedule change (causal ->
    // one-predicted, a skipped pair) can no longer rewrite views an in-flight list still reads. r.slot keeps naming the
    // GPU-only counters, verdict and cut words (written in queue order: no hazard there).
    const UINT base = UINT((readback_.next + 1) % kSlots) * kPerSlot;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = md.Format; stager_.stageSrv(r.motion, &srv, heap_.Get(), base + 0);
    srv.Format = td.Format; stager_.stageSrv(r.trust, &srv, heap_.Get(), base + 1);
    const bool accelerate = pushWarpPredicting(p) && p.acceleration && r.previousMotion && r.previousTrust;
    srv.Format = DXGI_FORMAT_R16G16_FLOAT; stager_.stageSrv(accelerate ? r.previousMotion : nullptr, &srv, heap_.Get(), base + 2);   // null views otherwise
    srv.Format = accelerate ? r.previousTrust->GetDesc().Format : DXGI_FORMAT_R8_UNORM; stager_.stageSrv(accelerate ? r.previousTrust : nullptr, &srv, heap_.Get(), base + 3);
    makeUav(device_, r.output, format_, cpu(base + 4));
    // Timestamps of the I this list records (its number once recorded).
    const uint64_t upcoming = readback_.next + 1;
    const UINT stamp = PushPredictReadbackRing::slotOf(upcoming) * kPushWarpStamps;
    if (stamps_ && r.timestampQueue && stampFrequency_ == 0) { UINT64 f = 0; if (SUCCEEDED(r.timestampQueue->GetTimestampFrequency(&f))) stampFrequency_ = f; }
    const bool timed = stamps_ && r.timestampQueue && stampFrequency_ != 0;
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 0);

    const uint64_t counterOffset = uint64_t(r.slot) * kCounterBytes, verdictOffset = uint64_t(r.slot) * 8;
    // The pair's cells (vector, trust, importance per flow cell) are reused by its later frames when the caller names the
    // pair (cellsToken) and everything they depend on is the same: the field, both frames, the sizes and the parameters.
    // The scratch is only written by this pass's lists, which the queue runs in recording order.
    const CellsKey cellsKey{r.cellsToken, r.motion, r.trust, r.previous, r.current, gw, gh, W, H, p.minConfidence, p.maxMotionShare, p.sadLow, p.sadHigh, p.beta};
    const bool reuse = r.cellsToken != 0 && !accelerate && lastCells_ == cellsKey;
    warpTransition(list, counters_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(counters_.Get(), counterOffset, countersZero_.Get(), counterOffset, kCounterBytes);
    warpTransition(list, counters_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    warpTransition(list, scratch_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    warpTransition(list, r.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    warpTransition(list, r.trust, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (accelerate) {
        warpTransition(list, r.previousMotion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        warpTransition(list, r.previousTrust, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    warpTransition(list, r.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    warpTransition(list, verdict_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    recordCutTest(list, r, W, H);   // SHOULD 5: the same GPU cut test as VFG's path (timed with the trust stage)
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootShaderResourceView(1, r.previous->GetGPUVirtualAddress());   // buffers in COMMON (or upload heaps): promoted
    list->SetComputeRootShaderResourceView(2, r.current->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(3, gpu(base));
    list->SetComputeRootUnorderedAccessView(4, scratch_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(5, counters_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(6, verdict_->GetGPUVirtualAddress());
    for (UINT i = 0; i < 3; ++i) list->SetComputeRootUnorderedAccessView(7 + i, masks_[i]->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(10, overlayCells_->GetGPUVirtualAddress());
    // Static overlays (interpolated frames only): the pair's mask is made once, by its first frame (after the cells), and
    // read by every frame of the pair.
    const unsigned overlayRules = pushWarpPredicting(p) ? 0u : (p.overlayRules & 3u);
    const bool maskNow = overlayRules != 0 && !(reuse && maskValid_ && maskOf_ == cellsKey);
    std::array<uint32_t, kConstants> c = {
        W, H, r.rowPitch, format_ == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u, gw, gh, uint32_t(counterOffset), uint32_t(verdictOffset),
        bitsOf(p.t), bitsOf(p.stillDelta), bitsOf(p.conflictDelta), bitsOf(p.minConfidence), bitsOf(p.maxMotionShare * float(gw)), bitsOf(p.sadLow),
        bitsOf(p.sadHigh), bitsOf(p.coverageFull), bitsOf(p.holeVisibility), bitsOf(p.fillKnown), bitsOf(p.beta), 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        (pushWarpPredicting(p) ? 1u : 0u) | (accelerate ? 2u : 0u) | (reuse ? 4u : 0u) | ((overlayRules & 1u) ? 8u : 0u) | ((overlayRules & 2u) ? 16u : 0u),
        bitsOf(p.accelMin)};
    const auto constants = [&] { list->SetComputeRoot32BitConstants(0, kConstants, c.data(), 0); };
    const auto groups = [](uint32_t v, uint32_t n) { return (v + n - 1) / n; };
    // W3
    list->SetPipelineState(trustPso_.Get());
    constants();
    list->Dispatch(groups(gw, 8), groups(gh, 8), 1);
    warpUavBarrier(list, scratch_.Get());
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 1);
    if (reuse) ++cellsReused_; else if (!accelerate && r.cellsToken) { lastCells_ = cellsKey; } else lastCells_ = CellsKey{};
    // R1: max, then add
    list->SetPipelineState(splatPso_.Get());
    for (uint32_t mode = 0; mode < 2; ++mode) {
        c[19] = mode;
        constants();
        list->Dispatch(groups(gw, 8), groups(gh, 8), 1);
        warpUavBarrier(list, scratch_.Get());
    }
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 2);
    // R2: resolve level 0, down to the coarsest, up to level 0
    const auto pyr = pushWarpPyramid(gw, gh);
    list->SetPipelineState(fillPso_.Get());
    const auto level = [&](uint32_t mode, unsigned l, int other) {
        c[19] = mode; c[20] = pyr.width[l]; c[21] = pyr.height[l]; c[22] = pyr.offset[l];
        if (other >= 0) { c[23] = pyr.offset[unsigned(other)]; c[24] = pyr.width[unsigned(other)]; c[25] = pyr.height[unsigned(other)]; }
        else { c[23] = 0; c[24] = 0; c[25] = 0; }
        constants();
        list->Dispatch(groups(pyr.width[l], 8), groups(pyr.height[l], 8), 1);
        warpUavBarrier(list, scratch_.Get());
    };
    level(0, 0, -1);
    for (unsigned l = 1; l < pyr.levels; ++l) level(1, l, int(l) - 1);
    for (unsigned l = pyr.levels; l-- > 0;) level(2, l, l + 1 < pyr.levels ? int(l) + 1 : -1);
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 3);
    ID3D12Resource* overlayBuffers[4] = {masks_[0].Get(), masks_[1].Get(), masks_[2].Get(), overlayCells_.Get()};
    if (overlayRules != 0)
        for (auto* b : overlayBuffers) warpTransition(list, b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (maskNow) {   // PushWarpOverlay.hlsl: spread, classify, run 1, run 2, overlay, sources
        list->SetPipelineState(overlayPso_.Get());
        const uint32_t rowWords = (W + 3u) / 4u;
        for (uint32_t mode = 0; mode < 6; ++mode) {
            c[19] = mode;
            constants();
            if (mode == 0) list->Dispatch(groups(gw, 16), groups(gh, 16), 1);
            else list->Dispatch(groups(rowWords, 16), groups(H, 16), 1);
            for (auto* b : overlayBuffers) warpUavBarrier(list, b);
        }
        warpUavBarrier(list, counters_.Get());
        maskOf_ = cellsKey;
        maskValid_ = true;
    }
    if (timed) list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 4);
    // R3
    c[19] = 0;
    list->SetPipelineState(composePso_.Get());
    constants();
    list->Dispatch(groups(W, 16), groups(H, 16), 1);
    warpUavBarrier(list, r.output);
    warpUavBarrier(list, counters_.Get());
    warpTransition(list, r.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    warpTransition(list, r.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    warpTransition(list, r.trust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (accelerate) {
        warpTransition(list, r.previousMotion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        warpTransition(list, r.previousTrust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    warpTransition(list, scratch_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (overlayRules != 0)
        for (auto* b : overlayBuffers) warpTransition(list, b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // Verdict: more holes than other moving pixels -> the nearer real frame over I as a whole (predicated copy).
    list->SetPipelineState(verdictPso_.Get());
    list->Dispatch(1, 1, 1);
    warpUavBarrier(list, verdict_.Get());
    warpTransition(list, verdict_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION);
    warpTransition(list, r.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = r.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = p.t >= 0.5f ? r.current : r.previous; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {format_, W, H, 1, r.rowPitch};
    list->SetPredication(verdict_.Get(), verdictOffset, D3D12_PREDICATION_OP_EQUAL_ZERO);   // skipped unless the predicate is 1
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // SHOULD 5: across a scene cut the frame is B, as VFG's frames and the predicted frame there.
    warpTransition(list, cut_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PREDICATION);
    src.pResource = r.current;
    list->SetPredication(cut_.Get(), uint64_t(r.slot) * kCutBytes, D3D12_PREDICATION_OP_EQUAL_ZERO);   // skipped unless a cut
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    warpTransition(list, cut_.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COMMON);
    warpTransition(list, r.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    warpTransition(list, verdict_.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COMMON);
    warpTransition(list, counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint64_t sequence = readback_.record();
    timedOf_[PushPredictReadbackRing::slotOf(sequence)] = 0;
    if (timed && sequence == upcoming) {
        list->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp + 5);
        list->ResolveQueryData(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp, kPushWarpStamps, stampsReadback_.Get(), uint64_t(stamp) * 8);
        timedOf_[PushPredictReadbackRing::slotOf(sequence)] = sequence;
    }
    list->CopyBufferRegion(countersReadback_.Get(), uint64_t(PushPredictReadbackRing::slotOf(sequence)) * kCounterBytes, counters_.Get(), counterOffset, kCounterBytes);
    warpTransition(list, counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

bool PushWarpPass::motionHistogramOf(uint64_t sequence, PushWarpMotionHistogram& out) const {
    if (!created_ || !readback_.valid(sequence)) return false;
    const SIZE_T at = SIZE_T(PushPredictReadbackRing::slotOf(sequence));
    const D3D12_RANGE range{at * kCounterBytes + 16, (at + 1) * kCounterBytes};
    void* data = nullptr;
    if (FAILED(countersReadback_->Map(0, &range, &data)) || !data) return false;
    std::memcpy(out.data(), static_cast<const uint8_t*>(data) + range.Begin, sizeof(uint32_t) * out.size());
    const D3D12_RANGE none{0, 0};
    countersReadback_->Unmap(0, &none);
    return true;
}

bool PushWarpPass::countersOf(uint64_t sequence, PushWarpCounters& out) const {
    if (!created_ || !readback_.valid(sequence)) return false;
    const SIZE_T at = SIZE_T(PushPredictReadbackRing::slotOf(sequence));
    const D3D12_RANGE range{at * kCounterBytes, (at + 1) * kCounterBytes};
    void* data = nullptr;
    if (FAILED(countersReadback_->Map(0, &range, &data)) || !data) return false;
    std::array<uint32_t, 4> values{};
    std::memcpy(values.data(), static_cast<const uint8_t*>(data) + range.Begin, sizeof(values));
    const D3D12_RANGE none{0, 0};
    countersReadback_->Unmap(0, &none);
    out.still = values[0]; out.blend = values[1]; out.conflict = values[2]; out.hole = values[3];
    return true;
}

bool PushWarpPass::overlayCountsOf(uint64_t sequence, uint32_t& total, uint32_t& centre) const {
    total = centre = 0;
    if (!created_ || !readback_.valid(sequence)) return false;
    const SIZE_T at = SIZE_T(PushPredictReadbackRing::slotOf(sequence));
    const D3D12_RANGE range{at * kCounterBytes + 16 + kPushWarpMotionBins * 4, (at + 1) * kCounterBytes};
    void* data = nullptr;
    if (FAILED(countersReadback_->Map(0, &range, &data)) || !data) return false;
    uint32_t values[2] = {};
    std::memcpy(values, static_cast<const uint8_t*>(data) + range.Begin, sizeof(values));
    const D3D12_RANGE none{0, 0};
    countersReadback_->Unmap(0, &none);
    total = values[0]; centre = values[1];
    return true;
}

bool PushWarpPass::stageTimesOf(uint64_t sequence, PushWarpStageMs& out) const {
    out.fill(-1.0);
    if (!created_ || !stamps_ || !readback_.valid(sequence)) return false;
    const unsigned at = PushPredictReadbackRing::slotOf(sequence);
    if (timedOf_[at] != sequence) return false;
    const D3D12_RANGE range{SIZE_T(at) * kPushWarpStamps * 8, SIZE_T(at + 1) * kPushWarpStamps * 8};
    void* data = nullptr;
    if (FAILED(stampsReadback_->Map(0, &range, &data)) || !data) return false;
    uint64_t ticks[kPushWarpStamps] = {};
    std::memcpy(ticks, static_cast<const uint8_t*>(data) + range.Begin, sizeof(ticks));
    const D3D12_RANGE none{0, 0};
    stampsReadback_->Unmap(0, &none);
    out = pushWarpStagesFromTicks(ticks, stampFrequency_);
    return true;
}

}  // namespace veyra::pipeline
