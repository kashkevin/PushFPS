// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s12-pace": the host time a capture pair's FG ADMISSION measures its deadlines from (EngineController admitFg,
// liveTimeline.resetPair). The defect (his Package 16 log, 20:31:45-52, 2X with duplicate removal on; reasoning first written in
// out/research/SETTINGS_AUDIT.md section 10, reproduced in tests/remoteplay/OnScreenCadenceTests.inc admission_*):
//   - with duplicate removal the PRESENTATION deadlines count from the content anchor, which on a clean 30 Hz signal sits half a
//     capture interval after the callback (LiveArrivalSmoother::contentAnchor: + transportInterval / 2; his log:
//     callbackMinusAnchorMs = -16.2), and the pair phase is learned against that anchor (watch->phaseAnchor);
//   - ADMISSION applied the same learned phase to the CALLBACK, i.e. judged every pair against deadlines about 16 ms earlier
//     than the real ones;
//   - at 3X the frame at 1/3 needs a phase of about 12-16 ms, so admission still saw a positive remaining time (4-5 ms in his
//     log, all pairs admitted); at 2X the only generated frame sits at T/2 and needs only about 7 ms of phase, so admission
//     saw -2 to -13 ms and rejected pairs whenever the learner reached its target: 36-61 FPS, on-grid down to 36 %.
// The fix: with a content anchor, admission uses the same anchor as presentation (the pair's own real deadlines). Transport
// anchoring (duplicate removal off) is unchanged by construction, so are files and streams. Restore:
// VEYRA_TEST_RP_LEGACY_DEDUP_ADMISSION (or PACKAGE16).
#include <cstdint>

namespace veyra::engine {

inline std::int64_t pushAdmissionAnchor(bool contentAnchor, bool legacyRestored, std::int64_t callback100ns, std::int64_t presentationAnchor100ns) {
    return contentAnchor && !legacyRestored ? presentationAnchor100ns : callback100ns;
}

}  // namespace veyra::engine
