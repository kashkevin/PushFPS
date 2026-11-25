// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Performance modes for the quick setup (row pushfps-mode, PushModeRow.qml).
//
// plan() is the interface's copy of pushModePlan() in include/veyra/engine/PushPerformanceMode.h. The two
// are held together by one table of rows that both test suites check
// (tests/remoteplay/PerformanceModeTests.inc and tests/qml/quick/tst_push_slipstream.qml): change the
// table in the header, here and in both tests together.
//
//   Mode                 30 fps game        60 fps game        presentation
//   ultra-low-latency    no generation      no generation      one-frame queue, variable refresh or tearing, no cap
//   competitive          no generation      2X  -> 120         one-frame queue, variable refresh
//   balanced             3X  -> 90          2X  -> 120         variable refresh
//   maximum-smoothness   4X  -> 120         3X  -> 180         variable refresh
//
// A mode only writes settings the interface already has (frame generation, multiplier, duplicate
// removal, NVIDIA low latency or the low queue, display sync, the variable refresh request, the output
// cap), through the same bridge properties as the rest of the quick setup. What was set before is kept
// in the pushModeRestore preference and comes back with "off", like the lowest latency switch.
// Nothing here runs for a user who never picks a mode. Unverified on Windows.
.pragma library

var ids = ["ultra-low-latency", "competitive", "balanced", "maximum-smoothness"]
var kMaxMultiplier = 4
// Same fit rule as kPushModeDisplayFit: source x M may exceed the refresh rate by 2 %.
var kDisplayFit = 1.02

function isMode(id) { return ids.indexOf(String(id)) >= 0 }

// 30 or 60 for a rate the table has a row for (29.97 and 59.94 on the stream), else 0.
function cadence(sourceFps) {
    var fps = Number(sourceFps)
    if (!isFinite(fps)) return 0
    if (fps >= 28.5 && fps <= 31.5) return 30
    if (fps >= 57.0 && fps <= 63.0) return 60
    return 0
}

// Minimum delay interpolation adds to every real picture: (M-1)/M x source interval, in milliseconds.
function delayMs(multiplier, sourceFps) {
    var m = Number(multiplier), fps = Number(sourceFps)
    if (!(m >= 2) || !isFinite(fps) || fps <= 0) return 0
    return (m - 1) / m * 1000 / fps
}

// pushModePlan(): what one mode means for a source rate and a display. reason: ok | display-limit |
// bad-source-rate | bad-display-rate | mode-not-concrete; only the first two come with valid.
function plan(id, sourceFps, displayHz) {
    var rank = ids.indexOf(String(id))
    var p = { valid: false, mode: rank >= 0 ? ids[rank] : "", frameGeneration: false, multiplier: 1, outputFps: 0,
              minAddedDelayMs: 0, lowQueue: false, presentNewestAtOnce: false, wantsVrr: true,
              tearingAcceptable: false, unevenWithoutVrr: false, onlyWhileStable: false, reason: "mode-not-concrete" }
    if (rank < 0) return p
    p.lowQueue = rank <= 1
    p.presentNewestAtOnce = rank === 0
    p.tearingAcceptable = rank === 0
    p.onlyWhileStable = rank === 3
    var fps = Number(sourceFps), hz = Number(displayHz)
    var c = cadence(fps)
    if (c === 0) { p.reason = "bad-source-rate"; return p }
    if (!isFinite(hz) || hz <= 0) { p.reason = "bad-display-rate"; return p }
    var m = [1, c === 30 ? 1 : 2, c === 30 ? 3 : 2, c === 30 ? 4 : 3][rank]
    p.reason = "ok"
    while (m > 1 && fps * m > hz * kDisplayFit) { --m; p.reason = "display-limit" }
    p.valid = true
    p.multiplier = m
    p.frameGeneration = m > 1
    p.outputFps = fps * m
    p.minAddedDelayMs = delayMs(m, fps)
    var refreshesPerFrame = hz / p.outputFps
    p.unevenWithoutVrr = refreshesPerFrame >= 1 && Math.abs(refreshesPerFrame / Math.round(refreshesPerFrame) - 1) > kDisplayFit - 1
    return p
}

