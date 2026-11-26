// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Package 14's rollback switches (veyra/engine/PushLegacy.h): each alone, the master, the aliases, from the file.
#include "veyra/engine/PushLegacy.h"
#include "veyra/engine/PushFgSchedule.h"
#include "veyra/engine/SlipstreamSettings.h"
#include <map>
#include <string>

namespace legacy_tests {
using veyra::engine::PushLegacyPart;
inline veyra::engine::PushEnvLookup env(std::map<std::string, std::string> vars) {
    return [vars](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        if (it == vars.end()) return std::nullopt;
        return it->second;
    };
}
inline unsigned restoredMask(const veyra::engine::PushEnvLookup& e) {
    unsigned m = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(PushLegacyPart::Count); ++i)
        if (veyra::engine::pushLegacyRestored(static_cast<PushLegacyPart>(i), e)) m |= 1u << i;
    return m;
}
// The environment a plain install has after slipstream.ini (what applySlipstreamSettingsFile would set).
inline veyra::engine::PushEnvLookup fromFile(std::string_view text) {
    std::map<std::string, std::string> vars;
    for (const auto& s : veyra::engine::splitSlipstreamForFile(veyra::engine::parseSlipstreamSettings(text)).apply) vars[s.name] = s.value;
    return env(vars);
}
}  // namespace legacy_tests

TEST(legacy_each_switch_alone_the_master_and_the_aliases) {
    using namespace legacy_tests;
    CHECK(restoredMask(env({})) == 0);
    for (unsigned i = 0; i < static_cast<unsigned>(PushLegacyPart::Count); ++i) {
        const auto& n = veyra::engine::kPushLegacyNames[i];
        CHECK(restoredMask(env({{std::string(n.name), "1"}})) == (1u << i));
        for (const char* not1 : {"0", "", "yes", "true", "2", "1 "}) CHECK(restoredMask(env({{std::string(n.name), not1}})) == 0);
        if (!n.alias.empty()) {
            CHECK(restoredMask(env({{std::string(n.alias), "1"}})) == (1u << i));
            CHECK(restoredMask(env({{std::string(n.alias), "0"}})) == 0);
        }
    }
    const unsigned all = (1u << static_cast<unsigned>(PushLegacyPart::Count)) - 1;
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}})) == all);
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "0"}})) == 0);
    // The earlier part list keeps working for the two scheduling parts.
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_FG_SCHED_LEGACY", "gridEarly"}})) == (1u << static_cast<unsigned>(PushLegacyPart::GridEarly)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_FG_SCHED_LEGACY", "all"}})) == 3u);
    CHECK(veyra::engine::pushLegacyDescribe(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}})) ==
          "package13=1 package14=0 package15=0 package16=0 restored=[robustAdmit gridEarly dedupCadence gpuRealWaits uploadRelease nrHold videoOverlay reflexSleep displayUi gsyncRestart stallWitness lowDelaySchedule lowDelayMigrate hudGuard hudGuardClean frameProbe presentDownscale dedupAdmission statsReadings]");
    CHECK(veyra::engine::pushLegacyDescribe(env({})) == "package13=0 package14=0 package15=0 package16=0 restored=[]");
}

TEST(legacy_package14_master_restores_only_what_came_after_package14) {
    using namespace legacy_tests;
    const unsigned first = static_cast<unsigned>(veyra::engine::kPushLegacyFirstAfter14);
    const unsigned count = static_cast<unsigned>(PushLegacyPart::Count);
    const unsigned after14 = ((1u << count) - 1) & ~((1u << first) - 1);
    // Package 14's own parts, and every part of a later package (PACKAGE14 = 1 means "as Package 14").
    CHECK((after14 & (1u << static_cast<unsigned>(PushLegacyPart::VideoOverlay))) && (after14 & (1u << static_cast<unsigned>(PushLegacyPart::ReflexSleep))));
    CHECK(!(after14 & (1u << static_cast<unsigned>(PushLegacyPart::NrHold))));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE14", "1"}})) == after14);
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE14", "0"}})) == 0);
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE14 = 1\n")) == after14);
    CHECK(veyra::engine::pushLegacyDescribe(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE14", "1"}})) ==
          "package13=0 package14=1 package15=0 package16=0 restored=[videoOverlay reflexSleep displayUi gsyncRestart stallWitness lowDelaySchedule lowDelayMigrate hudGuard hudGuardClean frameProbe presentDownscale dedupAdmission statsReadings]");
    // Package 13's master still restores everything, the parts after Package 14 included.
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}})) == (1u << count) - 1);
}

