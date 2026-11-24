// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "gpupath" (24 Nov 2026): two GPU waits of one frame-generation pair that waited for more than they need.
// Pure decisions, so they are unit-tested on Linux and MSVC (tests/remoteplay/GpuPathTests.inc). The map of every
// wait of a pair is docs/drafts/GPU_PATH_MAP.md.
//
// 1. The real frame's presentation-queue wait (EnhanceGraph::presentationReadyFence for a real frame). The graph
//    recorded ONE value for it, the fence of the pair's LAST list (uploadFences_): with VFG at 3X that is the copy back
//    of the second generated frame. The engine already treats the real frame as ready at its own list's fence
//    (lease->readyFence = out.videoFenceValue) and presents it at its deadline; the presentation queue then waited on
//    the GPU for the whole generation group. When the generated frames are shown first (the usual case) the queue's
//    FIFO order already put the real frame after them, so nothing changes. When they are not shown (a pair admitted
//    while LiveFgRejectionHold or FgCostSettling holds generation off, whose real frame goes out when ready; a late
//    generated frame dropped at its deadline), the real frame's blit waited for generated frames nobody shows.
//    Applied only where no list after the real frame's own touches the real frame's texture: the VFG backend (its
//    generated-frame lists copy from CUDA's buffers into genFrame_), with neither the Live HUD Lock (its compose may
//    read the held real frame of this parity on a reset pair) nor the effect exclusion (its compose reads and the
//    real frame is exclusionVideo_) nor a present-sink generator. Every other backend keeps the pair's last fence:
//    the DLSS path, FSR and PushFlow read or transition videoFrame_[parity] in their generated-frame lists, and AMF
//    is left as it was. `VEYRA_TEST_GPU_REAL_WAITS_GROUP=1` restores the old wait.
// 2. The capture upload buffer's release (CaptureUploadFrame::gpuRead). Its only reader is the base list's
//    CopyTextureRegion at the start of the pair; the graph released it at the pair's last fence, so a capture
//    callback that wanted it back while the generated frames still ran wrote its CPU planes instead and the graph then
//    copied 12-25 MB a second time on the engine thread (metrics_.captureUploadCpuCopies). Released at the fence of
//    the real frame's own lists (out.videoFenceValue), which follows the copy. Was the experiment
//    VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE (stream "delay90"); `VEYRA_TEST_UPLOAD_LATE_RELEASE=1` restores the old release.
//
// Features that read the real frame or its references, and why (1) leaves them as they were (code):
// - Live HUD Lock, effect exclusion: excluded by the decision (old wait kept).
// - DLSS path / FSR / ANR, PushFlow, AMF FRC, XeSS present-sink generation: excluded (pushFgPathKindOf, presentSinkFg).
// - Recording, instant replay, comparison clips, the HUD sampler and the quality probe (PresentCapture), and the live
//   comparison view: they read the real frame, sourceReferences_ / baseReferences_ inside the presenter's blit list,
//   AFTER the presentation queue's wait; all of these are written in the real frame's own lists (base list and its NR /
//   SR splits), which v0 covers. Nothing they read is written by a VFG generated-frame list.
// - The next pairs: pair n+1 never writes this parity's textures; pair n+2 waits for this blit on the graph queue
//   (presentationFences_), unchanged. An earlier blit can only overlap EARLIER graph work (this pair's VFG lists, which
//   do not touch the real frame), never later work.
//
// Why neither can make anything worse (code; the FIFO model below is the test of the first):
// - Cannot show a frame whose work is unfinished: the presentation queue still waits on the GPU for every list that
//   writes or transitions the real frame (v0 is signalled after the last of them); (2) still waits for the only list
//   that reads the upload buffer.
// - Both only LOWER a fence value that something waits on; neither adds a wait, a copy or a list.
// - Neither changes what is shown or in which order: the CPU presentation order and the deadlines are the engine's
//   (unchanged), and the presentation queue is first-in first-out, so a blit can never start before an earlier one.
// - Neither changes a picture: the real frame's texture is final at its own fence (no later list of the pair writes it
//   under the conditions above), and the upload buffer is not read after the base list.
// Benefit: unmeasured. Steady state at 3X with every generated frame shown: none expected for (1); none for (2) while
// the upload counter (uploadCpuCopyFps) is 0.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace veyra::pipeline {

enum class PushFgPathKind : std::uint8_t { None, Vfg, Other };

// The generation path of a graph, as EnhanceGraph::process's generated-frame loop picks it: PushFlow first, then AMF,
// then VFG, else the provider path (DLSS path, FSR, ANR). Only Vfg qualifies for (1).
inline PushFgPathKind pushFgPathKindOf(bool vfg, bool pushFlow, bool amf, bool anyGenerator) {
    if (pushFlow || amf) return PushFgPathKind::Other;
    if (vfg) return PushFgPathKind::Vfg;
    return anyGenerator ? PushFgPathKind::Other : PushFgPathKind::None;
}

