// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Mouse & Keyboard page (PushKbmPage.qml), key editor (PushKbmBindings.qml), first-launch wizard (PushKbmWizard.qml)
// and their helpers (PushKbm.js). The page talks to `bridge`; here that is a stub with the functions of
// src/ui/bridge/PushKbm.cpp (`stub`), or an object without any of them (`bare`: an engine that has no such core).
// Preferences go through the fixture bridge of QuickSmokeTests.cpp, whose setPreference refuses unknown push* keys
// as the real validator does. Interface logic only: nothing here captures a mouse or talks to a console.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushKbm.js" as Kbm
Item {
    id: root
    width: 1300; height: 900
    Item { id: fixture; anchors.fill: parent }

    // What src/ui/bridge/PushKbm.cpp answers, as plain data the tests change.
    QtObject {
        id: stub
        property var preferences: ({})
        property var state: ({})
        property var custom: ({})
        property var walk: []
        property var aim: []
        property var drive: []
        property var menu: []
        property var presets: []
        property var files: []
        property var calls: []
        property int resets: 0
        property int allows: 0
        property bool held: false
        property var calSteps: []
        property bool calRunning: false
        property int calStarts: 0
        property int calStops: 0
        property var recorded: []
        property var fitApplied: null
        property var loaded: []
        property var saved: []
        property var readout: ({ active: false })
        property real motionCounts: 1000
        property var calEnding: []
        property int keeps: 0
        // As the core's presets: dead zone and full-turn speed come from the game calibration (game) unless switched off.
        readonly property var presetTuning: ({ sensX: 1, sensY: 1, curve: "linear", exponent: 1, kneeIn: 0.25, kneeOut: 0.15,
                                               deadzone: 0.12, fullSpeed: 4000, saturation: "drop", carryMs: 0, maxTilt: 1, game: true })
        function reset() {
            preferences = ({}); custom = ({}); calls = []; resets = 0; allows = 0; held = false
            calSteps = []; calRunning = false; calStarts = 0; calStops = 0; recorded = []; fitApplied = null; loaded = []; saved = []
            keeps = 0; motionCounts = 1000; calEnding = []
            readout = ({ active: false })
            files = [{ name: "gta-6", active: false }]
            state = ({ available: true, core: true, live: true, liveBindings: true, calibration: true, fit: true, forced: false,
                       inputForced: false, eventSend: false,
                       enabled: false, streaming: false, active: false, canRelease: true, capture: "off",
                       releaseKeys: "Ctrl+Alt+Shift+Z", inputProfile: "legacy", contextOverride: "auto", inUse: "",
                       context: "none", fallback: "none", source: "none" })
            walk = [{ key: "W", action: "lup" }, { key: "S", action: "ldown" }, { key: "Space", action: "square" }, { key: "Mouse1", action: "r2" }]
            aim = []
            drive = [{ key: "W", action: "r2" }, { key: "S", action: "l2" }]
            menu = []; presets = []
        }
        function note(text) { const c = calls.slice(); c.push(text); calls = c }
        function patch(values) { state = Object.assign({}, state, values) }
        function setPreference(key, value) { const ok = veyra.setPreference(key, value); preferences = veyra.preferences; return ok }
        function logUi(category, text) {}
        function pushKbmState() { return Object.assign({}, state, { capture: held && state.active === true ? "captured" : state.capture }) }
        function pushKbmSetEnabled(on) { note("enabled=" + on); patch({ enabled: on }); return setPreference("kbmEnabled", on) }
        function pushKbmAllowCapture() { ++allows; patch({ capture: "released" }); return true }
        function pushKbmHold(on) { held = on; return on }
        function pushKbmSetInputProfile(id) { note("profile=" + id); patch({ inputProfile: id }); return true }
        function pushKbmSetContextOverride(id) { note("override=" + id); patch({ contextOverride: id }); return true }
        function pushKbmInputSettings(profile) {
            const id = profile === "" ? state.inputProfile : profile
            const tuning = key => Object.assign({}, presetTuning, id === "custom" ? (custom[key] || ({})) : ({}))
            return { available: true, profile: id, ads: id === "custom" && custom.ads !== undefined ? custom.ads : 1,
                     smoothingMs: id === "custom" && custom.smoothingMs !== undefined ? custom.smoothingMs : id === "legacy" ? 8 : 0,
                     transmit: id === "custom" && custom.transmit !== undefined ? custom.transmit : "proven", contextOverride: state.contextOverride,
                     contexts: { walk: tuning("walk"), aim: tuning("aim"), sprint: tuning("sprint"), drive: tuning("drive") } }
        }
        function pushKbmSetCustom(key, value) {
            note("custom " + key + "=" + value)
            const next = Object.assign({}, custom)
            const dot = key.indexOf(".")
            if (dot < 0) next[key] = value
            else { const c = key.slice(0, dot); next[c] = Object.assign({}, next[c] || ({})); next[c][key.slice(dot + 1)] = value }
            custom = next
            return true
        }
        function pushKbmCustomFrom(profile) { note("customFrom=" + profile); patch({ inputProfile: "custom" }); return true }
        function listOf(context) { return context === "drive" ? drive : context === "aim" ? aim : context === "menu" ? menu : walk }
        function store(context, list) { if (context === "drive") drive = list; else if (context === "aim") aim = list; else if (context === "menu") menu = list; else walk = list }
        function pushKbmBindings() {
            return { available: true, live: true, actions: ["cross", "circle", "square", "triangle", "l2", "r2", "lup", "ldown"],
                     keys: ["Mouse1", "Mouse2", "Mouse3", "W", "S", "G", "Space", "Esc"], walk: walk, aim: aim, drive: drive, menu: menu, file: "", edited: false,
                     layoutName: "GTA VI layout (press tables; untuned, unverified in the game)" }
        }
        function pushKbmSetBinding(context, key, action, replace) {
            const list = listOf(context)
            const existing = Kbm.actionOf(list, key)
            if (action !== "none" && existing !== "none" && existing !== action && !replace) return { ok: false, reason: "conflict", conflict: existing }
            const next = list.filter(b => b.key !== key)
            if (action !== "none") next.push({ key: key, action: action })
            store(context, next)
            note("bind " + context + " " + key + "=" + action)
            return { ok: true, replaced: existing !== "none" && existing !== action ? existing : "" }
        }
        function pushKbmResetBindings() { ++resets; walk = [{ key: "W", action: "lup" }]; return true }
        // As the core's kbmGtaPcProfiles (only what the tests read) and the Custom input it stores.
        function pushKbmApplyPreset(id) {
            const p = presets.slice(); p.push(id); presets = p
            if (id !== "gta-pc") return { ok: false, reason: "unknown-preset" }
            walk = [{ key: "W", action: "lup" }, { key: "Space", action: "cross" }, { key: "F", action: "triangle" }, { key: "Esc", action: "menu" }, { key: "Mouse1", action: "r2" }, { key: "Mouse3", action: "l2" }]
            menu = [{ key: "W", action: "dup" }, { key: "Space", action: "cross" }, { key: "Esc", action: "menu" }]
            patch({ inputProfile: "custom" })
            return { ok: true, input: true, unbound: ["reload"] }
        }
        function pushKbmProfileFiles() { return files }
        function pushKbmLoadProfile(name) { const l = loaded.slice(); l.push(name); loaded = l; return true }
        function pushKbmSaveProfile(name) { const s = saved.slice(); s.push(name); saved = s; return true }
        function pushKbmReadout() { return readout }
        function pushKbmCalibration(request) {
            if (request === "start") { ++calStarts; calRunning = held; return { available: true, ok: held, running: calRunning, step: "intro", captured: held, messages: ["calibration started"] } }
            if (request === "stop") { ++calStops; calRunning = false }
            if (request === "keep") { ++keeps; return { available: true, ok: true, running: false, file: "default" } }
            const step = calSteps.length > 0 ? calSteps[0] : "intro"
            const messages = calRunning ? [] : calEnding
            if (!calRunning) calEnding = []
            return { available: true, ok: true, running: calRunning, step: step, captured: held && state.active === true, messages: messages }
        }
        function pushKbmFit(request, args) {
            if (request === "record") { note("record " + args.kind); return { available: true, ok: held } }
            if (request === "stop") { const r = recorded.slice(); r.push(1); recorded = r; return { available: true, ok: true, counts: motionCounts * recorded.length, seconds: 0.5 } }
            if (request === "fit") return { available: true, ok: args.turnCounts.length === 3 && args.flicks.length === 3 && args.slow.length === 3, reason: "need-turns",
                                            tuning: Object.assign({}, presetTuning, { sensX: 1.5, fullSpeed: 6000, game: false }), explanation: "fitted",
                                            turnSeconds: args.gameFullTurnSeconds }
            if (request === "apply") { fitApplied = args; patch({ inputProfile: "custom" }); return { available: true, ok: true } }
            return { available: true, ok: false }
        }
    }
    // An engine without the page's functions: only what every bridge has.
    QtObject {
        id: bare
        property var preferences: ({})
        function setPreference(key, value) { return false }
    }

    PushKbmPage { id: page; pageId: "kbm"; parent: fixture; width: fixture.width; height: fixture.height; visible: true; enabled: true; bridge: stub }
    // As Main.qml creates it: the bridge is `veyra` itself (the fixture bridge has none of the page's functions).
    PushKbmPage { id: plain; pageId: "kbm-plain"; parent: fixture; width: fixture.width; height: fixture.height; visible: false; enabled: true }

    TestCase {
        name: "PushKbm"
        when: windowShown
        function init() {
            veyra.preferences = ({})
            stub.reset()
            page.bridge = stub
            page.view = "main"
            page.autoWizard = true
            page.shownContext = "walk"
            page.refresh()
        }
        function cleanup() { page.view = "main"; veyra.preferences = ({}) }
        function control(name) { const c = findChild(page, name); verify(c !== null, name); return c }
        function editor() { return control("pushfps-kbm-bindings") }
        function wizard() { return control("pushfps-kbm-wizard") }

        // ---- The page with and without the core --------------------------------------------------------------------
        function test_opens_without_the_core_functions() {
            page.bridge = bare
            page.refresh()
            verify(!page.hasBridge)
            verify(!page.usable)
            verify(!control("pushfps-kbm-enabled").enabled, "the switch cannot be used")
            verify(!control("pushfps-kbm-enabled").checked)
            verify(page.stateText.length > 0, "and the page says why")
            verify(!page.setEnabled(true), "nothing is switched")
            verify(control("pushfps-kbm-profile-unavailable").visible)
            verify(!page.setProfile("ultra-low-latency"))
            compare(page.profile, "legacy", "what runs is what ran before")
            verify(!control("pushfps-kbm-tuning").visible)
            compare(page.readoutRows.length, 0)
            for (let n = 1; n <= 4; ++n) verify(control("pushfps-kbm-limit-" + n).text.length > 0, "limit " + n)
            page.openBindings()
            verify(control("pushfps-kbm-bindings-unavailable").visible, "the key editor says it is not available")
            verify(!editor().available)
            page.closeView()
        }
        function test_default_bridge_is_veyra_and_an_older_engine_breaks_nothing() {
            compare(plain.bridge, veyra)
            plain.refresh()
            verify(!plain.hasBridge, "the fixture bridge is an engine without the page's functions")
            verify(!plain.usable)
            verify(!findChild(plain, "pushfps-kbm-enabled").enabled)
            plain.openBindings()
            verify(!findChild(plain, "pushfps-kbm-bindings").available)
            plain.openWizard(0)
            const w = findChild(plain, "pushfps-kbm-wizard")
            w.next(); w.next()
            compare(w.step, 2)
            verify(!w.canFit && !w.canCalibrate, "the wizard offers no measurement it cannot make")
            verify(!w.startMotion())
            w.cancel()
            compare(plain.view, "main")
            compare(Object.keys(veyra.preferences).length, 0, "and nothing was written")
        }
        function test_opens_with_the_core_and_changes_nothing() {
            verify(page.hasBridge && page.usable)
            verify(control("pushfps-kbm-enabled").enabled)
            verify(!control("pushfps-kbm-enabled").checked, "off by default")
            compare(page.profile, "legacy", "As before is the default")
            verify(control("pushfps-kbm-profile-legacy").selected)
            verify(!control("pushfps-kbm-profile-unavailable").visible)
            verify(control("pushfps-kbm-experimental").visible)
            compare(stub.calls.length, 0, "opening the page calls no setter")
            compare(Object.keys(veyra.preferences).length, 0, "and writes no preference")
            compare(control("pushfps-kbm-turn-why").text.length > 0, true)
        }
        function test_build_without_remote_play() {
            stub.patch({ available: false })
            page.refresh()
            verify(!page.usable)
            verify(!control("pushfps-kbm-enabled").enabled)
            verify(page.stateText.indexOf("PS5") >= 0)
        }

        // ---- The switch ------------------------------------------------------------------------------------------------
        function test_switch_toggles_through_the_bridge() {
            page.autoWizard = false
            control("pushfps-kbm-enabled").toggled(true)
            compare(stub.calls.join(";"), "enabled=true")
            verify(control("pushfps-kbm-enabled").checked)
            compare(veyra.preferences.kbmEnabled, true)
            control("pushfps-kbm-enabled").toggled(false)
            verify(!control("pushfps-kbm-enabled").checked)
            compare(veyra.preferences.kbmEnabled, false)
        }
        function test_switch_forced_by_the_start_up_variable() {
            stub.patch({ forced: true, enabled: true })
            page.refresh()
            verify(control("pushfps-kbm-enabled").checked)
            verify(!control("pushfps-kbm-enabled").enabled)
            verify(control("pushfps-kbm-forced").visible)
            verify(!page.setEnabled(false))
            compare(stub.calls.length, 0)
        }
        function test_capture_state_says_how_to_release() {
            stub.patch({ enabled: true, active: true, streaming: true, capture: "captured" })
            page.refresh()
            verify(page.stateText.indexOf("Ctrl+Alt+Shift+Z") >= 0, page.stateText)
            verify(!control("pushfps-kbm-recapture-row").visible)
            stub.patch({ capture: "suspended" })
            page.refresh()
            verify(control("pushfps-kbm-recapture-row").visible)
            control("pushfps-kbm-recapture").clicked()
            compare(stub.allows, 1)
            verify(!control("pushfps-kbm-recapture-row").visible)
            verify(control("pushfps-kbm-controller-row").hint.length > 0, "what happens to a connected controller")
            verify(control("pushfps-kbm-release-row").hint.indexOf("Ctrl+Alt+Shift+Z") >= 0, "the release keys are always on the page")
        }

        // ---- Input performance profiles ---------------------------------------------------------------------------------
        function test_each_profile_has_one_line() {
            const lines = []
            for (const id of Kbm.profileIds) {
                const line = control("pushfps-kbm-profile-line-" + id).text
                verify(line.length > 0, id)
                verify(lines.indexOf(line) < 0, id + " has its own line")
                lines.push(line)
            }
            // The input profiles are told apart from the picture profiles of quick setup (both have an Ultra Low Latency).
            verify(control("pushfps-kbm-profile-scope").visible)
            verify(control("pushfps-kbm-profile-scope").text.length > 0)
            verify(page.profileName("ultra-low-latency") !== "超低延迟", "the input profile has its own name")
        }
        function test_profiles_change_what_can_be_edited() {
            // As before: the numbers are the profile file's, nothing to edit here.
            verify(!page.editable)
            verify(!control("pushfps-kbm-smoothing").visible)
            verify(!control("pushfps-kbm-tuning").visible)
            verify(control("pushfps-kbm-legacy-note").visible)
            // Ultra Low Latency: smoothing shown as off and not editable.
            verify(page.setProfile("ultra-low-latency"))
            compare(stub.calls.join(";"), "profile=ultra-low-latency")
            verify(control("pushfps-kbm-profile-ultra-low-latency").selected)
            verify(control("pushfps-kbm-smoothing").visible)
            verify(!control("pushfps-kbm-smoothing").enabledControl, "smoothing cannot be edited")
            compare(control("pushfps-kbm-smoothing").value, 0)
            compare(control("pushfps-kbm-smoothing-row").value, qsTr("关闭", "off"))
            verify(control("pushfps-kbm-smoothing-row").hint.indexOf(qsTr("不可更改")) >= 0)
            verify(!page.editable)
            verify(!control("pushfps-kbm-transmit-row").visible)
            verify(control("pushfps-kbm-tuning").visible, "the values are shown")
            verify(control("pushfps-kbm-readonly").visible)
            verify(!page.setCustom("walk.sensX", 2), "and cannot be changed")
            compare(stub.calls.length, 1)
            // Adaptive Precision: no temporal buffering either.
            verify(page.setProfile("adaptive-precision"))
            verify(!control("pushfps-kbm-smoothing").enabledControl)
            verify(!page.editable)
            // Custom: everything can be edited.
            verify(page.setProfile("custom"))
            verify(page.editable)
            verify(control("pushfps-kbm-smoothing").enabledControl)
            verify(control("pushfps-kbm-transmit-row").visible, "the sending choice is Custom's")
            verify(!control("pushfps-kbm-readonly").visible)
            verify(page.setCustom("walk.sensX", 2))
            compare(Number(page.tuning.sensX), 2)
            // NaN and infinity are refused, never stored as 0 (the bridge refuses them as well).
            const sent = stub.calls.length
            verify(!page.setCustom("walk.deadzone", NaN))
            verify(!page.setCustom("walk.sensX", Infinity))
            compare(stub.calls.length, sent, "nothing reached the bridge")
            compare(Number(page.tuning.sensX), 2)
            control("pushfps-kbm-smoothing").moved(4.2)
            compare(Number(page.settings.smoothingMs), 4)
            control("pushfps-kbm-curve").picked("power")
            compare(String(page.tuning.curve), "power")
            // Sending on change is not acted on by this core: shown, not offered.
            verify(!control("pushfps-kbm-transmit").enabled)
            compare(control("pushfps-kbm-transmit").current, "proven")
            stub.patch({ eventSend: true })
            page.refresh()
            verify(control("pushfps-kbm-transmit").enabled)
            control("pushfps-kbm-transmit").picked("leading-edge")
            compare(String(page.settings.transmit), "leading-edge")
            compare(control("pushfps-kbm-transmit").current, "leading-edge")
            verify(!control("pushfps-kbm-leading-edge").visible, "chosen is not yet in effect")
            stub.patch({ active: true, leadingEdge: true })
            page.refresh()
            verify(control("pushfps-kbm-leading-edge").visible, "what the running stream really does is said")
        }
        function test_game_calibration_numbers_and_tilt_cap() {
            page.setProfile("custom")
            verify(page.gameNumbers, "the presets take dead zone and full-turn speed from the game calibration")
            verify(control("pushfps-kbm-game").checked)
            verify(!findChild(page, "pushfps-kbm-walk-deadzone").enabledControl, "so the context's own dead zone is not editable")
            compare(control("pushfps-kbm-deadzone-row").value, qsTr("来自游戏校准"))
            compare(control("pushfps-kbm-turn-value").text, qsTr("来自游戏校准"))
            control("pushfps-kbm-game").toggled(false)
            compare(stub.custom.walk.game, false)
            verify(!page.gameNumbers)
            verify(findChild(page, "pushfps-kbm-walk-deadzone").enabledControl)
            compare(control("pushfps-kbm-turn-value").text, qsTr("%1 计数/秒").arg(4000))
            verify(page.setCustom("walk.maxTilt", 0.8))
            compare(Number(page.tuning.maxTilt), 0.8)
            fuzzyCompare(Kbm.stickFor(page.tuning, 1), 0.8, 1e-9, "the plot stops at the cap")
            // Read-only profiles: the switch is shown but cannot be changed.
            page.setProfile("adaptive-precision")
            verify(!control("pushfps-kbm-game").enabled)
        }
        function test_profile_set_by_a_test_switch() {
            stub.patch({ inputForced: true, inputProfile: "custom", active: true, inUse: "ultra-low-latency" })
            page.refresh()
            verify(control("pushfps-kbm-input-forced").visible)
            verify(!control("pushfps-kbm-profile-legacy").enabled)
            verify(!page.setProfile("legacy"))
            verify(!page.editable, "Custom's values cannot be edited either")
            verify(!page.setCustom("walk.sensX", 3))
            verify(!page.customize())
            compare(stub.calls.length, 0)
            verify(control("pushfps-kbm-inuse").visible, "and the profile really in use is named")
        }
        function test_customise_starts_from_the_shown_profile() {
            page.setProfile("adaptive-precision")
            control("pushfps-kbm-customize").clicked()
            verify(stub.calls.indexOf("customFrom=adaptive-precision") >= 0)
            compare(page.profile, "custom")
            verify(page.editable)
        }
        function test_contexts_and_manual_override() {
            page.setProfile("custom")
            compare(control("pushfps-kbm-context").options.map(o => o.id).join(","), "walk,aim,sprint,drive")
            verify(!control("pushfps-kbm-ads-row").visible)
            control("pushfps-kbm-context").picked("aim")
            compare(page.shownContext, "aim")
            verify(control("pushfps-kbm-ads-row").visible, "ADS sensitivity belongs to aiming")
            page.setCustom("aim.sensY", 0.5)
            compare(Number(page.tuning.sensY), 0.5)
            compare(Number(stub.custom.aim.sensY), 0.5)
            compare(control("pushfps-kbm-override").options.map(o => o.id).join(","), "auto,walk,aim,sprint,drive")
            compare(control("pushfps-kbm-override").current, "auto")
            control("pushfps-kbm-override").picked("drive")
            compare(stub.state.contextOverride, "drive")
            compare(control("pushfps-kbm-override").current, "drive")
        }
        function test_fallback_and_profile_in_use_are_named() {
            page.setProfile("adaptive-precision")
            verify(!control("pushfps-kbm-fallback").visible)
            stub.patch({ active: true, streaming: true, inUse: "ultra-low-latency", fallback: "queue-older" })
            page.refresh()
            verify(control("pushfps-kbm-fallback").visible)
            verify(control("pushfps-kbm-fallback").text.indexOf(page.fallbackReason("queue-older")) >= 0)
            verify(control("pushfps-kbm-inuse").visible)
            verify(control("pushfps-kbm-inuse").text.indexOf(page.profileName("ultra-low-latency")) >= 0)
        }
        function test_curve_plot_points() {
            page.setProfile("custom")
            compare(page.plotPoints.length, 33, "without pushKbmCurvePoints the page computes them")
            compare(page.plotPoints[0].out, 0)
            compare(page.plotPoints[32].out, 1)
            verify(control("pushfps-kbm-curve-plot").visible)
            page.setCustom("walk.game", false)
            compare(control("pushfps-kbm-turn-value").text, qsTr("%1 计数/秒").arg(4000))
        }

        // ---- Live readout ------------------------------------------------------------------------------------------------
        function test_readout_only_while_streaming() {
            stub.readout = ({ active: true, source: "kbm", context: "walk", mouseHz: 1000, translateUsP95: 12, rawToTranslateUsP95: 80,
                              stateToSendMsP95: 1.5, eventToSendMsP95: -1, sendGapMsP95: 4.1, saturatedPct: 3, fallback: "none" })
            page.refresh()
            compare(page.readoutRows.length, 0, "no stream: no numbers")
            stub.patch({ enabled: true, active: true, streaming: true })
            page.refresh()
            compare(page.readoutRows.length, 10)
            verify(control("pushfps-kbm-readout-note").text === qsTr("这些是电脑内部的处理时间，不是从输入到画面的延迟。"))
            compare(control("pushfps-kbm-readout-value-translate").text, qsTr("%1 微秒").arg("12"))
            compare(control("pushfps-kbm-readout-value-event").text, qsTr("未知"), "a value the core does not know")
            compare(control("pushfps-kbm-readout-value-source").text, page.sourceName("kbm"))
            verify(!control("pushfps-kbm-readout-stats").visible)
            stub.readout = Object.assign({}, stub.readout, { stateToSendMsP95: -1, sendGapMsP95: -1 })
            page.refresh()
            verify(control("pushfps-kbm-readout-stats").visible, "says when the sending times are not measured")
        }

        // ---- Key editor ----------------------------------------------------------------------------------------------------
        function test_binding_conflict_is_refused_and_named() {
            page.openBindings()
            compare(page.view, "bindings")
            const e = editor()
            verify(e.available)
            compare(e.keysFor("lup").join(","), "W")
            control("pushfps-kbm-bind-cross").clicked()
            compare(e.capturing, "cross")
            verify(control("pushfps-kbm-capture").visible)
            verify(!e.captureKey("W"), "W already moves forward")
            compare(e.capturing, "")
            verify(e.conflict !== null)
            compare(e.conflict.existing, "lup")
            verify(control("pushfps-kbm-conflict").visible)
            verify(control("pushfps-kbm-conflict-text").text.indexOf(e.actionLabel("lup")) >= 0, "the other action is named")
            verify(control("pushfps-kbm-conflict-text").text.indexOf("W") >= 0)
            compare(Kbm.actionOf(stub.walk, "W"), "lup", "nothing was changed")
            control("pushfps-kbm-conflict-keep").clicked()
            verify(e.conflict === null)
            compare(Kbm.actionOf(stub.walk, "W"), "lup")
            // Again, and this time the user moves the key.
            e.beginCapture("cross")
            verify(!e.captureKey("W"))
            control("pushfps-kbm-conflict-replace").clicked()
            compare(Kbm.actionOf(stub.walk, "W"), "cross")
            compare(e.keysFor("lup").length, 0)
            compare(e.keysFor("cross").join(","), "W")
        }
        function test_binding_a_free_key_and_removing_it() {
            page.openBindings()
            const e = editor()
            e.beginCapture("triangle")
            verify(e.captureKey("G"))
            compare(Kbm.actionOf(stub.walk, "G"), "triangle")
            verify(e.status.length > 0)
            verify(findChild(page, "pushfps-kbm-key-triangle-G") !== null, "shown as a key of that action")
            verify(e.unbind("G"))
            compare(Kbm.actionOf(stub.walk, "G"), "none")
            // No key waiting: a stray key does nothing.
            verify(!e.captureKey("S"))
            compare(Kbm.actionOf(stub.walk, "S"), "ldown")
            // Esc is bound from the list, never by pressing it.
            e.beginCapture("circle")
            control("pushfps-kbm-key-list").picked("Esc")
            compare(Kbm.actionOf(stub.walk, "Esc"), "circle")
        }
        function test_bindings_per_context() {
            page.openBindings()
            const e = editor()
            control("pushfps-kbm-bindings-context").picked("drive")
            compare(e.context, "drive")
            compare(e.keysFor("r2").join(","), "W", "driving has its own keys")
            e.beginCapture("l2")
            verify(!e.captureKey("W"))
            compare(e.conflict.existing, "r2")
            e.resolveConflict(false)
            // Aiming shows walking's keys with its own differences on top.
            control("pushfps-kbm-bindings-context").picked("aim")
            compare(e.keysFor("lup").join(","), "W")
            e.beginCapture("triangle")
            verify(e.captureKey("G"))
            compare(Kbm.actionOf(stub.aim, "G"), "triangle")
            compare(Kbm.actionOf(stub.walk, "G"), "none", "walking is untouched")
        }
        function test_reset_is_asked_twice_and_files() {
            page.openBindings()
            const e = editor()
            control("pushfps-kbm-bindings-reset").clicked()
            compare(stub.resets, 0, "the first click only arms")
            verify(e.resetArmed)
            control("pushfps-kbm-bindings-reset").clicked()
            compare(stub.resets, 1)
            verify(!e.resetArmed)
            verify(findChild(page, "pushfps-kbm-bindings-reset") !== null)
            compare(e.bindings.layoutName.indexOf("press tables") >= 0, true, "the core's own words for the built-in layout are passed on")
            compare(control("pushfps-kbm-file").options.map(o => o.id).join(","), "gta-6")
            control("pushfps-kbm-file").picked("gta-6")
            compare(stub.loaded.join(","), "gta-6")
            verify(!e.saveFile("  "), "a name is needed")
            compare(stub.saved.length, 0)
            verify(e.saveFile("GTA 6 mine"))
            compare(stub.saved.join(","), "gta-6-mine")
            control("pushfps-kbm-bindings-back").clicked()
            compare(page.view, "main")
        }

        function test_gta_preset_and_menu_keys() {
            page.openBindings()
            const e = editor()
            verify(control("pushfps-kbm-preset-row").visible, "the core has the preset")
            compare(control("pushfps-kbm-bindings-context").options.map(o => o.id).join(","), "walk,aim,drive,menu")
            control("pushfps-kbm-preset-gta-pc").clicked()
            compare(stub.presets.join(","), "gta-pc", "one call, nothing else")
            verify(e.status.indexOf("换弹") >= 0, "the status says the reload button is not published")
            compare(e.keysFor("triangle").join(","), "F")
            compare(e.keysFor("menu").join(","), "Esc")
            compare(e.actionLabel("menu").length > 0 && e.actionLabel("menu") !== "menu", true)
            control("pushfps-kbm-bindings-context").picked("menu")
            compare(e.context, "menu")
            compare(e.keysFor("dup").join(","), "W", "the menu keys are their own table")
            e.beginCapture("circle")
            verify(e.captureKey("G"))
            compare(Kbm.actionOf(stub.menu, "G"), "circle")
            compare(Kbm.actionOf(stub.walk, "G"), "none", "walking is untouched")
            compare(e.contextLabel("menu").length > 0, true)
            // An engine without the preset or the menu keys: no row, three contexts, as before.
            page.bridge = bare
            page.openBindings()
            verify(!findChild(page, "pushfps-kbm-preset-row").visible)
        }

        // ---- Wizard ----------------------------------------------------------------------------------------------------------
        function test_wizard_opens_the_first_time_only() {
            verify(page.setEnabled(true))
            compare(page.view, "wizard", "first time the switch is turned on")
            compare(wizard().step, 0)
            control("pushfps-kbm-wizard-cancel").clicked()
            compare(page.view, "main")
            compare(veyra.preferences.pushKbmWizardDone, undefined, "cancelling does not mark it done")
            page.setEnabled(false)
            page.setEnabled(true)
            compare(page.view, "wizard")
            for (let n = 0; n < 3; ++n) control("pushfps-kbm-wizard-skip").clicked()
            compare(wizard().step, 3)
            control("pushfps-kbm-wizard-finish").clicked()
            compare(page.view, "main")
            compare(veyra.preferences.pushKbmWizardDone, true)
            page.setEnabled(false)
            page.setEnabled(true)
            compare(page.view, "main", "not shown again")
            control("pushfps-kbm-wizard-open").clicked()
            compare(page.view, "wizard", "still there from its button")
        }
        function test_wizard_cancel_applies_nothing() {
            page.openWizard(0)
            const w = wizard()
            w.layoutChoice = "builtin"
            control("pushfps-kbm-wizard-next").clicked()
            compare(w.step, 1)
            control("pushfps-kbm-wizard-next").clicked()
            control("pushfps-kbm-wizard-next").clicked()
            compare(w.step, 3)
            verify(control("pushfps-kbm-wizard-summary-0").text.length > 0)
            const before = stub.calls.length
            w.cancel()
            compare(page.view, "main")
            compare(stub.resets, 0, "the layout was not applied")
            compare(stub.loaded.length, 0)
            verify(stub.fitApplied === null)
            compare(stub.calls.length, before)
            compare(veyra.preferences.pushKbmWizardDone, undefined)
        }
        function test_wizard_finish_applies_and_marks_done() {
            page.openWizard(0)
            const w = wizard()
            w.layoutChoice = "builtin"
            compare(stub.resets, 0, "nothing before the end")
            w.next(); w.next(); w.next()
            compare(w.finish().join(","), "layout:builtin")
            compare(stub.resets, 1)
            verify(stub.fitApplied === null, "no sensitivity change was chosen")
            compare(veyra.preferences.pushKbmWizardDone, true)
            compare(page.view, "main")
            // A profile file as the layout; skipping step 1 drops the choice.
            page.openWizard(0)
            w.layoutChoice = "file:gta-6"
            w.next(); w.next(); w.next()
            compare(w.finish().join(","), "file:gta-6")
            compare(stub.loaded.join(","), "gta-6")
            page.openWizard(0)
            w.layoutChoice = "builtin"
            w.skip(); w.skip(); w.skip()
            compare(w.finish().length, 0, "skipped steps apply nothing")
            compare(stub.resets, 1)
        }
        function test_wizard_steps_need_a_stream() {
            page.openWizard(1)
            const w = wizard()
            compare(w.step, 1)
            verify(control("pushfps-kbm-wizard-cal-needs").visible, "says what is needed and that it can be skipped")
            verify(!control("pushfps-kbm-wizard-cal-start").visible)
            verify(!w.startCalibration())
            verify(!w.wantsPicture)
            compare(page.videoViewport.width, 0, "no picture without a stream")
            w.next()
            verify(control("pushfps-kbm-wizard-fit-needs").visible)
            verify(!w.startMotion())
            w.cancel()
            // A build whose capture has no page-driven calibration: the hotkey is named instead.
            stub.patch({ calibration: false, fit: false })
            page.openWizard(1)
            verify(control("pushfps-kbm-wizard-cal-hotkey").visible)
            w.next()
            verify(control("pushfps-kbm-wizard-fit-unavailable").visible)
            w.cancel()
        }
        function test_wizard_game_calibration_takes_and_returns_the_mouse() {
            stub.patch({ enabled: true, active: true, streaming: true, capture: "released" })
            page.openWizard(1)
            const w = wizard()
            verify(w.wantsPicture)
            verify(page.videoViewport.width > 0 && page.videoViewport.height > 0, "the game picture is shown beside the step")
            control("pushfps-kbm-wizard-cal-start").clicked()
            compare(w.calPhase, "arming")
            verify(stub.held, "the page asked for the mouse")
            compare(stub.calStarts, 0, "not started before the capture has it")
            w.pollCalibration()
            compare(stub.calStarts, 1)
            compare(w.calPhase, "running")
            stub.calSteps = ["turn-time"]
            w.addLines(["calibration: walk full turn 1.250 s (288.0 degrees per second)"])
            compare(w.gameTurnSeconds, 1.25, "the measured full turn is kept for step 3")
            w.pollCalibration()
            compare(w.calStep, "turn-time")
            verify(control("pushfps-kbm-wizard-cal-instruction").text === w.calInstruction("turn-time"))
            verify(w.busy)
            w.next()
            compare(w.step, 1, "no leaving the step while it runs")
            stub.calRunning = false
            stub.calEnding = ["calibration: walk dead zone 0.150", "calibration finished"]
            w.pollCalibration()
            compare(w.calPhase, "done")
            verify(!stub.held, "the mouse is given back")
            verify(w.calFinished)
            compare(stub.keeps, 0, "nothing is kept before Finish and apply")
            verify(control("pushfps-kbm-wizard-cal-kept").text === qsTr("引擎已写入并正在使用这些数值；点“完成并应用”后整个方案也会保存，下次连接 PS5 时继续使用"))
            // Cancelling while it runs stops it (a stopped calibration writes nothing) and gives the mouse back.
            w.startCalibration(); w.pollCalibration()
            compare(w.calPhase, "running")
            w.cancel()
            compare(stub.calStops, 1)
            verify(!stub.held)
            compare(stub.keeps, 0, "a stopped calibration keeps nothing")
            compare(page.videoViewport.width, 0)
        }
        function finishOneCalibration(w) {
            w.startCalibration(); w.pollCalibration()
            compare(w.calPhase, "running")
            stub.calRunning = false
            stub.calEnding = ["calibration: walk dead zone 0.150", "calibration finished"]
            w.pollCalibration()
            compare(w.calPhase, "done")
            verify(w.calFinished)
        }
        function test_wizard_finished_calibration_is_kept_only_by_finish() {
            stub.patch({ enabled: true, active: true, streaming: true, capture: "released" })
            page.openWizard(1)
            let w = wizard()
            finishOneCalibration(w)
            w.cancel()
            compare(stub.keeps, 0, "Cancel after a finished calibration keeps nothing")
            compare(veyra.preferences.pushKbmWizardDone, undefined)
            page.openWizard(1)
            w = wizard()
            finishOneCalibration(w)
            w.skip()
            compare(w.step, 2)
            w.skip()
            compare(w.finish().length, 0, "a skipped step 2 keeps nothing")
            compare(stub.keeps, 0)
            page.openWizard(1)
            w = wizard()
            finishOneCalibration(w)
            w.next(); w.next()
            compare(w.step, 3)
            compare(w.finish().join(","), "calibration:default")
            compare(stub.keeps, 1, "kept by Finish and apply")
            compare(w.calKept, "default")
        }
        function test_wizard_test_motions_fit_and_apply_only_at_the_end() {
            stub.patch({ enabled: true, active: true, streaming: true, capture: "released" })
            page.openWizard(2)
            const w = wizard()
            compare(w.step, 2)
            for (let n = 0; n < 9; ++n) {
                compare(w.motions.length, n)
                verify(w.startMotion())
                verify(stub.held)
                w.pollMotion()
                compare(w.motionPhase, "recording")
                verify(w.endMotion())
                verify(!stub.held, "the mouse comes back after each motion")
            }
            compare(stub.calls.filter(c => c.indexOf("record ") === 0).join(";"),
                    "record turn;record turn;record turn;record flick;record flick;record flick;record slow;record slow;record slow")
            verify(w.fit !== null && w.fit.ok === true)
            compare(w.motionNote, "")
            verify(control("pushfps-kbm-wizard-fit-result").visible)
            verify(w.tuning === null, "fitted numbers are not chosen by themselves")
            control("pushfps-kbm-wizard-use-fit").toggled(true)
            verify(w.tuning !== null)
            verify(stub.fitApplied === null, "and not applied before the end")
            w.next()
            compare(w.step, 3)
            compare(w.finish().join(","), "tuning")
            compare(stub.fitApplied.context, "all")
            compare(Number(stub.fitApplied.tuning.fullSpeed), 6000)
            compare(page.profile, "custom")
        }

        function test_wizard_motion_without_movement_is_asked_again() {
            stub.patch({ enabled: true, active: true, streaming: true, capture: "released" })
            page.openWizard(2)
            const w = wizard()
            stub.motionCounts = 0
            verify(w.startMotion())
            w.pollMotion()
            verify(!w.endMotion(), "nothing moved")
            compare(w.motions.length, 0)
            verify(control("pushfps-kbm-wizard-motion-note").visible)
            verify(!stub.held)
            compare(w.fitReason("need-flicks") !== "need-flicks", true, "the core's reasons are put in words")
            compare(w.fitReason("something-new"), "something-new")
            w.cancel()
        }

        // ---- Helpers and the fixture -------------------------------------------------------------------------------------------
        function test_key_names() {
            compare(Kbm.keyName(Qt.Key_W, Qt.NoModifier), "W")
            compare(Kbm.keyName(Qt.Key_5, Qt.NoModifier), "5")
            compare(Kbm.keyName(Qt.Key_5, Qt.KeypadModifier), "Num5")
            compare(Kbm.keyName(Qt.Key_Space, Qt.NoModifier), "Space")
            compare(Kbm.keyName(Qt.Key_Shift, Qt.ShiftModifier), "Shift")
            compare(Kbm.keyName(Qt.Key_Control, Qt.ControlModifier), "Ctrl")
            compare(Kbm.keyName(Qt.Key_Alt, Qt.AltModifier), "Alt")
            compare(Kbm.keyName(Qt.Key_Tab, Qt.NoModifier), "Tab")
            compare(Kbm.keyName(Qt.Key_Return, Qt.NoModifier), "Enter")
            compare(Kbm.keyName(Qt.Key_Backspace, Qt.NoModifier), "Back")
            compare(Kbm.keyName(Qt.Key_CapsLock, Qt.NoModifier), "Caps")
            compare(Kbm.keyName(Qt.Key_PageDown, Qt.NoModifier), "PageDown")
            compare(Kbm.keyName(Qt.Key_F1, Qt.NoModifier), "F1")
            compare(Kbm.keyName(Qt.Key_F12, Qt.NoModifier), "F12")
            compare(Kbm.keyName(Qt.Key_BracketLeft, Qt.NoModifier), "LBracket")
            compare(Kbm.keyName(Qt.Key_Meta, Qt.NoModifier), "", "a key the profile file has no name for")
            compare(Kbm.mouseName(Qt.LeftButton), "Mouse1")
            compare(Kbm.mouseName(Qt.MiddleButton), "Mouse2")
            compare(Kbm.mouseName(Qt.RightButton), "Mouse3")
            compare(Kbm.mouseName(Qt.BackButton), "Mouse4")
            compare(Kbm.mouseName(Qt.ForwardButton), "Mouse5")
            compare(Kbm.wheelName(120), "WheelUp")
            compare(Kbm.wheelName(-120), "WheelDown")
            compare(Kbm.slug(" GTA 6: mine! "), "gta-6-mine")
            compare(Kbm.slug("***"), "")
        }
        function test_curve_is_immediate_and_monotonic() {
            const curves = [{ curve: "linear" }, { curve: "power", exponent: 2 }, { curve: "power", exponent: 0.5 },
                            { curve: "precision-speed", kneeIn: 0.25, kneeOut: 0.1 }]
            for (const c of curves) {
                compare(Kbm.curve(c, 0), 0)
                compare(Kbm.curve(c, 1), 1)
                let last = 0
                for (let k = 1; k <= 100; ++k) { const v = Kbm.curve(c, k / 100); verify(v >= last, c.curve + " rises"); last = v }
            }
            fuzzyCompare(Kbm.curve({ curve: "power", exponent: 2 }, 0.5), 0.25, 1e-9)
            fuzzyCompare(Kbm.curve({ curve: "precision-speed", kneeIn: 0.25, kneeOut: 0.1 }, 0.25), 0.1, 1e-9)
            fuzzyCompare(Kbm.stickFor({ curve: "linear", deadzone: 0.2 }, 0.5), 0.6, 1e-9)
            compare(Kbm.stickFor({ curve: "linear", deadzone: 0.2 }, 0), 0, "no motion: no tilt")
            compare(Kbm.call(bare, "pushKbmState", [], "fallback"), "fallback")
            compare(Kbm.call(null, "pushKbmState", [], 7), 7)
            compare(Kbm.aimList([{ key: "W", action: "lup" }, { key: "G", action: "cross" }], [{ key: "G", action: "none" }, { key: "Mouse2", action: "fine" }]).map(b => b.key + "=" + b.action).join(","),
                    "W=lup,Mouse2=fine")
        }
        function test_fixture_takes_the_page_preferences() {
            verify(veyra.setPreference("pushKbmWizardDone", true))
            verify(!veyra.setPreference("pushKbmWizard", true), "an unknown push* key is refused, as by the real validator")
            verify(veyra.setPreference("kbmEnabled", true))
        }
    }
}
