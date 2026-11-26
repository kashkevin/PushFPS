// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s12-settings": PushEffectNote / PushRunningNow / PushEffectBoard against the fixture bridge in QuickSmokeTests.cpp.
// The facts are his Package 16 configuration (SETTINGS_AUDIT.md): NVIDIA VFG quality 5, 4X asked, 3X running under
// Smoothness Mode 90, duplicate removal on with nothing to remove, Vsync pacing, the driver reporting low latency off.
// Interface logic only; the rules are tested in tests/remoteplay/SettingsEffectTests.inc.
import QtQuick
import QtQuick.Layouts
import QtTest
import Veyra
Item {
    id: root
    width: 700; height: 700
    ColumnLayout {
        x: 10; y: 10; width: 600
        PushEffectBoard { id: board; active: false }
        PushRunningNow { id: running; effect: board.facts; liveSource: () => ({latencyMs: 30.6, latencyWaitMs: 22.2}) }
        PushEffectNote { id: multiplierNote; effect: board.facts; setting: "multiplier"; pauseSeconds: 3 }
        PushEffectNote { id: flowNote; effect: board.facts; setting: "flow" }
        PushEffectNote { id: dedupNote; effect: board.facts; setting: "dedup" }
        PushEffectNote { id: adaptiveNote; effect: board.facts; setting: "adaptive"; alsoDedup: true }
        PushEffectNote { id: llNote; effect: board.facts; setting: "nvidiaLowLatency" }
        PushEffectNote { id: queueNote; effect: board.facts; setting: "lowQueue" }
        PushEffectNote { id: enhanceNote; effect: board.facts; setting: "enhancement" }
        PushEffectNote { id: hudNote; effect: board.facts; setting: "hudLock" }
        PushEffectNote { id: vfgNote; effect: board.facts; setting: "vfgQuality" }
        PushEffectNote { id: groupNote; effect: board.facts; setting: "flow"; groupRows: 3 }
    }

    function his() {
        return {
            known: true, playing: true, requestedX: 4, runningX: 3, generatedPerReal: 2.0, smoothTargetFps: 90, capFps: 0,
            contentFps: 30, vfg: true, vfgQuality: 5, vfgModels: "L,M", hudLockState: 0, dedupOn: true, driverLowLatency: 0,
            enhanceWidth: 1920, enhanceHeight: 1080, outputWidth: 3840, outputHeight: 2160, backendLabel: "NVIDIA VFG",
            realPresented: 300, applying: false, lowDelayActive: false, presentSinkFg: false,
            vfgPositions: [{model: "L", gpuMs: 1.87}, {model: "M", gpuMs: 4.16}],
            rows: {
                multiplier: {why: "limited-by-smoothness", requested: true, executing: true},
                flow: {why: "own-motion", requested: true, executing: false},
                dedup: {why: "content-clock", requested: true, executing: true},
                adaptive: {why: "off", requested: false, executing: false},
                nvidiaLowLatency: {why: "driver-reports-off", requested: true, executing: false},
                lowQueue: {why: "already-by-vsync", requested: true, executing: false},
                enhancement: {why: "works-at-lower-size", requested: true, executing: true},
                hudLock: {why: "running", requested: true, executing: true},
                vfgQuality: {why: "running", requested: true, executing: true}
            }
        }
    }

    TestCase {
        name: "PushSettingsEffect"
        when: windowShown
        function init() { board.stateSource = () => root.his(); board.refresh() }

        function test_board_reads_the_source() {
            compare(board.facts.runningX, 3)
            board.stateSource = null
            veyra.settingsEffect = ({known: false, playing: false, rows: ({})})
            board.refresh()
            compare(board.facts.playing, false)
        }
        function test_multiplier_says_what_runs_and_why() {
            verify(multiplierNote.visible)
            verify(multiplierNote.text.indexOf("3") >= 0 && multiplierNote.text.indexOf("90") >= 0, multiplierNote.text)
            compare(multiplierNote.tone, "amber")
        }
        function test_dead_controls_are_not_silent() {
            verify(flowNote.visible && flowNote.text.indexOf("NVIDIA VFG") >= 0, flowNote.text)
            // Review 1 MUST 3: duplicate removal is not dead on his signal: grey, and it says what it does.
            verify(dedupNote.visible && dedupNote.text.indexOf("2 ms") >= 0, dedupNote.text)
            compare(dedupNote.tone, "grey")
            verify(groupNote.visible && groupNote.text.indexOf("3") >= 0 && groupNote.text.indexOf("NVIDIA VFG") >= 0, groupNote.text)
            verify(llNote.visible && llNote.text.length > 0)
            verify(queueNote.visible && queueNote.text.length > 0)
            verify(enhanceNote.visible && enhanceNote.text.indexOf("1920") >= 0 && enhanceNote.text.indexOf("3840") >= 0, enhanceNote.text)
        }
        function test_vfg_quality_names_the_model_per_position() {
            verify(vfgNote.visible, vfgNote.text)
            verify(vfgNote.text.indexOf("3X") >= 0 && vfgNote.text.indexOf("1.9") >= 0 && vfgNote.text.indexOf("4.2") >= 0, vfgNote.text)
            compare(vfgNote.tone, "grey")
        }
        function test_lower_delay_positions() {
            // Review 2 MUST 1: one VFG frame (the primary model) and the predicted frame; not "different models".
            const f = root.his(); f.lowDelayActive = true
            f.vfgPositions = [{model: "M", gpuMs: 4.2}, {model: "P", gpuMs: 0.6}]
            board.stateSource = () => f; board.refresh()
            verify(vfgNote.text.indexOf(qsTr("第 %1 个生成帧是预测帧").arg(2)) >= 0, vfgNote.text)
            verify(vfgNote.text.indexOf(qsTr("（相邻的生成帧来自不同模型）")) < 0, vfgNote.text)
            tryVerify(() => running.line2.indexOf(qsTr("预测帧")) >= 0)
        }
        function test_low_latency_and_queue_say_only_what_is_observed() {
            // The driver's Sleep status is the only observation: the text names it, never the control panel's mode.
            verify(llNote.text.indexOf("Sleep") >= 0 && llNote.text.indexOf(qsTr("低延迟模式")) < 0, llNote.text)
            const f = root.his(); f.rows.lowQueue = {why: "not-measurable", requested: true, executing: false}
            board.stateSource = () => f; board.refresh()
            verify(queueNote.visible && queueNote.text.indexOf("1 帧") < 0, queueNote.text)
            compare(queueNote.tone, "grey")
        }
        function test_not_measurable_is_grey() {
            const f = root.his(); f.rows.hudLock = {why: "not-measurable", requested: true, executing: false}
            board.stateSource = () => f; board.refresh()
            verify(hudNote.visible)
            compare(hudNote.tone, "grey")
        }
        function test_nothing_to_do_is_grey() {
            // Review 3 SHOULD 6: nothing to repair / to choose / already by Vsync are not faults.
            for (const why of ["nothing-to-repair", "nothing-to-choose", "already-by-vsync"]) {
                const f = root.his(); f.rows.hudLock = {why: why, requested: true, executing: false}
                board.stateSource = () => f; board.refresh()
                verify(hudNote.visible, why)
                compare(hudNote.tone, "grey", why)
            }
            const g = root.his(); g.rows.hudLock = {why: "needs-automatic-sync", requested: true, executing: false}
            board.stateSource = () => g; board.refresh()
            compare(hudNote.tone, "amber")
        }
        function test_applying_hides_stale_reasons() {
            const f = root.his(); f.applying = true
            board.stateSource = () => f; board.refresh()
            compare(multiplierNote.reason, "")
            verify(multiplierNote.parts.length >= 1)
            tryVerify(() => running.line1.length > 0)
            compare(running.line2, "")
            compare(running.line3, "")
        }
        function test_line3_hidden_with_lower_delay_or_xess() {
            const f = root.his(); f.lowDelayActive = true
            board.stateSource = () => f; board.refresh()
            compare(running.line3, "")
            f.lowDelayActive = false; f.presentSinkFg = true
            board.stateSource = () => f; board.refresh()
            compare(running.line3, "")
        }
        function test_line2_needs_enough_frames() {
            const f = root.his(); f.realPresented = 5
            board.stateSource = () => f; board.refresh()
            verify(running.line2.indexOf("2.00") < 0, running.line2)
        }
        function test_unknown_is_not_running() {
            const f = root.his(); f.rows.hudLock = {why: "unknown", requested: true, executing: false}
            board.stateSource = () => f; board.refresh()
            verify(hudNote.visible && hudNote.text.length > 0)
            verify(running.line4.length > 0 && running.line4 !== qsTr("实时 HUD 锁定：运行中"), running.line4)
        }
        function test_running_or_off_shows_nothing() {
            verify(!hudNote.visible)
            verify(!adaptiveNote.visible)   // off: and alsoDedup speaks only when the switch is on
            const f = root.his(); f.rows.adaptive = {why: "nothing-to-choose", requested: true, executing: false}
            board.stateSource = () => f; board.refresh()
            verify(adaptiveNote.visible && adaptiveNote.parts.length === 2, adaptiveNote.text)
        }
        function test_running_now_block() {
            tryVerify(() => running.line1.length > 0)
            verify(running.line1.indexOf("NVIDIA VFG") >= 0 && running.line1.indexOf("3X") >= 0 && running.line1.indexOf("4X") >= 0, running.line1)
            verify(running.line2.indexOf("2.00") >= 0, running.line2)
            tryVerify(() => running.line3.length > 0)
            verify(running.line3.indexOf("31") >= 0 && running.line3.indexOf("22") >= 0, running.line3)
            const f = root.his(); f.playing = false
            board.stateSource = () => f; board.refresh()
            compare(running.line2, "")
            verify(running.line1.length > 0)
        }
        function test_not_playing_hides_engine_only_reasons() {
            const f = root.his(); f.playing = false; f.rows.multiplier = {why: "not-playing", requested: true, executing: false}
            board.stateSource = () => f; board.refresh()
            // Only the pause line remains for the multiplier.
            compare(multiplierNote.parts.length, 1)
            compare(multiplierNote.tone, "grey")
        }
    }
}