// --- what this PC offers, read from the bridge ---------------------------------------------------------
// Capabilities, never a model name: whether frame generation can run and with which multipliers, whether
// NVIDIA low latency exists (else the plain low queue), what the driver says about variable refresh
// (0 unknown, 1 the screen cannot, 2 it can but not now, 3 possible, 4 active), the refresh rate, and
// whether the generator presents by itself (XeSS: pacing settings are then not ours to set).
function caps(veyra) {
    // Reading the notifying property makes a binding that calls this follow the capabilities (EffectSupport.js).
    var capabilities = veyra.effectCapabilities
    var fgStatus = typeof veyra.effectAvailability === "function" ? veyra.effectAvailability("frame-generation") : { available: true }
    var offered = (veyra.fgMultiplierChoices || []).map(function (o) { return parseInt(o.id) })
        .filter(function (m) { return m >= 2 && m <= kMaxMultiplier }).sort(function (a, b) { return a - b })
    return { fgPossible: !!(fgStatus && fgStatus.available) && offered.length > 0, multipliers: offered,
             nvidia: veyra.nvidiaLowLatencyAvailable === true, vrrState: Number(veyra.vrrState || 0),
             displayHz: Number(veyra.displayRefreshHz || 0), presentationOwned: veyra.presentationOwned === true }
}

// The settings a mode owns, as they are now.
function snapshot(veyra) {
    return { fg: veyra.fgEnabled === true, mult: Number(veyra.fgMultiplier || 1), content: Number(veyra.contentRate || 0),
             nvidia: Number(veyra.nvidiaLowLatency || 0), queue: veyra.fgLowQueue === true,
             display: Number(veyra.displaySync), vrr: veyra.variableRefresh === true,
             rate: Number(veyra.outputRateMode || 0), fps: Number(veyra.outputCustomFps || 0) }
}

// The settings a plan asks for on this PC. base: the settings from before any mode; what a mode does not
// own for this plan stays as it is there. notes: why the result is less than the table says, each a
// lower-latency choice (shown to the user by PushModeRow.noteText):
//   bad-source-rate | bad-display-rate   no row for this input: no generation
//   display-limit                        the multiplier was lowered so the output fits the refresh rate
//   backend-limit                        the frame generation method offers less than the plan's multiplier
//   fg-unavailable                       frame generation cannot run here: no generation
//   tearing                              no usable variable refresh: tearing instead of waiting (Ultra Low Latency)
//   uneven                               no usable variable refresh and the output does not divide the refresh rate
function target(p, c, base) {
    var t = { fg: false, mult: 1, content: base.content, nvidia: base.nvidia, queue: base.queue,
              display: base.display, vrr: base.vrr, rate: base.rate, fps: base.fps, notes: [] }
    var m = p.valid ? p.multiplier : 1
    if (!p.valid) t.notes.push(p.reason)
    else if (p.reason === "display-limit") t.notes.push("display-limit")
    if (m > 1) {
        var best = 1
        if (c.fgPossible)
            for (var i = 0; i < c.multipliers.length; ++i) if (c.multipliers[i] <= m) best = c.multipliers[i]
        if (best < m) t.notes.push(best > 1 ? "backend-limit" : "fg-unavailable")
        m = best
    }
    t.fg = m > 1
    t.mult = m
    // Generating between two identical pictures wastes the work and smears: automatic duplicate removal
    // first, as the target row does. A removal mode the user chose (60 -> 30) stays.
    if (t.fg && base.content === 0) t.content = 1
    if (p.lowQueue) {
        if (c.nvidia) t.nvidia = 1
        else t.queue = true
    }
    if (c.presentationOwned) return t
    var nominal = cadence(p.outputFps / Math.max(1, p.multiplier))
    var out = nominal * m
    // Variable refresh the driver reports as possible or active: every frame waits for a refresh that the
    // screen starts when the frame arrives. Anything less (unknown, off in the driver, not supported) is
    // not counted on by the two low-queue modes.
    var vrr = c.vrrState >= 3
    if (vrr) { t.display = 1; t.vrr = true }
    else if (p.tearingAcceptable) { t.display = 0; t.notes.push("tearing") }
    else if (p.lowQueue) t.display = 2
    else { t.display = 1; t.vrr = true }
    if (!vrr && t.fg && c.displayHz > 0) {
        var per = c.displayHz / out
        if (per >= 1 && Math.abs(per / Math.round(per) - 1) > kDisplayFit - 1) t.notes.push("uneven")
    }
    // Without generation nothing is held for pacing. With it the output is capped at what the plan says
    // (never above the display: plan() saw to that), so a source that turns out faster cannot push past it.
    if (!t.fg) t.rate = 0
    else if (c.displayHz > 0 && out >= c.displayHz - 0.5) t.rate = 1
    else { t.rate = 2; t.fps = out }
    return t
}

