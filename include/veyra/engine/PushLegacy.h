// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Package 14's rollback switches (review of Package 14, 24 Nov 2026). Every change that is ON by default in Package 14
// can be undone with one line in slipstream.ini (the file accepts the VEYRA_TEST_RP_LEGACY_* family, written as "1"), or
// all of them with ONE line:
//
//   VEYRA_TEST_RP_LEGACY_PACKAGE13 = 1          every part below as Package 13 (the inert switches stay inert)
//   VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT = 1       capture-card admission: the budget's P95 rule alone
//   VEYRA_TEST_RP_LEGACY_GRID_EARLY = 1         a skipped pair's real frame at the full phase
//   VEYRA_TEST_RP_LEGACY_DEDUP_CADENCE = 1      ContentDedup's old cadence fit
//   VEYRA_TEST_RP_LEGACY_GPU_REAL_WAITS = 1     a real frame's presentation waits for the whole pair's lists
//   VEYRA_TEST_RP_LEGACY_UPLOAD_RELEASE = 1     the capture upload buffer is released after the pair's last list
//   VEYRA_TEST_RP_LEGACY_NR_HOLD = 1            the neural layer's saved state is kept as it is (no hold marker)
//
// Since Package 14 (stream "present", 24 Nov 2026), restored together by ONE line:
//
//   VEYRA_TEST_RP_LEGACY_PACKAGE14 = 1          every part below as Package 14 (PACKAGE13 = 1 restores these too)
//   VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY = 1      the delay badge and the statistics overlay are cut out of the video
//                                               window again (a complex window region) instead of drawn in the picture
//   VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP = 1       NVIDIA low latency's per-frame Sleep on a capture card with frame
//                                               generation is not watched for sleeping past the sample's arrival
//
// Since Package 15 (stream "s10-display", 24 Nov 2026), restored together by ONE line (PACKAGE14 = 1 and PACKAGE13 = 1
// restore these too; later streams of the same package append their parts after these, before Count, and are covered):
//
//   VEYRA_TEST_RP_LEGACY_PACKAGE15 = 1          every part below as Package 15
//   VEYRA_TEST_RP_LEGACY_DISPLAY_UI = 1         no "to screen" line in the delay badge's tooltip and the quick setup
//   VEYRA_TEST_RP_LEGACY_GSYNC_RESTART = 1      no "restart PushFPS to finish" G-SYNC row (and no pending flag)
//   VEYRA_TEST_RP_LEGACY_STALL_WITNESS = 1      no capture-stall witness (stream "s10-stall": no sampler threads, no
//                                               [stall-witness] / [capture-stall-cause] lines, no cycle counts in the callback)
//   VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE = 1  "Lower delay" with the row ON runs Package 15's schedule: the strict rule
//                                               (any interruption leaves), no capture-stall ride or engine-stall handling,
//                                               no stop for the playback after three sustained exits, the grid exit at a
//                                               fixed 98 % (when interpolation's own median is at 98 % or better), no
//                                               entry guard for a motion source too slow at its self-check. The motion
//                                               source is NOT rolled back (Package 15's strict GPU DIS removed the device)
//   VEYRA_TEST_RP_LEGACY_LOWDELAY_MIGRATE = 1   no one-time switch-off of a saved "Lower delay" on (pushLowDelayMigrate)
//
// Since Package 17 (streams "s12-hud" and "s12-pace", 25 Nov 2026), restored together by ONE line (every earlier master restores these too):
//
//   VEYRA_TEST_RP_LEGACY_PACKAGE16 = 1          every part below as Package 16
//   VEYRA_TEST_RP_LEGACY_HUD_GUARD = 1          Live HUD Lock without HUD Guard (no crosshair pre-clean, no compose of
//                                               protected pixels from the real frames: Package 16's lock alone)
//   VEYRA_TEST_RP_LEGACY_HUD_GUARD_CLEAN = 1    HUD Guard composes but does not pre-clean the generator's input (O1 only)
//   VEYRA_TEST_RP_LEGACY_FRAME_PROBE = 1        no frame probe (no thumbnail list after each Present, no [frame-probe] line)
//   VEYRA_TEST_RP_LEGACY_PRESENT_DOWNSCALE = 1  the present blit shrinks a larger picture with one bilinear tap (Package 16)
//                                               instead of the area filter in linear light
//   VEYRA_TEST_RP_LEGACY_DEDUP_ADMISSION = 1    with duplicate removal, FG admission judges a pair against the callback, not
//                                               the content anchor its deadlines count from (PushAdmissionAnchor.h)
//
// Each value is exactly "1" (anything else, "0" included, restores nothing). The earlier names keep working from the
// environment as aliases: VEYRA_TEST_RP_FG_SCHED_LEGACY=<parts> (robustAdmit, gridEarly), VEYRA_TEST_RP_DEDUP_LEGACY_CADENCE,
// VEYRA_TEST_GPU_REAL_WAITS_GROUP, VEYRA_TEST_UPLOAD_LATE_RELEASE, VEYRA_TEST_NR_KEEP_SAVED_ON (each "1"; the
// FG_SCHED one keeps its part list). Portable: the caller passes how to read a variable (PushLegacyEnv.h on Windows).
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace veyra::engine {

// Parts are appended in package order (a later package's parts after an earlier one's, always before Count).
enum class PushLegacyPart : unsigned {
    RobustAdmit, GridEarly, DedupCadence, GpuRealWaits, UploadRelease, NrHold, VideoOverlay, ReflexSleep,
    // Package 16 (after Package 15): restored by PACKAGE15 = 1 and by the earlier masters.
    DisplayUi, GsyncRestart, StallWitness,
    LowDelaySchedule16, LowDelayMigrate16,   // stream "s10-lowdelay2" (row-ON schedule; the one-time preference reset)
    // Package 17 (after Package 16): restored by PACKAGE16 = 1 and by the earlier masters.
    HudGuard17, HudGuardClean17,             // stream "s12-hud"
    FrameProbe17, PresentDownscale17, DedupAdmission17,   // stream "s12-pace"
    Count
};
// The first part that Package 14 did not have: PACKAGE14 = 1 restores this one and every later one.
inline constexpr PushLegacyPart kPushLegacyFirstAfter14 = PushLegacyPart::VideoOverlay;
// The first part that Package 15 did not have: PACKAGE15 = 1 restores this one and every later one.
inline constexpr PushLegacyPart kPushLegacyFirstAfter15 = PushLegacyPart::DisplayUi;
// The first part that Package 16 did not have: PACKAGE16 = 1 restores this one and every later one.
inline constexpr PushLegacyPart kPushLegacyFirstAfter16 = PushLegacyPart::HudGuard17;
struct PushLegacyName {
    std::string_view part, name, alias;
};
inline constexpr std::string_view kPushLegacyMaster = "VEYRA_TEST_RP_LEGACY_PACKAGE13";
inline constexpr std::string_view kPushLegacyMaster14 = "VEYRA_TEST_RP_LEGACY_PACKAGE14";
inline constexpr std::string_view kPushLegacyMaster15 = "VEYRA_TEST_RP_LEGACY_PACKAGE15";
inline constexpr std::string_view kPushLegacyMaster16 = "VEYRA_TEST_RP_LEGACY_PACKAGE16";
inline constexpr std::array<PushLegacyName, static_cast<std::size_t>(PushLegacyPart::Count)> kPushLegacyNames{{
    {"robustAdmit", "VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT", ""},
    {"gridEarly", "VEYRA_TEST_RP_LEGACY_GRID_EARLY", ""},
    {"dedupCadence", "VEYRA_TEST_RP_LEGACY_DEDUP_CADENCE", "VEYRA_TEST_RP_DEDUP_LEGACY_CADENCE"},
    {"gpuRealWaits", "VEYRA_TEST_RP_LEGACY_GPU_REAL_WAITS", "VEYRA_TEST_GPU_REAL_WAITS_GROUP"},
    {"uploadRelease", "VEYRA_TEST_RP_LEGACY_UPLOAD_RELEASE", "VEYRA_TEST_UPLOAD_LATE_RELEASE"},
    {"nrHold", "VEYRA_TEST_RP_LEGACY_NR_HOLD", "VEYRA_TEST_NR_KEEP_SAVED_ON"},
    {"videoOverlay", "VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY", ""},
    {"reflexSleep", "VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP", ""},
    {"displayUi", "VEYRA_TEST_RP_LEGACY_DISPLAY_UI", ""},
    {"gsyncRestart", "VEYRA_TEST_RP_LEGACY_GSYNC_RESTART", ""},
    {"stallWitness", "VEYRA_TEST_RP_LEGACY_STALL_WITNESS", ""},
    {"lowDelaySchedule", "VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE", ""},
    {"lowDelayMigrate", "VEYRA_TEST_RP_LEGACY_LOWDELAY_MIGRATE", ""},
    {"hudGuard", "VEYRA_TEST_RP_LEGACY_HUD_GUARD", ""},
    {"hudGuardClean", "VEYRA_TEST_RP_LEGACY_HUD_GUARD_CLEAN", ""},
    {"frameProbe", "VEYRA_TEST_RP_LEGACY_FRAME_PROBE", ""},
    {"presentDownscale", "VEYRA_TEST_RP_LEGACY_PRESENT_DOWNSCALE", ""},
    {"dedupAdmission", "VEYRA_TEST_RP_LEGACY_DEDUP_ADMISSION", ""},
}};
// Reads one variable: its value, or nothing when it is not set.
using PushEnvLookup = std::function<std::optional<std::string>(std::string_view)>;

namespace push_legacy_detail {
inline bool one(const PushEnvLookup& env, std::string_view name) {
    if (name.empty()) return false;
    const auto v = env(name);
    return v && *v == "1";
}
// VEYRA_TEST_RP_FG_SCHED_LEGACY=<all | comma list>: does it name this part?
inline bool listed(const PushEnvLookup& env, std::string_view part) {
    const auto v = env("VEYRA_TEST_RP_FG_SCHED_LEGACY");
    if (!v || v->empty()) return false;
    if (*v == "all") return true;
    std::size_t start = 0;
    while (start <= v->size()) {
        const std::size_t comma = v->find(',', start);
        const std::string_view item = std::string_view(*v).substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item == part) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}
}  // namespace push_legacy_detail

inline bool pushLegacyMaster(const PushEnvLookup& env) { return push_legacy_detail::one(env, kPushLegacyMaster); }
inline bool pushLegacyMaster14(const PushEnvLookup& env) { return push_legacy_detail::one(env, kPushLegacyMaster14); }
inline bool pushLegacyMaster15(const PushEnvLookup& env) { return push_legacy_detail::one(env, kPushLegacyMaster15); }
inline bool pushLegacyMaster16(const PushEnvLookup& env) { return push_legacy_detail::one(env, kPushLegacyMaster16); }
// True when this part runs as before its change (Package 13 for the first six, Package 14 for the later ones).
inline bool pushLegacyRestored(PushLegacyPart part, const PushEnvLookup& env) {
    const auto& n = kPushLegacyNames[static_cast<std::size_t>(part)];
    if (pushLegacyMaster(env) || push_legacy_detail::one(env, n.name) || push_legacy_detail::one(env, n.alias)) return true;
    if (static_cast<unsigned>(part) >= static_cast<unsigned>(kPushLegacyFirstAfter14) && pushLegacyMaster14(env)) return true;
    if (static_cast<unsigned>(part) >= static_cast<unsigned>(kPushLegacyFirstAfter15) && pushLegacyMaster15(env)) return true;
    if (static_cast<unsigned>(part) >= static_cast<unsigned>(kPushLegacyFirstAfter16) && pushLegacyMaster16(env)) return true;
    return (part == PushLegacyPart::RobustAdmit || part == PushLegacyPart::GridEarly) && push_legacy_detail::listed(env, n.part);
}
// For the start-up lines: "package13=0 package14=0 package15=0 package16=0 restored=[robustAdmit gridEarly]" ("restored=[]" without any).
inline std::string pushLegacyDescribe(const PushEnvLookup& env) {
    std::string t = pushLegacyMaster(env) ? "package13=1" : "package13=0";
    t += pushLegacyMaster14(env) ? " package14=1" : " package14=0";
    t += pushLegacyMaster15(env) ? " package15=1" : " package15=0";
    t += pushLegacyMaster16(env) ? " package16=1 restored=[" : " package16=0 restored=[";
    bool first = true;
    for (unsigned i = 0; i < static_cast<unsigned>(PushLegacyPart::Count); ++i)
        if (pushLegacyRestored(static_cast<PushLegacyPart>(i), env)) {
            if (!first) t += ' ';
            t += kPushLegacyNames[i].part;
            first = false;
        }
    return t + ']';
}

}  // namespace veyra::engine
