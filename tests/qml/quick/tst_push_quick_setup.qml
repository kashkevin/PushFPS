// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PushFPS quick setup: game frame rate, target frame rate, console and game
// profiles. UI logic only, against the fixture bridge in QuickSmokeTests.cpp.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 640; height: 640
    PushQuickSetup { id: quick; x: 10; y: 10; width: 560 }
    Component {
        id: gsyncMock
        QtObject {
            property bool busy: false
            property bool canRepair: false
            property bool canRestore: false
            property string summary: ""
            property string detail: ""
            property int checks: 0
            property int repairs: 0
            signal changed()
            function check() { checks++; busy = true }
            function repair() { repairs++ }
        }
    }
    TestCase {
        name: "PushQuickSetup"
        when: windowShown
        function init() {
            veyra.preferences = ({exportStopsPlayback: false})
            veyra.fgMultiplier = 1
            veyra.fgEnabled = false
            veyra.outputRateMode = 0
            veyra.outputCustomFps = 0
            veyra.nrAnyEnabled = false
            veyra.presets = []
            veyra.currentPresetName = "自定义"
            veyra.lastAppliedPreset = -1
            veyra.fgAdaptive = false
            veyra.pacingRepair = false
            veyra.hudLock = false
            veyra.presentationOwned = false
            veyra.contentRate = 0
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            veyra.liveStats = ({running: false})
            veyra.nrLayers = [{index: 1}]
            veyra.compareMode = 0
            veyra.hasSource = true
        }
        function cleanup() { init() }
        function control(name) { const c = findChild(quick, name); verify(c !== null, name); return c }

        function test_target_sets_multiplier_and_cap() {
            compare(quick.baseFps, 30)
            quick.applyTarget("120")
            compare(veyra.fgMultiplier, 4)
            verify(veyra.fgEnabled)
            compare(veyra.outputRateMode, 2)
            compare(veyra.outputCustomFps, 120)
            compare(quick.currentTarget, "120")
            quick.applyTarget("60")
            compare(veyra.fgMultiplier, 2)
            compare(veyra.outputCustomFps, 60)
            quick.applyTarget("monitor")
            compare(veyra.outputRateMode, 1)
            compare(veyra.fgMultiplier, 4, "240 Hz monitor: the largest offered multiplier")
            quick.applyTarget("off")
            verify(!veyra.fgEnabled)
            compare(quick.currentTarget, "off")
        }
        function test_target_removes_duplicate_frames() {
            compare(veyra.contentRate, 0)
            quick.applyTarget("120")
            compare(veyra.contentRate, 1, "a 30 FPS game in a 60 Hz capture: duplicates removed before generating")
            veyra.contentRate = 5
            quick.applyTarget("60")
            compare(veyra.contentRate, 5, "60->30 stays as chosen")
            veyra.contentRate = 0
            quick.applyTarget("off")
            compare(veyra.contentRate, 0, "nothing generated: nothing changed")
        }
        function test_game_fps_recomputes_the_multiplier() {
            quick.applyTarget("120")
            compare(veyra.fgMultiplier, 4)
            quick.setGameFps("60")
            compare(quick.baseFps, 60)
            compare(veyra.fgMultiplier, 2, "60 FPS game to 120: 2x")
            compare(quick.currentTarget, "120")
            quick.setGameFps("40")
            compare(veyra.fgMultiplier, 3, "40 FPS game to 120: 3x")
        }
        function test_target_the_game_already_reaches_turns_generation_off() {
            quick.applyTarget("60")
            verify(veyra.fgEnabled)
            quick.setGameFps("60")
            verify(!veyra.fgEnabled, "nothing to generate for a 60 FPS game at 60")
            const ids = control("pushfps-target").options.map(o => o.id)
            verify(ids.indexOf("60") < 0, "60 FPS is not offered for a 60 FPS game")
            quick.applyTarget("60")
            verify(!veyra.fgEnabled)
        }
        function test_console_profiles_keep_their_own_choices() {
            compare(quick.consoleId, "ps5")
            quick.applyTarget("120")
            quick.setEnhancement(true)
            quick.switchConsole("xbox")
            compare(quick.consoleId, "xbox")
            quick.applyTarget("off")
            quick.setEnhancement(false)
            quick.switchConsole("ps5")
            compare(quick.currentTarget, "120", "PS5 profile restored")
            verify(veyra.nrAnyEnabled)
            quick.switchConsole("xbox")
            compare(quick.currentTarget, "off", "Xbox profile restored")
            verify(!veyra.nrAnyEnabled)
            verify(quick.setupTip.indexOf("Xbox") === 0)
        }
        function test_game_profiles_save_apply_delete() {
            quick.setGameFps("30")
            quick.applyTarget("120")
            verify(quick.saveGame("  GTA VI  "))
            compare(quick.games.length, 1)
            compare(quick.activeGame, "GTA VI")
            compare(quick.games[0].console, "ps5")
            compare(quick.games[0].preset, "", "no saved preset matched")
            quick.applyTarget("off")
            quick.setGameFps("60")
            quick.applyGame("GTA VI")
            compare(quick.baseFps, 30)
            compare(quick.currentTarget, "120")
            compare(veyra.fgMultiplier, 4)
            // Saving the same name replaces it.
            verify(quick.saveGame("GTA VI"))
            compare(quick.games.length, 1)
            quick.deleteGame("GTA VI")
            compare(quick.games.length, 0)
            compare(quick.activeGame, "")
        }
        function test_game_profile_applies_its_preset() {
            veyra.presets = [{index: 0, name: "Default"}, {index: 3, name: "Sharp"}]
            veyra.currentPresetName = "Sharp"
            verify(quick.saveGame("Forza"))
            compare(quick.games[0].preset, "Sharp")
            quick.applyGame("Forza")
            compare(veyra.lastAppliedPreset, 3)
            // A preset that was deleted since is skipped, the rest still applies.
            veyra.presets = [{index: 0, name: "Default"}]
            veyra.lastAppliedPreset = -1
            quick.applyGame("Forza")
            compare(veyra.lastAppliedPreset, -1)
            compare(quick.activeGame, "Forza")
        }
        function test_bad_input_is_harmless() {
            verify(!quick.saveGame("   "))
            verify(quick.saveGame("x".repeat(80)))
            compare(quick.games[0].name.length, 40)
            veyra.preferences = ({pushProfiles: "{not json", pushConsole: "gamecube", pushGameFps: 999})
            compare(quick.games.length, 0)
            compare(quick.consoleId, "ps5")
            compare(quick.baseFps, 30)
            quick.applyGame("missing")
            compare(quick.activeGame, "")
        }
        // --- adaptive multiplier -------------------------------------------------
        function test_adaptive_makes_the_multiplier_a_ceiling() {
            quick.applyTarget("60")
            compare(veyra.fgMultiplier, 2)
            control("pushfps-adaptive").toggled(true)
            verify(veyra.fgAdaptive)
            compare(veyra.contentRate, 1, "duplicate removal on: the game's frame rate is measured")
            compare(veyra.fgMultiplier, 3, "60 FPS target: room for a dip to 20 FPS")
            quick.applyTarget("120")
            compare(veyra.fgMultiplier, 4, "the largest offered (4X) for 120 FPS")
            compare(veyra.outputCustomFps, 120)
            verify(quick.targetHint.length > 0)
            control("pushfps-adaptive").toggled(false)
            verify(!veyra.fgAdaptive)
            compare(veyra.fgMultiplier, 4, "back to the fixed choice: 30 FPS x 4 = 120")
            quick.applyTarget("60")
            compare(veyra.fgMultiplier, 2)
        }
        function test_adaptive_keeps_a_chosen_duplicate_mode() {
            veyra.contentRate = 5
            quick.applyTarget("120")
            quick.setAdaptive(true)
            compare(veyra.contentRate, 5, "60->30 stays as chosen")
        }
        function test_adaptive_needs_more_than_2x() {
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"}]
            quick.applyTarget("60")
            verify(!quick.adaptivePossible)
            verify(!control("pushfps-adaptive").enabled)
            veyra.fgAdaptive = true
            verify(!quick.adaptive, "a stored on means nothing without a choice above 2X")
        }
        function test_adaptive_live_hint() {
            quick.applyTarget("120")
            quick.setAdaptive(true)
            veyra.liveStats = ({running: true, fgMultiplierNow: 3, contentFps: 38})
            quick.liveStats = veyra.pushLiveStats()
            const row = control("pushfps-adaptive-row")
            compare(row.hint, qsTr("现在 %1×：游戏 %2 FPS → 约 %3 FPS").arg(3).arg(38).arg(114))
        }
        function test_profiles_remember_adaptive() {
            quick.applyTarget("120")
            quick.setAdaptive(true)
            verify(quick.saveGame("Elden"))
            quick.setAdaptive(false)
            quick.applyGame("Elden")
            verify(veyra.fgAdaptive, "the game profile brings adaptive back")
        }
        // --- Frame Pacing Repair and Live HUD Lock ----------------------------------
        function test_pacing_repair_turns_on_duplicate_removal() {
            quick.applyTarget("120")
            control("pushfps-pacing").toggled(true)
            verify(veyra.pacingRepair)
            compare(veyra.contentRate, 1, "the repair re-times what duplicate removal finds")
            control("pushfps-pacing").toggled(false)
            verify(!veyra.pacingRepair)
        }
        function test_pacing_live_hint() {
            quick.applyTarget("120")
            quick.setPacingRepair(true)
            const row = control("pushfps-pacing-row")
            quick.lastPacing = { pacingFrames: 100, pacingRepaired: 50 }
            veyra.liveStats = ({ running: true, pacingFrames: 130, pacingRepaired: 77 })
            // One timer tick, as the real one does.
            const st = veyra.pushLiveStats(), p = quick.lastPacing
            quick.pacingShare = (st.pacingRepaired - p.pacingRepaired) / (st.pacingFrames - p.pacingFrames)
            compare(row.hint, qsTr("正在修复：%1% 的画面重新对齐").arg(90))
            quick.pacingShare = 0
            compare(row.hint, qsTr("这款游戏的节奏已经均匀，无需修复"))
        }
        function test_hud_lock_switch() {
            quick.applyTarget("120")
            control("pushfps-hudlock").toggled(true)
            verify(veyra.hudLock)
            verify(control("pushfps-hudlock").checked)
            control("pushfps-hudlock").toggled(false)
            verify(!veyra.hudLock)
        }
        function test_hud_lock_reports_what_runs() {
            quick.applyTarget("120")
            veyra.hudLock = true
            quick.liveStats = ({ hudLockState: 2 })
            compare(control("pushfps-hudlock-row").hint, qsTr("实时 HUD 锁定没能在这台电脑上启动，补帧照常运行（原因见日志）"))
            quick.liveStats = ({ hudLockState: 1 })
            compare(control("pushfps-hudlock-row").hint, qsTr("正在运行：静止的 HUD 像素在补出来的帧里用主机原来的像素"))
            veyra.hudLock = false
            quick.liveStats = ({})
        }
        function test_own_tech_is_off_with_xess() {
            quick.applyTarget("120")
            veyra.presentationOwned = true
            verify(!control("pushfps-pacing").enabled)
            verify(!control("pushfps-hudlock").enabled)
            compare(control("pushfps-pacing-row").hint, qsTr("XeSS 自己控制节奏，不能同时使用"))
            veyra.presentationOwned = false
        }
        function test_profiles_remember_pacing_and_hud_lock() {
            quick.applyTarget("120")
            quick.setPacingRepair(true)
            veyra.hudLock = true
            verify(quick.saveGame("Bloodborne"))
            quick.setPacingRepair(false)
            veyra.hudLock = false
            quick.applyGame("Bloodborne")
            verify(veyra.pacingRepair && veyra.hudLock)
        }
        function test_rows_are_shown() {
            for (const n of ["pushfps-console-row", "pushfps-game-row", "pushfps-game-fps-row",
                             "pushfps-target-row", "pushfps-adaptive-row", "pushfps-pacing-row", "pushfps-hudlock-row", "pushfps-enhance-row", "pushfps-smooth-row", "pushfps-low-latency-row", "pushfps-lowest-latency-row"])
                verify(control(n).visible, n)
            verify(quick.height > 0)
        }
        function test_optimise_tile_is_green_only_after_optimising() {
            const tile = control("pushfps-optimize")
            veyra.setPreference("pushBenchmark", "")
            verify(!tile.primary, "grey before Optimise for my PC has run (green would look already on)")
            veyra.setPreference("pushBenchmark", JSON.stringify({ v: 3, applied: false, best: "" }))
            verify(!tile.primary, "grey when nothing was applied or it was undone")
            veyra.setPreference("pushBenchmark", JSON.stringify({ v: 3, applied: true, best: "VFG 4X" }))
            verify(tile.primary, "green while the optimised settings are kept")
            veyra.setPreference("pushBenchmark", "{not json")
            verify(!tile.primary)
        }
        // A 60 FPS target and smooth pacing together (3.1.6): turning it on keeps the cap, the switch
        // reads on with the cap, and turning it off keeps the target.
        function test_cadence_hint_offers_60_to_30() {
            veyra.setPreference("pushGameFps", 30)
            veyra.fgEnabled = true; veyra.contentRate = 1
            quick.liveStats = ({ running: true, captureFps: 60, contentFps: 59.9 })
            verify(quick.cadenceMismatch, "30 FPS set, 60 measured in a 60 Hz capture")
            control("pushfps-cadence-fix").clicked()
            compare(veyra.contentRate, 5, "fixed 60->30")
            verify(!quick.cadenceMismatch)
            quick.liveStats = ({ running: true, captureFps: 60, contentFps: 30 })
            veyra.contentRate = 1
            verify(!quick.cadenceMismatch, "no hint when 30 is measured")
            quick.liveStats = ({})
            veyra.fgEnabled = false
        }
        function test_smooth_frame_pacing_keeps_a_target() {
            veyra.displaySync = 2; veyra.variableRefresh = false
            veyra.outputCustomFps = 60; veyra.outputRateMode = 2
            const sw = findChild(quick, "pushfps-smooth")
            verify(!sw.checked)
            sw.toggled(true)
            compare(veyra.displaySync, 1)
            compare(veyra.outputRateMode, 2, "the 60 FPS target stays")
            verify(sw.checked, "on with a target")
            verify(!quick.unevenTarget, "60 divides 240 Hz")
            veyra.displayRefreshHz = 144
            verify(quick.unevenTarget, "60 on 144 Hz without VRR is uneven")
            veyra.displayRefreshHz = 240
            sw.toggled(false)
            compare(veyra.displaySync, 2)
            compare(veyra.outputRateMode, 2, "off keeps the target")
            veyra.outputRateMode = 0
        }
        function test_smooth_frame_pacing() {
            veyra.displaySync = 2; veyra.variableRefresh = false; veyra.outputRateMode = 0
            const sw = findChild(quick, "pushfps-smooth")
            verify(sw, "smooth frame pacing switch exists")
            verify(!sw.checked, "off while display sync is Automatic")
            sw.toggled(true)
            compare(veyra.displaySync, 1, "on: V-Sync")
            compare(veyra.variableRefresh, true, "on: variable refresh requested")
            compare(veyra.outputRateMode, 1, "on: output capped at the display")
            compare(veyra.preferences.pushSmoothPacing, true)
            verify(sw.checked)
            sw.toggled(false)
            compare(veyra.displaySync, 2, "off: back to Automatic")
            compare(veyra.variableRefresh, false)
            compare(veyra.outputRateMode, 0, "off: no output cap")
            compare(veyra.preferences.pushSmoothPacing, false)
        }
        // G-SYNC set-up row: shown while the driver has not confirmed variable refresh; the button checks
        // the driver profile and repairs it when that is possible.
        function test_gsync_row_checks_then_repairs() {
            const tools = gsyncMock.createObject(root)
            veyra.nvidiaVrrProfile = tools
            veyra.hasSource = true
            veyra.displaySync = 1; veyra.variableRefresh = true; veyra.outputRateMode = 1
            veyra.vrrState = 3
            const row = findChild(root, "pushfps-gsync-row"), button = findChild(root, "pushfps-gsync-fix")
            verify(row && button, "the row exists")
            tryVerify(() => row.visible, 2000, "shown while variable refresh is requested but not confirmed")
            button.clicked()
            compare(tools.checks, 1, "the button starts with a check")
            tools.canRepair = true; tools.busy = false; tools.changed()
            compare(tools.repairs, 1, "a profile that can be set is repaired")
            tools.canRepair = false; tools.changed()
            tryVerify(() => !row.visible, 2000, "nothing left to set: the row goes")
            compare(veyra.preferences.pushGsyncChecked, true, "and that is remembered")
            // Stream "s10-display": the repair is applied by the driver only after a restart of PushFPS: said until then.
            compare(veyra.preferences.pushGsyncRestartPending, true, "a repair waits for a restart")
            compare(quick.gsyncRun, 1)
            verify(control("pushfps-gsync-restart-row").visible, "the row says: restart PushFPS to finish")
            quick.gsyncRun = 0
            veyra.vrrState = 0; veyra.displaySync = 2; veyra.variableRefresh = false
            veyra.nvidiaVrrProfile = null
            tools.destroy()
        }

        // Stream "s10-display" (review MUST 1): after the restart only the driver's answer is stated, and only where PushFPS
        // asked for variable refresh; never a troubleshooting instruction.
        function test_gsync_after_restart_states_only_what_the_driver_answered() {
            compare(quick.gsyncVerdict(false, 3), 0, "variable refresh not requested: nothing to say")
            compare(quick.gsyncVerdict(false, 4), 0)
            compare(quick.gsyncVerdict(true, 4), 2, "requested and reported active")
            compare(quick.gsyncVerdict(true, 3), 3, "requested, not reported active: said as such")
            quick.gsyncRun = 3
            const row = control("pushfps-gsync-restart-row")
            verify(row.visible)
            verify(row.hint.indexOf("控制面板") < 0, "no control-panel instruction")
            quick.gsyncRun = 2
            verify(row.visible)
            quick.gsyncRun = 0
            verify(!row.visible)
        }
        // Stream "s10-display": the auto-fullscreen row shows this PC's measured display path in the current window mode.
        function test_auto_fullscreen_hint_shows_the_measured_to_screen_figure() {
            veyra.liveStats = ({running: true, toScreenKnown: true, toScreenP50Ms: 9.43, toScreenP95Ms: 14.5})
            control("pushfps-display-path-timer").triggered()
            verify(quick.toScreenText.indexOf("9.4") >= 0, quick.toScreenText)
            veyra.liveStats = ({running: true, toScreenKnown: false})
            control("pushfps-display-path-timer").triggered()
            compare(quick.toScreenText, "", "nothing measured: nothing said")
        }

        // The look of AI picture enhancement: presets, two sliders, reset, every layer.
        function test_look_presets_sliders_and_reset() {
            verify(!quick.lookShown, "enhancement off: no look controls")
            verify(!control("pushfps-look-row").visible)
            verify(!control("pushfps-look-tone-row").visible)
            veyra.nrAnyEnabled = true
            verify(quick.lookShown)
            verify(control("pushfps-look-row").visible)
            compare(quick.lookId, "strong", "a layer that was never tuned is at the model's full strength")
            verify(!control("pushfps-look-reset").enabled, "nothing to reset at the default")
            control("pushfps-look").picked("subtle")
            compare(veyra.nrLayers[0].tone, 0.4)
            compare(veyra.nrLayers[0].structure, 0.5)
            compare(quick.lookId, "subtle")
            compare(control("pushfps-look").current, "subtle")
            control("pushfps-look").picked("balanced")
            compare(veyra.nrLayers[0].tone, 0.7)
            compare(veyra.nrLayers[0].structure, 0.8)
            // A slider moves one value and leaves the other.
            control("pushfps-look-tone").moved(0.55)
            compare(veyra.nrLayers[0].tone, 0.55)
            compare(veyra.nrLayers[0].structure, 0.8)
            compare(quick.lookId, "", "no preset matches: custom")
            compare(control("pushfps-look").current, "")
            compare(control("pushfps-look-tone-row").value, "55%")
            control("pushfps-look-detail").moved(0.25)
            compare(veyra.nrLayers[0].structure, 0.25)
            compare(veyra.nrLayers[0].tone, 0.55)
            // Out-of-range input is clamped, never passed on.
            control("pushfps-look-tone").moved(7)
            compare(veyra.nrLayers[0].tone, 1)
            control("pushfps-look-detail").moved(-3)
            compare(veyra.nrLayers[0].structure, 0)
            verify(control("pushfps-look-reset").enabled)
            control("pushfps-look-reset").clicked()
            compare(veyra.nrLayers[0].tone, 1)
            compare(veyra.nrLayers[0].structure, 1)
            compare(quick.lookId, "strong")
            // Turning the enhancement off hides the controls and keeps the values.
            control("pushfps-look").picked("subtle")
            control("pushfps-enhance").toggled(false)
            verify(!quick.lookShown)
            compare(veyra.nrLayers[0].tone, 0.4)
        }
        function test_look_reaches_every_enhancement_layer() {
            veyra.nrLayers = [{index: 1, tone: 1, structure: 1}, {index: 4, tone: 0.2, structure: 0.9}]
            veyra.nrAnyEnabled = true
            compare(quick.lookTone, 1, "the first layer is the one shown")
            verify(quick.applyLook("balanced"))
            compare(veyra.nrLayers[0].tone, 0.7); compare(veyra.nrLayers[1].tone, 0.7)
            compare(veyra.nrLayers[0].structure, 0.8); compare(veyra.nrLayers[1].structure, 0.8)
            verify(!quick.applyLook("no-such-look"))
            // A profile whose look is the one already shown writes nothing: layers given different
            // values in Professional mode stay different.
            veyra.nrLayers = [{index: 1, tone: 1, structure: 1}, {index: 4, tone: 0.2, structure: 0.9}]
            quick.applySnapshot({fps: 30, target: "off", enhance: true, look: {tone: 1, structure: 1}})
            compare(veyra.nrLayers[1].tone, 0.2); compare(veyra.nrLayers[1].structure, 0.9)
            quick.applySnapshot({fps: 30, target: "off", enhance: true, look: {tone: 0.4, structure: 0.5}})
            compare(veyra.nrLayers[0].tone, 0.4); compare(veyra.nrLayers[1].tone, 0.4)
            compare(veyra.nrLayers[1].structure, 0.5)
            veyra.nrLayers = []
            verify(!quick.lookShown, "no layer: nothing to tune")
            verify(!quick.setLook(0.5, 0.5))
        }
        function test_look_is_kept_per_game_and_console_profile() {
            veyra.nrAnyEnabled = true
            quick.applyLook("subtle")
            verify(quick.saveGame("Quiet game"))
            compare(quick.games[0].look.tone, 0.4)
            compare(quick.games[0].look.structure, 0.5)
            quick.setLook(0.9, 0.3)
            verify(quick.saveGame("Loud game"))
            quick.applyGame("Quiet game")
            compare(quick.lookId, "subtle")
            quick.applyGame("Loud game")
            compare(veyra.nrLayers[0].tone, 0.9); compare(veyra.nrLayers[0].structure, 0.3)
            // Each console remembers its own look too.
            quick.applyLook("balanced")
            quick.switchConsole("xbox")
            quick.applyLook("strong")
            quick.switchConsole("ps5")
            compare(quick.lookId, "balanced", "PS5 look restored")
            quick.switchConsole("xbox")
            compare(quick.lookId, "strong", "Xbox look restored")
            // A profile saved before the look existed leaves the look alone.
            quick.applyLook("subtle")
            quick.applySnapshot({fps: 30, target: "off", enhance: true})
            compare(quick.lookId, "subtle")
            quick.applySnapshot({fps: 30, target: "off", enhance: true, look: {tone: "x", structure: 0.5}})
            compare(quick.lookId, "subtle", "a broken look is ignored")
            quick.deleteGame("Quiet game"); quick.deleteGame("Loud game")
        }
        function test_look_compare_splits_the_picture_and_never_stays_on() {
            veyra.nrAnyEnabled = true
            const sw = control("pushfps-look-compare")
            verify(control("pushfps-look-compare-row").visible)
            verify(sw.enabled && !sw.checked)
            sw.toggled(true)
            compare(veyra.compareMode, 2, "split view: original left, enhanced right")
            verify(sw.checked && quick.lookCompare)
            verify(control("pushfps-look-compare-timer").running, "it ends by itself")
            sw.toggled(false)
            compare(veyra.compareMode, 0)
            verify(!quick.lookCompare && !control("pushfps-look-compare-timer").running)
            // Turned off somewhere else (Professional mode's display panel): this panel follows.
            sw.toggled(true)
            veyra.compareMode = 0
            verify(!quick.lookCompare && !sw.checked)
            // Enhancement turned off while comparing: the split ends with it.
            sw.toggled(true)
            compare(veyra.compareMode, 2)
            control("pushfps-enhance").toggled(false)
            compare(veyra.compareMode, 0)
            // The timer ends it.
            veyra.nrAnyEnabled = true
            sw.toggled(true)
            control("pushfps-look-compare-timer").triggered()
            compare(veyra.compareMode, 0)
            // A split the user set elsewhere is shown as on, and is not this panel's to end by itself.
            veyra.compareMode = 2
            verify(sw.checked && !quick.lookCompare)
            verify(!control("pushfps-look-compare-timer").running)
            veyra.nrAnyEnabled = false
            compare(veyra.compareMode, 2, "left as the user set it")
            veyra.compareMode = 0
            // Nothing playing: nothing to compare.
            veyra.nrAnyEnabled = true
            veyra.hasSource = false
            verify(!sw.enabled)
            quick.setLookCompare(true)
            compare(veyra.compareMode, 0)
        }

        function test_lowest_latency_one_tap_keeps_quality_and_restores() {
            // A G-SYNC/FreeSync screen on an NVIDIA card, display sync Automatic before.
            veyra.nvidiaLowLatencyAvailable = true; veyra.nvidiaLowLatency = 0; veyra.vrrState = 4
            veyra.displaySync = 2; veyra.variableRefresh = false; veyra.outputRateMode = 0
            veyra.fgMultiplier = 3; veyra.nrAnyEnabled = true; veyra.hudLock = true; veyra.pacingRepair = true
            const sw = control("pushfps-lowest-latency")
            verify(!sw.checked)
            sw.toggled(true)
            compare(veyra.nvidiaLowLatency, 1, "NVIDIA low latency: one-frame queue")
            compare(veyra.preferences.pushAutoFullscreen, true, "fullscreen on connect")
            compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true); compare(veyra.outputRateMode, 1)
            verify(quick.lowestLatency && sw.checked)
            // Picture and frame generation untouched.
            compare(veyra.fgMultiplier, 3); verify(veyra.nrAnyEnabled); verify(veyra.hudLock); verify(veyra.pacingRepair)
            sw.toggled(false)
            compare(veyra.nvidiaLowLatency, 0, "restored")
            compare(veyra.preferences.pushAutoFullscreen, false)
            compare(veyra.displaySync, 2); compare(veyra.variableRefresh, false); compare(veyra.outputRateMode, 0)
            verify(!quick.lowestLatency)
            // Another GPU, no variable refresh: the low queue, display sync left alone (no tearing).
            veyra.nvidiaLowLatencyAvailable = false; veyra.vrrState = 0; veyra.fgLowQueue = false
            veyra.displaySync = 2
            sw.toggled(true)
            compare(veyra.fgLowQueue, true)
            compare(veyra.displaySync, 2, "no VRR: sync unchanged")
            verify(quick.lowestLatency)
            sw.toggled(false)
            compare(veyra.fgLowQueue, false)
            veyra.vrrState = 0
        }
    }
}