// Writes settings, only the ones that differ, frame generation first.
function write(veyra, c, t) {
    if (t.fg) { if (veyra.fgEnabled !== true || Number(veyra.fgMultiplier) !== t.mult) veyra.fgMultiplier = t.mult }
    else if (veyra.fgEnabled === true) veyra.fgEnabled = false
    if (Number(veyra.contentRate || 0) !== t.content) veyra.contentRate = t.content
    if (c.nvidia) { if (Number(veyra.nvidiaLowLatency || 0) !== t.nvidia) veyra.nvidiaLowLatency = t.nvidia }
    else if ((veyra.fgLowQueue === true) !== t.queue) veyra.fgLowQueue = t.queue
    if (c.presentationOwned) return
    if (Number(veyra.displaySync) !== t.display) veyra.displaySync = t.display
    if ((veyra.variableRefresh === true) !== t.vrr) veyra.variableRefresh = t.vrr
    // The custom cap's number too when it is not the cap in use, so that "off" puts the old one back.
    if (t.fps >= 1 && Math.abs(Number(veyra.outputCustomFps || 0) - t.fps) > 0.001) veyra.outputCustomFps = t.fps
    if (Number(veyra.outputRateMode || 0) !== t.rate) veyra.outputRateMode = t.rate
}

// The chosen mode ("" = off, the user's own settings) and the settings saved from before it.
function current(veyra) {
    var id = String((veyra.preferences || {}).pushMode || "")
    return isMode(id) ? id : ""
}
function saved(veyra) {
    var s = null
    try { s = JSON.parse(String((veyra.preferences || {}).pushModeRestore || "")) } catch (e) { s = null }
    if (!s || typeof s !== "object" || typeof s.fg !== "boolean" || typeof s.display !== "number" || typeof s.rate !== "number") return null
    return { fg: s.fg, mult: Number(s.mult) >= 2 ? Number(s.mult) : 1, content: Number(s.content) >= 0 ? Number(s.content) : 0,
             nvidia: Number(s.nvidia) > 0 ? 1 : 0, queue: s.queue === true,
             display: s.display >= 0 && s.display <= 2 ? s.display : 2, vrr: s.vrr === true,
             rate: s.rate >= 0 && s.rate <= 2 ? s.rate : 0, fps: Number(s.fps) >= 1 ? Number(s.fps) : 0, notes: [] }
}

