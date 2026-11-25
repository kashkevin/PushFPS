// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushGpuPath.h (stream "gpupath"): the real frame's presentation-queue wait and the capture
// upload buffer's release. Times are made-up model values, never measurements.
#include "veyra/pipeline/PushGpuPath.h"
#include <cstdint>
#include <vector>

namespace gpupath_tests {
using namespace veyra::pipeline;

inline PushRealPresentFenceInputs vfgPair() {
    PushRealPresentFenceInputs in;
    in.backend = PushFgPathKind::Vfg;
    in.realFence = 10;   // base list
    in.pairFence = 12;   // second generated frame's copy back
    return in;
}

TEST(gpupath_real_frame_waits_for_its_own_list_with_vfg) {
    CHECK(pushRealPresentFence(vfgPair()) == 10);
}

TEST(gpupath_real_frame_keeps_the_group_wait_where_a_later_list_touches_it) {
    auto in = vfgPair();
    in.backend = PushFgPathKind::Other;   // DLSS path, FSR, PushFlow, AMF
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.hudLock = true;    // its compose can read the held real frame of this parity
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.exclusion = true;  // the real frame is exclusionVideo_, composed after the base list
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.presentSinkFg = true;
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.backend = PushFgPathKind::None;   // no generation: both values are the same list anyway
    CHECK(pushRealPresentFence(in) == 12);
}

TEST(gpupath_backend_classification_follows_the_generation_loop) {
    CHECK(pushFgPathKindOf(true, false, false, true) == PushFgPathKind::Vfg);
    CHECK(pushFgPathKindOf(false, true, false, true) == PushFgPathKind::Other);    // PushFlow
    CHECK(pushFgPathKindOf(false, false, true, true) == PushFgPathKind::Other);    // AMF FRC
    CHECK(pushFgPathKindOf(true, true, false, true) == PushFgPathKind::Other);     // the loop takes PushFlow first
    CHECK(pushFgPathKindOf(true, false, true, true) == PushFgPathKind::Other);     // then AMF
    CHECK(pushFgPathKindOf(false, false, false, true) == PushFgPathKind::Other);   // DLSS path, FSR, ANR
    CHECK(pushFgPathKindOf(false, false, false, false) == PushFgPathKind::None);
    // Only VFG changes the wait.
    for (const auto kind : {PushFgPathKind::Other, PushFgPathKind::None}) {
        auto in = vfgPair(); in.backend = kind;
        CHECK(pushRealPresentFence(in) == in.pairFence);
    }
}

TEST(gpupath_real_frame_restore_switch_and_bounds) {
    auto in = vfgPair(); in.restore = true;
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.realFence = 0;                // unknown: the old value
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.realFence = 13;               // never later than the old value
    CHECK(pushRealPresentFence(in) == 12);
    in = vfgPair(); in.realFence = in.pairFence = 7; // a pair without generated frames: unchanged
    CHECK(pushRealPresentFence(in) == 7);
}

TEST(gpupath_upload_release_at_the_base_list) {
    CHECK(pushUploadReleaseFence(10, 12, false) == 10);
    CHECK(pushUploadReleaseFence(10, 12, true) == 12);   // VEYRA_TEST_UPLOAD_LATE_RELEASE=1
    CHECK(pushUploadReleaseFence(0, 12, false) == 12);
    CHECK(pushUploadReleaseFence(13, 12, false) == 12);
    CHECK(pushUploadReleaseFence(12, 12, false) == 12);
}

// One pair at 3X on the presentation queue, model times in ms from the real frame's pickup: base list done at 5,
// g1's copy back at 12, g2's at 19. Shown: g1 at 27.9, g2 at 39.0, the real frame at 50.1 (the usual case).
inline std::vector<PushQueueItem> shownPair(double realReady) {
    return {{27.9, 12.0, 0.4}, {39.0, 19.0, 0.4}, {50.1, realReady, 0.4}};
}

TEST(gpupath_fifo_model_usual_pair_is_unchanged) {
    const auto before = pushFifoStarts(shownPair(19.0));   // real frame waited for the group
    const auto after = pushFifoStarts(shownPair(5.0));     // waits for its own list
    CHECK(before.size() == 3 && after.size() == 3);
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(before[i] == after[i]);
}

TEST(gpupath_fifo_model_held_pair_shows_the_real_frame_when_it_is_ready) {
    // LiveFgRejectionHold admitted a probe pair: its generated frames are never presented and the real frame goes out
    // when ready (CPU sees the base list done at 5 ms, presents at 5.2). Its GPU group still runs: g2 ends at 19.
    const std::vector<PushQueueItem> before{{5.2, 19.0, 0.4}};
    const std::vector<PushQueueItem> after{{5.2, 5.0, 0.4}};
    CHECK(pushFifoStarts(before)[0] == 19.0);
    CHECK(pushFifoStarts(after)[0] == 5.2);   // 13.8 ms earlier in this model; the size on a PC is unmeasured
}

TEST(gpupath_fifo_model_late_generated_frame_dropped) {
    // g2 is late (ready at 52) and dropped at its deadline: only g1 and the real frame reach the queue.
    const std::vector<PushQueueItem> before{{27.9, 12.0, 0.4}, {50.1, 52.0, 0.4}};
    const std::vector<PushQueueItem> after{{27.9, 12.0, 0.4}, {50.1, 5.0, 0.4}};
    CHECK(pushFifoStarts(before)[1] == 52.0);
    CHECK(pushFifoStarts(after)[1] == 50.1);
    CHECK(pushFifoStarts(before)[0] == pushFifoStarts(after)[0]);
}

TEST(gpupath_fifo_model_lowering_a_wait_never_delays_or_reorders_anything) {
    // Deterministic pseudo-random sequences (no <random>: same numbers on every compiler).
    std::uint32_t state = 0x2545F491u;
    auto next = [&state](double scale) {
        state = state * 1664525u + 1013904223u;
        return scale * double(state >> 8) / double(1u << 24);
    };
    for (int run = 0; run < 400; ++run) {
        std::vector<PushQueueItem> items;
        double t = 0;
        for (int i = 0; i < 12; ++i) {
            t += next(12.0);
            items.push_back({t, t + next(30.0) - 10.0, next(1.0)});
        }
        const auto before = pushFifoStarts(items);
        auto lowered = items;
        for (auto& item : lowered) if (next(1.0) < 0.5) item.readyMs -= next(25.0);
        const auto after = pushFifoStarts(lowered);
        for (std::size_t i = 0; i < items.size(); ++i) {
            CHECK(after[i] <= before[i]);
            CHECK(after[i] >= items[i].submitMs);
            if (i) CHECK(after[i] >= after[i - 1] + lowered[i - 1].costMs);   // order kept
        }
    }
}

TEST(gpupath_vfg_back_to_back_only_when_asked_and_only_for_later_subframes) {
    // (enabled, subframe, reset, the pair's first producer value, the input revision it was signalled for, current revision)
    CHECK(pushVfgChainedWait(false, 2, false, 41, 7, 7) == 0);   // off by default: every subframe signals its own value
    CHECK(pushVfgChainedWait(true, 1, false, 41, 7, 7) == 0);    // the first subframe always signals
    CHECK(pushVfgChainedWait(true, 2, false, 41, 7, 7) == 41);
    CHECK(pushVfgChainedWait(true, 3, false, 41, 7, 7) == 41);
    CHECK(pushVfgChainedWait(true, 2, true, 41, 7, 7) == 0);     // a reset pair runs one subframe; never chained
    CHECK(pushVfgChainedWait(true, 2, false, 0, 7, 7) == 0);     // no first value of this pair known
}

// Package 15 review SHOULD 3: a predicted pair runs subframe 2 alone. The first producer value known is the PREVIOUS
// pair's (signalled before this pair's B was copied in): it must never be waited for.
TEST(gpupath_vfg_back_to_back_never_chains_to_another_pairs_value) {
    CHECK(pushVfgChainedWait(true, 2, false, 41, 6, 7) == 0);
    CHECK(pushVfgChainedWait(true, 3, false, 41, 6, 7) == 0);
}

}  // namespace gpupath_tests
