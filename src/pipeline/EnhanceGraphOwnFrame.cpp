// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg", step 2 (include/veyra/pipeline/PushOwnFrame.h): EnhanceGraph's members for PushWarp's I(2/3) as the
// second source of a "Lower delay" predicted pair. Kept apart from EnhanceGraph.cpp, whose predicted-pair code is the
// other engineer's: there only short hooks marked "stream ownfg" call these. Nothing here creates or runs anything unless
// initPredict created the predicted-frame pass (the Lower delay row or the kit's switch).
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/VfgBackend.h"
#include "veyra/engine/PushLowDelay.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/Log.h"

#include <chrono>
#include <format>

namespace veyra::pipeline {
using diagnostics::GpuStage;

namespace {
void ownTransition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
// VEYRA_TEST_OWNFRAME_OFF=1 (tests only; read once per process): no PushWarp at all, the behaviour before stream "ownfg"
// (a refusal by VFG leaves Lower delay unavailable; PushFlow is not served). Logged at the first graph that would use it.
// TODO(stream ownfg, at the merge into the package tree; coordinated with the rollback owner): the rollback master
// VEYRA_TEST_RP_LEGACY_PACKAGE15 must switch PushWarp off too: a part after the last one in engine/PushLegacy.h
// (e.g. PushLegacyPart::OwnFrame16, restored by PACKAGE15 = 1) read here beside VEYRA_TEST_OWNFRAME_OFF.
bool ownFrameTestOff() {
    static const bool off = GetEnvironmentVariableW(L"VEYRA_TEST_OWNFRAME_OFF", nullptr, 0) > 0;
    return off;
}
// VEYRA_TEST_OWNFRAME_COST_GUARD=0 (tests only, read once: WARP is a software rasteriser, far slower than any card) leaves
// out PushWarp's entry and exit cost guard (review 1, SHOULD 4), as VEYRA_TEST_PREDICT_ENTRY_GUARD=0 does for P's flow.
bool ownCostGuard() {
    static const bool on = GetEnvironmentVariableW(L"VEYRA_TEST_OWNFRAME_COST_GUARD", nullptr, 0) == 0;
    return on;
}
std::int64_t ownNow100ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}
}  // namespace

void EnhanceGraph::resetOwnFrame() {
    ownWarp_.reset();
    for (auto& f : ownFrames_) f.Reset();
    ownFootprint_ = {};
    ownIThisPair_ = false; ownFailLogged_ = false;
    ownUse_ = PushOwnFrameUse::None;
    ownEvidence_ = PushOwnFrameEvidence{};
    ownCost_.clear();
    ownPending_.clear();
    ownLastSequence_ = 0;
    ownCaptured_[0] = ownCaptured_[1] = 0;
    ownTooSlow_ = false; ownPairInterval100ns_ = 0;
    causalThisPair_ = false; causalFailLogged_ = false; causalPairs_ = 0;   // step 4
    ownFieldOf_[0] = ownFieldOf_[1] = 0;
    causalFrames_.clear();
    causalCost_[0].clear(); causalCost_[1].clear();
    pushOwnFrameShared().warp.store(0, std::memory_order_relaxed);
    pushOwnFrameShared().inUse.store(0, std::memory_order_relaxed);
}

PushOwnFrameGenerator EnhanceGraph::ownGenerator() const {
    if (ownFrameTestOff()) return PushOwnFrameGenerator::Other;   // nothing served, nothing created
    if (pushFlow_) return PushOwnFrameGenerator::PushFlow;   // the generated-frame loop's first choice
    if (amfFrc_ || fsrFgBackend_ || fgBackend_) return PushOwnFrameGenerator::Other;
    if (vfgBackend_) return PushOwnFrameGenerator::Vfg;
    return PushOwnFrameGenerator::Other;
}