// The source rate a plan is made for: the measured one when it is a rate the table has a row for, else
// the game frame rate chosen in the quick setup (pushGameFps: 30, 40 or 60; 40 has no row).
// stats: pushLiveStats(), or nothing.
function measuredFps(stats) {
    if (!stats || stats.running !== true) return 0
    var unique = Number(stats.cadenceActive === true ? stats.cadenceSourceFps : 0)
    if (cadence(unique) > 0) return unique
    var content = Number(stats.contentFps || 0)
    return cadence(content) > 0 ? content : 0
}
function chosenFps(veyra) {
    var n = Number((veyra.preferences || {}).pushGameFps || 30)
    return n === 40 || n === 60 ? n : 30
}
function sourceFps(veyra, stats) {
    var measured = measuredFps(stats)
    return measured > 0 ? measured : chosenFps(veyra)
}

// Puts one mode's settings in place and returns what happened:
//   { mode, plan, multiplier (the one in use after writing), delayMs, outputFps, notes }
// The settings from before the first mode are saved once; moving between modes keeps them.
// Returns null when they could not be saved (nothing is changed then).
function engage(veyra, id, fps) {
    if (!isMode(id)) return null
    var base = current(veyra) !== "" ? saved(veyra) : null
    if (!base) {
        base = snapshot(veyra)
        if (!veyra.setPreference("pushModeRestore", JSON.stringify(base))) return null
    }
    var c = caps(veyra)
    var p = plan(id, fps, c.displayHz)
    var t = target(p, c, base)
    write(veyra, c, t)
    // What the bridge took: a refused multiplier leaves the old one, and the disclosure follows it.
    var on = veyra.fgEnabled === true
    var m = on ? Number(veyra.fgMultiplier || 1) : 1
    var notes = t.notes.slice()
    if (t.fg && (!on || m !== t.mult) && notes.indexOf("fg-unavailable") < 0) notes.push("fg-unavailable")
    var nominal = cadence(fps)
    return { mode: id, plan: p, multiplier: m, delayMs: delayMs(m, fps), outputFps: nominal > 0 ? nominal * m : 0, notes: notes }
}

// The experiment switches the chosen mode asks for (input poll, pair phase; with Automatic, connection health),
// for the next PS5 connection: the bridge sets only those not already in the environment and removes only its
// own (pushCalibration("switches"), engine::pushInterfaceSwitches). Returns { applied, set, unset, kept }, or
// null without the bridge call.
function syncSwitches(veyra) {
    return typeof veyra.pushCalibration === "function" ? (veyra.pushCalibration("switches", {}) || null) : null
}
// true when the last sync changed something that only counts from the next connection.
function switchesPending(result) {
    return !!result && ((result.set || []).length > 0 || (result.unset || []).length > 0)
}

// Chooses a mode (or "" for off) and remembers it. Off puts back what was set before the first mode.
function select(veyra, id, fps) {
    if (!isMode(id)) {
        var before = current(veyra) !== "" ? saved(veyra) : null
        if (before) write(veyra, caps(veyra), before)
        veyra.setPreference("pushModeRestore", "{}")
        veyra.setPreference("pushMode", "")
        veyra.setPreference("pushModeSwitches", false)
        veyra.logUi("push-mode", "mode=off restored=" + (before !== null))
        return { mode: "", plan: null, multiplier: 0, delayMs: 0, outputFps: 0, notes: [], switches: syncSwitches(veyra) }
    }
    var result = engage(veyra, id, fps)
    if (!result) return null
    veyra.setPreference("pushMode", id)
    // Chosen with this version: its experiment switches are wanted (a mode chosen before keeps working
    // without them until it is chosen again; the bridge reads this flag).
    veyra.setPreference("pushModeSwitches", true)
    veyra.logUi("push-mode", "mode=" + id + " sourceFps=" + Number(fps).toFixed(2) + " reason=" + result.plan.reason
                + " multiplier=" + result.multiplier + " minAddedDelayMs=" + result.delayMs.toFixed(2)
                + " notes=" + (result.notes.length ? result.notes.join(",") : "none"))
    result.switches = syncSwitches(veyra)
    return result
}

