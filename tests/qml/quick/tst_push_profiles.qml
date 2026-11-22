// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Picture profiles: the table (PushProfiles.js against the rows of tests/remoteplay/PerformanceModeTests.inc),
// the copy of the pipeline planner's rules, what choosing a profile writes and what "none" puts back,
// changes kept per profile, the stand-ins on hardware without DLSS or without the neural runtime, and the
// chooser in the quick setup (PushProfileRow.qml). UI logic only, against the fixture bridge in
// QuickSmokeTests.cpp, which has no planner (pushPipelinePlan) and no restoration preference of its own:
// the planner's copy runs here, one test hands in a planner, and "pushClean" is allowed by init().
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushProfiles.js" as Profiles
Item {
    id: root
    width: 700; height: 2600
    PushQuickSetup { id: quick; x: 10; y: 10; width: 560 }
    PushModeDriver { id: driver }

    TestCase {
        name: "PushProfiles"
        when: windowShown
        readonly property var screen1440: ({ width: 2560, height: 1440 })
        function init() {
            driver.timed = false
            driver.screenSize = screen1440
            // The restore stream's preference, when this build's fixture does not list it yet.
            if (veyra.pushKeys.indexOf("pushClean") < 0) veyra.pushKeys = veyra.pushKeys.concat(["pushClean"])
            veyra.preferences = ({})
            veyra.fgMultiplier = 1
            veyra.fgEnabled = false
            veyra.fgMultiplierChoices = [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
            veyra.fgBackendName = "vfg"
            // nr3 with a blocker: the capability as the neural-runtime stream's bridge reports it, so profiles may turn
            // the neural layer on (a build without that stream has no neural layer for profiles: see test_build_without_*).
            veyra.effectCapabilities = ({nr2:{available:true},nr3:{available:true,blocker:"none"},nr4:{available:true}})
            // The neural-runtime stream's mock (merged build): the runtime installed and usable, as nr3 says.
            if (typeof veyra.pushNeuralRuntimeState === "function") veyra.neuralRuntime = veyra.neuralRuntimeAfterInstall
            veyra.nrLayers = [{index:1}]
            veyra.nrAnyEnabled = false
            veyra.srEnabled = false
            veyra.videoSrQuality = 2
            veyra.srTargetIndex = 2
            veyra.dualSrEnabled = false
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
            veyra.displayRefreshHz = 180
            veyra.hudLock = false
            veyra.pacingRepair = false
            veyra.hasSource = false
            veyra.liveStats = ({running: false})
            rows().screenSize = screen1440
            rows().expanded = true
            rows().lastResult = null
            rows().stats = ({})
        }
        function cleanup() { init(); veyra.hasSource = true; veyra.displayRefreshHz = 240; veyra.fgBackendName = "dlss" }
        function control(name) { const c = findChild(quick, name); verify(c !== null, name); return c }
        function rows() { return control("pushfps-profile-rows") }
        function env(screen) { return Profiles.environment(veyra, ({}), screen === undefined ? screen1440 : screen) }
        function stageNames(plan) { return plan.stages.map(s => s.name).join(",") }
        function has(drops, stage, reason) { return drops.some(d => d.stage === stage && d.reason === reason) }
        // The looks of engine::kPushNrLooks in include/veyra/engine/PushNrRuntime.h (the neural-runtime stream's table).
        readonly property var nrLooks: ({ conservative: [0.15, 0.35], subtle: [0.4, 0.5], balanced: [0.7, 0.8], strong: [1, 1] })
        // Every enhancement layer shows the look: through the bridge's pushSetNeuralLook when this build has it
        // (the merged fixture writes tone / structure the same way), else through this stream's own writes.
        function verifyLook(name) {
            const v = nrLooks[name]
            for (const l of veyra.nrLayers) {
                verify(Math.abs(l.tone - v[0]) < 0.001, name + " tone " + l.tone)
                verify(Math.abs(l.structure - v[1]) < 0.001, name + " structure " + l.structure)
            }
            if (typeof veyra.pushSetNeuralLook === "function") compare(veyra.neuralLook, name)
        }
        function test_look_values_equal_the_neural_runtime_table() {
            compare(Object.keys(Profiles.kLookValues).sort().join(","), Object.keys(nrLooks).sort().join(","))
            for (const id of Profiles.looks) {
                compare(Profiles.kLookValues[id].tone, nrLooks[id][0], id); compare(Profiles.kLookValues[id].structure, nrLooks[id][1], id)
            }
            if (typeof veyra.pushNeuralLooks === "function")
                for (const look of veyra.pushNeuralLooks()) {
                    compare(Profiles.kLookValues[look.id].tone, look.tone, look.id); compare(Profiles.kLookValues[look.id].structure, look.structure, look.id)
                }
        }
        // Everything a profile may write, as one string.
        function state() {
            return JSON.stringify([veyra.srEnabled, veyra.videoSrQuality, veyra.srTargetIndex, veyra.dualSrEnabled, veyra.nrAnyEnabled,
                Number(veyra.preferences.pushClean || 0), veyra.fgEnabled, veyra.fgMultiplier, veyra.contentRate, veyra.nvidiaLowLatency,
                veyra.fgLowQueue, veyra.displaySync, veyra.variableRefresh, veyra.outputRateMode, veyra.hudLock, veyra.pacingRepair])
        }

        // --- 1. the tables, shared with the engine header ------------------------------------------
        // The same rows, value for value, as user_profile_table_rows_shared_with_the_interface in
        // tests/remoteplay/PerformanceModeTests.inc: id, valid, clean, upscaler, DLSS model, target,
        // supersample, neural, look, neural lines, frame generation, low-latency presentation, experimental,
        // sets the DLSS model (only the reference does: M; the others keep the user's own).
        readonly property var profileTable: [
            ["none", false, 0, "off", 1, "none", false, false, "conservative", "1080", "off", false, false, false],
            ["original", true, 0, "off", 1, "none", false, false, "conservative", "1080", "off", false, false, false],
            ["clean-1440", true, 50, "dlss", 1, "qhd", false, false, "conservative", "1080", "keep", false, false, false],
            ["enhanced-1440", true, 50, "dlss", 1, "qhd", false, true, "conservative", "1080", "keep", false, true, false],
            ["max-fidelity", true, 0, "dlss", 1, "4k", true, false, "conservative", "1080", "120", false, false, true],
            ["ultra-low-latency", true, 0, "off", 1, "none", false, false, "conservative", "1080", "keep", true, false, false]
        ]
        function test_profile_rows_shared_with_the_engine_header() {
            for (const r of profileTable) {
                const d = Profiles.defaults(r[0])
                compare(d.profile, r[0]); compare(d.valid, r[1], r[0]); compare(d.clean, r[2], r[0]); compare(d.upscaler, r[3], r[0])
                compare(d.srModel, r[4], r[0]); compare(d.srTarget, r[5], r[0]); compare(d.supersample, r[6], r[0])
                compare(d.neural, r[7], r[0]); compare(d.look, r[8], r[0]); compare(d.nrSize, r[9], r[0]); compare(d.fg, r[10], r[0])
                compare(d.lowLatency, r[11], r[0]); compare(d.experimental, r[12], r[0]); compare(d.setsSrModel, r[13], r[0])
            }
            compare(Profiles.ids.length, 5)
            compare(Profiles.kCleanStrength, 50)
            verify(!Profiles.isProfile("Original")); verify(!Profiles.isProfile("")); verify(!Profiles.isProfile("clean1440"))
            verify(!Profiles.defaults("anything").valid)
            for (const id of Profiles.ids) {
                const d = Profiles.defaults(id)
                verify(d.fg !== "180", id + ": 180 FPS is never a default")
                if (d.srTarget === "4k") verify(id === "max-fidelity" && d.supersample, "only the reference makes a 4K picture")
                compare(d.neural, id === "enhanced-1440"); compare(d.experimental, d.neural)
                if (id === "original" || id === "ultra-low-latency") verify(d.clean === 0 && d.upscaler === "off" && !d.neural, id)
            }
        }
        // user_profile_multiplier_rows_shared_with_the_interface: source fps, display Hz, target, multiplier.
        readonly property var multiplierRows: [
            [59.94, 180, 120, 2], [60, 180, 120, 2], [30, 180, 120, 4], [29.97, 165, 120, 4], [40, 180, 120, 3], [60, 144, 120, 2],
            [60, 100, 120, 1], [30, 60, 120, 2], [59.94, 180, 180, 3], [30, 180, 180, 4], [60, 120, 180, 2],
            [0, 180, 120, 1], [60, 0, 120, 1], [60, 180, 0, 1], [NaN, 180, 120, 1], [60, Infinity, 120, 1]
        ]
        function test_multiplier_rows_shared_with_the_engine_header() {
            for (const r of multiplierRows) compare(Profiles.multiplier(r[0], r[1], r[2]), r[3], r.join(" "))
            for (const fps of [29.97, 30, 40, 59.94, 60])
                for (let hz = 60; hz <= 360; hz += 5)
                    for (const target of [120, 180]) {
                        const m = Profiles.multiplier(fps, hz, target)
                        verify(m >= 1 && m <= 4)
                        verify(m === 1 || (fps * m <= target * 1.02 && fps * m <= hz * 1.02), fps + " @ " + hz + " -> " + target)
                    }
            compare(Profiles.targetFps("120"), 120); compare(Profiles.targetFps("180"), 180)
            compare(Profiles.targetFps("keep"), 0); compare(Profiles.targetFps("off"), 0)
        }
        // user_profile_upscaler_fallback_rows_shared_with_the_interface: wanted, DLSS, FSR, NIS, chosen.
        readonly property var upscalerRows: [
            ["dlss", true, true, true, "dlss"], ["dlss", false, true, true, "fsr"], ["dlss", false, false, true, "nis"],
            ["dlss", false, false, false, "off"], ["fsr", true, true, true, "fsr"], ["fsr", true, false, true, "nis"],
            ["fsr", true, false, false, "off"], ["nis", true, true, true, "nis"], ["nis", true, true, false, "fsr"],
            ["off", true, true, true, "off"]
        ]
        function test_upscaler_rows_shared_with_the_engine_header() {
            for (const r of upscalerRows) compare(Profiles.upscalerFor(r[0], { dlssSr: r[1], fsr: r[2], nis: r[3] }), r[4], r.join(" "))
            compare(Profiles.upscalerFor("vsr", { dlssSr: true, fsr: true, nis: true }), "off", "not an upscaler a profile has")
        }

        // --- 2. the copy of the planner's rules (rows of tests/remoteplay/PipelinePlanTests.inc) -----
        readonly property var allCaps: ({ dlssSr: true, vsr: true, nis: true, fsr: true, nrRuntime: true, nrMaxHeight: 1440,
                                          vfg: true, dlssFg: true, pushFlow: true, fsrFg: true })
        function base(extra) {
            return Object.assign({ streamWidth: 1920, streamHeight: 1080, streamFps: 59.94, contentFps: 30, hdr: true,
                                   displayWidth: 2560, displayHeight: 1440, displayHz: 180 }, extra)
        }
        function changed(plan, stage, reason) { return plan.changes.find(c => c.stage === stage && c.reason === reason) || null }
        function test_plan_rows_shared_with_the_planner() {
            // Order: clean first, then the upscaler, the neural layer, frame generation, present.
            let p = Profiles.mirror(base({ clean: 40, sr: "dlss", srTarget: "qhd", nr: true, fg: "vfg" }), allCaps)
            compare(stageNames(p), "decode,clean,upscale,neural,framegen,present")
            compare(p.stages[1].inWidth, 1920); compare(p.stages[1].outHeight, 1080); compare(p.stages[1].detail, 40)
            compare(p.stages[2].outWidth, 2560); compare(p.stages[2].outHeight, 1440)
            compare(p.outputWidth, 2560); compare(p.outputHeight, 1440); verify(p.experimental)
            // Nothing was changed; the one entry is the note that the neural layer is not verified for HDR (base is HDR).
            compare(p.changes.length, 1); compare(changed(p, "neural", "neural-hdr-unverified").action, "noted")
            p = Profiles.mirror(base({ clean: 40, sr: "dlss", srTarget: "qhd", nr: true, fg: "vfg", hdr: false }), allCaps)
            compare(p.changes.length, 0, "SDR: no note")
            // The neural layer before the upscaler runs on the cleaned stream.
            p = Profiles.mirror(base({ clean: 20, sr: "dlss", nr: true, nrPlace: "before" }), allCaps)
            compare(stageNames(p), "decode,clean,neural,upscale,present"); compare(p.stages[2].inHeight, 1080)
            // Its size: each choice, the runtime's cap, and never above its input.
            for (const row of [["480", "480"], ["720", "720"], ["900", "900"], ["1080", "1080"], ["1440", "1440"], ["native", "native"]]) {
                p = Profiles.mirror(base({ sr: "dlss", nr: true, nrSize: row[0] }), allCaps)
                compare(p.nrSize, row[1]); compare(p.stages[2].name, "neural"); compare(p.stages[2].inHeight, 1440)
            }
            p = Profiles.mirror(base({ sr: "dlss", nr: true, nrSize: "1440" }), Object.assign({}, allCaps, { nrMaxHeight: 1080 }))
            compare(p.nrSize, "1080"); compare(changed(p, "neural", "nr-size-cap").from, 1440)
            p = Profiles.mirror(base({ nr: true, nrSize: "1440" }), allCaps)
            compare(p.nrSize, "1080"); verify(changed(p, "neural", "nr-above-input") !== null)
            // A missing neural runtime: dropped with the reason, the rest still runs.
            p = Profiles.mirror(base({ nr: true, sr: "dlss" }), Object.assign({}, allCaps, { nrRuntime: false }))
            compare(stageNames(p), "decode,upscale,present"); compare(changed(p, "neural", "runtime-missing").action, "dropped"); verify(!p.experimental)
            // A target above the display is reduced unless supersampling.
            p = Profiles.mirror(base({ sr: "dlss", srTarget: "4k" }), allCaps)
            compare(p.outputHeight, 1440); compare(changed(p, "upscale", "above-display").from, 2160); compare(changed(p, "upscale", "above-display").to, 1440)
            p = Profiles.mirror(base({ sr: "dlss", srTarget: "4k", supersample: true }), allCaps)
            compare(p.outputWidth, 3840); compare(p.outputHeight, 2160); compare(p.changes.length, 0)
            p = Profiles.mirror(base({ sr: "dlss", srTarget: "4k", displayWidth: 3840, displayHeight: 2160 }), allCaps)
            compare(p.outputHeight, 2160); compare(p.changes.length, 0)
            // Scale 1:1 and a second upscaler are dropped.
            p = Profiles.mirror(base({ sr: "nis", displayWidth: 1920, displayHeight: 1080 }), allCaps)
            compare(stageNames(p), "decode,present"); verify(changed(p, "upscale", "scale-one") !== null)
            p = Profiles.mirror(base({ sr: "dlss", sr2: "vsr" }), allCaps)
            compare(p.stages.filter(s => s.name === "upscale").length, 1); verify(changed(p, "upscale", "double-upscaler") !== null)
            // DLSS missing: FSR stands in, then NIS, then nothing; a vendor-neutral choice is never replaced.
            p = Profiles.mirror(base({ sr: "dlss" }), Object.assign({}, allCaps, { dlssSr: false }))
            compare(p.stages[1].detail, Profiles.kUpscalers.indexOf("fsr")); compare(p.outputHeight, 1440)
            compare(changed(p, "upscale", "fallback-vendor-neutral").action, "replaced")
            compare(changed(p, "upscale", "fallback-vendor-neutral").to, Profiles.kUpscalers.indexOf("fsr"))
            p = Profiles.mirror(base({ sr: "dlss" }), Object.assign({}, allCaps, { dlssSr: false, fsr: false }))
            compare(p.stages[1].detail, Profiles.kUpscalers.indexOf("nis"))
            p = Profiles.mirror(base({ sr: "dlss" }), Object.assign({}, allCaps, { dlssSr: false, fsr: false, nis: false }))
            compare(stageNames(p), "decode,present"); compare(changed(p, "upscale", "runtime-missing").action, "dropped"); compare(p.outputHeight, 1080)
            p = Profiles.mirror(base({ sr: "nis" }), { nis: true, fsr: true })
            compare(p.stages[1].detail, Profiles.kUpscalers.indexOf("nis")); compare(p.changes.length, 0)
            // Frame generation: the stand-in's own multipliers, the target, the refresh rate.
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 2 }), Object.assign({}, allCaps, { vfg: false, dlssFg: false }))
            compare(changed(p, "framegen", "fallback-vendor-neutral").to, Profiles.kFgNames.indexOf("fsr")); compare(p.stages[1].detail, 2)
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 3, fgTarget: 180, fgExperimental180: true }), Object.assign({}, allCaps, { vfg: false, dlssFg: false }))
            compare(p.stages[1].detail, 2); verify(changed(p, "framegen", "fg-multiplier-unsupported") !== null)
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 3, fgTarget: 180, fgExperimental180: true }), Object.assign({}, allCaps, { vfg: false, dlssFg: false, fsrFg: false, pushFlow: false }))
            compare(stageNames(p), "decode,present"); verify(changed(p, "framegen", "runtime-missing") !== null)
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 4 }), allCaps)
            compare(p.stages[1].detail, 2); compare(changed(p, "framegen", "fg-above-target").to, 2); verify(Math.abs(p.submittedFps - 119.88) < 0.001)
            compare(p.uniqueMotionFpsMax, 60, "submitted frames are not all new motion: the game makes 30")
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 3, fgTarget: 180 }), allCaps)
            compare(p.stages[1].detail, 2, "180 only as the explicit experiment")
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 3, fgTarget: 180, fgExperimental180: true }), allCaps)
            compare(p.stages[1].detail, 3); verify(p.experimental)
            p = Profiles.mirror(base({ fg: "vfg", fgMultiplier: 2, displayHz: 100 }), allCaps)
            compare(stageNames(p), "decode,present"); verify(changed(p, "framegen", "fg-above-refresh") !== null); verify(changed(p, "framegen", "fg-multiplier-one") !== null)
            // The planner of 02:30 (6bd23e19): NIS's sharpness, one scale-one for a reduction to the stream's size, the change cap.
            p = Profiles.mirror(base({ sr: "dlss", srTarget: "qhd", displayWidth: 1920, displayHeight: 1080 }), allCaps)
            compare(p.changes.length, 1); compare(p.changes[0].reason, "scale-one"); compare(p.srSharpness, -1)
            p = Profiles.mirror(base({ sr: "dlss", srSharpness: 60 }), Object.assign({}, allCaps, { dlssSr: false, fsr: false }))
            compare(p.stages[1].detail, Profiles.kUpscalers.indexOf("nis")); compare(p.srSharpness, 0, "a stand-in NIS does not sharpen")
            p = Profiles.mirror(base({ sr: "nis", srSharpness: 40 }), allCaps)
            compare(p.srSharpness, 40, "a NIS the user chose keeps its sharpness")
            p = Profiles.mirror(base({ sr: "nis", srSharpness: 400 }), allCaps)
            compare(p.srSharpness, 100)
            p = Profiles.mirror(base({ sr: "dlss", sr2: "nis" }), allCaps)
            verify(changed(p, "upscale", "double-upscaler") !== null, "DLSS SR does not sharpen here: not a double sharpening")
            verify(p.changesTruncated === false)
            // A request that cannot be planned: decode and present only.
            p = Profiles.mirror(base({ streamWidth: 0, sr: "dlss", clean: 50 }), allCaps)
            compare(stageNames(p), "decode,present"); compare(p.changes[0].reason, "invalid-request")
        }

        // --- 3. a user who never picks a profile ---------------------------------------------------
        function test_no_profile_is_the_default_and_changes_nothing() {
            const before = state()
            compare(rows().profile, "none"); verify(!rows().active); compare(rows().shown, null)
            compare(Profiles.current(veyra), "none")
            compare(JSON.stringify(veyra.preferences), "{}", "nothing was written")
            compare(state(), before)
            verify(!control("pushfps-profile-clean-row").visible); verify(!control("pushfps-profile-reset-row").visible)
            verify(rows().disclosure.length > 0)
            // "none" picked without a profile before: still nothing but the choice itself.
            verify(rows().pick("none")); compare(state(), before); compare(veyra.preferences.pushProfile, "none")
        }

        // --- 4. each profile applies and "none" restores -------------------------------------------
        function test_each_profile_applies_and_none_restores() {
            // The user's own settings: an upscaler to 4K, 3X generation with a cap, enhancement on.
            veyra.srEnabled = true; veyra.videoSrQuality = 3; veyra.srTargetIndex = 2
            veyra.nrAnyEnabled = true; veyra.fgMultiplier = 3; veyra.outputRateMode = 1
            veyra.setPreference("pushClean", 20)
            const own = state()
            const expected = {
                "original":          { stages: "decode,present", sr: false, nr: false, clean: 0, fg: false, mult: 1 },
                "clean-1440":        { stages: "decode,clean,upscale,present", sr: true, nr: false, clean: 50, fg: true, mult: 3 },
                "enhanced-1440":     { stages: "decode,clean,upscale,neural,present", sr: true, nr: true, clean: 50, fg: true, mult: 3 },
                "max-fidelity":      { stages: "decode,upscale,framegen,present", sr: true, nr: false, clean: 0, fg: true, mult: 2 },
                "ultra-low-latency": { stages: "decode,present", sr: false, nr: false, clean: 0, fg: true, mult: 3 }
            }
            for (const id of Profiles.ids) {
                verify(rows().pick(id), id)
                const e = expected[id], r = rows().lastResult
                compare(veyra.preferences.pushProfile, id); compare(rows().profile, id)
                compare(stageNames(r.plan), e.stages, id)
                compare(veyra.srEnabled, e.sr, id + " upscaler"); compare(veyra.nrAnyEnabled, e.nr, id + " neural layer")
                compare(Number(veyra.preferences.pushClean), e.clean, id + " clean-up")
                compare(veyra.fgEnabled, e.fg, id + " frame generation"); compare(veyra.fgMultiplier, e.mult, id + " multiplier")
                compare(veyra.contentRate, 0, id + ": duplicate removal is never turned on")
                compare(veyra.dualSrEnabled, false)
                if (e.sr) { compare(veyra.videoSrQuality, 0, id + ": DLSS SR"); compare(veyra.srTargetIndex, id === "max-fidelity" ? 2 : 1, id + " target") }
                if (id === "max-fidelity") { compare(veyra.outputRateMode, 2); compare(veyra.outputCustomFps, 120, id + ": held at 120") }
                // The 1440 profiles keep the user's own frame generation (3x here), never set one.
                compare(r.drops.filter(d => d.stage !== "note").length, 0, id + ": nothing dropped on a PC that runs everything")
                verify(rows().disclosure.indexOf(rows().label(id)) >= 0)
            }
            // The 1440 profiles never make a 4K picture; the reference does, on purpose.
            compare(Profiles.preview(veyra, "clean-1440", env()).plan.outputHeight, 1440)
            compare(Profiles.preview(veyra, "enhanced-1440", env()).plan.outputHeight, 1440)
            compare(Profiles.preview(veyra, "max-fidelity", env()).plan.outputHeight, 2160)
            // The settings from before the first profile, kept across every change of profile, come back.
            const kept = JSON.parse(veyra.preferences.pushProfileRestore)
            compare(kept.sr, true); compare(kept.srQuality, 3); compare(kept.srTarget, 2); compare(kept.clean, 20); compare(kept.mult, 3)
            verify(rows().pick("none"))
            compare(state(), own, "exactly what was set before")
            compare(veyra.preferences.pushProfile, "none"); compare(veyra.preferences.pushProfileRestore, "{}")
            compare(rows().shown, null)
        }
        // "None" writes back only what the profile in place wrote, and never a performance mode's frame generation.
        function test_none_never_brings_back_a_modes_or_a_hand_made_frame_generation() {
            // FG off -> a mode turns 2x on -> Clean 1440p (record taken under the mode) -> mode cleared -> None.
            veyra.setPreference("pushMode", "competitive"); veyra.fgMultiplier = 2
            verify(Profiles.select(veyra, "clean-1440", env()) !== null)
            compare(JSON.parse(veyra.preferences.pushProfileRestore).mode, "competitive")
            veyra.setPreference("pushMode", ""); veyra.fgEnabled = false
            Profiles.select(veyra, "none", env())
            verify(!veyra.fgEnabled, "the mode's 2x is not turned back on")
            // Ultra Low Latency keeps frame generation: one set by hand meanwhile stays after None.
            verify(Profiles.select(veyra, "ultra-low-latency", env()) !== null)
            veyra.fgMultiplier = 3
            Profiles.select(veyra, "none", env())
            verify(veyra.fgEnabled); compare(veyra.fgMultiplier, 3)
            compare(veyra.displaySync, 2, "presentation back after Ultra Low Latency")
        }
        // One owner of "before" for frame generation and presentation, shared with the performance modes (the reviewer's
        // random sequences r5 / r6; the mode's own restore is played by hand here, as PushModes.select("") does).
        function test_none_under_a_mode_is_finished_when_the_mode_is_turned_off() {
            // (a) Maximum Fidelity -> a mode (it records Max's 2x) -> None (the mode owns FG: deferred) -> mode off.
            verify(Profiles.select(veyra, "max-fidelity", env()) !== null); verify(veyra.fgEnabled)
            veyra.setPreference("pushMode", "competitive")
            Profiles.select(veyra, "none", env())
            verify(JSON.parse(veyra.preferences.pushProfileRestore).pending, "kept until the mode is off")
            veyra.setPreference("pushMode", "")   // the mode's own restore leaves Max's 2x on
            verify(Profiles.settlePending(veyra) || !veyra.fgEnabled)
            verify(!veyra.fgEnabled, "the user's own (off) is back"); compare(veyra.preferences.pushProfileRestore, "{}")
            // (b) A profile first picked under a mode, then Original after the mode is off: None brings the user's 3x back.
            veyra.fgMultiplier = 3
            veyra.setPreference("pushMode", "competitive"); veyra.fgMultiplier = 2
            verify(Profiles.select(veyra, "clean-1440", env()) !== null)
            veyra.setPreference("pushMode", ""); veyra.fgMultiplier = 3   // the mode's restore
            verify(Profiles.select(veyra, "original", env()) !== null); verify(!veyra.fgEnabled)
            Profiles.select(veyra, "none", env())
            verify(veyra.fgEnabled); compare(veyra.fgMultiplier, 3)
        }
        // Ultra Low Latency and the lowest-latency switch write the same settings: one record of "before" only
        // (the reviewer's r2.js ended with tearing and both controls off).
        function test_ultra_low_latency_profile_and_the_lowest_latency_switch_share_one_record() {
            veyra.setPreference("pushAutoFullscreen", false)
            quick.setLowestLatency(true)
            verify(quick.lowestLatency); verify(veyra.fgLowQueue)
            verify(rows().pick("ultra-low-latency"))
            compare(veyra.preferences.pushAutoFullscreen, false, "the switch put its own settings back first")
            verify(!quick.lowestLatency); verify(veyra.fgLowQueue, "the profile's one-frame queue")
            verify(!control("pushfps-lowest-latency").enabled, "refused while the profile is in place")
            verify(control("pushfps-lowest-latency-row").hint.indexOf(rows().label("ultra-low-latency")) >= 0)
            control("pushfps-lowest-latency").toggled(true); verify(!quick.lowestLatency)
            verify(rows().pick("none"))
            compare(veyra.fgLowQueue, false); compare(veyra.displaySync, 2, "no tearing left behind")
            verify(control("pushfps-lowest-latency").enabled)
            quick.setLowestLatency(true); verify(quick.lowestLatency)
            quick.setLowestLatency(false); compare(veyra.fgLowQueue, false); compare(veyra.displaySync, 2)
        }
        // An unreadable record ("before"): said in the row, and never replaced by the state of the profile in place.
        function test_unreadable_record_is_said_and_never_replaced_by_a_profiles_state() {
            verify(rows().pick("clean-1440"))
            veyra.setPreference("pushProfileRestore", JSON.stringify({ v: 9 }))
            verify(rows().pick("max-fidelity"))
            verify(has(rows().lastResult.drops, "note", "restore-missing"))
            compare(JSON.parse(veyra.preferences.pushProfileRestore).v, 9, "Clean 1440p's settings are not stored as the user's")
            const said = rows().dropText({ stage: "note", reason: "restore-missing" })
            verify(said.length > 0); verify(rows().disclosure.indexOf(said) >= 0)
            verify(rows().pick("none"))
            verify(has(rows().lastResult.drops, "note", "restore-missing"))
            verify(rows().disclosure.indexOf(said) >= 0, "None says it could not put anything back")
            compare(veyra.preferences.pushProfileRestore, "{}")
            verify(rows().pick("clean-1440")); verify(!has(rows().lastResult.drops, "note", "restore-missing"), "a new record from the user's own state")
        }
        // The 8A profile: exactly 2560x1440, the restoration default, no second upscaler or sharpening after the first,
        // and the honest line (compare with Original) instead of a promise.
        function test_clean_1440_is_exactly_1440_without_a_second_stage() {
            veyra.dualSrEnabled = true
            verify(rows().pick("clean-1440"))
            compare(veyra.dualSrEnabled, false, "no NIS or other stage layered after DLSS")
            compare(veyra.srTargetIndex, 1); compare(rows().lastResult.plan.outputWidth, 2560); compare(rows().lastResult.plan.outputHeight, 1440)
            compare(stageNames(rows().lastResult.plan), "decode,clean,upscale,present")
            compare(Number(veyra.preferences.pushClean), Profiles.kCleanStrength); verify(!veyra.fgEnabled, "frame generation kept (off)")
            verify(rows().disclosure.indexOf(rows().label("original")) >= 0, "compare it with Original on your screen")
            verify(rows().pick("original"))
            verify(rows().disclosure.indexOf("「" + rows().label("original") + "」") < 0, "Original is the comparison itself")
            verify(rows().pick("none")); compare(veyra.dualSrEnabled, true, "the user's own second stage comes back")
        }
        function test_enhanced_profile_sets_the_conservative_look_and_size() {
            verify(rows().pick("enhanced-1440"))
            compare(veyra.nrAnyEnabled, true)
            verifyLook("conservative")
            compare(veyra.nrLayers[0].sizePolicy, 0, "1080 lines: NrSizePolicy::Realtime")
            compare(veyra.nrLayers[0].runtime, 3, "NrRuntime::NvidiaOriginal")
            verify(rows().own.experimental)
            verify(rows().change("look", "balanced"))
            verifyLook("balanced")
            verify(rows().change("nrSize", "720")); compare(veyra.nrLayers[0].sizePolicy, 3, "NrSizePolicy::P720")
            verify(rows().pick("none"))
            compare(veyra.nrAnyEnabled, false)
        }

        // --- 5. changes belong to one profile ------------------------------------------------------
        function test_customisation_is_per_profile_and_reset_restores_defaults() {
            verify(rows().pick("clean-1440"))
            verify(rows().change("clean", 80))
            compare(Number(veyra.preferences.pushClean), 80, "applied at once")
            compare(JSON.stringify(JSON.parse(veyra.preferences.pushProfileSettings)), JSON.stringify({ "clean-1440": { clean: 80 } }))
            verify(rows().own.customised)
            verify(rows().pick("enhanced-1440"))
            compare(Number(veyra.preferences.pushClean), 50, "the other profile keeps its own default")
            verify(!rows().own.customised)
            // A change to a profile that is not in use is remembered only.
            compare(Profiles.customise(veyra, "clean-1440", "fg", "120", env()), true)
            verify(!veyra.fgEnabled, "the profile in use is untouched")
            verify(rows().pick("clean-1440"))
            compare(Number(veyra.preferences.pushClean), 80); verify(veyra.fgEnabled, "its own changes come back with it")
            // A value back at the default is no longer kept; a bad value is refused.
            verify(rows().change("clean", 50))
            compare(JSON.stringify(JSON.parse(veyra.preferences.pushProfileSettings)), JSON.stringify({ "clean-1440": { fg: "120" } }))
            verify(!rows().change("clean", "much")); verify(!rows().change("look", "wild")); verify(!rows().change("upscaler", "vsr"))
            compare(Profiles.customise(veyra, "nothing", "clean", 10, env()), null)
            // Reset: this profile's defaults again, at once.
            verify(rows().change("clean", 30))
            verify(rows().resetProfile())
            compare(Number(veyra.preferences.pushClean), 50); verify(!veyra.fgEnabled, "keep: the user's own (off) is back")
            compare(veyra.preferences.pushProfileSettings, "{}")
            verify(!rows().own.customised)
            verify(rows().pick("none"))
            verify(!veyra.fgEnabled); verify(!veyra.srEnabled); compare(Number(veyra.preferences.pushClean), 0)
        }
        function test_profile_leaves_what_it_does_not_own() {
            verify(rows().pick("clean-1440"))
            // Display sync changed by hand in another row stays through a change of the profile's settings.
            veyra.displaySync = 1; veyra.variableRefresh = true
            verify(rows().change("clean", 30))
            compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
            // Ultra Low Latency owns presentation; the profile after it gets the user's own back, and the user's own
            // is what was set by hand above (display sync 1 with variable refresh), not what was there before it.
            verify(rows().pick("ultra-low-latency")); compare(veyra.displaySync, 0); compare(veyra.fgLowQueue, true)
            verify(rows().pick("original")); compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true); compare(veyra.fgLowQueue, false)
            verify(rows().pick("none")); compare(veyra.displaySync, 1, "None keeps the hand-made setting too"); compare(veyra.variableRefresh, true)
            // "Keep" after a profile that set frame generation: the user's own (none here) is back.
            verify(rows().pick("clean-1440")); verify(!veyra.fgEnabled, "the 1440 profiles never turn frame generation on")
            verify(rows().pick("max-fidelity")); verify(veyra.fgEnabled)
            verify(rows().pick("ultra-low-latency")); verify(!veyra.fgEnabled)
        }
        function test_experimental_180_only_by_choice() {
            verify(rows().pick("clean-1440"))
            verify(!veyra.fgEnabled, "kept as the user had it"); verify(rows().change("fg", "120"))
            compare(veyra.fgMultiplier, 2); verify(!rows().own.experimental)
            verify(rows().change("fg", "180"))
            compare(veyra.fgMultiplier, 3, "59.94 x 3 on a 180 Hz screen"); verify(rows().own.experimental)
            compare(veyra.outputRateMode, 1, "the display's own rate")
            veyra.displayRefreshHz = 144
            verify(rows().pick("clean-1440")); compare(veyra.fgMultiplier, 2, "never above the display")
        }

        // --- 6. hardware: what cannot run is replaced or dropped, and the row says why -----------------
        function test_missing_neural_runtime_drops_the_stage_and_says_why() {
            // The neural layer of a profile runs only on the NVIDIA-signed original runtime (capability nr3); the
            // installer keeps that file only on an explicit yes, so this is the usual state.
            veyra.effectCapabilities = ({ nr2: { available: true }, nr3: { available: false, reason: "RUNTIME-REASON", blocker: "runtime-missing" } })
            // Said on the card before the profile is picked, and only on that card.
            verify(control("pushfps-profile-enhanced-1440").note.indexOf("RUNTIME-REASON") >= 0)
            compare(control("pushfps-profile-clean-1440").note, ""); compare(control("pushfps-profile-original").note, "")
            verify(rows().pick("enhanced-1440"))
            verify(!veyra.nrAnyEnabled, "the neural layer is not turned on, not even with another runtime that could run")
            verify(veyra.srEnabled); compare(Number(veyra.preferences.pushClean), 50, "the rest of the profile runs")
            verify(has(rows().lastResult.drops, "neural", "runtime-missing"))
            compare(stageNames(rows().lastResult.plan), "decode,clean,upscale,present")
            verify(rows().disclosure.indexOf("RUNTIME-REASON") >= 0, "the bridge's own reason is shown")
            verify(control("pushfps-profile-look-row").hint.indexOf("RUNTIME-REASON") >= 0)
            // What runs instead is named; nothing is shown as running that does not run.
            verify(rows().stageLine.indexOf("2560×1440") >= 0)
            verify(rows().stageLine.indexOf(rows().lookLabel("conservative")) < 0)
            verify(!rows().shown.target.nr)
            // The runtime arrives (installed from the components page): the same profile, picked again, has its layer.
            veyra.effectCapabilities = ({ nr3: { available: true, blocker: "none" } })
            compare(control("pushfps-profile-enhanced-1440").note, "")
            verify(rows().pick("enhanced-1440")); verify(veyra.nrAnyEnabled)
            compare(veyra.nrLayers[0].runtime, 3, "on the signed original runtime")
            verify(rows().disclosure.indexOf("HDR") >= 0, "HDR with the neural layer is not verified: said while it runs")
        }
        function test_neural_runtime_state_of_the_bridge_is_honoured() {
            // A plain object in place of the bridge: capability nr3 says yes, the runtime state says no.
            const bridge = {
                effectAvailability: function () { return { available: true, reason: "" } },
                pushNeuralRuntimeState: function () { return { usable: false, blocker: "driver-too-old", text: "STATE-TEXT" } }
            }
            let c = Profiles.caps(bridge)
            verify(!c.nrRuntime); compare(c.nrReason, "STATE-TEXT")
            verify(c.dlssSr, "only the neural layer is affected")
            bridge.pushNeuralRuntimeState = function () { return { usable: true, blocker: "runtime-version-untested", text: "untested" } }
            c = Profiles.caps(bridge)
            verify(c.nrRuntime, "usable with an untested version, as the bridge says")
            bridge.effectAvailability = function (id) { return id === "nr3" ? { available: false, reason: "CAP-REASON" } : { available: true } }
            c = Profiles.caps(bridge)
            verify(!c.nrRuntime); compare(c.nrReason, "CAP-REASON")
        }
        // Release 8A: no neural-runtime stream in the build. Enhanced 1440p drops its neural stage, says it is not in
        // this build (no error, no pointer to a runtime to install), and everything else in it runs.
        function test_build_without_the_neural_runtime_stream_drops_the_layer_and_says_so() {
            const bridge = { effectCapabilities: ({ nr3: { available: true } }), effectAvailability: function () { return { available: true, reason: "" } } }
            let c = Profiles.caps(bridge)
            verify(!c.nrRuntime, "the older runtime file alone is not enough"); compare(c.nrInBuild, false); verify(c.dlssSr)
            bridge.effectCapabilities = ({ nr3: { available: true, blocker: "none" } })
            c = Profiles.caps(bridge)
            verify(c.nrRuntime); compare(c.nrInBuild, true)
            // The row, where this build's fixture has no neural-runtime functions (8A); with them (8C) only the above.
            if (typeof veyra.pushNeuralRuntimeState === "function") return
            veyra.effectCapabilities = ({ nr2: { available: true }, nr3: { available: true } })
            verify(control("pushfps-profile-enhanced-1440").note.length > 0, "said on the card before it is picked")
            compare(control("pushfps-profile-clean-1440").note, "")
            verify(rows().pick("enhanced-1440"))
            verify(!veyra.nrAnyEnabled); verify(veyra.srEnabled); compare(Number(veyra.preferences.pushClean), 50)
            verify(has(rows().lastResult.drops, "neural", "runtime-missing"))
            compare(stageNames(rows().lastResult.plan), "decode,clean,upscale,present")
            const notInBuild = control("pushfps-profile-look-row").hint
            verify(notInBuild.length > 0); verify(rows().disclosure.indexOf(notInBuild) >= 0)
            verify(notInBuild.indexOf("(") < 0, "no runtime reason in brackets: there is nothing to install")
            verify(rows().disclosure.indexOf("HDR") < 0, "nothing said about a layer that does not run")
            // Every other profile works fully.
            for (const id of ["original", "clean-1440", "max-fidelity", "ultra-low-latency"]) {
                verify(rows().pick(id), id)
                verify(!rows().lastResult.drops.some(d => d.stage === "neural"), id)
            }
            verify(!control("pushfps-profile-neural").enabled, "the switch cannot turn on a layer this build does not have")
            verify(rows().pick("none")); verify(!veyra.nrAnyEnabled)
        }
        function test_neural_layer_only_on_the_signed_original_runtime() {
            // Layers the user has on another runtime are moved for the profile and come back with "none".
            veyra.nrLayers = [{ index: 1, runtime: 2 }]
            verify(rows().pick("enhanced-1440"))
            compare(veyra.nrLayers[0].runtime, 3); verify(veyra.nrAnyEnabled)
            verify(rows().pick("none"))
            compare(veyra.nrLayers[0].runtime, 2); verify(!veyra.nrAnyEnabled)
        }
        function test_dlss_model_is_the_users_own_except_in_the_reference() {
            // A plain object in place of the bridge: the fixture has no DLSS model choice.
            const bridge = {
                preferences: ({}), nrLayers: [{ index: 1, runtime: 3 }], nrAnyEnabled: false, srEnabled: false, videoSrQuality: 2, srTargetIndex: 2,
                dlssSrModel: 0, fgEnabled: false, fgMultiplier: 1, fgMultiplierChoices: [{ id: "2" }], fgBackendName: "vfg", displayRefreshHz: 180,
                displaySync: 2, variableRefresh: false, outputRateMode: 0, outputCustomFps: 60,
                setPreference: function (key, value) { const copy = Object.assign({}, this.preferences); copy[key] = value; this.preferences = copy; return true },
                setAllNrEnabled: function (on) { this.nrAnyEnabled = on }, addEffect: function () {}, logUi: function () {},
                setNrLayerParameter: function () { return true }
            }
            const e = () => Profiles.environment(bridge, ({}), screen1440)
            verify(Profiles.select(bridge, "clean-1440", e()) !== null)
            compare(bridge.videoSrQuality, 0); compare(bridge.dlssSrModel, 0, "the user's model (K here) is kept")
            compare(Profiles.preview(bridge, "clean-1440", e()).plan.request.srModel, 0)
            verify(Profiles.select(bridge, "max-fidelity", e()) !== null)
            compare(bridge.dlssSrModel, 1, "the reference configuration: model M")
            verify(Profiles.select(bridge, "enhanced-1440", e()) !== null)
            compare(bridge.dlssSrModel, 0, "the user's own again")
            bridge.dlssSrModel = 2
            verify(Profiles.customise(bridge, "enhanced-1440", "clean", 20, e()) !== null)
            compare(bridge.dlssSrModel, 2, "changed by hand meanwhile: left alone")
            Profiles.select(bridge, "max-fidelity", e()); Profiles.select(bridge, "none", e())
            compare(bridge.dlssSrModel, 0, "none: as before the first profile")
            // The row says which it is.
            verify(rows().pick("clean-1440")); const kept = control("pushfps-profile-upscaler-row").hint
            verify(kept.length > 0)
            verify(rows().pick("max-fidelity")); verify(control("pushfps-profile-upscaler-row").hint !== kept)
        }
        function test_no_dlss_uses_the_vendor_neutral_upscaler_and_says_so() {
            // A card without DLSS, FSR 2X generation only.
            veyra.fgBackendName = "fsr3"; veyra.fgMultiplierChoices = [{id:"2"}]
            veyra.effectCapabilities = ({ sr0: { available: false, reason: "DLSS-REASON" } })
            verify(rows().pick("clean-1440"))
            verify(veyra.srEnabled); compare(veyra.videoSrQuality, 5, "FSR"); compare(veyra.srTargetIndex, 1)
            verify(has(rows().lastResult.drops, "note", "sr-fallback-fsr"))
            verify(rows().disclosure.indexOf("FSR") >= 0); verify(rows().disclosure.indexOf("DLSS-REASON") >= 0)
            verify(!veyra.fgEnabled, "frame generation kept as the user had it")
            veyra.effectCapabilities = ({ sr0: { available: false }, sr5: { available: false } })
            verify(rows().pick("clean-1440")); compare(veyra.videoSrQuality, 6, "NIS")
            verify(has(rows().lastResult.drops, "note", "sr-fallback-nis"))
            compare(rows().lastResult.plan.srSharpness, 0, "NIS standing in for DLSS adds no sharpening")
            compare(Number(veyra.preferences.nisSharpness), 0)
            veyra.effectCapabilities = ({ sr0: { available: false }, sr5: { available: false }, sr6: { available: false } })
            verify(rows().pick("clean-1440")); verify(!veyra.srEnabled, "nothing can upscale: the stage is dropped")
            verify(has(rows().lastResult.drops, "note", "sr-unavailable")); verify(control("pushfps-profile-upscaler-row").hint.length > 0)
            compare(Number(veyra.preferences.pushClean), 50, "clean-up needs no vendor's runtime")
            verify(rows().pick("none")); compare(veyra.videoSrQuality, 2); compare(veyra.srTargetIndex, 2)
            compare(Number(veyra.preferences.nisSharpness), 50, "NIS's own default back: the user had set none")
        }
        function test_screen_smaller_than_the_target_drops_the_upscaler() {
            rows().screenSize = ({ width: 1920, height: 1080 })
            verify(rows().pick("clean-1440"))
            verify(!veyra.srEnabled); verify(has(rows().lastResult.drops, "upscale", "scale-one"))
            compare(Number(veyra.preferences.pushClean), 50)
        }
        function test_build_without_the_restoration_preference_says_so() {
            veyra.pushKeys = veyra.pushKeys.filter(k => k !== "pushClean")
            verify(rows().pick("clean-1440"))
            verify(has(rows().lastResult.drops, "note", "clean-missing")); verify(veyra.srEnabled)
            verify(rows().refused.length > 0); verify(control("pushfps-profile-clean-row").hint.length > 0)
        }
        function test_a_performance_mode_keeps_frame_generation_and_presentation() {
            veyra.preferences = ({ pushMode: "balanced" }); veyra.fgMultiplier = 3; veyra.displaySync = 1
            verify(rows().pick("original"))
            compare(veyra.fgMultiplier, 3); compare(veyra.displaySync, 1)
            verify(has(rows().lastResult.drops, "note", "mode-owns-fg"))
            verify(!control("pushfps-profile-fg").enabled)
        }
        function test_the_bridge_planner_is_used_when_the_build_has_one() {
            // A plain object in place of the bridge, with a planner that reports no neural runtime.
            const seen = []
            const bridge = {
                preferences: ({}), nrLayers: [{ index: 1 }], nrAnyEnabled: false, srEnabled: false, videoSrQuality: 2, srTargetIndex: 2,
                fgEnabled: false, fgMultiplier: 1, fgMultiplierChoices: [{ id: "2" }], fgBackendName: "vfg", displayRefreshHz: 180,
                displaySync: 2, variableRefresh: false, outputRateMode: 0, outputCustomFps: 60,
                setPreference: function (key, value) { this.preferences[key] = value; return true },
                setAllNrEnabled: function (on) { this.nrAnyEnabled = on }, addEffect: function () {}, logUi: function () {},
                setNrLayerParameter: function () { return true },
                pushPipelinePlan: function (request) {
                    seen.push(request)
                    return Profiles.mirror(request, { dlssSr: true, fsr: true, nis: true, nrRuntime: false, vfg: true })
                }
            }
            const r = Profiles.select(bridge, "enhanced-1440", Profiles.environment(bridge, ({}), screen1440))
            compare(seen.length, 1); compare(seen[0].sr, "dlss"); compare(seen[0].nr, true); compare(seen[0].nrLook, "conservative")
            compare(seen[0].srTarget, "qhd"); compare(seen[0].clean, 50); compare(seen[0].fgTarget, 120)
            compare(seen[0].caps.nrRuntime, false, "this script's gate goes with the request (no neural-runtime stream here)")
            verify(!bridge.nrAnyEnabled); verify(bridge.srEnabled); verify(has(r.drops, "neural", "runtime-missing"))
            verify(r.plan.mirror === true, "the stand-in planner here is the copy itself")
            // A bridge plan with a neural stage the script's gate refuses: the stage is not run (gate AND bridge).
            bridge.pushPipelinePlan = function (request) { return Profiles.mirror(request, { dlssSr: true, fsr: true, nis: true, nrRuntime: true, vfg: true }) }
            const gated = Profiles.plan(bridge, Profiles.settings(bridge, "enhanced-1440"), Profiles.environment(bridge, ({}), screen1440))
            verify(!gated.stages.some(st => st.name === "neural")); verify(changed(gated, "neural", "runtime-missing") !== null)
            // An answer that is not a plan (another shape, an empty list) is not acted on: the copy of the rules is used.
            for (const answer of [null, ({}), ({ stages: [] }), ({ stages: [{ stage: "decode" }, { stage: "present" }] })]) {
                bridge.pushPipelinePlan = function () { return answer }
                const p = Profiles.plan(bridge, Profiles.settings(bridge, "clean-1440"), Profiles.environment(bridge, ({}), screen1440))
                compare(stageNames(p), "decode,clean,upscale,present")
            }
        }

        // The test kit writes the preference (VEYRA_TEST_RP_PIPELINE "profile="); the driver puts the profile in place.
        function test_profile_set_outside_the_row_is_adopted_while_something_plays() {
            veyra.setPreference("pushProfile", "clean-1440")
            wait(20)
            verify(!veyra.srEnabled, "nothing plays: nothing is applied")
            veyra.preferences = ({})
            veyra.hasSource = true
            veyra.liveStats = ({ running: true, captureFps: 59.94 })
            const own = state()
            veyra.setPreference("pushProfile", "ultra-low-latency")
            tryCompare(veyra, "fgLowQueue", true); compare(veyra.displaySync, 0)
            veyra.setPreference("pushProfile", "clean-1440")
            tryCompare(veyra, "srEnabled", true); compare(veyra.videoSrQuality, 0); compare(Number(veyra.preferences.pushClean), 50)
            verify(!veyra.fgEnabled, "the 1440 profile keeps frame generation as it was (off)")
            compare(veyra.fgLowQueue, false, "the profile before owned the queue: the user's own is back"); compare(veyra.displaySync, 2)
            compare(rows().profile, "clean-1440")
            // A profile picked in the row is put in place by the row, and not a second time here.
            verify(rows().pick("original")); verify(!veyra.srEnabled); compare(Profiles.takeApplied(), "", "the driver saw the row's pick")
            wait(20); verify(!driver.adoptPending); verify(!veyra.srEnabled)
            veyra.setPreference("pushProfile", "none")
            wait(50)
            compare(veyra.preferences.pushProfileRestore, "{}")
            compare(state(), own, "none from outside restores too")
            // An id that is no profile is refused by the preference itself.
            verify(!veyra.setPreference("pushProfile", "clean1440"))
        }

        // --- 7. Ultra Low Latency ----------------------------------------------------------------------
        function test_ultra_low_latency_turns_enhancements_off_and_leaves_generation_hud_lock_and_pacing() {
            veyra.srEnabled = true; veyra.nrAnyEnabled = true; veyra.setPreference("pushClean", 40)
            veyra.fgMultiplier = 3; veyra.hudLock = true; veyra.pacingRepair = true; veyra.contentRate = 1
            verify(rows().pick("ultra-low-latency"))
            verify(!veyra.srEnabled); verify(!veyra.nrAnyEnabled); compare(Number(veyra.preferences.pushClean), 0)
            verify(veyra.fgEnabled); compare(veyra.fgMultiplier, 3, "frame generation as the user had it")
            verify(veyra.hudLock); verify(veyra.pacingRepair); compare(veyra.contentRate, 1)
            // The Ultra Low Latency mode's presentation: a one-frame queue, and without confirmed variable
            // refresh the newest picture at once (tearing instead of waiting).
            compare(veyra.fgLowQueue, true); compare(veyra.displaySync, 0)
            verify(has(rows().lastResult.drops, "note", "tearing"))
            verify(rows().pick("none"))
            verify(veyra.srEnabled); verify(veyra.nrAnyEnabled); compare(Number(veyra.preferences.pushClean), 40)
            compare(veyra.fgLowQueue, false); compare(veyra.displaySync, 2); compare(veyra.fgMultiplier, 3)
            // With NVIDIA low latency and variable refresh the driver reports as possible.
            veyra.nvidiaLowLatencyAvailable = true; veyra.vrrState = 3
            verify(rows().pick("ultra-low-latency"))
            compare(veyra.nvidiaLowLatency, 1); compare(veyra.fgLowQueue, false); compare(veyra.displaySync, 1); compare(veyra.variableRefresh, true)
            verify(rows().pick("none")); compare(veyra.nvidiaLowLatency, 0); compare(veyra.displaySync, 2); compare(veyra.variableRefresh, false)
        }

        // --- 8. the chooser ---------------------------------------------------------------------------
        function test_chooser_has_five_named_profiles_and_none() {
            control("pushfps-profile")
            for (const id of ["none"].concat(Profiles.ids)) {
                const card = control("pushfps-profile-" + id)
                verify(card.modelData.label.length > 0); verify(card.modelData.line.length > 0, "one line on what it does and costs")
            }
            compare(rows().choices.length, 6)
            compare(rows().choices.filter(c => c.experimental).map(c => c.id).join(","), "enhanced-1440", "the neural layer is the experiment")
            control("pushfps-profile-clean-1440").clicked()
            compare(veyra.preferences.pushProfile, "clean-1440")
            verify(control("pushfps-profile-clean-1440").current); verify(!control("pushfps-profile-original").current)
            compare(control("pushfps-profile-current").text, rows().label("clean-1440"))
            // Its own settings appear underneath.
            verify(control("pushfps-profile-clean-row").visible); verify(control("pushfps-profile-look-row").visible)
            compare(control("pushfps-profile-clean").value, 50)
            compare(control("pushfps-profile-upscaler").current, "dlss"); compare(control("pushfps-profile-target").current, "qhd")
            compare(control("pushfps-profile-fg").current, "keep")
            compare(control("pushfps-profile-look").current, "", "no look while the neural layer is off")
            verify(!control("pushfps-profile-reset").enabled)
            control("pushfps-profile-neural").toggled(true)
            verify(veyra.nrAnyEnabled); compare(control("pushfps-profile-look").current, "conservative")
            control("pushfps-profile-look").picked("subtle")
            verifyLook("subtle")
            control("pushfps-profile-upscaler").picked("nis"); compare(veyra.videoSrQuality, 6)
            control("pushfps-profile-target").picked("4k"); compare(veyra.srTargetIndex, 2)
            control("pushfps-profile-fg").picked("off"); verify(!veyra.fgEnabled)
            control("pushfps-profile-clean").moved(65); compare(Number(veyra.preferences.pushClean), 65)
            verify(control("pushfps-profile-reset").enabled)
            control("pushfps-profile-reset").clicked()
            compare(Number(veyra.preferences.pushClean), 50); verify(!veyra.nrAnyEnabled); compare(veyra.videoSrQuality, 0)
            compare(veyra.srTargetIndex, 1); verify(!veyra.fgEnabled)
            verify(!control("pushfps-profile-reset").enabled)
            control("pushfps-profile-none").clicked()
            compare(veyra.preferences.pushProfile, "none"); verify(!veyra.srEnabled)
            verify(!control("pushfps-profile-clean-row").visible)
        }
        function test_short_form_opens_with_the_button() {
            rows().expanded = false
            verify(!control("pushfps-profile").visible)
            control("pushfps-profile-expand").clicked()
            verify(rows().expanded); verify(control("pushfps-profile").visible)
        }
    }
}
