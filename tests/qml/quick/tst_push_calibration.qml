// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// First-launch recommendation: PushCalibrationPanel.qml (shown, applied only on Apply, Undo, Keep my
// settings), PushCalibrationRun.qml (the short calibration: what it changes, that it restores everything on
// finish, cancel and after a crash) and the welcome card around them. UI logic only, against the fixture
// bridge in QuickSmokeTests.cpp, which has no pushCalibration and no PS5 source: the plan, the samples and
// the PS5 codec are supplied here. The plan's own rules are tested in tests/remoteplay/CalibrationPlanTests.inc.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushModes.js" as Modes
Item {
    id: root
    width: 700; height: 1800
    property int codec: 0
    readonly property var codecIo: ({ codec: () => root.codec, setCodec: n => { root.codec = n } })
    property var asked: []
    property var answer: null
    function hook(args) { root.asked = root.asked.concat([args]); return root.answer }
    PushCalibrationPanel { id: panel; x: 10; y: 10; width: 560; planHook: root.hook; codecIo: root.codecIo }
    PushWelcomeCard { id: welcome; x: 10; y: 700; width: 560 }

    TestCase {
        name: "PushCalibration"
        when: windowShown

        readonly property var ullPlan: ({ valid: true, mode: "ultra-low-latency", frameGeneration: false, fgMethod: "pushflow", fgMethodKnown: true,
            multiplier: 1, tier: 0, outputFps: 60, minAddedDelayMs: 0, enhancement: false, codec: "hevc", hdr: false,
            modeReason: "lowest-latency", fgReason: "not-measured", codecReason: "hevc-decode", hdrReason: "hdr-display-off",
            enhancementReason: "enhancement-costs-gpu-time", unknown: ["gpu-cost"], measured: false, smootherOffered: true,
            smootherMultiplier: 2, smootherOutputFps: 120, smootherDelayMs: 8.33, vendor: "nvidia", gpuClass: "dedicated", vramGB: 12,
            refreshHz: 240, vrrState: 4 })
        function fgPlan() {
            const p = Object.assign({}, ullPlan)
            p.mode = "competitive"; p.frameGeneration = true; p.multiplier = 2; p.outputFps = 120; p.minAddedDelayMs = 8.33
            p.fgReason = "generation-fits"; p.modeReason = "generation-fits"; p.measured = true; p.unknown = []
            return p
        }

        function init() {
            veyra.preferences = ({})
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            veyra.effectCapabilities = ({nr2:{available:true},nr3:{available:true},nr4:{available:true}})
            veyra.fgBackendName = "dlss"
            veyra.vfgQuality = 1
            veyra.fgMultiplier = 3
            veyra.contentRate = 1
            veyra.nrAnyEnabled = true
            veyra.srEnabled = true
            veyra.nvidiaLowLatencyAvailable = false
            veyra.nvidiaLowLatency = 0
            veyra.fgLowQueue = false
            veyra.vrrState = 4
            veyra.displaySync = 1
            veyra.variableRefresh = true
            veyra.outputRateMode = 2
            veyra.outputCustomFps = 90
            veyra.presentationOwned = false
            veyra.displayRefreshHz = 240
            veyra.hasSource = false
            veyra.liveStats = ({running: false})
            root.codec = 0
            root.asked = []
            root.answer = null
            panel.plan = null
            panel.loaded = false
            panel.measured = null
            panel.showDetails = false
            panel.runFailure = ""
            const run = findChild(panel, "pushfps-calibration-run")
            run.cancel()
            run.timed = false
            run.sampleHook = null
            welcome.loaded = true
            welcome.hardware = ({})
            welcome.pairingPossible = false
        }
        function cleanup() { init() }
        // Every setting the recommendation, its mode or the calibration can touch.
        function state() {
            return JSON.stringify({ fg: veyra.fgEnabled, mult: veyra.fgMultiplier, backend: veyra.fgBackendName, tier: veyra.vfgQuality,
                nr: veyra.nrAnyEnabled, sr: veyra.srEnabled, codec: root.codec, content: veyra.contentRate, queue: veyra.fgLowQueue,
                nvidia: veyra.nvidiaLowLatency, sync: veyra.displaySync, vrr: veyra.variableRefresh, rate: veyra.outputRateMode,
                cap: veyra.outputCustomFps, mode: String(veyra.preferences.pushMode || "") })
        }
        function button(item, name) { const b = findChild(item, name); verify(b !== null, name); return b }

        function test_recommendation_is_shown_and_applied_only_on_apply() {
            const before = state()
            root.answer = ullPlan
            panel.load()
            compare(root.asked.length, 1)
            compare(root.asked[0].game, 0, "nothing measured and no game rate chosen: unknown")
            compare(root.asked[0].method, "dlss")
            verify(panel.usable)
            compare(state(), before, "showing the recommendation changes nothing")
            verify(findChild(panel, "pushfps-calibration-hardware").text.indexOf("NVIDIA") === 0)
            verify(findChild(panel, "pushfps-calibration-hardware").text.indexOf("240 Hz") > 0)
            verify(findChild(panel, "pushfps-calibration-line-0").text.length > 0)
            verify(findChild(panel, "pushfps-calibration-estimate").text.length > 0, "says it is an estimate")
            compare(panel.pending.map(c => c.key).join(","), "nr,sr,codec,mode")
            verify(!findChild(panel, "pushfps-calibration-changed").visible)
            // Details add the reasons, the smoother choice and the unknowns; still nothing changes.
            button(panel, "pushfps-calibration-details").clicked()
            verify(panel.showDetails)
            verify(findChild(panel, "pushfps-calibration-details-text").text.indexOf("120") >= 0)
            compare(state(), before)

            button(panel, "pushfps-calibration-apply").clicked()
            compare(veyra.preferences.pushMode, "ultra-low-latency")
            verify(!veyra.fgEnabled, "Ultra Low Latency: no generation")
            verify(!veyra.nrAnyEnabled); verify(!veyra.srEnabled)
            compare(root.codec, 1, "H.265 for the next PS5 connection")
            verify(panel.record !== null)
            compare(panel.record.changes.length, 4)
            verify(findChild(panel, "pushfps-calibration-changed").visible)
            verify(findChild(panel, "pushfps-calibration-changed").text.split("\n").length === 5, "a heading and one line per change")
            compare(panel.pending.length, 0, "nothing left to apply")
            verify(!button(panel, "pushfps-calibration-apply").enabled)
        }
        function test_undo_restores_exactly() {
            const before = state()
            root.answer = ullPlan
            panel.load()
            verify(panel.apply() !== null)
            verify(state() !== before)
            button(panel, "pushfps-calibration-undo").clicked()
            compare(state(), before, "every setting back as it was")
            compare(panel.record, null, "Undo clears the record")

            // With frame generation: method, tier and Competitive 2X of a 60 fps game; Undo again exact.
            veyra.setPreference("pushGameFps", 60)
            const before2 = state()
            root.answer = fgPlan()
            panel.load()
            compare(root.asked[root.asked.length - 1].game, 60)
            compare(panel.pending.map(c => c.key).join(","), "backend,tier,nr,sr,codec,mode")
            verify(panel.apply() !== null)
            compare(veyra.fgBackendName, "pushflow"); compare(veyra.vfgQuality, 0)
            verify(veyra.fgEnabled); compare(veyra.fgMultiplier, 2)
            compare(veyra.preferences.pushMode, "competitive")
            panel.undo()
            compare(state(), before2)
        }
        function test_manual_change_after_apply_is_respected() {
            root.answer = ullPlan
            panel.load()
            verify(panel.apply() !== null)
            // The user turns enhancement back on and picks another codec by hand.
            veyra.setAllNrEnabled(true)
            root.codec = 2
            // Nothing applies again by itself: a new look at the plan, a source starting, time passing.
            panel.load()
            veyra.hasSource = true
            wait(50)
            verify(veyra.nrAnyEnabled); compare(root.codec, 2)
            // Undo leaves the hand-made changes alone and puts back the rest.
            const back = panel.undo()
            verify(back.indexOf("nr") < 0 && back.indexOf("codec") < 0)
            verify(back.indexOf("sr") >= 0 && back.indexOf("mode") >= 0)
            verify(veyra.nrAnyEnabled); compare(root.codec, 2); verify(veyra.srEnabled)
        }
        function test_nothing_changes_when_the_capability_report_is_empty() {
            const before = state()
            for (const empty of [null, ({}), ({valid: false, mode: "ultra-low-latency"})]) {
                root.answer = empty
                panel.load()
                verify(!panel.usable)
                compare(panel.pending.length, 0)
                compare(panel.apply(), null)
                verify(!findChild(panel, "pushfps-calibration-apply").visible)
                verify(findChild(panel, "pushfps-calibration-empty").visible)
                compare(state(), before)
            }
            // The fixture bridge has no pushCalibration at all: the same.
            panel.planHook = null
            panel.load()
            verify(!panel.usable)
            compare(state(), before)
            panel.planHook = root.hook
            compare(veyra.preferences.pushCalibrationApplied, undefined)
        }
        function test_keep_my_settings_and_dismissal_are_remembered() {
            root.answer = ullPlan
            const card = findChild(welcome, "pushfps-welcome-recommended")
            verify(card !== null)
            card.planHook = root.hook
            card.codecIo = root.codecIo
            const before = state()
            card.load()
            verify(card.visible)
            button(card, "pushfps-calibration-welcome-keep").clicked()
            compare(veyra.preferences.pushCalibrationKept, true)
            verify(!card.visible, "kept: not offered again on the card")
            compare(state(), before, "Keep my settings changes nothing")
            // The mode row's entry still offers it on request.
            verify(panel.kept)
            panel.load()
            verify(panel.usable)
            // Dismissed: the card never shows again.
            verify(welcome.visible)
            button(welcome, "pushfps-welcome-dismiss").clicked()
            verify(!welcome.visible)
            card.planHook = null
            card.codecIo = null
        }
        function test_measure_is_offered_only_when_a_measurement_is_what_is_missing() {
            root.answer = ullPlan
            panel.load()
            verify(panel.measurable)
            verify(findChild(panel, "pushfps-calibration-measure").visible)
            verify(!findChild(panel, "pushfps-calibration-measure").enabled, "nothing plays")
            const other = Object.assign({}, ullPlan); other.fgReason = "game-thirty"
            root.answer = other
            panel.load()
            verify(!panel.measurable)
            verify(!findChild(panel, "pushfps-calibration-measure").visible)
            compare(panel.measure(), false)
        }

        // --- the calibration ------------------------------------------------------------------------
        // A source that plays and whose settings are in force; GPU time grows with generation on.
        function sampler() {
            let presented = 1000
            return () => {
                presented += 30
                return { running: true, settled: true, sessionId: 7, fgBackend: veyra.fgBackendName, fgMultiplier: veyra.fgMultiplier,
                         gpuMs: veyra.fgEnabled ? 5 : 3, fgBatchMs: veyra.fgEnabled ? 2 : -1, presented: presented, expired: 0,
                         pairsCandidate: presented / 3, pairsSkipped: 0 }
            }
        }
        function test_calibration_measures_restores_and_feeds_the_plan() {
            const run = findChild(panel, "pushfps-calibration-run")
            root.answer = ullPlan
            panel.load()
            veyra.hasSource = true
            run.sampleHook = sampler()
            const before = state()
            verify(panel.measure())
            verify(run.running)
            verify(Modes.calibrationRunning(veyra), "restore point saved first")
            compare(run.phase, "base")
            verify(!veyra.fgEnabled && !veyra.nrAnyEnabled && !veyra.srEnabled, "presentation alone")
            let steps = 0
            while (run.phase === "base" && steps++ < 100) run.step()
            compare(run.phase, "fg")
            compare(run.baseGpuMs, 3)
            compare(veyra.fgBackendName, "pushflow"); compare(veyra.vfgQuality, 0); compare(veyra.fgMultiplier, 2)
            compare(veyra.outputRateMode, 1, "output follows the display while measuring")
            const asked = root.asked.length
            while (run.running && steps++ < 200) run.step()
            verify(!run.running)
            verify(steps * run.tickMs <= 20000, "under 20 s: " + steps * run.tickMs)
            compare(state(), before, "everything restored")
            verify(!Modes.calibrationRunning(veyra))
            compare(root.asked.length, asked + 1, "the plan was asked again with the numbers")
            const m = root.asked[root.asked.length - 1].measured
            compare(m.baseGpuMs, 3); compare(m.totalGpuMs, 5); compare(m.fgBatchGpuMs, 2); compare(m.multiplier, 2); compare(m.method, "pushflow")
            verify(m.end.presented > m.start.presented)
            compare(panel.runFailure, "")
        }
        function test_calibration_cancel_failure_and_crash_restore() {
            const run = findChild(panel, "pushfps-calibration-run")
            root.answer = ullPlan
            panel.load()
            veyra.hasSource = true
            run.sampleHook = sampler()
            const before = state()
            verify(panel.measure())
            for (let i = 0; i < 30; ++i) run.step()
            compare(run.phase, "fg")
            button(panel, "pushfps-calibration-cancel").clicked()
            verify(!run.running)
            compare(state(), before, "cancel restores")
            compare(panel.runFailure, "cancelled")
            verify(findChild(panel, "pushfps-calibration-status").text.length > 0)

            // The source goes away halfway: stops and restores.
            verify(panel.measure())
            run.sampleHook = () => ({ running: false })
            run.step()
            verify(!run.running); compare(panel.runFailure, "no-source"); compare(state(), before)

            // Nothing measured (GPU timing unavailable): no numbers, everything restored.
            run.sampleHook = () => ({ running: true, settled: true, sessionId: 7, gpuMs: -1 })
            verify(panel.measure())
            for (let i = 0; i < 40 && run.running; ++i) run.step()
            verify(!run.running); compare(panel.runFailure, "not-measured"); compare(state(), before)

            // Nothing plays: it does not start and changes nothing.
            veyra.hasSource = false
            verify(!panel.measure()); compare(panel.runFailure, "no-source"); compare(state(), before)

            // PushFPS stopped halfway: the next start puts the settings back.
            veyra.setPreference("pushCalibrationRestore", JSON.stringify(Modes.calibrationSnapshot(veyra)))
            veyra.fgBackendName = "pushflow"; veyra.vfgQuality = 0; veyra.fgMultiplier = 2; veyra.setAllNrEnabled(false); veyra.srEnabled = false
            veyra.outputRateMode = 1
            const driver = Qt.createQmlObject("import Veyra; PushModeDriver { timed: false }", root)
            verify(driver !== null)
            compare(state(), before, "restored at start-up")
            verify(!Modes.calibrationRunning(veyra))
            driver.destroy()
        }
        function test_mode_driver_keeps_away_while_calibrating() {
            veyra.setPreference("pushMode", "balanced")
            veyra.setPreference("pushCalibrationRestore", JSON.stringify(Modes.calibrationSnapshot(veyra)))
            const driver = Qt.createQmlObject("import Veyra; PushModeDriver { timed: false }", root)
            // Created while a restore point exists: it restores first, then is no longer busy.
            verify(!driver.busy())
            veyra.setPreference("pushCalibrationRestore", JSON.stringify(Modes.calibrationSnapshot(veyra)))
            verify(driver.busy())
            const before = state()
            driver.look()
            compare(state(), before, "nothing changed under the calibration")
            veyra.setPreference("pushCalibrationRestore", "{}")
            driver.destroy()
        }
        function test_welcome_card_only_for_new_installs() {
            // The rule: a PushFPS preference from an earlier run means PushFPS ran before; keys written by
            // itself on any first start, the card's own and other programs' (Veyra) keys do not count.
            verify(!Modes.existingUser(({})))
            verify(!Modes.existingUser(({ exportStopsPlayback: false, pushSmoothPacing: true, pushSmoothPacing2: true, pushWelcomeChecked: true })))
            verify(!Modes.existingUser(({ pushCalibrationKept: true })))
            verify(Modes.existingUser(({ pushGameFps: 60 })))
            verify(Modes.existingUser(({ pushMode: "", pushSmoothPacing: true })))
            // A new install: the card shows, and the check is made once.
            veyra.preferences = ({ pushSmoothPacing: true })
            verify(!Modes.settleWelcome(veyra))
            compare(veyra.preferences.pushWelcomeChecked, true)
            verify(veyra.preferences.pushWelcomeDone !== true)
            verify(welcome.visible)
            // An existing install: marked done without being shown; a later run does not look again.
            veyra.preferences = ({ pushGameFps: 30, pushConsole: "ps5" })
            verify(Modes.settleWelcome(veyra))
            compare(veyra.preferences.pushWelcomeDone, true)
            verify(!welcome.visible)
            veyra.setPreference("pushWelcomeDone", false)
            verify(!Modes.settleWelcome(veyra), "checked once only")
            compare(veyra.preferences.pushWelcomeDone, false)
            // A card created on an existing install never appears.
            veyra.preferences = ({ pushMode: "balanced" })
            const card = Qt.createQmlObject("import Veyra; PushWelcomeCard { width: 400 }", root)
            verify(!card.visible)
            compare(veyra.preferences.pushWelcomeDone, true)
            card.destroy()
            // "Recommended settings" stays reachable there from the mode row (test_mode_row_opens_the_recommendation_on_request).
        }
        function test_mode_switch_hint_for_the_next_connection() {
            compare(Modes.syncSwitches(veyra), null, "the fixture bridge has no switch call: nothing claimed")
            verify(!Modes.switchesPending(null))
            verify(!Modes.switchesPending(({ applied: true, set: [], unset: [], kept: ["VEYRA_EXP_RP_PAD_POLL_MS"] })), "the user's own value: nothing changes")
            verify(Modes.switchesPending(({ applied: true, set: ["VEYRA_EXP_RP_PAD_POLL_MS"], unset: [], kept: [] })))
            verify(Modes.switchesPending(({ applied: true, set: [], unset: ["VEYRA_EXP_RP_HEALTH"], kept: [] })))
            const quick = Qt.createQmlObject("import Veyra; PushModeRow { width: 560 }", root)
            const row = findChild(quick, "pushfps-mode-row")
            const plain = row.hint
            quick.switchState = ({ applied: true, set: ["VEYRA_EXP_RP_ADAPTIVE_PHASE"], unset: [], kept: [] })
            verify(row.hint.length > plain.length && row.hint.indexOf(quick.switchLine) > 0)
            verify(quick.pick("balanced"))
            compare(quick.lastResult.switches, null)
            quick.destroy()
        }
        function test_never_two_measurements_at_once() {
            const run = findChild(panel, "pushfps-calibration-run")
            root.answer = ullPlan
            panel.load()
            veyra.hasSource = true
            run.sampleHook = sampler()
            const before = state()
            // A second panel (the first-launch card next to the mode row) while one calibration runs.
            const other = Qt.createQmlObject("import Veyra; PushCalibrationPanel { host: 'second' }", root)
            const otherRun = findChild(other, "pushfps-calibration-second-run")
            verify(otherRun !== null)
            otherRun.timed = false
            otherRun.sampleHook = sampler()
            other.planHook = root.hook
            other.codecIo = root.codecIo
            other.load()
            verify(panel.measure())
            for (let i = 0; i < 20; ++i) run.step()
            const saved = veyra.preferences.pushCalibrationRestore
            compare(other.measure(), false)
            compare(other.runFailure, "busy")
            compare(veyra.preferences.pushCalibrationRestore, saved, "the first run's restore point is untouched")
            compare(other.apply(), null, "no Apply while a calibration runs")
            run.cancel()
            compare(state(), before, "the user's settings come back exactly")
            compare(veyra.preferences.pushCalibrationRestore, "", "empty again once nothing runs")
            other.destroy()
            // The optimiser, the quality test, the latency test: refused too, and nothing changes.
            veyra.setPreference("pushOptimizeRestore", JSON.stringify({ fgEnabled: true }))
            compare(panel.measure(), false); compare(panel.runFailure, "busy"); compare(panel.apply(), null); compare(state(), before)
            veyra.setPreference("pushOptimizeRestore", "{}")
            veyra.qualityState = ({ running: true, done: false, failed: false })
            compare(panel.measure(), false); compare(panel.apply(), null); compare(state(), before)
            veyra.qualityState = ({ running: false, done: false, failed: false })
            verify(Modes.measurementBusy(({ preferences: ({}), pushLatencyTestStatus: () => ({ running: true }) })))
            verify(!Modes.measurementBusy(({ preferences: ({}), pushLatencyTestStatus: () => ({ running: false }) })))
            verify(panel.apply() !== null, "allowed again once nothing runs")
        }
        function test_untimed_base_is_not_measured() {
            const run = findChild(panel, "pushfps-calibration-run")
            root.answer = ullPlan
            panel.load()
            veyra.hasSource = true
            const before = state()
            run.sampleHook = () => ({ running: true, settled: true, sessionId: 7, gpuMs: 0, fgBatchMs: -1, presented: 10 })
            verify(panel.measure())
            for (let i = 0; i < 40 && run.running; ++i) run.step()
            compare(panel.runFailure, "not-measured", "0 ms is not fast")
            compare(state(), before)
        }
        function test_restore_is_exact_and_undo_keeps_a_multiplier_changed_by_hand() {
            // Generation off with a multiplier of 2 kept: both come back (a plain object stands for the bridge).
            const fake = { fgEnabled: false, fgMultiplier: 2, fgBackendName: "dlss", vfgQuality: 1, nrAnyEnabled: false, srEnabled: false,
                           outputRateMode: 0, presentationOwned: false, nrLayers: [{ index: 1, enabled: true }, { index: 2, enabled: false }],
                           preferences: ({}), calls: [],
                           setEffectEnabled: function (i, on) { this.calls.push(i + ":" + on); this.nrLayers = this.nrLayers.map(l => l.index === i ? { index: i, enabled: on } : l) },
                           setAllNrEnabled: function (on) { this.calls.push("all:" + on) },
                           setPreference: function (k, v) { const copy = Object.assign({}, this.preferences); copy[k] = v; this.preferences = copy; return true } }
            fake.setPreference("pushCalibrationRestore", JSON.stringify(Modes.calibrationSnapshot(fake)))
            fake.fgMultiplier = 3; fake.fgEnabled = true; fake.nrLayers = [{ index: 1, enabled: false }, { index: 2, enabled: false }]
            verify(Modes.calibrationRestore(fake))
            compare(fake.fgMultiplier, 2); compare(fake.fgEnabled, false)
            compare(JSON.stringify(fake.nrLayers), JSON.stringify([{ index: 1, enabled: true }, { index: 2, enabled: false }]), "each layer as it was")
            compare(fake.calls.join(","), "1:true", "never every layer on")
            compare(fake.preferences.pushCalibrationRestore, "")

            // Undo after the user changed the multiplier by hand: that multiplier stays, the rest comes back.
            veyra.setPreference("pushGameFps", 60)
            root.answer = fgPlan()
            panel.load()
            const before = JSON.parse(state())
            verify(panel.apply() !== null)
            compare(veyra.fgMultiplier, 2)
            veyra.fgMultiplier = 4
            panel.undo()
            compare(veyra.fgMultiplier, 4, "the hand-made multiplier stays")
            compare(veyra.fgBackendName, before.backend); compare(veyra.nrAnyEnabled, before.nr); compare(veyra.srEnabled, before.sr)
            compare(veyra.fgLowQueue, before.queue); compare(veyra.outputRateMode, before.rate); compare(String(veyra.preferences.pushMode || ""), before.mode)
        }
        function test_switches_only_for_modes_chosen_with_this_version() {
            // A mode chosen before this version (no flag): the driver changes nothing at start.
            veyra.setPreference("pushMode", "competitive")
            verify(veyra.preferences.pushModeSwitches !== true)
            verify(Modes.select(veyra, "balanced", 30) !== null)
            compare(veyra.preferences.pushModeSwitches, true, "chosen now: its switches are wanted")
            Modes.select(veyra, "", 30)
            compare(veyra.preferences.pushModeSwitches, false)
        }
        function test_hardware_line_names_the_vendor_once() {
            const other = Object.assign({}, ullPlan); other.vendor = "other"
            root.answer = other
            panel.load()
            const text = findChild(panel, "pushfps-calibration-hardware").text
            verify(text.indexOf("NVIDIA") < 0)
            verify(text.split(" · ")[0].length > 0 && text.indexOf("其他") < 0, text)
            root.answer = ullPlan
            panel.load()
            verify(findChild(panel, "pushfps-calibration-hardware").text.indexOf("NVIDIA · ") === 0)
        }
        function test_mode_row_opens_the_recommendation_on_request() {
            const quick = Qt.createQmlObject("import Veyra; PushModeRow { width: 560 }", root)
            const open = findChild(quick, "pushfps-mode-recommended")
            const inner = findChild(quick, "pushfps-mode-recommended-panel")
            verify(open !== null && inner !== null)
            verify(!inner.visible, "closed until asked")
            inner.planHook = root.hook
            inner.codecIo = root.codecIo
            root.answer = ullPlan
            const before = state()
            open.clicked()
            verify(inner.visible && inner.usable)
            compare(state(), before, "opening changes nothing")
            open.clicked()
            verify(!inner.visible)
            quick.destroy()
        }
    }
}