// With the predicted pass only (initPredict). PushWarp runs its start-up self-check here, once per run for this device and
// format (PushWarpPass): the row says "checking" meanwhile. Without VFG (PushFlow) the real frames are also copied into
// two pitch-linear buffers of their own (about 2 x 33 MB at 3840x2160): VFG's input buffers play that part otherwise.
void EnhanceGraph::initOwnFrame(DXGI_FORMAT format, PushExtent flow) {
    if (ownFrameTestOff()) {
        veyra::log::info("push-warp", "VEYRA_TEST_OWNFRAME_OFF: PushWarp not created (the behaviour before stream ownfg: NVIDIA's I only)");
        return;
    }
    const auto generator = ownGenerator();
    if (generator == PushOwnFrameGenerator::Other || !videoFrame_[0] || !videoFrame_[1]) return;
    // Review 1, SHOULD 9: with VFG nothing of PushWarp exists unless the user chose the PushFPS source or the causal
    // schedule (EnhancementSettings::lowDelayOwnWarp: that choice rebuilds the graph once).
    if (!pushOwnFrameCreate(generator, desc_.lowDelayOwnWarp)) {
        veyra::log::info("push-warp", "not created: the frame source is NVIDIA's (PushWarp is created only when the PushFPS source or the causal schedule is chosen)");
        return;
    }
    // SHOULD 6: one start-up budget for the motion check (initPredict, just before) and PushWarp's checks together: what the
    // row's "at most 3 seconds" says. What the motion check used is taken off PushWarp's share.
    // VEYRA_TEST_OWNFRAME_COST_GUARD=0 (WARP: a software rasteriser) leaves only the hang guard.
    const double budgetMs = ownCostGuard() ? std::max(0.0, kOwnStartBudgetMs - double(steadyMs() - predictBuiltAtMs_)) : 600000.0;
    auto warp = std::make_unique<PushWarpPass>();
    engine::pushLowDelayShared().checking.store(true, std::memory_order_relaxed);
    const bool made = budgetMs > 0 && warp->initialize(context_.device(), workW_, workH_, format, flow.width, flow.height, budgetMs);
    engine::pushLowDelayShared().checking.store(false, std::memory_order_relaxed);
    // SHOULD 4, entry: B waits for I, so PushWarp's I may take at most kOwnSlotShare of an output slot. The play-size run's
    // one sample against a 30 FPS game at 3X (the slowest content the row serves: the largest slot is the least strict
    // test, so a faster game is judged again in play by the exit below).
    const double entryMs = kOwnSlotShare * (1000.0 / 29.97) / 3.0;
    const bool tooSlow = made && ownCostGuard() && warp->playGpuMs() > entryMs;
    ownEvidence_.warp = !made ? 2 : tooSlow ? 3 : 1;
    pushOwnFrameShared().warp.store(uint8_t(ownEvidence_.warp), std::memory_order_relaxed);
    if (!made || tooSlow) {
        veyra::log::warn("push-warp", std::format("PushWarp unavailable ({}{}); {}", budgetMs > 0 ? warp->checkText() : "no start-up budget left after the motion check",
            tooSlow ? std::format("; too slow on this card: its I took {:.2f} ms at the play size, more than {:.2f} ms ({:.0f} % of an output slot of a 30 FPS game at 3X)",
                                  warp->playGpuMs(), entryMs, kOwnSlotShare * 100.0) : std::string(),
            generator == PushOwnFrameGenerator::Vfg ? "Lower delay is unavailable with the PushFPS frame source (choose NVIDIA to use NVIDIA's frame)"
                                                    : "Lower delay is unavailable with this generator"));
        return;
    }
    uint64_t copyBytes = 0;
    if (!vfgBackend_) {
        const auto td = videoFrame_[0]->GetDesc();
        if (td.Format != format) { veyra::log::warn("push-warp", "the real frames' format is not the predicted pass's: no own copies"); return; }
        UINT64 bytes = 0;
        context_.device()->GetCopyableFootprints(&td, 0, 1, 0, &ownFootprint_, nullptr, nullptr, &bytes);
        for (auto& f : ownFrames_) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(context_.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&f)))) {
                for (auto& g : ownFrames_) g.Reset();
                veyra::log::warn("push-warp", "the real frames' own copies could not be created: Lower delay is unavailable with this generator");
                return;
            }
        }
        copyBytes = uint64_t(bytes) * 2;
    }
    ownWarp_ = std::move(warp);
    const double scratch = double(pushWarpScratchBytes(flow.width, flow.height));
    veyra::log::info("push-warp", std::format("PushWarp ready for the Lower delay pairs' I (source preference {}, generator {}): about {:.1f} MB (scratch {:.1f} MB at {}x{}, "
        "real-frame copies {:.1f} MB); self-check: {}; no GPU work unless a pair's I is PushWarp's",
        pushOwnFrameSourceName(PushOwnFrameSource(pushOwnFrameShared().source.load(std::memory_order_relaxed))),
        generator == PushOwnFrameGenerator::Vfg ? "vfg" : "pushflow", (scratch + double(copyBytes)) / 1048576.0, scratch / 1048576.0, flow.width, flow.height,
        double(copyBytes) / 1048576.0, ownWarp_->checkText()));
}

