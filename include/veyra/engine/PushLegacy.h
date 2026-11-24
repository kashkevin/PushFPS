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

enum class PushLegacyPart : unsigned { RobustAdmit, GridEarly, DedupCadence, GpuRealWaits, UploadRelease, NrHold, Count };
struct PushLegacyName {
    std::string_view part, name, alias;
};
inline constexpr std::string_view kPushLegacyMaster = "VEYRA_TEST_RP_LEGACY_PACKAGE13";
inline constexpr std::array<PushLegacyName, static_cast<std::size_t>(PushLegacyPart::Count)> kPushLegacyNames{{
    {"robustAdmit", "VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT", ""},
    {"gridEarly", "VEYRA_TEST_RP_LEGACY_GRID_EARLY", ""},
    {"dedupCadence", "VEYRA_TEST_RP_LEGACY_DEDUP_CADENCE", "VEYRA_TEST_RP_DEDUP_LEGACY_CADENCE"},
    {"gpuRealWaits", "VEYRA_TEST_RP_LEGACY_GPU_REAL_WAITS", "VEYRA_TEST_GPU_REAL_WAITS_GROUP"},
    {"uploadRelease", "VEYRA_TEST_RP_LEGACY_UPLOAD_RELEASE", "VEYRA_TEST_UPLOAD_LATE_RELEASE"},
    {"nrHold", "VEYRA_TEST_RP_LEGACY_NR_HOLD", "VEYRA_TEST_NR_KEEP_SAVED_ON"},
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
// True when this part runs as Package 13.
inline bool pushLegacyRestored(PushLegacyPart part, const PushEnvLookup& env) {
    const auto& n = kPushLegacyNames[static_cast<std::size_t>(part)];
    if (pushLegacyMaster(env) || push_legacy_detail::one(env, n.name) || push_legacy_detail::one(env, n.alias)) return true;
    return (part == PushLegacyPart::RobustAdmit || part == PushLegacyPart::GridEarly) && push_legacy_detail::listed(env, n.part);
}
// For the start-up lines: "package13=0 restored=[robustAdmit gridEarly]" ("restored=[]" without any).
inline std::string pushLegacyDescribe(const PushEnvLookup& env) {
    std::string t = pushLegacyMaster(env) ? "package13=1 restored=[" : "package13=0 restored=[";
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