// What a mode would do now, without changing anything (the row's disclosure line).
function preview(veyra, id, fps) {
    if (!isMode(id)) return null
    var c = caps(veyra)
    var p = plan(id, fps, c.displayHz)
    var t = target(p, c, saved(veyra) || snapshot(veyra))
    var nominal = cadence(fps)
    return { mode: id, plan: p, multiplier: t.mult, delayMs: delayMs(t.mult, fps), outputFps: nominal > 0 ? nominal * t.mult : 0, notes: t.notes }
}

// --- first-launch recommendation (PushCalibrationPanel.qml; rules in engine/PushCalibrationPlan.h) -----
//
// The plan comes from the bridge (pushCalibration("plan"), C++, tested in CalibrationPlanTests.inc). These
// functions only put it in place when the user presses Apply, and take it back with Undo:
//   - frame generation method (and PushFlow's lowest tier) when the plan generates,
//   - AI picture enhancement and super resolution off,
//   - the PS5 stream codec (H.264 / H.265 / H.265 HDR; read at the next connection),
//   - the performance mode, through select() above (which keeps its own pushModeRestore).
// What was there before and what Apply left are kept in the preference pushCalibrationApplied. Undo puts a
// setting back only while it still holds what Apply left: a change the user made afterwards stays. Nothing
// is ever applied again by itself. Unverified on Windows.

// The game's rate for the plan: measured when the table has a row for it, else the quick setup's game frame
// rate when the user chose 30 or 60 there, else 0 (unknown: the plan then never generates).
function calibrationGame(veyra, stats) {
    var measured = cadence(measuredFps(stats))
    if (measured > 0) return measured
    var chosen = Number((veyra.preferences || {}).pushGameFps)
    return chosen === 30 || chosen === 60 ? chosen : 0
}

// Another measurement or a calibration is in progress: no calibration starts and Apply does nothing, so
// one run's restore point is never overwritten with settings another one changed.
function measurementBusy(veyra) {
    if (calibrationRunning(veyra)) return true
    var optimizing = String((veyra.preferences || {}).pushOptimizeRestore || "")
    if (optimizing.length > 0 && optimizing !== "{}") return true
    if (typeof veyra.pushQualityStatus === "function" && (veyra.pushQualityStatus() || {}).running === true) return true
    if (typeof veyra.pushLatencyTestStatus === "function" && (veyra.pushLatencyTestStatus() || {}).running === true) return true
    return false
}

// The PS5 stream codec: 0 H.264, 1 H.265, 2 H.265 HDR; -1 when this build has no PS5 source.
function ps5Codec(veyra) {
    if (!veyra.ps5 || typeof veyra.ps5Set !== "function") return -1
    var c = Number(veyra.ps5.codec || 0)
    return c >= 0 && c <= 2 ? c : 0
}
function planCodec(plan) { return plan.codec === "hevc" ? (plan.hdr === true ? 2 : 1) : 0 }

// The settings Apply may touch, as they are now. io: { codec(), setCodec(n) } (tests), else the bridge.
function calibrationState(veyra, io) {
    return { mode: current(veyra), backend: String(veyra.fgBackendName || ""), tier: Number(veyra.vfgQuality || 0),
             nr: veyra.nrAnyEnabled === true, sr: veyra.srEnabled === true, codec: io ? io.codec() : ps5Codec(veyra),
             fg: veyra.fgEnabled === true, mult: Number(veyra.fgMultiplier || 1) }
}
function writeCodec(veyra, io, n) {
    if (io) io.setCodec(n)
    else if (ps5Codec(veyra) >= 0) veyra.ps5Set("codec", n)
}
function setEnhancement(veyra, on) {
    if (on) { if ((veyra.nrLayers || []).length === 0) veyra.addEffect("nr"); else veyra.setAllNrEnabled(true) }
    else if ((veyra.nrLayers || []).length > 0) veyra.setAllNrEnabled(false)
}