bool EnhanceGraph::ownFramesWanted() const {
    return ownWarp_ && ownFrames_[0] && ownFrames_[1] &&
           (requestedSchedule_ != PushPairSchedule::Interpolate || predictFromSwitch_ || engine::pushLowDelayShared().wanted.load(std::memory_order_relaxed));
}

ID3D12Resource* EnhanceGraph::predictInputBuffer(unsigned parity) const {
    if (vfgBackend_) return vfgBackend_->inputBuffer(parity);
    return ownWarp_ && ownFrames_[parity % 2] ? ownFrames_[parity % 2].Get() : nullptr;
}
uint32_t EnhanceGraph::predictInputPitch() const {
    return vfgBackend_ ? vfgBackend_->inputRowPitch() : ownFootprint_.Footprint.RowPitch;
}

// B (the pair's current real frame) from the pitch-linear copy into `texture` (in COPY_DEST). VFG: its own copy, which
// can be predicated on its GPU cut test. Own copies: no GPU cut test exists, so a copy `onlyOnCut` is not made (a cut is
// handled by P's own whole-frame verdict: every moving pixel falls back to B).
bool EnhanceGraph::copyPredictInput(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, unsigned parity, bool onlyOnCut) {
    if (vfgBackend_) return vfgBackend_->copyInput(list, texture, parity, onlyOnCut);
    if (!list || !texture || !ownFrames_[parity % 2]) return false;
    if (onlyOnCut) return true;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = ownFrames_[parity % 2].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = ownFootprint_;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    // The buffer is read without an explicit barrier: it is in COMMON (buffers decay to it between submissions) or was
    // promoted to a read state earlier in this list; buffers promote implicitly to COPY_SOURCE (as VfgBackend::copyInput).
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return true;
}

