// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// The short calibration behind "Recommended settings" (PushCalibrationPanel.qml): started by the user,
// cancellable, at most about 16 s, on what is playing now (there is no built-in test source to fall back on).
//   1. base   frame generation, enhancement and super resolution off; after the settings are in force and a
//             3 s look, the GPU time of one source frame (pushCalibration("sample").gpuMs, a P95).
//   2. fg     the plan's method at its lowest tier, 2X, output following the display; after the settings are
//             in force and 1 s of warm-up, 6 s between two samples: GPU time with generation, the batch time,
//             and the counters (outputs that expired, pairs the admission skipped). The optimiser's
//             measurement window (pushBenchmarkBegin/End) runs over the same 6 s and is logged by the bridge.
// Before anything changes, the settings it touches are saved in pushCalibrationRestore (the optimiser's
// pattern); they are put back when it finishes, fails, is cancelled or destroyed, and at the next start if
// PushFPS stopped halfway (PushModeDriver.qml). finished(measured) hands the numbers to the plan; failed(why)
// says why there are none: busy | no-source | source-changed | not-settled | not-measured | refused | cancelled.
// Unverified on Windows.
import QtQuick
import "PushModes.js" as Modes

Item {
    id: run
    objectName: "pushfps-calibration-run"
    visible: false
    signal finished(var measured)
    signal failed(string why)

    // Tests turn the timer off and call step() themselves, and may replace the sample hook.
    property bool timed: true
    property var sampleHook: null
    readonly property int tickMs: 250
    property string phase: ""          // "" | base | fg
    property int elapsedMs: 0          // in this phase
    property int settledAtMs: -1
    property string method: ""
    property var startSample: null
    property real baseGpuMs: -1
    property var sessionId: null
    readonly property bool running: run.phase !== ""
    readonly property int settleLimitMs: 3000
    readonly property int baseLookMs: 3000
    readonly property int warmMs: 1000
    readonly property int fgLookMs: 6000
    // 0..1 for the progress line.
    readonly property real progress: run.phase === "base" ? Math.min(1, run.elapsedMs / (run.settleLimitMs + run.baseLookMs)) * 0.4
                                   : run.phase === "fg" ? 0.4 + Math.min(1, run.elapsedMs / (run.settleLimitMs + run.warmMs + run.fgLookMs)) * 0.6 : 0

    function sample() {
        if (run.sampleHook) return run.sampleHook() || ({})
        return typeof veyra.pushCalibration === "function" ? (veyra.pushCalibration("sample", ({})) || ({})) : ({})
    }
    function begin(fgMethod) {
        if (run.running) return false
        // Another calibration (another panel), the optimiser, the quality test or the latency test: never two
        // at once, so no run saves settings another one already changed as the ones to restore.
        if (Modes.measurementBusy(veyra)) { run.failed("busy"); return false }
        if (veyra.hasSource !== true) { run.failed("no-source"); return false }
        const first = run.sample()
        if (first.running !== true) { run.failed("no-source"); return false }
        if (!veyra.setPreference("pushCalibrationRestore", JSON.stringify(Modes.calibrationSnapshot(veyra)))) { run.failed("refused"); return false }
        run.method = String(fgMethod || "")
        run.sessionId = first.sessionId
        run.baseGpuMs = -1
        run.startSample = null
        veyra.logUi("push-calibration", "run start method=" + run.method)
        // Phase 1: presentation alone.
        if (veyra.fgEnabled === true) veyra.fgEnabled = false
        if (veyra.srEnabled === true) veyra.srEnabled = false
        if (veyra.nrAnyEnabled === true) Modes.setEnhancement(veyra, false)
        run.enter("base")
        return true
    }
    function enter(name) { run.phase = name; run.elapsedMs = 0; run.settledAtMs = -1 }
    function end(measured, why) {
        const was = run.phase
        run.phase = ""
        if (typeof veyra.pushBenchmarkEnd === "function" && was === "fg" && run.startSample !== null) veyra.pushBenchmarkEnd()
        Modes.calibrationRestore(veyra)
        veyra.logUi("push-calibration", "run end " + (measured ? "measured" : "failed=" + why))
        if (measured) run.finished(measured)
        else run.failed(why)
    }
    function cancel() { if (run.running) run.end(null, "cancelled") }
    function step() {
        if (!run.running) return
        run.elapsedMs += run.tickMs
        const s = run.sample()
        if (s.running !== true) { run.end(null, "no-source"); return }
        if (run.sessionId !== null && s.sessionId !== undefined && String(s.sessionId) !== String(run.sessionId)) { run.end(null, "source-changed"); return }
        if (run.settledAtMs < 0) {
            if (s.settled === true) run.settledAtMs = run.elapsedMs
            else if (run.elapsedMs >= run.settleLimitMs) { run.end(null, "not-settled"); return }
            return
        }
        const since = run.elapsedMs - run.settledAtMs
        if (run.phase === "base") {
            if (since < run.baseLookMs) return
            const gpu = Number(s.gpuMs)
            // 0 is "not timed", never "fast" (the enhancement time leaves out the colour and blit passes).
            if (!(gpu > 0)) { run.end(null, "not-measured"); return }
            run.baseGpuMs = gpu
            // Phase 2: the cheapest generation of the method the plan would use.
            if (run.method.length === 0) { run.end(null, "not-measured"); return }
            if (String(veyra.fgBackendName) !== run.method) veyra.fgBackendName = run.method
            if (String(veyra.fgBackendName) !== run.method) { run.end(null, "refused"); return }
            if (run.method === "pushflow" && Number(veyra.vfgQuality) !== 0) veyra.vfgQuality = 0
            veyra.fgMultiplier = 2
            if (veyra.fgEnabled !== true || Number(veyra.fgMultiplier) !== 2) { run.end(null, "refused"); return }
            if (veyra.presentationOwned !== true && Number(veyra.outputRateMode || 0) !== 1) veyra.outputRateMode = 1
            run.enter("fg")
            return
        }
        if (run.startSample === null) {
            if (since < run.warmMs) return
            run.startSample = s
            if (typeof veyra.pushBenchmarkBegin === "function") veyra.pushBenchmarkBegin()
            return
        }
        if (since < run.warmMs + run.fgLookMs) return
        if (String(s.fgBackend || run.method) !== run.method || Number(s.fgMultiplier || 2) !== 2) { run.end(null, "refused"); return }
        const counters = c => ({ presented: Number(c.presented || 0), expired: Number(c.expired || 0),
                                 pairsCandidate: Number(c.pairsCandidate || 0), pairsSkipped: Number(c.pairsSkipped || 0) })
        const measured = { baseGpuMs: run.baseGpuMs, totalGpuMs: Number(s.gpuMs), fgBatchGpuMs: Number(s.fgBatchMs), multiplier: 2,
                           method: run.method, start: counters(run.startSample), end: counters(s) }
        if (!(measured.totalGpuMs > 0) || !(measured.fgBatchGpuMs > 0)) { run.end(null, "not-measured"); return }
        run.end(measured, "")
    }
    Timer {
        objectName: "pushfps-calibration-timer"
        interval: run.tickMs; repeat: true
        running: run.timed && run.running
        onTriggered: run.step()
    }
    Component.onDestruction: if (run.running) { run.phase = ""; Modes.calibrationRestore(veyra) }
}