// The list of what Apply would change (and, after Apply, did change): [{ key, before, after }].
// key: backend | tier | nr | sr | codec | mode. Frame generation itself is the mode's (shown with it).
function calibrationChanges(veyra, plan, fps, io) {
    var now = calibrationState(veyra, io)
    var out = []
    if (!plan || plan.valid !== true || !isMode(String(plan.mode || ""))) return out
    if (plan.frameGeneration === true && String(plan.fgMethod || "").length > 0) {
        if (now.backend !== plan.fgMethod) out.push({ key: "backend", before: now.backend, after: String(plan.fgMethod) })
        if (plan.fgMethod === "pushflow" && now.tier !== Number(plan.tier || 0)) out.push({ key: "tier", before: now.tier, after: Number(plan.tier || 0) })
    }
    if (plan.enhancement !== true) {
        if (now.nr) out.push({ key: "nr", before: true, after: false })
        if (now.sr) out.push({ key: "sr", before: true, after: false })
    }
    var codec = planCodec(plan)
    if (now.codec >= 0 && now.codec !== codec) out.push({ key: "codec", before: now.codec, after: codec })
    // A mode already chosen is put in place again for this source (select() saves nothing new then).
    var expectFg = plan.frameGeneration === true ? Number(plan.multiplier) : 1
    if (now.mode !== plan.mode || (now.fg ? now.mult : 1) !== expectFg)
        out.push({ key: "mode", before: now.mode, after: String(plan.mode) })
    return out
}

function appliedRecord(veyra) {
    var r = null
    try { r = JSON.parse(String((veyra.preferences || {}).pushCalibrationApplied || "")) } catch (e) { r = null }
    return r && typeof r === "object" && Array.isArray(r.changes) && r.changes.length > 0 ? r : null
}
function readKey(veyra, io, key) {
    var s = calibrationState(veyra, io)
    return s[key]
}
// The settings a mode writes, field by field, for Undo (written only where the user left Apply's value).
var kModeFields = ["fg", "content", "nvidia", "queue", "display", "vrr", "fps", "rate"]
function writeModeField(veyra, key, before) {
    if (key === "fg") {
        if (before.fg && before.mult >= 2) veyra.fgMultiplier = before.mult
        else if (veyra.fgEnabled === true) veyra.fgEnabled = false
    } else if (key === "content") veyra.contentRate = before.content
    else if (key === "nvidia") { if (veyra.nvidiaLowLatencyAvailable === true) veyra.nvidiaLowLatency = before.nvidia }
    else if (key === "queue") veyra.fgLowQueue = before.queue
    else if (veyra.presentationOwned === true) return
    else if (key === "display") veyra.displaySync = before.display
    else if (key === "vrr") veyra.variableRefresh = before.vrr
    else if (key === "fps") { if (before.fps >= 1) veyra.outputCustomFps = before.fps }
    else if (key === "rate") veyra.outputRateMode = before.rate
}
function modeFieldValue(snap, key) { return key === "fg" ? (snap.fg ? snap.mult : 1) : snap[key] }
function writeKey(veyra, io, key, value, fps) {
    if (key === "backend") { if (String(veyra.fgBackendName) !== value) veyra.fgBackendName = value }
    else if (key === "tier") { if (Number(veyra.vfgQuality) !== value) veyra.vfgQuality = value }
    else if (key === "nr") setEnhancement(veyra, value === true)
    else if (key === "sr") { if ((veyra.srEnabled === true) !== value) veyra.srEnabled = value }
    else if (key === "codec") writeCodec(veyra, io, value)
    else if (key === "mode") select(veyra, value, fps)
}