TEST(legacy_package15_master_restores_only_what_came_after_package15) {
    using namespace legacy_tests;
    const unsigned first = static_cast<unsigned>(veyra::engine::kPushLegacyFirstAfter15);
    const unsigned count = static_cast<unsigned>(PushLegacyPart::Count);
    const unsigned after15 = ((1u << count) - 1) & ~((1u << first) - 1);
    CHECK(after15 & (1u << static_cast<unsigned>(PushLegacyPart::DisplayUi)));
    CHECK(after15 & (1u << static_cast<unsigned>(PushLegacyPart::GsyncRestart)));
    CHECK(after15 & (1u << static_cast<unsigned>(PushLegacyPart::StallWitness)));   // stream "s10-stall"
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_STALL_WITNESS", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::StallWitness)));
    CHECK(after15 & (1u << static_cast<unsigned>(PushLegacyPart::LowDelaySchedule16)));   // stream "s10-lowdelay2"
    CHECK(after15 & (1u << static_cast<unsigned>(PushLegacyPart::LowDelayMigrate16)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::LowDelaySchedule16)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_LOWDELAY_MIGRATE", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::LowDelayMigrate16)));
    CHECK(!(after15 & (1u << static_cast<unsigned>(PushLegacyPart::ReflexSleep))));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE15", "1"}})) == after15);
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE15", "0"}})) == 0);
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE15 = 1\n")) == after15);
    // The earlier masters imply it.
    CHECK((restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE14", "1"}})) & after15) == after15);
    CHECK((restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}})) & after15) == after15);
    CHECK(veyra::engine::pushLegacyDescribe(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE15", "1"}})).rfind("package13=0 package14=0 package15=1 package16=0 restored=[displayUi gsyncRestart stallWitness lowDelaySchedule lowDelayMigrate hudGuard hudGuardClean frameProbe presentDownscale dedupAdmission statsReadings]", 0) == 0);
}

TEST(legacy_package16_master_restores_only_what_came_after_package16) {
    using namespace legacy_tests;
    const unsigned first = static_cast<unsigned>(veyra::engine::kPushLegacyFirstAfter16);
    const unsigned count = static_cast<unsigned>(PushLegacyPart::Count);
    const unsigned after16 = ((1u << count) - 1) & ~((1u << first) - 1);
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::HudGuard17)));   // stream "s12-hud"
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::HudGuardClean17)));
    CHECK(!(after16 & (1u << static_cast<unsigned>(PushLegacyPart::LowDelayMigrate16))));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_HUD_GUARD", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::HudGuard17)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_HUD_GUARD_CLEAN", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::HudGuardClean17)));
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::FrameProbe17)));   // stream "s12-pace"
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::PresentDownscale17)));
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::DedupAdmission17)));
    CHECK(after16 & (1u << static_cast<unsigned>(PushLegacyPart::StatsReadings17)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_DEDUP_ADMISSION", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::DedupAdmission17)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_FRAME_PROBE", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::FrameProbe17)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PRESENT_DOWNSCALE", "1"}})) == (1u << static_cast<unsigned>(PushLegacyPart::PresentDownscale17)));
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE16", "1"}})) == after16);
    CHECK(restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE16", "0"}})) == 0);
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE16 = 1\n")) == after16);
    CHECK((restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE15", "1"}})) & after16) == after16);
    CHECK((restoredMask(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}})) & after16) == after16);
    CHECK(veyra::engine::pushLegacyDescribe(env({{"VEYRA_TEST_RP_LEGACY_PACKAGE16", "1"}})) ==
          "package13=0 package14=0 package15=0 package16=1 restored=[hudGuard hudGuardClean frameProbe presentDownscale dedupAdmission statsReadings]");
}

TEST(legacy_switches_work_from_slipstream_ini) {
    using namespace legacy_tests;
    // One line for everything; "on" and "1" are both written as 1 by the file (presence switches).
    const unsigned all = (1u << static_cast<unsigned>(PushLegacyPart::Count)) - 1;
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE13 = 1\n")) == all);
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE13 = on\n")) == all);
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_LEGACY_PACKAGE13 = 0\n")) == 0);
    for (unsigned i = 0; i < static_cast<unsigned>(PushLegacyPart::Count); ++i) {
        const auto& n = veyra::engine::kPushLegacyNames[i];
        CHECK(restoredMask(fromFile(std::string(n.name) + " = 1\n")) == (1u << i));
    }
    // The earlier names are environment-only aliases: the file does not take them.
    CHECK(restoredMask(fromFile("VEYRA_TEST_RP_DEDUP_LEGACY_CADENCE = 1\nVEYRA_TEST_RP_FG_SCHED_LEGACY = all\n")) == 0);
}

TEST(legacy_master_gives_package13_rows_in_the_simulator) {
    namespace F = fgsim;
    // The card defaults through the master switch, in the simulator's terms.
    const auto master = legacy_tests::env({{"VEYRA_TEST_RP_LEGACY_PACKAGE13", "1"}});
    const auto s = veyra::engine::pushFgSchedApplyLegacy(veyra::engine::pushFgSchedResolve(L"", L"", veyra::engine::kPushFgSchedCardDefault), master);
    CHECK(!s.robustAdmit && !s.gridEarly && !s.phaseBehindWarmup && !s.streamSmooth && !s.holdCardsOnly);
    F::Fixes f;
    f.admit = s.robustAdmit ? 1 : 0;
    f.gridEarly = s.gridEarly;
    f.phaseBehindWarmup = s.phaseBehindWarmup;
    for (const F::Costs& c : F::costSets())
        for (const F::Source src : {F::Source::Card, F::Source::Stream})
            CHECK(fgsim_tests::same(F::simulate(c, src, F::Config{"master", F::parts(false, false, false, false), false, false, f}, 7, 60),
                                    F::simulate(c, src, F::Config{"package13", F::parts(false, false, false, false), false, true}, 7, 60)));
    // The dedup cadence: the master gives the old fit (setLegacyCadence(true) is what the engine passes).
    CHECK(veyra::engine::pushLegacyRestored(veyra::engine::PushLegacyPart::DedupCadence, master));
}
