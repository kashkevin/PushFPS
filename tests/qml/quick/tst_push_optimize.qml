// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// "Optimise for my PC" (PushOptimizeDialog) against the fixture bridge: the plan,
// the pass rules, the confirmation run, applying the winner and undo. The engine's
// measurement window is replaced by a model of a PC (veyra.benchmarkModel).
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 900; height: 900
    Item { id: host; anchors.fill: parent; property string dialog: "optimize"; function close() { dialog = "" } }
    PushOptimizeDialog {
        id: opt
        dialogHost: host
        settleTimeout: 1; settleHold: 0.05; warmSeconds: 0.05; measureSeconds: 0.1; tickMs: 15
    }

    // A PC: VFG reaches 120 FPS with 18 ms, DLSS 120 with 24 ms, FSR only 60; AI
    // enhancement costs too much at 120; nothing generates under "broken".
    property string broken: ""
    property real vfgLatency: 18
    property int confirmFailures: 0
    function model() {
        const b = veyra.fgEnabled ? veyra.fgBackendName : ""
        const m = veyra.fgEnabled ? veyra.fgMultiplier : 1
        const base = { valid: true, presents: 600, generatedPresents: 0, low01Fps: 0, jitterMs: 0.4, latencyP50Ms: 0, latencyP95Ms: 0 }
        if (!b) return Object.assign(base, { fps: 60, low1Fps: 58, latencyMeanMs: 12 })
        const want = 30 * m
        let fps = want, latency = 20
        if (b === "vfg") latency = root.vfgLatency
        if (b === "dlss") latency = 24
        if (b === "fsr3") fps = Math.min(want, 60)
        if (veyra.nrAnyEnabled) fps = want * 0.8
        const generated = b === root.broken ? 0 : Math.round(600 * (m - 1) / m)
        return Object.assign(base, { fps: fps, low1Fps: fps * 0.92, latencyMeanMs: latency, generatedPresents: generated })
    }

    TestCase {
        name: "PushOptimize"
        when: windowShown
        function init() {
            veyra.preferences = ({ pushGameFps: 30 })
            veyra.fgEnabled = false; veyra.fgMultiplier = 1; veyra.fgBackendName = "dlss"
            veyra.outputRateMode = 0; veyra.outputCustomFps = 0; veyra.nrAnyEnabled = false; veyra.srEnabled = false
            veyra.displayRefreshHz = 144
            veyra.fgBackendChoices = [{ id: "dlss", label: "DLSS" }, { id: "fsr3", label: "FSR 3" }, { id: "vfg", label: "VFG" },
                                      { id: "anr", label: "ANR", danger: true }, { id: "xess", label: "XeSS" },
                                      { id: "amdfrc", label: "AMD FRC", disabled: true, note: "needs an AMD GPU" }]
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            root.vfgLatency = 18
            veyra.liveStats = ({ running: true, settled: true, contentFps: 30, displayRefreshHz: 144, sourceHeight: 2160,
                                 captureReceived: 1000, captureDropped: 0, backendWarning: "" })
            veyra.benchmarkModel = root.model
            root.broken = ""
            veyra.qualityModel = null
            host.dialog = "optimize"
            opt.phase = "idle"
        }
        function cleanup() { if (opt.phase === "running") opt.cancel(); init() }

        // Ground-truth quality (PushQualityProbe): measured scores replace the fixed quality order.
        function test_measured_quality_picks_the_winner() {
            veyra.preferences = ({ pushGameFps: 30, pushOptimizeGoal: "picture" })
            veyra.qualityModel = function(backend) {
                return { score: backend === "dlss" ? 91 : backend === "vfg" ? 42 : 30, psnr: backend === "dlss" ? 36.5 : 29.0, blendPsnr: 27.0,
                         gainOverBlend: 2, realPsnr: 44, hudPsnr: 33, hudSamples: 10, badShare: 0.02, onTime: 0.99 }
            }
            const startsBefore = veyra.qualityStarts
            opt.start()
            tryCompare(opt, "phase", "done", 15000)
            veyra.qualityModel = null
            verify(veyra.qualityStarts > startsBefore, "the quality test ran")
            compare(opt.best.config.backend, "dlss", "the best measured picture wins, not the fixed order")
            const dlss = opt.steps.find(s => s.config.backend === "dlss" && s.result && s.result.quality)
            verify(dlss && dlss.result.quality.psnr === 36.5, "the result keeps the measured PSNR")
            const record = JSON.parse(veyra.preferences.pushBenchmark)
            compare(record.v, 3)
            verify(record.steps.some(x => x.quality === 91), "the record keeps the scores")
        }
        function test_untestable_source_falls_back_to_the_order() {
            veyra.qualityModel = null
            opt.start()
            tryCompare(opt, "phase", "done", 15000)
            verify(opt.qualityUnsupported, "an untestable source is remembered")
            compare(opt.best.config.backend, "vfg", "the fixed order decides")
        }
        // Stream "ownfg" (review 2 MUST 1): PushWarp (an "experimental" choice) is only ever the user's own click: offered and
        // available, it is neither in the optimiser's plan nor in its "unavailable" list.
        function test_pushwarp_is_never_planned() {
            veyra.fgBackendChoices = veyra.fgBackendChoices.concat([{ id: "pushwarp", label: "PushWarp", experimental: true }])
            verify(opt.backends().indexOf("pushwarp") < 0, "not tested, so never applied")
            verify(opt.backends().indexOf("vfg") >= 0, "the other methods stay")
            verify(opt.unavailable().every(u => String(u.label).indexOf("PushWarp") < 0), "not listed as unavailable either")
        }
        function test_needs_a_picture() {
            veyra.liveStats = ({ running: false })
            opt.start()
            compare(opt.phase, "failed")
            verify(opt.failure.length > 0)
            verify(!veyra.fgEnabled, "nothing changed")
        }
        function test_picks_the_best_measured_combination() {
            opt.start()
            tryCompare(opt, "phase", "done", 15000)
            verify(opt.best !== null)
            compare(opt.best.config.backend, "vfg", "highest quality method that held 120 FPS")
            compare(opt.best.config.target, "120")
            compare(opt.best.config.enhance, false, "AI enhancement could not hold 120 here")
            compare(veyra.fgBackendName, "vfg")
            compare(veyra.fgMultiplier, 4)
            compare(veyra.outputRateMode, 2)
            compare(veyra.outputCustomFps, 120)
            const kinds = opt.steps.map(s => s.config.kind)
            compare(kinds[0], "baseline")
            verify(kinds.indexOf("enhance") > 0 && kinds[kinds.length - 1] === "confirm", kinds.join(","))
            // Every available method is measured (audit item), the unavailable one is listed with its reason.
            verify(opt.steps.some(s => s.config.backend === "anr"), "ANR measured")
            verify(opt.steps.some(s => s.config.backend === "xess"), "XeSS measured")
            verify(!opt.steps.some(s => s.config.backend === "amdfrc"), "AMD FRC not available here")
            verify(opt.skipped.some(x => x.label === "AMD FRC" && x.reason === "needs an AMD GPU"), "skip reason shown")
            // 144 Hz: no whole multiplier of 30 reaches 144, so 120 is the top target and that is said.
            verify(!opt.steps.some(s => s.config.target === "monitor"), "no unreachable 144 FPS target")
            verify(opt.skipped.some(x => x.reason.indexOf("144") >= 0), "the 144 Hz limit is explained")
            const fsr = opt.steps.find(s => s.config.backend === "fsr3")
            verify(fsr && !fsr.result.ok && fsr.result.reason.length > 0, "FSR at 60 FPS fails the 120 target")
            compare(veyra.preferences.pushOptimizeRestore, "{}", "a finished run leaves nothing to restore after a crash")
            const record = JSON.parse(veyra.preferences.pushBenchmark)
            compare(record.steps.length, opt.steps.length)
            verify(record.best.indexOf("VFG") >= 0)
            compare(veyra.preferences.pushGameFps, 30)
        }
        function test_frames_must_really_be_generated() {
            root.broken = "vfg"
            opt.start()
            tryCompare(opt, "phase", "done", 15000)
            const vfg = opt.steps.find(s => s.config.backend === "vfg" && s.config.kind === "fg-high")
            verify(!vfg.result.ok)
            compare(vfg.result.reason, qsTr("没有真正生成帧"))
            compare(opt.best.config.backend, "dlss", "next best that really generated frames")
        }
        function test_undo_restores_the_settings() {
            veyra.fgEnabled = true; veyra.fgBackendName = "dlss"; veyra.fgMultiplier = 2
            veyra.outputRateMode = 2; veyra.outputCustomFps = 60
            opt.start()
            tryCompare(opt, "phase", "done", 15000)
            compare(JSON.parse(veyra.preferences.pushBenchmark).applied, true, "kept result: the tile turns green")
            opt.undo()
            compare(JSON.parse(veyra.preferences.pushBenchmark).applied, false, "undone: the tile turns grey again")
            compare(veyra.fgBackendName, "dlss")
            compare(veyra.fgMultiplier, 2)
            compare(veyra.outputCustomFps, 60)
            compare(host.dialog, "")
        }
        function test_stops_when_the_picture_stops() {
            opt.start()
            wait(100)
            veyra.liveStats = ({ running: false })
            tryCompare(opt, "phase", "failed", 5000)
            verify(!veyra.fgEnabled, "settings restored")
        }
        function test_240hz_screen_is_tested_at_240() {
            veyra.displayRefreshHz = 240
            veyra.liveStats = Object.assign({}, veyra.liveStats, { displayRefreshHz: 240 })
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"},{id:"6"},{id:"8"}]
            opt.start()
            tryCompare(opt, "phase", "done", 20000)
            verify(opt.steps.some(s => s.config.target === "monitor" && s.config.multiplier === 8), "8X at 240 Hz measured")
            compare(opt.best.config.target, "monitor", "240 FPS held on the model PC")
            compare(veyra.fgMultiplier, 8)
        }
        function test_lowest_latency_goal() {
            root.vfgLatency = 30
            veyra.preferences = ({ pushGameFps: 30, pushOptimizeGoal: "latency" })
            opt.start()
            tryCompare(opt, "phase", "done", 20000)
            verify(opt.best.config.backend !== "vfg", "the 30 ms method loses on latency")
            verify(opt.best.result.latency <= 20.5, "lowest measured latency wins (" + opt.best.result.latency + ")")
            compare(JSON.parse(veyra.preferences.pushBenchmark).goal, "latency")
        }
        function test_balanced_goal_keeps_quality_at_equal_rate() {
            root.vfgLatency = 30
            veyra.preferences = ({ pushGameFps: 30, pushOptimizeGoal: "balanced" })
            opt.start()
            tryCompare(opt, "phase", "done", 20000)
            compare(opt.best.config.backend, "vfg", "same 120 FPS: the better picture method, latency only breaks ties")
        }
        // Crash memory: the step being tested is on disk; a method PushFPS stopped in is skipped next time.
        function test_the_running_method_is_on_disk_and_a_crashed_one_is_skipped() {
            veyra.version = "9.9.9"
            opt.start()
            tryVerify(() => opt.current >= 1 && opt.phase === "running", 5000)
            const saved = JSON.parse(veyra.preferences.pushOptimizeRestore)
            compare(saved.testing, opt.steps[opt.current].config.backend, "the method under test is recorded")
            // A step that adds something on top blames what it added, not the method under it.
            compare(opt.blame({ backend: "vfg", kind: "enhance", enhance: true, sr: 0 }), "+enhance")
            compare(opt.blame({ backend: "vfg", kind: "sr", enhance: true, sr: 6 }), "+sr6")
            compare(opt.blame({ backend: "vfg", kind: "confirm", enhance: true, sr: 0 }), "+enhance")
            compare(opt.blame({ backend: "vfg", kind: "confirm", enhance: false, sr: 0 }), "vfg")
            compare(opt.blame({ backend: "", kind: "baseline" }), "")
            tryVerify(() => opt.phase !== "running", 15000)
            compare(veyra.preferences.pushOptimizeRestore, "{}", "nothing to restore after a finished run")
            // As after a crash while VFG was being tested (the list a restart would write).
            veyra.preferences = Object.assign({}, veyra.preferences, { pushOptimizeSkip: JSON.stringify([{ backend: "vfg", version: "9.9.9" }, { backend: "dlss", version: "1.0.0" }]) })
            verify(opt.backends().indexOf("vfg") < 0, "the method that crashed in this version is not tested")
            verify(opt.backends().indexOf("dlss") >= 0, "an entry from another version no longer counts")
            verify(opt.unavailable().some(u => u.label.indexOf("VFG") >= 0), "and it is listed with the reason")
            opt.start()
            tryVerify(() => opt.phase !== "running", 15000)
            verify(!opt.steps.some(s => s.config.backend === "vfg"), "no VFG step in the next run")
            // AI enhancement crashed last time: no enhance step, and it is listed.
            veyra.preferences = Object.assign({}, veyra.preferences, { pushOptimizeSkip: JSON.stringify([{ backend: "+enhance", version: "9.9.9" }]) })
            opt.start()
            tryVerify(() => opt.phase !== "running", 15000)
            verify(!opt.steps.some(s => s.config.kind === "enhance"), "no AI enhancement step after it crashed")
            verify(opt.steps.some(s => s.config.backend === "vfg"), "the methods themselves are tested again")
        }
        function test_notes_a_30_fps_game_measured_as_60() {
            veyra.liveStats = Object.assign({}, veyra.liveStats, { contentFps: 60, captureFps: 60 })
            veyra.contentRate = 1
            opt.start()
            tryVerify(() => opt.phase !== "running", 15000)
            verify(opt.skipped.some(s => String(s.reason).indexOf("60→30") >= 0), "the 60 -> 30 hint is in the notes")
        }
        function test_detects_a_60_fps_game() {
            veyra.liveStats = Object.assign({}, veyra.liveStats, { contentFps: 60 })
            veyra.displayRefreshHz = 144
            opt.start()
            tryVerify(() => opt.phase !== "running", 15000)
            compare(opt.gameFps, 60)
            verify(!opt.steps.some(s => s.config.target === "60" && s.config.backend), "no 60 FPS target for a 60 FPS game")
        }
    }
}