// Apply: returns the record kept in pushCalibrationApplied ({ v, changes }), or null when nothing could be
// changed (no plan, an empty capability report, or the settings from before could not be saved).
function applyCalibration(veyra, plan, fps, io) {
    var changes = calibrationChanges(veyra, plan, fps, io)
    if (changes.length === 0) return null
    if (measurementBusy(veyra)) return null
    // The mode's own settings and preferences as they are, so Undo can put them back value by value.
    var modeBefore = { settings: snapshot(veyra), mode: current(veyra), restore: String((veyra.preferences || {}).pushModeRestore || ""),
                       switches: (veyra.preferences || {}).pushModeSwitches === true }
    // Saved before anything changes, so a crash halfway still leaves an Undo.
    if (!veyra.setPreference("pushCalibrationApplied", JSON.stringify({ v: 2, changes: changes, modeBefore: modeBefore }))) return null
    // The method before the mode: the mode's multiplier is checked against the method's choices.
    var order = ["backend", "tier", "nr", "sr", "codec", "mode"]
    for (var i = 0; i < order.length; ++i)
        for (var j = 0; j < changes.length; ++j)
            if (changes[j].key === order[i]) writeKey(veyra, io, changes[j].key, changes[j].after, fps)
    // What the bridge took is what Undo compares with.
    for (var k = 0; k < changes.length; ++k) changes[k].after = readKey(veyra, io, changes[k].key)
    var record = { v: 2, changes: changes, mode: String(plan.mode), multiplier: Number(plan.multiplier || 1),
                   modeBefore: modeBefore, modeAfter: snapshot(veyra) }
    veyra.setPreference("pushCalibrationApplied", JSON.stringify(record))
    veyra.logUi("push-calibration", "applied " + changes.map(function (c) { return c.key + "=" + c.after }).join(" "))
    return record
}

// Undo: each setting back while it still holds what Apply left; a setting changed since then stays.
// Returns the keys put back.
function undoCalibration(veyra, fps, io) {
    var record = appliedRecord(veyra)
    if (!record) return []
    var back = []
    var order = ["backend", "tier", "mode", "nr", "sr", "codec"]
    for (var i = 0; i < order.length; ++i)
        for (var j = 0; j < record.changes.length; ++j) {
            var c = record.changes[j]
            if (c.key !== order[i]) continue
            if (JSON.stringify(readKey(veyra, io, c.key)) !== JSON.stringify(c.after)) continue
            if (c.key === "mode" && record.modeBefore && record.modeAfter) {
                // Value by value, never through select(): a setting changed by hand since Apply stays.
                var now = snapshot(veyra)
                for (var f = 0; f < kModeFields.length; ++f) {
                    var k = kModeFields[f]
                    if (JSON.stringify(modeFieldValue(now, k)) === JSON.stringify(modeFieldValue(record.modeAfter, k))
                        && JSON.stringify(modeFieldValue(record.modeBefore.settings, k)) !== JSON.stringify(modeFieldValue(now, k)))
                        writeModeField(veyra, k, record.modeBefore.settings)
                }
                veyra.setPreference("pushModeRestore", record.modeBefore.restore.length > 0 ? record.modeBefore.restore : "{}")
                veyra.setPreference("pushMode", record.modeBefore.mode)
                veyra.setPreference("pushModeSwitches", record.modeBefore.switches === true)
                syncSwitches(veyra)
            } else writeKey(veyra, io, c.key, c.before, fps)
            back.push(c.key)
        }
    veyra.setPreference("pushCalibrationApplied", "{}")
    veyra.logUi("push-calibration", "undo restored=[" + back.join(",") + "]")
    return back
}

