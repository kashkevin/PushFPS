// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Performance modes: the table (PushModes.js against the rows of tests/remoteplay/PerformanceModeTests.inc),
// what choosing a mode writes and what "off" puts back, the quick setup row with its disclosure line and
// LAN switch, the driver that follows the source, the first-launch card and the statistics overlay.
// UI logic only, against the fixture bridge in QuickSmokeTests.cpp, which has no Automatic mode and no
// hardware summary: the driver's two Automatic hooks and the card's hardware are set here.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushModes.js" as Modes
Item {
    id: root
    width: 700; height: 1600
    PushQuickSetup { id: quick; x: 10; y: 10; width: 560 }
    property var notices: []
    PushModeDriver { id: driver; onNotice: text => root.notices = root.notices.concat([text]) }
    PushWelcomeCard { id: welcome; x: 10; y: 900; width: 560 }
    PushStatsOverlay { id: overlay; x: 10; y: 1300 }
    property string navigated: ""
    Connections { target: veyra; function onNavigate(p) { root.navigated = p } }

    TestCase {
        name: "PushSlipstreamModes"
        when: windowShown
        function init() {
            driver.timed = false
            driver.autoAvailable = false
            driver.autoStep = null
            veyra.preferences = ({})
            veyra.fgMultiplier = 1
            veyra.fgEnabled = false
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            veyra.effectCapabilities = ({nr2:{available:true},nr3:{available:true},nr4:{available:true}})
            veyra.contentRate = 0
            veyra.nvidiaLowLatencyAvailable = false
            veyra.nvidiaLowLatency = 0
            veyra.fgLowQueue = false
            veyra.vrrState = 0
            veyra.displaySync = 2
            veyra.variableRefresh = false
            veyra.outputRateMode = 0
            veyra.outputCustomFps = 60
            veyra.presentationOwned = false
            veyra.displayRefreshHz = 240
            veyra.hasSource = false
            veyra.liveStats = ({running: false})
            driver.measured = 0
            driver.applied = ""
            driver.forget()
            root.notices = []
            root.navigated = ""
            welcome.loaded = true        // the card's own timer would ask a bridge the fixture does not have
            welcome.hardware = ({})
            welcome.pairingPossible = false
            overlay.stats = ({})
        }
        function cleanup() { init(); veyra.hasSource = true }
        function control(name) { const c = findChild(quick, name); verify(c !== null, name); return c }
        function rows() { return control("pushfps-mode-rows") }
        function near(a, b, tolerance) { return Math.abs(a - b) <= (tolerance === undefined ? 0.01 : tolerance) }

        // The same rows, value for value, as performance_mode_plan_rows_shared_with_the_interface in
        // tests/remoteplay/PerformanceModeTests.inc: mode, source fps, display Hz, valid, multiplier,
        // output fps, least added delay in ms, uneven without variable refresh, reason. Change both.
        readonly property var table: [
            ["ultra-low-latency", 29.97, 180, true, 1, 29.9700, 0.0000, false, "ok"],
            ["ultra-low-latency", 59.94, 180, true, 1, 59.9400, 0.0000, false, "ok"],
            ["competitive", 30, 180, true, 1, 30.0000, 0.0000, false, "ok"],
            ["competitive", 59.94, 180, true, 2, 119.8800, 8.3417, true, "ok"],
            ["balanced", 30, 180, true, 3, 90.0000, 22.2222, false, "ok"],
            ["balanced", 60, 180, true, 2, 120.0000, 8.3333, true, "ok"],
            ["maximum-smoothness", 30, 180, true, 4, 120.0000, 25.0000, true, "ok"],
            ["maximum-smoothness", 60, 180, true, 3, 180.0000, 11.1111, false, "ok"],
            ["maximum-smoothness", 59.94, 120, true, 2, 119.8800, 8.3417, false, "display-limit"],
            ["balanced", 30, 60, true, 2, 60.0000, 16.6667, false, "display-limit"],
            ["competitive", 60, 60, true, 1, 60.0000, 0.0000, false, "display-limit"],
            ["maximum-smoothness", 60, 179.82, true, 3, 180.0000, 11.1111, false, "ok"],
            ["maximum-smoothness", 30, 144, true, 4, 120.0000, 25.0000, true, "ok"],
            ["maximum-smoothness", 60, 144, true, 2, 120.0000, 8.3333, true, "display-limit"],
            ["balanced", 40, 180, false, 1, 0.0000, 0.0000, false, "bad-source-rate"],
            ["balanced", 30, 0, false, 1, 0.0000, 0.0000, false, "bad-display-rate"],
            ["maximum-smoothness", 60, 240, true, 3, 180.0000, 11.1111, true, "ok"],
            ["maximum-smoothness", 30, 360, true, 4, 120.0000, 25.0000, false, "ok"],
            ["balanced", 60, 360, true, 2, 120.0000, 8.3333, false, "ok"],
            ["maximum-smoothness", 29.97, 165, true, 4, 119.8800, 25.0250, true, "ok"],
            ["competitive", 59.94, 100, true, 1, 59.9400, 0.0000, true, "display-limit"]
        ]
        function test_plan_rows_shared_with_the_engine_header() {
            for (const r of table) {
                const p = Modes.plan(r[0], r[1], r[2])
                const what = r[0] + " " + r[1] + " @ " + r[2]
                compare(p.valid, r[3], what)
                compare(p.multiplier, r[4], what)
                compare(p.frameGeneration, r[4] > 1, what)
                verify(near(p.outputFps, r[5], 0.0001), what + " output " + p.outputFps)
                verify(near(p.minAddedDelayMs, r[6], 0.0001), what + " delay " + p.minAddedDelayMs)
                compare(p.unevenWithoutVrr, r[7], what)
                compare(p.reason, r[8], what)
            }
            // The output is never above the display, for any refresh rate from 60 to 360 Hz.
            for (const id of Modes.ids)
                for (const fps of [29.97, 30, 59.94, 60])
                    for (let hz = 60; hz <= 360; hz += 5) {
                        const p = Modes.plan(id, fps, hz)
                        verify(p.valid && p.multiplier >= 1 && p.multiplier <= 4)
                        verify(p.multiplier === 1 || p.outputFps <= hz * 1.02, id + " " + fps + " @ " + hz)
                        verify(near(p.minAddedDelayMs, p.multiplier > 1 ? (p.multiplier - 1) / p.multiplier * 1000 / fps : 0, 1e-9))
                    }
            compare(Modes.plan("automatic", 30, 180).reason, "mode-not-concrete")
            compare(Modes.delayMs(1, 30), 0)
            compare(Modes.delayMs(3, 0), 0)
        }

        // --- 1. a mode sets the real settings, and off puts back what was there ------------------
        function test_mode_sets_generation_removal_queue_sync_and_cap() {
            // 30 FPS game, 240 Hz screen, a GPU without NVIDIA low latency, variable refresh not confirmed.
            verify(rows().pick("balanced"))
            verify(veyra.fgEnabled); compare(veyra.fgMultiplier, 3, "30 -> 90")
            compare(veyra.contentRate, 1, "automatic duplicate removal before generating")
            compare(veyra.fgLowQueue, false, "Balanced is not a low-queue mode")
            compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
            compare(veyra.outputRateMode, 2); compare(veyra.outputCustomFps, 90)
            compare(veyra.preferences.pushMode, "balanced")
            verify(near(rows().lastResult.delayMs, 22.22))
            compare(JSON.stringify(JSON.parse(veyra.preferences.pushModeRestore)),
                    JSON.stringify({fg: false, mult: 1, content: 0, nvidia: 0, queue: false, display: 2, vrr: false, rate: 0, fps: 60}))
            const kept = veyra.preferences.pushModeRestore

            verify(rows().pick("maximum-smoothness"))
            compare(veyra.fgMultiplier, 4, "30 -> 120"); compare(veyra.outputCustomFps, 120)
            verify(near(rows().lastResult.delayMs, 25))
            compare(veyra.preferences.pushModeRestore, kept, "moving between modes keeps the settings from before the first")

            verify(rows().pick("competitive"))
            verify(!veyra.fgEnabled, "a 30 FPS game: Competitive does not generate")
            compare(veyra.contentRate, 0, "as before"); compare(veyra.fgLowQueue, true); compare(veyra.displaySync, 2)
            compare(veyra.outputRateMode, 0); compare(rows().lastResult.delayMs, 0)

            verify(rows().pick("ultra-low-latency"))
            verify(!veyra.fgEnabled); compare(veyra.fgLowQueue, true)
            compare(veyra.displaySync, 0, "no confirmed variable refresh: tearing rather than waiting")
            compare(veyra.outputRateMode, 0, "nothing held for pacing")
            verify(rows().lastResult.notes.indexOf("tearing") >= 0)

            verify(rows().pick(""))
            verify(!veyra.fgEnabled); compare(veyra.contentRate, 0); compare(veyra.fgLowQueue, false)
            compare(veyra.displaySync, 2); compare(veyra.variableRefresh, false)
            compare(veyra.outputRateMode, 0); compare(veyra.outputCustomFps, 60)
            compare(veyra.preferences.pushMode, ""); compare(veyra.preferences.pushModeRestore, "{}")
        }
        function test_sixty_fps_game_on_a_180_hz_gsync_screen() {
            // What the user had: 2X capped at 120 with the fixed 60 -> 30 removal, V-Sync with variable refresh.
            veyra.setPreference("pushGameFps", 60)
            veyra.nvidiaLowLatencyAvailable = true; veyra.vrrState = 4; veyra.displayRefreshHz = 180
            veyra.fgMultiplier = 2; veyra.outputCustomFps = 120; veyra.outputRateMode = 2; veyra.contentRate = 5
            veyra.displaySync = 1; veyra.variableRefresh = true

            verify(rows().pick("competitive"))
            compare(veyra.fgMultiplier, 2, "60 -> 120"); compare(veyra.nvidiaLowLatency, 1, "NVIDIA low latency, not the plain queue")
            compare(veyra.fgLowQueue, false); compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
            compare(veyra.outputRateMode, 2); compare(veyra.outputCustomFps, 120)
            compare(veyra.contentRate, 5, "a removal mode the user chose stays")
            verify(near(rows().lastResult.delayMs, 8.33)); compare(rows().lastResult.notes.length, 0)

            verify(rows().pick("maximum-smoothness"))
            compare(veyra.fgMultiplier, 3, "60 -> 180 because the display allows it")
            compare(veyra.outputRateMode, 1, "180 on 180 Hz: the cap follows the display")
            compare(veyra.nvidiaLowLatency, 0, "not a low-queue mode: as it was before")
            verify(near(rows().lastResult.delayMs, 11.11))

            verify(rows().pick("ultra-low-latency"))
            verify(!veyra.fgEnabled); compare(veyra.nvidiaLowLatency, 1)
            compare(veyra.displaySync, 1, "variable refresh is active: no tearing needed"); compare(veyra.outputRateMode, 0)
            compare(rows().lastResult.notes.length, 0)

            verify(rows().pick(""))
            compare(veyra.fgMultiplier, 2); compare(veyra.outputRateMode, 2); compare(veyra.outputCustomFps, 120)
            compare(veyra.nvidiaLowLatency, 0); compare(veyra.contentRate, 5); compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
        }
        function test_unsupported_combinations_fall_back_to_lower_latency_and_say_so() {
            // 60 -> 180 does not fit a 120 Hz screen: 2X.
            veyra.setPreference("pushGameFps", 60); veyra.displayRefreshHz = 120
            verify(rows().pick("maximum-smoothness"))
            compare(veyra.fgMultiplier, 2); verify(rows().lastResult.notes.indexOf("display-limit") >= 0)
            compare(veyra.outputRateMode, 1, "never asks for more than the display shows")
            verify(rows().disclosure.indexOf("120") >= 0, rows().disclosure)
            // The source already fills a 60 Hz screen: nothing to generate.
            veyra.displayRefreshHz = 60
            verify(rows().pick("competitive")); verify(!veyra.fgEnabled)
            verify(rows().lastResult.notes.indexOf("display-limit") >= 0)
            rows().pick("")
            // A method that only offers 2X: Balanced for a 30 FPS game uses 2X, and the delay shown is 2X's.
            veyra.setPreference("pushGameFps", 30); veyra.displayRefreshHz = 240
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"}]
            verify(rows().pick("balanced"))
            compare(veyra.fgMultiplier, 2); verify(rows().lastResult.notes.indexOf("backend-limit") >= 0)
            verify(near(rows().lastResult.delayMs, 16.67)); compare(veyra.outputCustomFps, 60)
            rows().pick("")
            // Frame generation cannot run at all: no generation, said so, no delay claimed.
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            veyra.effectCapabilities = ({"frame-generation": {available: false, reason: "unavailable"}})
            verify(rows().pick("maximum-smoothness"))
            verify(!veyra.fgEnabled); verify(rows().lastResult.notes.indexOf("fg-unavailable") >= 0)
            compare(rows().lastResult.delayMs, 0); compare(veyra.outputRateMode, 0)
            rows().pick("")
            // A 40 FPS game has no row: unknown input never adds delay.
            veyra.effectCapabilities = ({})
            veyra.setPreference("pushGameFps", 40)
            verify(rows().pick("balanced")); verify(!veyra.fgEnabled)
            compare(rows().lastResult.notes[0], "bad-source-rate")
            rows().pick("")
            // A refresh rate nobody has read yet: the same.
            veyra.setPreference("pushGameFps", 30); veyra.displayRefreshHz = 0
            verify(rows().pick("balanced")); verify(!veyra.fgEnabled)
            compare(rows().lastResult.notes[0], "bad-display-rate")
        }
        function test_variable_refresh_decides_sync_by_capability() {
            // 2: the screen can, but not now. Not counted on by a low-queue mode.
            veyra.vrrState = 2
            rows().pick("ultra-low-latency"); compare(veyra.displaySync, 0)
            rows().pick("")
            // 3: possible.
            veyra.vrrState = 3
            rows().pick("ultra-low-latency"); compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
            rows().pick("")
            compare(veyra.displaySync, 2); compare(veyra.variableRefresh, false)
            // A method that presents by itself (XeSS): pacing is not ours to set.
            veyra.vrrState = 0; veyra.presentationOwned = true
            rows().pick("balanced")
            compare(veyra.fgMultiplier, 3); compare(veyra.displaySync, 2); compare(veyra.outputRateMode, 0); compare(veyra.variableRefresh, false)
        }
        function test_measured_source_rate_wins_over_the_chosen_one() {
            veyra.hasSource = true
            veyra.liveStats = ({running: true, cadenceActive: true, cadenceSourceFps: 59.94, contentFps: 30})
            verify(rows().pick("balanced"))
            compare(veyra.fgMultiplier, 2, "the stream's pictures are all new: 60 -> 120")
            compare(veyra.outputCustomFps, 120)
            verify(near(rows().lastResult.delayMs, 8.34))
            verify(rows().measured)
            // A rate the table has no row for is not used: the chosen 30 stays.
            veyra.liveStats = ({running: true, contentFps: 45})
            verify(rows().pick("balanced")); compare(veyra.fgMultiplier, 3)
        }
        function test_nothing_changes_until_a_mode_is_picked() {
            veyra.fgMultiplier = 2; veyra.outputRateMode = 1; veyra.displaySync = 1
            const before = JSON.stringify(Modes.snapshot(veyra))
            compare(rows().mode, "")
            compare(rows().shown, null)
            // Looking at what a mode would do writes nothing.
            const preview = Modes.preview(veyra, "maximum-smoothness", 30)
            compare(preview.multiplier, 4)
            compare(JSON.stringify(Modes.snapshot(veyra)), before)
            compare(Object.keys(veyra.preferences).length, 0)
            // Off without a mode leaves the user's settings alone.
            rows().pick("")
            compare(JSON.stringify(Modes.snapshot(veyra)), before)
        }

        // --- 2. the row ---------------------------------------------------------------------------
        function test_selector_offers_the_four_modes_and_off() {
            const select = control("pushfps-mode")
            compare(select.options.map(o => o.id).join(","), ",ultra-low-latency,competitive,balanced,maximum-smoothness")
            compare(select.value, select.options[0].label, "off (my own settings)")
            select.picked("balanced")
            compare(veyra.preferences.pushMode, "balanced")
            compare(select.value, select.options[3].label)
            verify(veyra.fgEnabled)
            select.picked("")
            compare(veyra.preferences.pushMode, "")
            compare(select.value, select.options[0].label)
            verify(!veyra.fgEnabled)
            // A value nobody wrote is treated as off.
            veyra.setPreference("pushMode", "automatic")
            compare(rows().mode, "")
        }
        function test_disclosure_line_gives_the_least_added_delay() {
            const row = control("pushfps-mode-row")
            const off = row.hint
            verify(off.length > 0)
            rows().pick("balanced")
            verify(row.hint !== off)
            verify(row.hint.indexOf("22.2") >= 0, row.hint)   // (3-1)/3 x 33.3 ms
            verify(row.hint.indexOf("90") >= 0, row.hint)
            rows().pick("maximum-smoothness")
            verify(row.hint.indexOf("25.0") >= 0, row.hint)   // (4-1)/4 x 33.3 ms
            veyra.setPreference("pushGameFps", 60)
            verify(rows().shown.multiplier === 3 && near(rows().shown.delayMs, 11.11), "the line follows the game frame rate")
            verify(row.hint.indexOf("11.1") >= 0, row.hint)
            rows().pick("ultra-low-latency")
            compare(rows().shown.delayMs, 0)
            verify(row.hint.indexOf("11.1") < 0 && row.hint.indexOf("25.0") < 0, row.hint)
            // Changed in another row afterwards: the line says so.
            rows().pick("balanced")
            verify(!rows().changedByHand)
            veyra.fgMultiplier = 4
            verify(rows().changedByHand)
        }
        function test_lan_profile_is_a_separate_switch_for_the_next_connection() {
            const sw = control("pushfps-mode-lan")
            const row = control("pushfps-mode-row")
            verify(!sw.checked)
            compare(rows().lanLine, "")
            const before = JSON.stringify(Modes.snapshot(veyra))
            sw.toggled(true)
            compare(veyra.preferences.pushLanProfile, true)
            verify(sw.checked)
            verify(rows().lanLine.length > 0 && row.hint.indexOf(rows().lanLine) >= 0, "says it applies at the next connection")
            compare(JSON.stringify(Modes.snapshot(veyra)), before, "no setting of the running picture changes")
            compare(rows().mode, "", "works without a mode")
            rows().pick("competitive")
            compare(veyra.preferences.pushLanProfile, true, "and with any of them")
            rows().pick("")
            compare(veyra.preferences.pushLanProfile, true)
            sw.toggled(false)
            compare(veyra.preferences.pushLanProfile, false)
        }
        function test_automatic_is_hidden_without_the_experiment_switch() {
            verify(!control("pushfps-mode-auto-row").visible)
            verify(!rows().autoOn)
            veyra.setPreference("pushModeAuto", true)
            verify(!rows().autoOn, "the preference alone does nothing")
            rows().autoPossible = true
            verify(control("pushfps-mode-auto-row").visible)
            verify(!control("pushfps-mode-auto").enabled, "needs a mode as its ceiling")
            rows().pick("balanced")
            verify(control("pushfps-mode-auto").enabled && rows().autoOn)
            rows().autoPossible = false
        }

        // --- the driver ----------------------------------------------------------------------------
        function test_driver_is_idle_without_a_mode() {
            const timer = findChild(driver, "pushfps-mode-driver-timer")
            verify(timer !== null)
            driver.timed = true
            veyra.hasSource = true
            verify(!timer.running, "no mode: no timer")
            driver.look()
            compare(driver.planKey, "")
            compare(root.notices.length, 0)
            driver.timed = false
        }
        function test_driver_follows_a_source_rate_that_stays_changed() {
            rows().pick("balanced")
            compare(veyra.fgMultiplier, 3)
            veyra.hasSource = true
            veyra.liveStats = ({running: true, contentFps: 30})
            driver.look()                        // first look at this source: the mode is put in place, quietly
            compare(veyra.fgMultiplier, 3); compare(root.notices.length, 0)
            // One look at 60 is not enough.
            veyra.liveStats = ({running: true, contentFps: 60})
            driver.look(); compare(veyra.fgMultiplier, 3)
            veyra.liveStats = ({running: true, contentFps: 30})
            driver.look(); driver.look(); driver.look()
            compare(veyra.fgMultiplier, 3); compare(root.notices.length, 0)
            // Three in a row are.
            veyra.liveStats = ({running: true, contentFps: 60})
            driver.look(); driver.look(); compare(veyra.fgMultiplier, 3)
            driver.look()
            compare(veyra.fgMultiplier, 2, "60 -> 120"); compare(veyra.outputCustomFps, 120)
            compare(root.notices.length, 1)
            // A rate without a row changes nothing; the last known one stays.
            veyra.liveStats = ({running: true, contentFps: 45})
            for (let i = 0; i < 6; ++i) driver.look()
            compare(veyra.fgMultiplier, 2); compare(root.notices.length, 1)
            // The display changing to another row counts the same way.
            veyra.liveStats = ({running: true, contentFps: 60})
            veyra.displayRefreshHz = 60
            driver.look(); driver.look(); verify(veyra.fgEnabled)
            driver.look()
            verify(!veyra.fgEnabled, "60 on a 60 Hz screen: nothing to generate")
        }
        function test_driver_keeps_away_while_the_optimiser_or_quality_test_runs() {
            rows().pick("balanced")
            veyra.hasSource = true
            veyra.liveStats = ({running: true, contentFps: 30})
            driver.look()
            // The optimiser is trying 4X: left alone, however long the source reads 60.
            veyra.setPreference("pushOptimizeRestore", JSON.stringify({fg: true, testing: "pushflow"}))
            veyra.fgMultiplier = 4
            veyra.liveStats = ({running: true, contentFps: 60})
            for (let i = 0; i < 6; ++i) driver.look()
            compare(veyra.fgMultiplier, 4); compare(root.notices.length, 0)
            veyra.setPreference("pushOptimizeRestore", "{}")
            veyra.qualityState = ({running: true, done: false, failed: false})
            for (let i = 0; i < 6; ++i) driver.look()
            compare(veyra.fgMultiplier, 4)
            veyra.qualityState = ({running: false, done: false, failed: false})
            // Afterwards the usual three looks apply.
            driver.look(); driver.look(); compare(veyra.fgMultiplier, 4)
            driver.look(); compare(veyra.fgMultiplier, 2)
        }
        function test_driver_applies_a_changed_game_frame_rate_at_once_when_nothing_is_measured() {
            rows().pick("balanced")
            compare(veyra.fgMultiplier, 3)
            veyra.setPreference("pushGameFps", 60)
            compare(veyra.fgMultiplier, 2)
            compare(root.notices.length, 0)
            rows().pick("")
            veyra.setPreference("pushGameFps", 30)
            verify(!veyra.fgEnabled, "off: the preference changes nothing")
        }
        function test_driver_applies_automatic_decisions_and_shows_the_reason() {
            let decision = { available: true, mode: "maximum-smoothness", changed: true, started: true, reason: "start" }
            let ceilings = []
            driver.autoAvailable = true
            driver.autoStep = function (ceiling) { ceilings.push(ceiling); return decision }
            veyra.setPreference("pushModeAuto", true)
            rows().pick("maximum-smoothness")
            veyra.hasSource = true
            veyra.liveStats = ({running: true, contentFps: 30})
            driver.look()
            compare(ceilings[0], "maximum-smoothness", "the chosen mode is the ceiling")
            compare(veyra.fgMultiplier, 4); compare(root.notices.length, 0, "starting at the ceiling is not a change to announce")
            decision = { available: true, mode: "maximum-smoothness", changed: false, reason: "at-ceiling" }
            driver.look(); compare(root.notices.length, 0)
            decision = { available: true, mode: "balanced", changed: true, reason: "step-down-gpu-busy" }
            driver.look()
            compare(veyra.fgMultiplier, 3, "one step towards lower latency")
            compare(driver.applied, "balanced")
            compare(veyra.preferences.pushMode, "maximum-smoothness", "the ceiling is the user's and stays")
            compare(root.notices.length, 1)
            decision = { available: true, mode: "balanced", changed: false, reason: "healthy-waiting" }
            driver.look(); compare(root.notices.length, 1); compare(veyra.fgMultiplier, 3)
            // Switched off: back to the chosen mode.
            veyra.setPreference("pushModeAuto", false)
            driver.look()
            compare(veyra.fgMultiplier, 4); compare(driver.applied, "maximum-smoothness")
            // Off restores what was there before any of it.
            rows().pick("")
            verify(!veyra.fgEnabled); compare(veyra.outputRateMode, 0)
        }

        // --- 5. the first-launch card ---------------------------------------------------------------
        function textOf(item, prefix, index) { const t = findChild(item, prefix + index); verify(t !== null, prefix + index); return t.text }
        function test_welcome_card_shows_what_the_pc_reported() {
            verify(welcome.visible, "shown until dismissed")
            // Nothing reported yet: said as such, not guessed. The display is read live.
            compare(textOf(welcome, "pushfps-welcome-value-", 0), welcome.none)
            compare(textOf(welcome, "pushfps-welcome-value-", 1), welcome.none)
            compare(textOf(welcome, "pushfps-welcome-value-", 2), welcome.none)
            verify(textOf(welcome, "pushfps-welcome-value-", 3).indexOf("240 Hz") === 0)
            compare(textOf(welcome, "pushfps-welcome-value-", 4), welcome.none)
            // One PC: wired, the saved console on the same network.
            welcome.hardware = ({ gpuName: "Adapter A", gpuVendor: "nvidia", decodeKnown: true, decodeH264: true, decodeHevc: true, decodeHevc10: true,
                                  fgBackends: ["Method one", "Method two"], networkKnown: true, ethernet: true, wifi: false,
                                  routeKnown: true, routeDirectLan: true, routeLink: "ethernet" })
            compare(textOf(welcome, "pushfps-welcome-value-", 0), "Adapter A")
            compare(textOf(welcome, "pushfps-welcome-value-", 1), "H.264 · HEVC · HEVC 10-bit")
            compare(textOf(welcome, "pushfps-welcome-value-", 2), "Method one · Method two")
            verify(!welcome.wifiOnly)
            const wired = textOf(welcome, "pushfps-welcome-value-", 4)
            const advice = findChild(welcome, "pushfps-welcome-advice").text
            // Another PC, another vendor: fewer decode profiles, one method, Wi-Fi only, console elsewhere.
            welcome.hardware = ({ gpuName: "Adapter B", gpuVendor: "amd", decodeKnown: true, decodeH264: true, decodeHevc: false, decodeHevc10: false,
                                  fgBackends: ["Method three"], networkKnown: true, ethernet: false, wifi: true,
                                  routeKnown: true, routeDirectLan: false, routeLink: "wifi" })
            compare(textOf(welcome, "pushfps-welcome-value-", 1), "H.264")
            compare(textOf(welcome, "pushfps-welcome-value-", 2), "Method three")
            verify(welcome.wifiOnly)
            verify(textOf(welcome, "pushfps-welcome-value-", 4) !== wired)
            verify(findChild(welcome, "pushfps-welcome-advice").text.length > advice.length, "Wi-Fi only: wired is suggested")
            // An adapter that lists no decoder at all, and nothing that generates frames.
            welcome.hardware = ({ gpuName: "Adapter C", gpuVendor: "", decodeKnown: true, decodeH264: false, decodeHevc: false, decodeHevc10: false,
                                  fgBackends: [], networkKnown: false })
            verify(textOf(welcome, "pushfps-welcome-value-", 1) !== welcome.none && textOf(welcome, "pushfps-welcome-value-", 1).indexOf("H.264") >= 0)
            verify(textOf(welcome, "pushfps-welcome-value-", 2) !== welcome.none)
            compare(textOf(welcome, "pushfps-welcome-value-", 4), welcome.none)
        }
        function test_welcome_card_reads_any_refresh_rate_and_the_variable_refresh_state() {
            const seen = {}
            for (const hz of [60, 75, 120, 144, 165, 180, 240, 360]) {
                veyra.displayRefreshHz = hz
                verify(textOf(welcome, "pushfps-welcome-value-", 3).indexOf(hz + " Hz") === 0, String(hz))
            }
            veyra.displayRefreshHz = 179.82
            verify(textOf(welcome, "pushfps-welcome-value-", 3).indexOf("180 Hz") === 0)
            for (let state = 0; state <= 4; ++state) {
                veyra.vrrState = state
                seen[textOf(welcome, "pushfps-welcome-value-", 3)] = true
            }
            compare(Object.keys(seen).length, 5, "each variable refresh state reads differently")
            veyra.displayRefreshHz = 0
            verify(textOf(welcome, "pushfps-welcome-value-", 3).indexOf(welcome.none) === 0)
        }
        function test_welcome_card_recommends_ultra_low_latency_links_to_pairing_and_goes_away() {
            veyra.fgMultiplier = 3
            const mode = findChild(welcome, "pushfps-welcome-mode")
            const pair = findChild(welcome, "pushfps-welcome-pair")
            verify(mode.enabled && !welcome.lowLatencyChosen)
            verify(!pair.visible, "no PS5 source in this build: no button")
            welcome.pairingPossible = true
            verify(pair.visible)
            pair.clicked()
            compare(root.navigated, "ps5", "the existing pairing dialog")
            mode.clicked()
            compare(veyra.preferences.pushMode, "ultra-low-latency")
            verify(!veyra.fgEnabled, "no generation in the first test")
            verify(welcome.lowLatencyChosen && !mode.enabled)
            compare(control("pushfps-mode").value, control("pushfps-mode").options[1].label, "the quick setup row shows the same choice")
            verify(welcome.visible, "choosing the mode does not dismiss it")
            findChild(welcome, "pushfps-welcome-dismiss").clicked()
            compare(veyra.preferences.pushWelcomeDone, true)
            verify(!welcome.visible)
            compare(veyra.preferences.pushMode, "ultra-low-latency", "dismissing changes nothing else")
            // Off still restores what was there before the card's button.
            rows().pick("")
            compare(veyra.fgMultiplier, 3)
        }

        // --- 4. the statistics overlay ---------------------------------------------------------------
        function test_overlay_is_off_by_default_and_reads_only_what_is_there() {
            const timer = findChild(overlay, "pushfps-stats-overlay-timer")
            verify(!overlay.wanted && !overlay.visible && !timer.running, "off: not shown, not polling")
            compare(timer.interval, 1000, "at most once a second")
            veyra.liveStats = ({running: true, capture: true, captureFps: 60, contentFps: 0})
            control("pushfps-stats-overlay").toggled(true)
            compare(veyra.preferences.pushStatsOverlay, true)
            overlay.refresh()
            verify(overlay.visible)
            // A capture card, nothing else known: everything that was not measured says so.
            verify(textOf(overlay, "pushfps-stats-value-", 1).indexOf("60.0") >= 0)
            verify(textOf(overlay, "pushfps-stats-value-", 1).indexOf(overlay.none) >= 0, "game and presented rate not measured")
            for (const i of [3, 4, 5, 6, 7, 8]) compare(textOf(overlay, "pushfps-stats-value-", i), overlay.none)
            // A PS5 stream with the cadence and health experiments reporting.
            rows().pick("balanced")
            veyra.liveStats = ({running: true, remotePlay: true, fgActive: true, fgMultiplierNow: 3, submitFps: 89.9, latencyMs: 12.34, latencyP95Ms: 15.5,
                                realPresentFps: 29.97, generatedPresentFps: 59.94, cadenceActive: true, cadenceStreamFps: 59.94, cadenceSourceFps: 29.97,
                                healthValid: true, healthGrade: "unsteady", healthLossPercent: 0.125, healthJitterMs: 1.26, healthRecoveryEvents: 4,
                                routeKnown: true, routeDirectLan: true, routeLink: "ethernet"})
            overlay.refresh()
            verify(textOf(overlay, "pushfps-stats-value-", 0).indexOf("3×") > 0)
            const rates = textOf(overlay, "pushfps-stats-value-", 1)
            verify(rates.indexOf("59.9") >= 0 && rates.indexOf("30.0") >= 0 && rates.indexOf("89.9") >= 0, rates)
            // Submitted unless the presenter knows what is shown; "uneven" when it is not shown evenly.
            compare(rates, qsTr("来源 %1 · 游戏 %2 · 提交 %3").arg("59.9").arg("30.0").arg("89.9"))
            veyra.liveStats = Object.assign({}, veyra.liveStats, {shownFpsKnown: true, shownFps: 89.9, uneven: true})
            overlay.refresh()
            compare(textOf(overlay, "pushfps-stats-value-", 1), qsTr("来源 %1 · 游戏 %2 · 显示 %3").arg("59.9").arg("30.0").arg("89.9" + qsTr(" · 不均匀")))
            const frames = textOf(overlay, "pushfps-stats-value-", 2)
            verify(frames.indexOf("59.9") >= 0 && frames.indexOf("30.0") >= 0, frames)
            verify(textOf(overlay, "pushfps-stats-value-", 3).indexOf("12.3") === 0)
            verify(textOf(overlay, "pushfps-stats-value-", 4) !== overlay.none)
            compare(textOf(overlay, "pushfps-stats-value-", 5), "0.13 %")
            compare(textOf(overlay, "pushfps-stats-value-", 6), "1.3 ms")
            compare(textOf(overlay, "pushfps-stats-value-", 7), "4")
            const lan = textOf(overlay, "pushfps-stats-value-", 8)
            verify(lan !== overlay.none)
            // The same stream over Wi-Fi through a router reads differently; without a health snapshot: not measured.
            const wifi = Object.assign({}, veyra.liveStats, {routeDirectLan: false, routeLink: "wifi"})
            veyra.liveStats = wifi
            overlay.refresh()
            verify(textOf(overlay, "pushfps-stats-value-", 8) !== lan && textOf(overlay, "pushfps-stats-value-", 8).indexOf("Wi-Fi") >= 0)
            veyra.liveStats = Object.assign({}, wifi, {healthValid: false})
            overlay.refresh()
            for (const i of [4, 5, 6, 7, 8]) compare(textOf(overlay, "pushfps-stats-value-", i), overlay.none)
            control("pushfps-stats-overlay").toggled(false)
            verify(!overlay.visible && !timer.running)
        }
    }
}