struct PushRealPresentFenceInputs {
    PushFgPathKind backend = PushFgPathKind::None;
    bool hudLock = false;         // EnhanceGraph::hudLock_ exists
    bool exclusion = false;       // desc_.exclusion.enabled
    bool presentSinkFg = false;   // a present-sink generator (XeSS) owns generation
    std::uint64_t realFence = 0;  // out.videoFenceValue: the last list that writes or transitions the real frame
    std::uint64_t pairFence = 0;  // uploadFences_[parity]: the pair's last list
    bool restore = false;         // VEYRA_TEST_GPU_REAL_WAITS_GROUP=1
};

// True when no list submitted after the real frame's own lists touches the real frame's texture (see 1 above).
inline bool pushRealFrameFinalAtOwnFence(const PushRealPresentFenceInputs& in) {
    return in.backend == PushFgPathKind::Vfg && !in.hudLock && !in.exclusion && !in.presentSinkFg;
}

// The fence value the presentation queue waits for before it blits the real frame.
inline std::uint64_t pushRealPresentFence(const PushRealPresentFenceInputs& in) {
    if (in.restore || !pushRealFrameFinalAtOwnFence(in)) return in.pairFence;
    if (in.realFence == 0 || in.realFence > in.pairFence) return in.pairFence;   // never later than before
    return in.realFence;
}

// The fence value after which the capture callback may write the frame's upload buffer again.
inline std::uint64_t pushUploadReleaseFence(std::uint64_t baseFence, std::uint64_t pairFence, bool restore) {
    if (restore || baseFence == 0 || baseFence > pairFence) return pairFence;
    return baseFence;
}

// 3. (experiment, OFF by default) VEYRA_EXP_RP_VFG_BACK_TO_BACK=1 (VfgBackend::handoff). Today each subframe's CUDA
//    work waits for a producer value the graph queue signals after the previous subframe's copy back, so between two
//    Runs of one pair the CUDA stream idles for a D3D12 round trip (completion -> graph queue -> copy back -> producer
//    signal -> CUDA): VEYRA_TEST_VFG_GPU_PROFILE's handoffIdle. Chained, a later subframe waits for the pair's FIRST
//    producer value: everything it needs is behind that value (both inputs and the shot-change test are in the base
//    list; its output buffer outputs[parity + 2 (sub - 1)] was last read by the copy back of the pair two real frames
//    ago, which is before it in queue order; the earlier copy backs of this pair only read other buffers), and the
//    CUDA stream itself keeps the Runs in order. Pictures are unchanged. Why it is OFF: the first generated frame's
//    copy back on the graph queue then runs while CUDA already runs the second Run; two contexts share the GPU by
//    time slices, so the first generated frame (the one on the delay's critical path) could be ready LATER. Only the
//    owner's PC can say which wins (gpuFg1P50/P95Ms, gpuFgP50Ms, VEYRA_TEST_VFG_GPU_PROFILE handoffIdle).
// Returns the producer value a subframe's CUDA work waits for when it is chained, 0 when it needs its own new value.
// Package 15 review (SHOULD 3): the value is the pair's only when it was signalled for THIS pair's inputs
// (pairRevision == inputRevision, VfgBackend's capture() count). A predicted pair runs subframe 2 alone; without this
// check it waited for the previous pair's producer value, i.e. before this pair's B was copied in (a torn frame).
inline std::uint64_t pushVfgChainedWait(bool enabled, unsigned subframe, bool reset, std::uint64_t pairProducer,
                                        std::uint64_t pairRevision, std::uint64_t inputRevision) {
    return enabled && subframe > 1 && !reset && pairProducer != 0 && pairRevision == inputRevision ? pairProducer : 0;
}

// A model of one D3D12 queue (the presentation queue): items run in submission order; each starts when it has been
// submitted, its fence wait is satisfied and the previous item has finished. Returns the start times. It is used by
// the tests to show that lowering any item's fence-ready time never makes any item start later and never reorders.
struct PushQueueItem {
    double submitMs = 0;  // ExecuteCommandLists (after the queue's Wait was recorded)
    double readyMs = 0;   // when the waited fence value completes
    double costMs = 0;    // GPU time of the item
};
inline std::vector<double> pushFifoStarts(const std::vector<PushQueueItem>& items) {
    std::vector<double> starts;
    starts.reserve(items.size());
    double free = 0;
    for (const auto& item : items) {
        const double start = std::max({free, item.submitMs, item.readyMs});
        starts.push_back(start);
        free = start + item.costMs;
    }
    return starts;
}

}  // namespace veyra::pipeline