// The calibration's own restore point (pushCalibrationRestore), the optimiser's pattern: written before the
// calibration changes anything, cleared when it ends, and put back at the next start if PushFPS stopped
// halfway (PushModeDriver.qml, Component.onCompleted).
function calibrationSnapshot(veyra) {
    return { fg: veyra.fgEnabled === true, mult: Number(veyra.fgMultiplier || 1), backend: String(veyra.fgBackendName || ""),
             tier: Number(veyra.vfgQuality || 0), nr: veyra.nrAnyEnabled === true, sr: veyra.srEnabled === true,
             rate: Number(veyra.outputRateMode || 0),
             layers: (veyra.nrLayers || []).map(function (l) { return { index: Number(l.index), enabled: l.enabled === true } }) }
}
// Each enhancement layer back as it was when the layers are the same ones; otherwise the master switch.
function restoreLayers(veyra, s) {
    var now = veyra.nrLayers || []
    var same = Array.isArray(s.layers) && s.layers.length === now.length && typeof veyra.setEffectEnabled === "function"
               && s.layers.every(function (l, i) { return Number(now[i].index) === l.index })
    if (!same) { if ((veyra.nrAnyEnabled === true) !== s.nr) setEnhancement(veyra, s.nr === true); return }
    for (var i = 0; i < s.layers.length; ++i)
        if ((now[i].enabled === true) !== s.layers[i].enabled) veyra.setEffectEnabled(s.layers[i].index, s.layers[i].enabled)
}
function calibrationRunning(veyra) {
    var text = String((veyra.preferences || {}).pushCalibrationRestore || "")
    return text.length > 0 && text !== "{}"
}
function calibrationRestore(veyra) {
    var s = null
    try { s = JSON.parse(String((veyra.preferences || {}).pushCalibrationRestore || "")) } catch (e) { s = null }
    if (s && typeof s === "object" && typeof s.fg === "boolean") {
        if (s.backend && String(veyra.fgBackendName) !== s.backend) veyra.fgBackendName = s.backend
        if (Number(veyra.vfgQuality) !== Number(s.tier)) veyra.vfgQuality = Number(s.tier)
        // The multiplier too when generation was off (setting it turns generation on, so off comes after).
        if (Number(s.mult) >= 2 && Number(veyra.fgMultiplier) !== Number(s.mult)) veyra.fgMultiplier = Number(s.mult)
        if (!s.fg && veyra.fgEnabled === true) veyra.fgEnabled = false
        restoreLayers(veyra, s)
        if ((veyra.srEnabled === true) !== s.sr) veyra.srEnabled = s.sr === true
        if (veyra.presentationOwned !== true && Number(veyra.outputRateMode || 0) !== Number(s.rate)) veyra.outputRateMode = Number(s.rate)
    }
    // "{}" (as pushOptimizeRestore) exactly while no calibration runs; other watchers pause while it is anything else.
    veyra.setPreference("pushCalibrationRestore", "{}")
    return s !== null
}

// --- first-launch card: new installs only -----------------------------------------------------------------
// The card (PushWelcomeCard.qml) is for a first run. When it is first created in a version that has this rule
// (preference pushWelcomeChecked absent), a PushFPS preference ("push..." key) already there means PushFPS ran
// before: the card is marked done without being shown, and "Recommended settings" stays reachable from the
// mode row. Keys PushFPS writes by itself on any first start do not count.
var kAutomaticPushKeys = ["pushSmoothPacing", "pushSmoothPacing2", "pushWelcomeChecked", "pushWelcomeDone"]
function existingUser(prefs) {
    var keys = Object.keys(prefs || {})
    for (var i = 0; i < keys.length; ++i) {
        var k = keys[i]
        if (k.indexOf("push") !== 0 || kAutomaticPushKeys.indexOf(k) >= 0 || k.indexOf("pushCalibration") === 0) continue
        return true
    }
    return false
}
// Once per install: returns true when the card was turned off because PushFPS ran before.
function settleWelcome(veyra) {
    var prefs = veyra.preferences || {}
    if (prefs.pushWelcomeChecked === true) return false
    var existing = existingUser(prefs)
    veyra.setPreference("pushWelcomeChecked", true)
    if (existing && prefs.pushWelcomeDone !== true) {
        veyra.setPreference("pushWelcomeDone", true)
        veyra.logUi("push-welcome", "earlier PushFPS preferences found: first-launch card not shown")
        return true
    }
    return false
}