// The base list (before the real frame's own fence): the enhanced real frame into its own copy, as VFG's capture copies
// it into VFG's input buffer. Only while the predicted pass is wanted, without VFG.
void EnhanceGraph::captureOwnFrame(ID3D12GraphicsCommandList* list, unsigned parity) {
    if (!list || !ownFramesWanted()) return;
    auto* buffer = ownFrames_[parity % 2].Get();
    tracker_.transition(list, videoFrame_[parity].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    ownTransition(list, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = videoFrame_[parity].Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = buffer; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = ownFootprint_;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    ownTransition(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    tracker_.transition(list, videoFrame_[parity].Get(), D3D12_RESOURCE_STATE_COMMON);
    ownCaptured_[parity % 2] = realFrameIndex_ + 1;   // the index this real frame gets at the end of process()
}

// Both real frames as pitch-linear buffers: VFG's, or the own copies of this frame and the one before it.
bool EnhanceGraph::ownInputsReady(unsigned parity) const {
    return vfgBackend_ ? predictInputBuffer(parity) && predictInputBuffer(1 - parity)
                       : ownFrames_[parity % 2] && ownFrames_[(parity + 1) % 2] && ownCaptured_[parity % 2] == realFrameIndex_ + 1 &&
                             ownCaptured_[(parity + 1) % 2] == realFrameIndex_ && realFrameIndex_ > 0;
}

// Whether the frame source the user chose is PushWarp's while PushWarp cannot make it (not created yet, its check failed,
// over the start-up budget, or too slow on this card).
bool EnhanceGraph::ownWarpUnusable(PushOwnFrameSource source) const {
    const auto generator = ownGenerator();
    const bool wanted = generator == PushOwnFrameGenerator::Vfg ? source == PushOwnFrameSource::PushFps
                                                                : generator == PushOwnFrameGenerator::PushFlow && source != PushOwnFrameSource::Nvidia;
    return wanted && (!ownWarp_ || !ownWarp_->ready() || ownTooSlow_);
}

// Per pair, after the other engineer's eligibility is filled and before its refusal is computed.
void EnhanceGraph::ownFrameEligibility(PushPredictEligibility& pe, unsigned parity) {
    pollOwnFrames();
    ownIThisPair_ = false;
    const auto source = PushOwnFrameSource(pushOwnFrameShared().source.load(std::memory_order_relaxed));
    ownEvidence_.source = source;
    const bool inputs = ownInputsReady(parity);
    // The user's choice and nothing else (review 1, MUST 3): PushFPS chosen but PushWarp unavailable, too slow (SHOULD 4)
    // or not built yet: the mode does not run (pass-unavailable), never NVIDIA's I instead.
    if (ownWarpUnusable(source)) { pe.passReady = false; return; }
    ownUse_ = pushOwnFrameUse(source, ownGenerator(), ownWarp_ && ownWarp_->ready(), inputs);
    if (ownUse_ == PushOwnFrameUse::None) return;
    pushOwnFrameEligibility(pe, ownUse_);
    pe.passReady = predictAvailable() && inputs;
    ownIThisPair_ = true;
}
void EnhanceGraph::ownFrameStaticEligibility(PushPredictEligibility& pe) const {
    const auto source = PushOwnFrameSource(pushOwnFrameShared().source.load(std::memory_order_relaxed));
    const bool inputs = vfgBackend_ != nullptr || (ownFrames_[0] && ownFrames_[1]);
    if (ownWarpUnusable(source)) { pe.passReady = false; return; }
    const auto use = pushOwnFrameUse(source, ownGenerator(), ownWarp_ && ownWarp_->ready(), inputs);
    if (use == PushOwnFrameUse::None) return;
    pushOwnFrameEligibility(pe, use);
    pe.passReady = predictAvailable();
}

// I(2/3) of a predicted pair into genFrame_[slot] (the slot NVIDIA's FrameIndex 2 would have used), in `list`. Without
// VFG the generation's GPU stages start here (FgBatch, Fg1: VFG starts them in the base list). P's motion is this pair's
// B -> A field: FidelityFX recorded it with the real frame (its own list, before); a GPU DIS source (comparison only) or
// the test-only zero flow is recorded here as well as in P's list (where it is recorded again: its cost then counts twice,
// test sources only). Where PushWarp cannot record, I is B (as P is), logged once.
bool EnhanceGraph::recordOwnInterpolated(ID3D12GraphicsCommandList* list, unsigned parity, unsigned slot) {
    if (!vfgBackend_) { gpuTimer_.mark(list, GpuStage::FgBatch); gpuTimer_.mark(list, GpuStage::Fg1); }
    auto* previous = predictInputBuffer(1 - parity);
    auto* current = predictInputBuffer(parity);
    const uint32_t pitch = predictInputPitch();
    bool recorded = previous && current && ownWarp_ && predict_;
    if (recorded && !predict_->fidelityFlow()) {
        recorded = predict_->recordMotion(list, ring_.queue(), context_.fence(), ring_.lastSignaledValue() + 1, parity, previous, current, pitch, realFrameIndex_, realFrameIndex_ + 1);
        if (recorded) ownFieldOf_[parity % 2] = realFrameIndex_ + 1;   // step 4: the next causal pair's previous field
    }
    tracker_.transition(list, genFrame_[slot].Get(), D3D12_RESOURCE_STATE_COMMON);
    ownLastSequence_ = 0;
    if (recorded) {
        PushWarpParams params;
        params.t = pushVfgTimestep(pushOnePredictedPlan(parity).vfgFrameIndex, pushOnePredictedPlan(parity).vfgMultiplier);   // 2/3
        recorded = ownWarp_->recordInterpolate(list, (parity % 2) * 2, previous, current, pitch, predict_->motion(parity), predict_->trust(parity),
                                               genFrame_[slot].Get(), params, ring_.queue());
        if (recorded) ownLastSequence_ = ownWarp_->lastSequence();
    }
    if (recorded) return true;
    if (!ownFailLogged_) {
        ownFailLogged_ = true;
        veyra::log::warn("push-warp", "PushWarp's I was not recorded; this pair shows the real frame in its place (logged once per graph)");
    }
    if (!current) return false;
    tracker_.transition(list, genFrame_[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!copyPredictInput(list, genFrame_[slot].Get(), parity, false)) return false;
    tracker_.transition(list, genFrame_[slot].Get(), D3D12_RESOURCE_STATE_COMMON);
    return true;
}

// After a predicted pair was submitted: whose I it was (the row and the evidence), and PushWarp's readback to collect.
void EnhanceGraph::noteOwnFrame(bool own, uint64_t fence, std::int64_t pairInterval100ns) {
    if (!ownWarp_) return;
    if (pairInterval100ns > 0 && pairInterval100ns < 10000000) ownPairInterval100ns_ = pairInterval100ns;
    if (own) {
        ++ownEvidence_.ownPairs;
        pushOwnFrameShared().ownPairs.fetch_add(1, std::memory_order_relaxed);
        if (ownLastSequence_) ownPending_.push_back({ownLastSequence_, fence});
        while (ownPending_.size() > PushPredictReadbackRing::kSlots) ownPending_.pop_front();   // older readbacks are reused by then
    } else {
        ++ownEvidence_.vfgPairs;
    }
    ownEvidence_.lastUse = own ? ownUse_ : PushOwnFrameUse::None;
    pushOwnFrameShared().inUse.store(uint8_t(ownEvidence_.lastUse), std::memory_order_relaxed);
}

// Finished own I: counters (whole-frame verdict, hole share) and the GPU time by stage (its own timestamps).
void EnhanceGraph::pollOwnFrames() {
    if (!ownWarp_ || ownPending_.empty()) return;
    const uint64_t done = context_.fence()->GetCompletedValue();
    while (!ownPending_.empty() && ownPending_.front().second <= done) {
        const uint64_t sequence = ownPending_.front().first;
        ownPending_.pop_front();
        PushWarpCounters counters;
        if (ownWarp_->countersOf(sequence, counters)) {
            ++ownEvidence_.measured;
            ownEvidence_.holeShareSum += pushWarpHoleShare(counters);
            ownEvidence_.wholeFrameNearest += pushWarpWholeFrameNearest(counters);
        }
        PushWarpStageMs stages{};
        if (ownWarp_->stageTimesOf(sequence, stages)) ownCost_.add(ownNow100ns(), stages);
    }
    // SHOULD 4, exit: PushWarp's I measured in play (P95 of its own timestamps over 10 s, at least kOwnCostSamples) above
    // kOwnSlotShare of this content's output slot: the mode leaves for the rest of the graph with that reason.
    if (!ownTooSlow_ && ownCostGuard() && ownPairInterval100ns_ > 0 && ownCost_.size() >= kOwnCostSamples) {
        const double p95 = ownCost_.percentile(PushWarpStage::Total, 0.95), limit = kOwnSlotShare * double(ownPairInterval100ns_) / 3.0 / 10000.0;
        if (p95 > limit) {
            ownTooSlow_ = true; ownEvidence_.warp = 3;
            pushOwnFrameShared().warp.store(3, std::memory_order_relaxed);
            veyra::log::warn("push-warp", std::format("PushFPS frame too slow on this card: its I took {:.2f} ms (P95 of {} in play), more than {:.2f} ms ({:.0f} % of an output slot); "
                                                      "Lower delay leaves (pass-unavailable) until the graph is built again", p95, ownCost_.size(), limit, kOwnSlotShare * 100.0));
        }
    }
}

// ---- Step 4: the fully causal schedule (PushCausal.h), an experimental choice -------------------------------------------
// B is shown first and both generated frames are PREDICTED from B with this pair's one B -> A field (PushWarp forward
// projection, t = 1 + j / n), acceleration from the previous pair's field where that field is this pair's predecessor
// (starts and stops of motion: the offline study's only gain). HUD Lock and the exclusion boxes come from B for both (they
// follow B, as the one-predicted P does). Nothing of B's own texture is touched after its base list.

bool EnhanceGraph::causalReady() const {
    return ownWarp_ && ownWarp_->ready() && !ownTooSlow_ && predict_ && ownGenerator() != PushOwnFrameGenerator::Other &&
           (vfgBackend_ != nullptr || (ownFrames_[0] && ownFrames_[1]));
}

// After the one-predicted decision (predictPair is false whenever the engine asked for Causal: its refusal reads
// switch-off). Only here does lastPredictRefusal_ become the causal pair's own refusal.
void EnhanceGraph::causalEligibility(const PushPredictEligibility& pe, unsigned parity, bool generationReady) {
    causalThisPair_ = false;
    if (pe.requested != PushPairSchedule::Causal) return;
    PushPredictEligibility e = pe;
    e.passReady = predictAvailable() && ownInputsReady(parity);
    lastPredictRefusal_ = pushCausalPairRefusal(e, causalReady());
    causalThisPair_ = lastPredictRefusal_ == PushPredictRefusal::Ok && generationReady;
    causalMultiplier_ = pe.multiplier;
}

// P1 (and at 3X P2), each in a list of its own after B's lists, into the slots today's subframes 1 and 2 use. Without VFG
// the generation's GPU stages start here (VFG starts them in the base list). Where PushWarp cannot record, the frame is B
// (logged once), as the one-predicted P.
bool EnhanceGraph::recordCausalPair(FrameOutputs& out, unsigned parity, unsigned multiplier, uint32_t& slot, Status& st, bool reset, double ptsMs,
                                    const std::function<void()>& previewCheckpoint) {
    causalFrames_.clear();
    auto* previous = predictInputBuffer(1 - parity);
    auto* current = predictInputBuffer(parity);
    const uint32_t pitch = predictInputPitch();
    const unsigned count = multiplier >= 3 ? 2u : 1u;
    const auto fromB = pushPredictComposeSources(true, parity);
    // The previous pair's field, only when it is this pair's predecessor (FidelityFX: its own bookkeeping; otherwise the
    // field a PushWarp list of the previous pair recorded).
    const bool previousField = predict_ && (predict_->fidelityFlow() ? predict_->motionReady(1 - parity, realFrameIndex_)
                                                                     : ownFieldOf_[(parity + 1) % 2] == realFrameIndex_ && realFrameIndex_ > 0);
    bool motion = previous && current && ownWarp_ && predict_;
    out.causalPair = true; out.causalMultiplier = multiplier; out.causalSequence[0] = out.causalSequence[1] = 0;
    const double pairMs = ptsMs - prevPtsMs_;
    for (unsigned j = 1; j <= count; ++j) {
        const unsigned target = parity + 2u * (j - 1u);
        auto* list = ring_.acquireNext(slot, st);
        if (!list) return false;
        if (j == 1 && !vfgBackend_) { gpuTimer_.mark(list, GpuStage::FgBatch); gpuTimer_.mark(list, GpuStage::Fg1); }
        if (j == 2) gpuTimer_.mark(list, GpuStage::Fg2);
        if (j == 1 && motion && !predict_->fidelityFlow()) {
            motion = predict_->recordMotion(list, ring_.queue(), context_.fence(), ring_.lastSignaledValue() + 1, parity, previous, current, pitch, realFrameIndex_, realFrameIndex_ + 1);
            if (motion) ownFieldOf_[parity % 2] = realFrameIndex_ + 1;
        }
        tracker_.transition(list, genFrame_[target].Get(), D3D12_RESOURCE_STATE_COMMON);
        bool recorded = motion;
        if (recorded) {
            PushWarpParams params;
            params.t = 1.0f + pushCausalStep(j, multiplier);
            params.acceleration = previousField;
            recorded = ownWarp_->recordPredicted(list, (parity % 2) * 2 + (j - 1), previous, current, pitch, predict_->motion(parity), predict_->trust(parity),
                                                 previousField ? predict_->motion(1 - parity) : nullptr, previousField ? predict_->trust(1 - parity) : nullptr,
                                                 genFrame_[target].Get(), params, ring_.queue());
            if (recorded) out.causalSequence[j - 1] = ownWarp_->lastSequence();
        }
        if (!recorded) {
            if (!causalFailLogged_) {
                causalFailLogged_ = true;
                veyra::log::warn("push-warp", "a causal pair's prediction was not recorded; that frame shows the real frame in its place (logged once per graph)");
            }
            if (!current) return false;
            tracker_.transition(list, genFrame_[target].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            if (!copyPredictInput(list, genFrame_[target].Get(), parity, false)) return false;
            tracker_.transition(list, genFrame_[target].Get(), D3D12_RESOURCE_STATE_COMMON);
        }
        // HUD-locked pixels from B (copied once into the lock's predicted-held texture, from the pitch-linear B: never B's
        // own texture), boxes from raw B; the lock's state is not updated here.
        if (hudLock_ && hudLock_->predictedHeld()) {
            if (j == 1) {
                tracker_.transition(list, hudLock_->predictedHeld(), D3D12_RESOURCE_STATE_COPY_DEST);
                if (!copyPredictInput(list, hudLock_->predictedHeld(), parity, false)) return false;
                tracker_.transition(list, hudLock_->predictedHeld(), D3D12_RESOURCE_STATE_COMMON);
            }
            composeHudLock(list, target, fromB.hudHeld);
        }
        if (!composeExclusion(list, target + 2, fromB.exclusionRaw, false)) return false;
        tracker_.transition(list, genFrame_[target].Get(), D3D12_RESOURCE_STATE_COMMON);
        if (j == count) {
            gpuTimer_.mark(list, count == 1 ? GpuStage::Fg1 : GpuStage::Fg2, true);
            gpuTimer_.mark(list, GpuStage::FgBatch, true);
            gpuTimer_.resolve(list);
        } else {
            gpuTimer_.mark(list, GpuStage::Fg1, true);
        }
        if (!submitGraph(slot)) return false;
        if (j == count) gpuTimer_.submitted(ring_.lastSignaledValue());
        if (previewCheckpoint && !reset) previewCheckpoint();
        ++out.fgEvaluated; out.hasGenerated = true; out.genSlot = parity;
        out.genFenceValue = ring_.lastSignaledValue(); out.genFenceObject = context_.fence();
        out.generatedPtsMs = ptsMs + pairMs * double(j) / double(multiplier);
        ++metrics_.fgSubmittedCandidates;
        BatchFrame f; f.identity = out.batch.identity; f.kind = FrameKind::Generated; f.validity = GenerationValidity::Pending; f.subframe = j;
        f.pts100ns = pushCausalPts(out.batch.a100ns, out.batch.b100ns, j, multiplier);
        f.lease = std::make_shared<FrameLease>(); f.lease->texture = genFrame_[target]; f.lease->slot = target;
        f.lease->predicted = true; f.lease->ownFrame = true; f.lease->causal = true;
        f.lease->readyFence = out.genFenceValue; f.lease->readyFenceObject = out.genFenceObject;
        generatedLeases_[target] = f.lease;
        causalFrames_.push_back(std::move(f));
    }
    ++metrics_.predictedPairs; ++causalPairs_;
    out.predictedPair = true; out.predictSequence = 0; out.predictFailed = false;
    return true;
}

// After B in the batch: display order B, P1, P2 (nothing generated precedes B).
void EnhanceGraph::appendCausalFrames(FrameOutputs& out) {
    for (auto& f : causalFrames_) out.batch.append(std::move(f));
    causalFrames_.clear();
}

// A finished causal pair (the engine calls it once the batch's GPU work completed): each prediction's counters decide
// "usable" (PushCausal.h); its GPU time joins P1's or P2's window for the evidence.
PushCausalVerdict EnhanceGraph::takeCausalVerdict(const FrameOutputs& finished) {
    PushCausalVerdict v;
    v.twoPredictions = finished.causalMultiplier >= 3;
    if (!ownWarp_) { v.p1Usable = v.p2Usable = false; return v; }
    const auto now = ownNow100ns();
    for (unsigned j = 0; j < (v.twoPredictions ? 2u : 1u); ++j) {
        bool usable = false;   // not recorded (the frame was B) or its readback reused: counts as unusable
        double ms = -1.0;
        const uint64_t sequence = finished.causalSequence[j];
        PushWarpCounters counters;
        if (sequence && ownWarp_->countersOf(sequence, counters)) usable = pushCausalFrameUsable(counters);
        PushWarpStageMs stages{};
        if (sequence && ownWarp_->stageTimesOf(sequence, stages)) {
            causalCost_[j].add(now, stages);
            ms = stages[static_cast<unsigned>(PushWarpStage::Total)];
        }
        (j == 0 ? v.p1Usable : v.p2Usable) = usable;
        (j == 0 ? v.p1Ms : v.p2Ms) = ms;
    }
    return v;
}

}  // namespace veyra::pipeline
