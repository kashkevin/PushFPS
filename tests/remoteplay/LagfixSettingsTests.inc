// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "lagfix": what a plain install takes from slipstream.ini (veyra/engine/SlipstreamSettings.h,
// slipstreamFileMayApply / splitSlipstreamForFile / describeSwitchesInForce).
#include "veyra/engine/SlipstreamSettings.h"

TEST(lagfix_ini_lines_left_from_a_test_do_not_reach_normal_play) {
    namespace se = veyra::engine;
    // A slipstream.ini as a test session could leave it: every frame-path experiment and test switch is ignored, input,
    // network and audio switches still apply. Before stream "lagfix" every one of these was set.
    const auto settings = se::parseSlipstreamSettings(
        "mode=balanced\nlan=1\nVEYRA_EXP_RP_FG_ULL=hold,phase,grace\nVEYRA_EXP_RP_STATS=1\nVEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high\n"
        "VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1\nVEYRA_EXP_RP_SMOOTH_TARGET=90\nVEYRA_EXP_RP_DELIVERY=catchup\nVEYRA_TEST_RP_PIPELINE=1\nVEYRA_TEST_RP_LEGACY_TIMING=1\n"
        "VEYRA_EXP_RP_KBM_SENS=2\nVEYRA_EXP_RP_AUDIO_SMOOTH=1\nVEYRA_EXP_RP_RCVBUF=4\nsmartlink=observe\n");
    CHECK(settings.valid);
    const auto split = se::splitSlipstreamForFile(settings);
    std::vector<std::string> applied, ignored;
    for (const auto& s : split.apply) applied.push_back(s.name);
    for (const auto& s : split.ignored) ignored.push_back(s.name);
    const auto has = [](const std::vector<std::string>& list, const char* name) { return std::find(list.begin(), list.end(), name) != list.end(); };
    for (const char* name : {"VEYRA_EXP_RP_FG_ULL", "VEYRA_EXP_RP_STATS", "VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY", "VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE",
                             "VEYRA_EXP_RP_SMOOTH_TARGET", "VEYRA_EXP_RP_DELIVERY", "VEYRA_EXP_RP_ADAPTIVE_PHASE", "VEYRA_TEST_RP_PIPELINE"}) {
        CHECK(has(ignored, name));
        CHECK(!has(applied, name));
    }
    for (const char* name : {"VEYRA_EXP_RP_KBM_SENS", "VEYRA_EXP_RP_AUDIO_SMOOTH", "VEYRA_EXP_RP_RCVBUF", "VEYRA_EXP_RP_SMARTLINK", "VEYRA_EXP_RP_LAN_PROFILE",
                             "VEYRA_TEST_RP_LEGACY_TIMING"})   // docs/SETUP.md rollback step 1
        CHECK(has(applied, name));
    CHECK(applied.size() + ignored.size() == settings.switches.size());
    // ADAPTIVE_PHASE came from the mode line: the log says so.
    CHECK(se::describeIgnoredSlipstreamSwitches(split.ignored).find("VEYRA_EXP_RP_ADAPTIVE_PHASE=1(mode)") != std::string::npos);
}

TEST(lagfix_no_frame_path_switch_is_on_the_ini_list) {
    namespace se = veyra::engine;
    // Every switch the program reads that touches how frames are processed, scheduled or presented (and the statistics,
    // which change the PS5 frame hand-off path).
    for (const char* name : {"VEYRA_EXP_RP_ADAPTIVE_PHASE", "VEYRA_EXP_RP_FG_ULL", "VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY", "VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE",
                             "VEYRA_EXP_RP_FIT_DISPLAY", "VEYRA_EXP_RP_SMOOTH_CAP_FIT", "VEYRA_EXP_RP_SMOOTH_SR_DISPLAY", "VEYRA_EXP_RP_SMOOTH_STEADY",
                             "VEYRA_EXP_RP_SMOOTH_TARGET", "VEYRA_EXP_RP_STREAM_DEDUP", "VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN", "VEYRA_EXP_RP_DELIVERY",
                             "VEYRA_EXP_RP_STATS", "VEYRA_EXP_RP_NR_ADAPT", "VEYRA_EXP_RP_GOVERNOR", "VEYRA_EXP_RP_GOVERNOR_BUDGET_MS",
                             "VEYRA_EXP_RP_GPU_BUDGET", "VEYRA_EXP_RP_DECODE_KEEP_HW", "VEYRA_EXP_RP_TIMER_1MS", "VEYRA_EXP_RP_CLOCK_RATE",
                             "VEYRA_EXP_RP_EXPIRY_FIX", "VEYRA_TEST_RP_PIPELINE", "VEYRA_TEST_RP_PROBE_FRAMES", "VEYRA_TEST_RP_CODEC",
                             "VEYRA_EXP_RP_SOMETHING_NEW"})
        CHECK(!se::slipstreamFileMayApply(name));
}

TEST(lagfix_switches_in_force_line_names_where_each_came_from) {
    namespace se = veyra::engine;
    CHECK(se::describeSwitchesInForce({}, {}) == "none");
    CHECK(se::describeSwitchesInForce({{"PATH", "x"}, {"QT_QPA_PLATFORM", "windows"}}, {}) == "none");
    const std::string line = se::describeSwitchesInForce(
        {{"VEYRA_EXP_RP_STATS", "1"}, {"VEYRA_EXP_RP_KBM_SENS", "2"}, {"VEYRA_TEST_RP_NO_SETTINGS_FILE", "1"}, {"PATH", "x"}}, {"VEYRA_EXP_RP_KBM_SENS"});
    CHECK(line == "VEYRA_EXP_RP_KBM_SENS=2(ini) VEYRA_EXP_RP_STATS=1(env) VEYRA_TEST_RP_NO_SETTINGS_FILE=1(env)");
    const std::string longValue(80, 'a');
    CHECK(se::describeSwitchesInForce({{"VEYRA_EXP_RP_KBM_BINDS", longValue}}, {}) == "VEYRA_EXP_RP_KBM_BINDS=" + std::string(48, 'a') + "(env)");
}
