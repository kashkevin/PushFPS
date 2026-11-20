// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Picture profiles for the quick setup (PushProfileRow.qml, object pushfps-profile).
//
// defaults() is the interface's copy of pushProfileDefaults() in include/veyra/engine/PushPerformanceMode.h,
// and multiplier() of pushProfileMultiplier(). Both test suites check the same rows
// (tests/remoteplay/PerformanceModeTests.inc and tests/qml/quick/tst_push_profiles.qml): change the
// header, this file and both tests together.
//
//   Profile             clean-up   upscaler            neural layer                 frame generation
//   none                (the user's own settings: nothing is written; the default)
//   original            off        off                 off                          off
//   clean-1440          50         DLSS SR M -> 1440   off                          target 120
//   enhanced-1440       50         DLSS SR M -> 1440   on, conservative, 1080 lines target 120
//   max-fidelity        off        DLSS SR M -> 4K     off                          target 120 (the owner's reference)
//   ultra-low-latency   off        off                 off                          kept as the user had it;
//                                                                                   presentation of the Ultra Low
//                                                                                   Latency performance mode
//
// A profile only writes settings the interface already has, through the same bridge properties as the rest
// of the quick setup, and only the ones that differ (so moving between profiles rebuilds no more than it
// must and never stops what plays). Frame generation keeps the user's own method and never turns duplicate
// removal on; the multiplier is the largest at or under the target and the display. While a performance
// mode is chosen (PushModeRow.qml) the mode owns frame generation and presentation: a profile then leaves
// both alone and says so.
//
// Hardware (decided by the bridge's capability answers, never by a card or vendor name): where DLSS cannot
// run, a profile's upscaler becomes the app's vendor-neutral one (upscalerFor(): FSR, then NIS); where the
// neural layer's runtime is missing or the card cannot run it, that stage is dropped. Both are reported with
// the bridge's own reason, so the row says what the profile does instead.
//
// Which stages run is decided by the pipeline planner: the bridge's pushPipelinePlan(request) when this
// build has it (include/veyra/engine/PushPipelinePlan.h, through src/ui/bridge/PushNeural.cpp), else
// mirror() below, a copy of its rules held to the same test rows. A stage the planner drops (its runtime
// is missing, it would scale 1:1, ...) is not turned on, and the row says which and why.
//
// Preferences: pushProfile ("none" | an id), pushProfileSettings (JSON: one member per id holding only what
// the user changed in that profile), pushProfileRestore (JSON: the settings from before the first profile,
// put back by "none"). Nothing here runs for a user who never picks a profile. Unverified on Windows.
.pragma library

var ids = ["original", "clean-1440", "enhanced-1440", "max-fidelity", "ultra-low-latency"]
// The restoration clean-up strength the 1440 profiles start with: kPushProfileCleanStrength in the header.
var kCleanStrength = 50
var kTargetFps = 120
var kExperimentalTargetFps = 180
var kMaxMultiplier = 4
var kDisplayFit = 1.02
var looks = ["conservative", "subtle", "balanced", "strong"]
var fgChoices = ["off", "keep", "120", "180"]
var upscalers = ["off", "dlss", "fsr", "nis"]
// videoSrQuality (EnhancementSettings.h): 0 DLSS SR, 5 FSR (kVideoSrFsr), 6 NVIDIA Image Scaling (kVideoSrNis).
var kSrQuality = { dlss: 0, fsr: 5, nis: 6 }
var nrSizes = ["480", "720", "900", "1080", "1440", "native"]
var srTargets = ["qhd", "4k"]
// pipeline::NrSizePolicy (include/veyra/pipeline/ResolutionPlan.h): Realtime = 1080 lines.
var kNrPolicy = { "480": 2, "720": 3, "900": 4, "1080": 0, "1440": 5, "native": 1 }
// The neural layer's looks as tone / structure of the enhancement model: the values of kPushNrLooks in
// include/veyra/engine/PushNrRuntime.h (the nrrun stream's table; subtle, balanced and strong are the quick
// setup's own looks). Used only while the bridge has no pushSetNeuralLook.
var kLookValues = { conservative: { tone: 0.25, structure: 0.3 }, subtle: { tone: 0.4, structure: 0.5 },
                    balanced: { tone: 0.7, structure: 0.8 }, strong: { tone: 1, structure: 1 } }

function isProfile(id) { return ids.indexOf(String(id)) >= 0 }

// pushProfileDefaults(): what a profile asks for before the user changed anything in it.
function defaults(id) {
    var d = { valid: false, profile: isProfile(id) ? String(id) : "none", clean: 0, upscaler: "off", srModel: 1,
              srTarget: "none", supersample: false, neural: false, look: "conservative", nrSize: "1080",
              fg: "off", lowLatency: false, experimental: false }
    switch (d.profile) {
    case "original": break
    case "clean-1440":
        d.clean = kCleanStrength; d.upscaler = "dlss"; d.srTarget = "qhd"; d.fg = "120"; break
    case "enhanced-1440":
        d.clean = kCleanStrength; d.upscaler = "dlss"; d.srTarget = "qhd"; d.neural = true; d.look = "conservative"
        d.nrSize = "1080"; d.fg = "120"; d.experimental = true; break
    case "max-fidelity":
        d.upscaler = "dlss"; d.srTarget = "4k"; d.supersample = true; d.fg = "120"; break
    case "ultra-low-latency":
        d.fg = "keep"; d.lowLatency = true; break
    default: return d
    }
    d.valid = true
    return d
}

// pushProfileMultiplier(): the largest whole M up to kMaxMultiplier with source x M at or under the target
// and at or under the display (2 % fit). Unusable input: 1.
function multiplier(sourceFps, displayHz, targetFps, maxMultiplier) {
    var fps = Number(sourceFps), hz = Number(displayHz), target = Number(targetFps)
    var top = maxMultiplier === undefined ? kMaxMultiplier : Number(maxMultiplier)
    if (!isFinite(fps) || !isFinite(hz) || !isFinite(target) || fps <= 0 || hz <= 0 || target <= 0) return 1
    var best = 1
    for (var m = 2; m <= top; ++m) {
        var out = fps * m
        if (out <= target * kDisplayFit && out <= hz * kDisplayFit) best = m
    }
    return best
}
function targetFps(fg) { return fg === "180" ? kExperimentalTargetFps : fg === "120" ? kTargetFps : 0 }

// pushProfileUpscalerFor(): the wanted upscaler when it can run here; else a vendor-neutral one, FSR first
// (what the app itself starts with on cards without DLSS), then NIS; else "off" (the stage is dropped).
// DLSS is never chosen for a user who asked for FSR or NIS. c: { dlssSr, fsr, nis } (caps()).
function upscalerFor(wanted, c) {
    wanted = String(wanted)
    if (wanted !== "dlss" && wanted !== "fsr" && wanted !== "nis") return "off"
    if (wanted === "dlss" && c.dlssSr === true) return wanted
    if (wanted === "nis" && c.nis === true) return wanted
    if (c.fsr === true) return "fsr"
    if (c.nis === true) return "nis"
    return "off"
}

// --- the user's own changes, per profile ---------------------------------------------------------------
// A value the user may change in a profile, checked; undefined when it is not one.
function clean(key, value) {
    switch (key) {
    case "clean": { var n = Number(value); return isFinite(n) && n >= 0 && n <= 100 ? Math.round(n) : undefined }
    case "upscaler": return upscalers.indexOf(String(value)) >= 0 ? String(value) : undefined
    case "srTarget": return srTargets.indexOf(String(value)) >= 0 ? String(value) : undefined
    case "neural": return typeof value === "boolean" ? value : undefined
    case "look": return looks.indexOf(String(value)) >= 0 ? String(value) : undefined
    case "nrSize": return nrSizes.indexOf(String(value)) >= 0 ? String(value) : undefined
    case "fg": return fgChoices.indexOf(String(value)) >= 0 ? String(value) : undefined
    }
    return undefined
}
var kCustomKeys = ["clean", "upscaler", "srTarget", "neural", "look", "nrSize", "fg"]

function customAll(veyra) {
    var c = null
    try { c = JSON.parse(String((veyra.preferences || {}).pushProfileSettings || "{}")) } catch (e) { c = null }
    return c && typeof c === "object" && !Array.isArray(c) ? c : {}
}
// Only the recognised, valid values the user changed in this profile.
function custom(veyra, id) {
    var all = customAll(veyra)
    var own = all[id]
    var out = {}
    if (!own || typeof own !== "object") return out
    for (var i = 0; i < kCustomKeys.length; ++i) {
        var k = kCustomKeys[i]
        if (own[k] === undefined) continue
        var v = clean(k, own[k])
        if (v !== undefined) out[k] = v
    }
    return out
}
// The profile as the user has it: its defaults with his own changes on top.
function settings(veyra, id) {
    var s = defaults(id)
    if (!s.valid) return s
    var own = custom(veyra, id)
    for (var k in own) s[k] = own[k]
    if (s.upscaler === "off") s.srTarget = "none"
    else if (s.srTarget === "none") s.srTarget = "qhd"
    // Supersampling is the owner's 4K reference; a target the user chose follows the display.
    s.supersample = s.upscaler !== "off" && s.srTarget === "4k" && (own.srTarget === undefined ? defaults(id).supersample : true)
    s.experimental = s.neural || s.fg === "180"
    s.customised = Object.keys(own).length > 0
    return s
}

// --- which profile, and the settings from before ----------------------------------------------------------
function current(veyra) {
    var id = String((veyra.preferences || {}).pushProfile || "none")
    return isProfile(id) ? id : "none"
}
// The settings a profile can change, as they are now.
function snapshot(veyra) {
    var layers = (veyra.nrLayers || []).map(function (l) {
        return { index: Number(l.index), enabled: l.enabled !== false, tone: l.tone === undefined ? null : Number(l.tone),
                 structure: l.structure === undefined ? null : Number(l.structure),
                 sizePolicy: l.sizePolicy === undefined ? null : Number(l.sizePolicy) }
    })
    return { v: 1, sr: veyra.srEnabled === true, srQuality: Number(veyra.videoSrQuality || 0),
             srModel: veyra.dlssSrModel === undefined ? null : Number(veyra.dlssSrModel),
             srTarget: Number(veyra.srTargetIndex || 0), dualSr: veyra.dualSrEnabled === true,
             clean: Number((veyra.preferences || {}).pushClean || 0),
             nr: veyra.nrAnyEnabled === true, layers: layers,
             fg: veyra.fgEnabled === true, mult: Number(veyra.fgMultiplier || 1),
             rate: Number(veyra.outputRateMode || 0), fps: Number(veyra.outputCustomFps || 0),
             nvidia: Number(veyra.nvidiaLowLatency || 0), queue: veyra.fgLowQueue === true,
             display: Number(veyra.displaySync), vrr: veyra.variableRefresh === true }
}
function saved(veyra) {
    var s = null
    try { s = JSON.parse(String((veyra.preferences || {}).pushProfileRestore || "")) } catch (e) { s = null }
    if (!s || typeof s !== "object" || s.v !== 1 || typeof s.fg !== "boolean" || typeof s.sr !== "boolean") return null
    if (!Array.isArray(s.layers)) s.layers = []
    return s
}

// --- the environment a plan is made for -----------------------------------------------------------------
// display: { width, height } of the screen in pixels (the row reads Screen), or nothing.
function environment(veyra, stats, display) {
    stats = stats || {}
    var prefs = veyra.preferences || {}
    var running = stats.running === true
    var transport = running && Number(stats.captureFps) > 0 ? Number(stats.captureFps)
                  : Number(veyra.sourceFps) > 0 ? Number(veyra.sourceFps) : 60
    var removal = Number(veyra.contentRate || 0) !== 0
    var game = Number(prefs.pushGameFps || 30)
    game = game === 40 || game === 60 ? game : 30
    if (running && Number(stats.contentFps) > 0) game = Number(stats.contentFps)
    var fgStatus = typeof veyra.effectAvailability === "function" ? veyra.effectAvailability("frame-generation") : { available: true }
    var offered = (veyra.fgMultiplierChoices || []).map(function (o) { return parseInt(o.id) })
        .filter(function (m) { return m >= 2 && m <= kMaxMultiplier }).sort(function (a, b) { return a - b })
    var w = Number(stats.sourceWidth || veyra.sourceWidth || 0), h = Number(stats.sourceHeight || veyra.sourceHeight || 0)
    return {
        streamWidth: w > 0 && h > 0 ? w : 1920, streamHeight: w > 0 && h > 0 ? h : 1080, streamKnown: w > 0 && h > 0,
        streamFps: transport,
        // What the generator multiplies: the game's rate only when the user has duplicate removal on.
        sourceFps: removal ? game : transport, removal: removal,
        displayWidth: display && Number(display.width) > 0 ? Number(display.width) : 0,
        displayHeight: display && Number(display.height) > 0 ? Number(display.height) : 0,
        displayHz: Number(veyra.displayRefreshHz || 0), dualSr: veyra.dualSrEnabled === true,
        hdr: veyra.videoHdr === true || stats.hdr === true,
        mode: String(prefs.pushMode || ""),
        fgPossible: !!(fgStatus && fgStatus.available !== false) && offered.length > 0, multipliers: offered,
        nvidia: veyra.nvidiaLowLatencyAvailable === true, vrrState: Number(veyra.vrrState || 0),
        presentationOwned: veyra.presentationOwned === true
    }
}
function availability(veyra, id) {
    if (typeof veyra.effectAvailability !== "function") return { available: true, reason: "" }
    var s = veyra.effectAvailability(id) || {}
    return { available: s.available !== false, reason: String(s.reason || "") }
}
// Capabilities as the planner takes them (PipelineCaps), read from the bridge: never a card name.
function caps(veyra) {
    var c = veyra.effectCapabilities   // reading the notifying property keeps bindings current (EffectSupport.js)
    var fg = availability(veyra, "frame-generation")
    var nr = availability(veyra, "nr")
    var dlss = availability(veyra, "sr0")
    // The neural layer: the capability of the runtime in use (Hardware.cpp: the file is there and this card
    // can run it), with the bridge's own reason when it is not.
    return { dlssSr: dlss.available, dlssReason: dlss.reason, vsr: availability(veyra, "sr4").available,
             nis: availability(veyra, "sr6").available, fsr: availability(veyra, "sr5").available,
             nrRuntime: nr.available, nrReason: nr.reason, nrMaxHeight: 1440,
             vfg: fg.available, dlssFg: fg.available, pushFlow: fg.available, fsrFg: fg.available,
             clean: true, gpuBudgetMs: -1, cleanMs: -1, nrMs: -1, srMs: -1, fgMs: -1, vramFreeMiB: -1, nrVramMiB: -1 }
}
// The user's frame generation method by the planner's names.
function fgName(veyra) {
    var n = String(veyra.fgBackendName || "").toLowerCase()
    if (n.indexOf("fsr") >= 0) return "fsr"
    if (n === "vfg" || n === "dlss" || n === "pushflow") return n
    return "vfg"
}

// The planner request (S5_PIPELINE_API.md, "JavaScript-facing shape") for a profile's settings on this PC.
// c: caps(). notes (machine-readable, the row turns each into a sentence) are returned in request.notes:
//   sr-fallback-fsr | sr-fallback-nis   DLSS (or the upscaler the user chose) cannot run here: the vendor-neutral one instead
//   sr-unavailable                      no upscaler at all can run here: the stage is dropped
//   display-unknown                     the screen's size is not known: planned as if it were the profile's target
//   display-rate-unknown                the refresh rate is not known: planned for 60 Hz (never more generation than that)
function request(s, env, veyra, c) {
    var notes = []
    var sr = s.upscaler === "off" ? "off" : upscalerFor(s.upscaler, c)
    if (sr !== "off" && sr !== s.upscaler) notes.push("sr-fallback-" + sr)
    else if (sr === "off" && s.upscaler !== "off") notes.push("sr-unavailable")
    // The stream rate the planner multiplies is what the generator multiplies here: the game's own rate when
    // the user has duplicate removal on, else the stream's transport rate.
    var target = targetFps(s.fg)
    var m = target > 0 ? multiplier(env.sourceFps, target, target, kMaxMultiplier) : 1
    var box = s.srTarget === "4k" ? { width: 3840, height: 2160 } : { width: 2560, height: 1440 }
    var dw = env.displayWidth, dh = env.displayHeight
    if (!(dw > 0 && dh > 0)) {
        dw = Math.max(box.width, env.streamWidth); dh = Math.max(box.height, env.streamHeight)
        if (sr !== "off") notes.push("display-unknown")
    }
    var hz = env.displayHz
    if (!(hz > 0)) { hz = 60; if (target > 0) notes.push("display-rate-unknown") }
    return { streamWidth: env.streamWidth, streamHeight: env.streamHeight, streamFps: env.sourceFps,
             contentFps: 0, hdr: env.hdr, displayWidth: dw, displayHeight: dh, displayHz: hz,
             clean: s.clean, sr: sr, srModel: s.srModel, srTarget: s.srTarget === "4k" ? "4k" : "qhd",
             supersample: s.supersample === true, sr2: sr !== "off" && env.dualSr === true ? sr : "off",
             nr: s.neural === true, nrLook: s.look, nrSize: s.nrSize, nrPlace: "after",
             fg: target > 0 && env.mode === "" ? fgName(veyra) : "off", fgMultiplier: m,
             fgTarget: target > 0 ? target : kTargetFps, fgExperimental180: s.fg === "180", notes: notes }
}

// --- the planner's rules (a copy of planPipeline() in PushPipelinePlan.h, for builds without it) ------------
// The rules for what a profile can ask for: clean-up, one upscaler (DLSS, FSR or NIS) to 1440p or 4K, the
// neural layer, one frame generation method; a missing runtime replaced by a vendor-neutral one or dropped.
// Held to the header by rows both tests check (tests/remoteplay/PipelinePlanTests.inc and `planRows` in
// tests/qml/quick/tst_push_profiles.qml). Not copied: the rules for measured GPU costs (the interface gives
// the planner no measurements: nothing is assumed) and for stages a profile never asks for (a separate
// sharpening stage, the RTX Video SR families' own clean-up). A request member left out takes
// PipelineRequest's default.
var kUpscalers = ["off", "dlss", "vsr-stream", "nis", "fsr", "vsr", "vsr-high-bitrate"]   // PipeUpscaler in order: an upscale stage's detail
var kFgNames = ["off", "vfg", "dlss", "pushflow", "fsr"]                                    // PipeFg in order
function validExtent(w, h) { return w >= 16 && h >= 16 && w <= 16384 && h <= 16384 }
function validRate(r) { return r > 0 && r <= 1000 }
function even(v) { return Math.floor(v / 2) * 2 }
// `from` fitted into `box` keeping its shape, even sizes, enlarging only.
function enlargeInto(from, box) {
    var scale = Math.min(box.width / from.width, box.height / from.height)
    if (scale <= 1) return { width: from.width, height: from.height }
    return { width: Math.max(2, even(Math.floor(from.width * scale + 1e-6))), height: Math.max(2, even(Math.floor(from.height * scale + 1e-6))) }
}
// The neural layer's own size: a 16:9 box of `lines`, never larger than its input.
function nrInternal(input, lines) {
    if (lines === 0) return input
    var scale = Math.min(1, (lines * 16 / 9) / input.width, lines / input.height)
    if (scale >= 1) return input
    return { width: Math.max(1, even(Math.floor(input.width * scale))), height: Math.max(1, even(Math.floor(input.height * scale))) }
}
function nrSizeForLines(lines) { return lines === 0 ? "native" : lines <= 480 ? "480" : lines <= 720 ? "720" : lines <= 900 ? "900" : lines <= 1080 ? "1080" : "1440" }
function upscalerAvailable(name, c) {
    return name === "dlss" ? c.dlssSr === true : name === "nis" ? c.nis === true : name === "fsr" ? c.fsr === true
         : name === "vsr" || name === "vsr-stream" || name === "vsr-high-bitrate" ? c.vsr === true : false
}
// The vendor-neutral stand-in: the temporal one (FSR) for DLSS, the spatial one (NIS) otherwise; then the other.
function upscalerFallback(name, c) {
    var first = name === "dlss" ? "fsr" : "nis", second = name === "dlss" ? "nis" : "fsr"
    if (first !== name && upscalerAvailable(first, c)) return first
    if (second !== name && upscalerAvailable(second, c)) return second
    return "off"
}
function fgAvailable(name, c) {
    return name === "vfg" ? c.vfg === true : name === "dlss" ? c.dlssFg === true : name === "pushflow" ? c.pushFlow === true
         : name === "fsr" ? c.fsrFg === true : false
}
function fgFallback(name, c) {
    if (name !== "fsr" && c.fsrFg === true) return "fsr"
    if (name !== "pushflow" && c.pushFlow === true) return "pushflow"
    return "off"
}
function fgMaxMultiplier(name) { return name === "vfg" ? 8 : name === "dlss" ? 6 : name === "pushflow" ? 4 : name === "fsr" ? 2 : 1 }
function fgOffers(name, m) { return m >= 2 && m <= fgMaxMultiplier(name) && (name !== "dlss" || m !== 5) }
function mirror(req, c) {
    var r = { stages: [], changes: [], outputWidth: 0, outputHeight: 0, nrSize: "1080", submittedFps: 0, uniqueMotionFpsMax: 0,
              experimental: false, overBudget: false, mirror: true }
    function change(stage, action, reason, from, to) { r.changes.push({ stage: stage, action: action, reason: reason, from: from || 0, to: to || 0 }) }
    function stage(name, from, to, detail) {
        r.stages.push({ name: name, inWidth: from.width, inHeight: from.height, outWidth: to.width, outHeight: to.height, detail: detail || 0 })
    }
    function or(value, fallback) { return value === undefined || value === null ? fallback : value }
    c = c || {}
    var sw = Number(or(req.streamWidth, 1920)), sh = Number(or(req.streamHeight, 1080)), fps = Number(or(req.streamFps, 60))
    var dw = Number(or(req.displayWidth, 2560)), dh = Number(or(req.displayHeight, 1440)), hz = Number(or(req.displayHz, 60))
    var contentFps = Number(or(req.contentFps, 0)), fgTarget = Number(or(req.fgTarget, kTargetFps))
    var stream = { width: sw, height: sh }
    if (!validExtent(sw, sh) || !validExtent(dw, dh) || !validRate(fps) || !validRate(hz) || !(contentFps >= 0 && contentFps <= 1000) || !(fgTarget >= 0)) {
        change("decode", "reduced", "invalid-request")
        stage("decode", stream, stream); stage("present", stream, stream)
        r.outputWidth = sw; r.outputHeight = sh
        return r
    }
    var display = { width: dw, height: dh }
    // 1. The upscaler: its runtime (with the vendor-neutral stand-in), its target, redundancy.
    var up = String(or(req.sr, "off"))
    if (up !== "off" && !upscalerAvailable(up, c)) {
        var standIn = upscalerFallback(up, c)
        if (standIn === "off") change("upscale", "dropped", "runtime-missing")
        else change("upscale", "replaced", "fallback-vendor-neutral", kUpscalers.indexOf(up), kUpscalers.indexOf(standIn))
        up = standIn
    }
    var srOut = stream
    if (up !== "off") {
        var box = req.srTarget === "qhd" ? { width: 2560, height: 1440 } : req.srTarget === "4k" ? { width: 3840, height: 2160 } : display
        var out = enlargeInto(stream, box)
        if ((out.width > display.width || out.height > display.height) && req.supersample !== true) {
            var reduced = enlargeInto(stream, display)
            change("upscale", "reduced", "above-display", out.height, reduced.height)
            out = reduced
        }
        if (out.width === stream.width && out.height === stream.height) { change("upscale", "dropped", "scale-one", stream.height, out.height); up = "off" }
        else srOut = out
    }
    if (String(or(req.sr2, "off")) !== "off") change("upscale", "dropped", "double-upscaler")
    // 2. Clean-up: on the decoded picture, at the stream's size.
    var strength = Math.max(0, Math.min(100, Math.round(Number(or(req.clean, 0)))))
    // 3. The neural layer: its runtime, video memory, size. Nothing stands in for it.
    var neural = req.nr === true
    if (neural && c.nrRuntime !== true) { change("neural", "dropped", "runtime-missing"); neural = false }
    var vramFree = Number(or(c.vramFreeMiB, -1)), vramNeed = Number(or(c.nrVramMiB, -1))
    if (neural && vramFree >= 0 && vramNeed >= 0 && vramNeed > vramFree) { change("neural", "dropped", "vram-budget", vramNeed, vramFree); neural = false }
    var nrSize = String(or(req.nrSize, "1080"))
    var before = String(or(req.nrPlace, "after")) === "before"
    var lines = 0
    if (neural) {
        var input = before ? stream : srOut
        var nrCap = Number(or(c.nrMaxHeight, 1440))
        lines = nrSize === "native" ? 0 : Number(nrSize) > 0 ? Number(nrSize) : 1080
        if (lines === 0) lines = input.height
        if (nrCap > 0 && lines > nrCap) { change("neural", "reduced", "nr-size-cap", lines, nrCap); lines = nrCap }
        if (lines > input.height) {
            if (nrSize !== "native") change("neural", "reduced", "nr-above-input", lines, input.height)
            lines = input.height
        }
    }
    // 4. Frame generation: its runtime (stand-in), the method's multipliers, the target, the refresh rate.
    var fg = String(or(req.fg, "off"))
    var multiplierNow = 1
    if (fg !== "off" && !fgAvailable(fg, c)) {
        var other = fgFallback(fg, c)
        if (other === "off") change("framegen", "dropped", "runtime-missing")
        else change("framegen", "replaced", "fallback-vendor-neutral", kFgNames.indexOf(fg), kFgNames.indexOf(other))
        fg = other
    }
    if (fg !== "off") {
        var m = Math.max(1, Math.round(Number(or(req.fgMultiplier, 2))))
        var asked = m
        if (m > 1 && !fgOffers(fg, m)) {
            var n = Math.min(m, fgMaxMultiplier(fg))
            while (n > 1 && !fgOffers(fg, n)) --n
            change("framegen", "reduced", "fg-multiplier-unsupported", m, n)
            m = n
        }
        var target = fgTarget > 0 ? fgTarget : kTargetFps
        target = Math.min(target, req.fgExperimental180 === true ? kExperimentalTargetFps : kTargetFps)
        var stepDown = function (limit) {
            var k = m
            while (k > 1 && fps * k > limit * kDisplayFit) {
                --k
                while (k > 1 && !fgOffers(fg, k)) --k
            }
            return k
        }
        var byTarget = stepDown(target)
        if (byTarget < m) { change("framegen", "reduced", "fg-above-target", m, byTarget); m = byTarget }
        var byRefresh = stepDown(hz)
        if (byRefresh < m) { change("framegen", "reduced", "fg-above-refresh", m, byRefresh); m = byRefresh }
        if (m <= 1) { change("framegen", "dropped", "fg-multiplier-one", asked, 1); fg = "off" }
        else multiplierNow = m
    }
    // 5. The stages in order.
    stage("decode", stream, stream)
    if (strength > 0) stage("clean", stream, stream, strength)
    var cur = stream
    function placeNeural(at) {
        var internal = nrInternal(at, lines)
        r.nrSize = nrSize === "native" && internal.width === at.width && internal.height === at.height ? "native" : nrSizeForLines(internal.height)
        stage("neural", at, at, Math.max(0, looks.indexOf(String(or(req.nrLook, "conservative")))))
        r.experimental = true
    }
    if (neural && before) placeNeural(cur)
    if (up !== "off") { stage("upscale", cur, srOut, kUpscalers.indexOf(up)); cur = srOut }
    if (neural && !before) placeNeural(cur)
    r.submittedFps = fps
    var content = contentFps > 0 ? Math.min(contentFps, fps) : fps
    r.uniqueMotionFpsMax = content
    if (fg !== "off") {
        stage("framegen", cur, cur, multiplierNow)
        r.submittedFps = fps * multiplierNow
        r.uniqueMotionFpsMax = Math.min(r.submittedFps, content * multiplierNow)
        if (fps * multiplierNow > kTargetFps * kDisplayFit) r.experimental = true
    }
    stage("present", cur, cur)
    r.outputWidth = cur.width; r.outputHeight = cur.height
    return r
}
// A plan in the shape S5_PIPELINE_API.md gives: named stages from decode to present. Anything else from the
// bridge is not acted on (the copy of the rules is used instead), so a profile never switches stages off
// because an answer could not be read.
function validPlan(p) {
    if (!p || typeof p !== "object" || !Array.isArray(p.stages) || p.stages.length < 2) return false
    for (var i = 0; i < p.stages.length; ++i) {
        var st = p.stages[i]
        if (!st || typeof st.name !== "string" || !(Number(st.outWidth) > 0) || !(Number(st.outHeight) > 0)) return false
    }
    return p.stages[0].name === "decode" && p.stages[p.stages.length - 1].name === "present"
}
// The plan for a profile's settings: the bridge's planner when this build has it, else the mirror.
function plan(veyra, s, env) {
    var c = caps(veyra)
    var req = request(s, env, veyra, c)
    var result = null
    if (typeof veyra.pushPipelinePlan === "function") {
        try { result = veyra.pushPipelinePlan(req) } catch (e) { result = null }
    }
    if (!validPlan(result)) result = mirror(req, c)
    if (!Array.isArray(result.changes)) result.changes = []
    result.request = req
    result.caps = c
    return result
}
function stageOf(p, name) {
    for (var i = 0; i < p.stages.length; ++i) if (p.stages[i].name === name) return p.stages[i]
    return null
}

// --- what a profile writes -------------------------------------------------------------------------------
// The values to put in place for a profile on this PC: from its settings, its plan, and the settings from
// before the first profile (base) for what the profile leaves as the user had it. prev: the settings of the
// profile in place until now (null: none). What a profile does not own is not written at all, so a change
// made by hand in another row stays: frame generation under "keep" (fgWrite false), and the present queue
// and display sync in every profile but Ultra Low Latency (presentation "leave"). Only when the profile
// before did own them are the user's own values from before put back (fgWrite true / presentation "base").
function target(veyra, s, p, env, base, prev) {
    var t = { sr: false, srQuality: base.srQuality, srModel: base.srModel, srTarget: base.srTarget, dualSr: base.dualSr === true, clean: 0,
              nr: false, look: s.look, nrPolicy: null,
              fg: base.fg, mult: base.mult, rate: base.rate, fps: base.fps,
              nvidia: base.nvidia, queue: base.queue, display: base.display, vrr: base.vrr,
              ownsFg: env.mode === "", ownsPresentation: env.mode === "" && !env.presentationOwned,
              fgWrite: s.fg !== "keep" || (!!prev && prev.fg !== "keep"),
              presentation: s.lowLatency === true ? "own" : !!prev && prev.lowLatency === true ? "base" : "leave", notes: [] }
    if (!t.fgWrite) { t.fg = veyra.fgEnabled === true; t.mult = Number(veyra.fgMultiplier || 1) }
    var up = stageOf(p, "upscale")
    if (up) {
        // The family the plan runs: the one asked for, or the stand-in the planner chose.
        var planned = kUpscalers[Number(up.detail)]
        var family = planned !== undefined && kSrQuality[planned] !== undefined ? planned : p.request.sr
        t.sr = true; t.srQuality = kSrQuality[family] !== undefined ? kSrQuality[family] : 0
        // The DLSS model only counts for DLSS (the bridge refuses it elsewhere).
        t.srModel = family === "dlss" ? s.srModel : base.srModel
        t.srTarget = up.outHeight > 1440 ? 2 : 1
        // One upscaler in a row: a second one would be the redundant stage the planner drops.
        t.dualSr = false
    }
    t.notes = (p.request.notes || []).slice()
    var cl = stageOf(p, "clean")
    if (cl) t.clean = Number(cl.detail || s.clean)
    var nr = stageOf(p, "neural")
    if (nr) {
        t.nr = true
        var size = p.nrSize !== undefined && String(p.nrSize).length > 0 ? String(p.nrSize) : s.nrSize
        t.nrPolicy = kNrPolicy[size] !== undefined ? kNrPolicy[size] : (s.nrSize === "native" ? kNrPolicy.native : kNrPolicy["1080"])
    }
    if (!t.ownsFg) t.notes.push("mode-owns-fg")
    else if (!t.fgWrite) { t.rate = Number(veyra.outputRateMode || 0); t.fps = Number(veyra.outputCustomFps || 0) }
    else if (s.fg !== "keep") {
        var g = stageOf(p, "framegen")
        var m = g ? Number(g.detail || 1) : 1
        if (m > 1) {
            // Only what this frame generation method offers, never more than the plan.
            var best = 1
            if (env.fgPossible) for (var i = 0; i < env.multipliers.length; ++i) if (env.multipliers[i] <= m) best = env.multipliers[i]
            if (best < m) t.notes.push(best > 1 ? "backend-limit" : "fg-unavailable")
            m = best
        }
        t.fg = m > 1
        t.mult = m > 1 ? m : 1
        if (!t.fg) { t.rate = base.rate; t.fps = base.fps }
        else {
            // The output held at what the plan makes (the display's own rate when it is that).
            var out = Math.round(env.sourceFps) * m
            if (env.displayHz > 0 && out >= env.displayHz - 0.5) t.rate = 1
            else { t.rate = 2; t.fps = out }
        }
    }
    if (t.ownsPresentation && s.lowLatency) {
        // The Ultra Low Latency performance mode's presentation (PushModes.js target() for that mode): a
        // one-frame queue, and the newest picture at once: variable refresh where the driver reports it
        // possible, otherwise tearing rather than waiting for the refresh.
        if (env.nvidia) t.nvidia = 1
        else t.queue = true
        if (env.vrrState >= 3) { t.display = 1; t.vrr = true }
        else { t.display = 0; t.vrr = base.vrr; t.notes.push("tearing") }
        if (!t.fg) t.rate = 0
    }
    return t
}

function setLook(veyra, name) {
    if (typeof veyra.pushSetNeuralLook === "function") return veyra.pushSetNeuralLook(name) !== false
    var v = kLookValues[name]
    if (!v || typeof veyra.setNrLayerParameter !== "function") return false
    var ok = true
    var layers = veyra.nrLayers || []
    for (var i = 0; i < layers.length; ++i) {
        var l = layers[i]
        if (l.tone === undefined || Math.abs(Number(l.tone) - v.tone) >= 0.005) ok = veyra.setNrLayerParameter(Number(l.index), "tone", v.tone) && ok
        if (l.structure === undefined || Math.abs(Number(l.structure) - v.structure) >= 0.005) ok = veyra.setNrLayerParameter(Number(l.index), "structure", v.structure) && ok
    }
    return ok && layers.length > 0
}
function setNrPolicy(veyra, policy) {
    var layers = veyra.nrLayers || []
    for (var i = 0; i < layers.length; ++i)
        if (layers[i].sizePolicy === undefined || Number(layers[i].sizePolicy) !== policy)
            veyra.setNrLayerParameter(Number(layers[i].index), "sizePolicy", policy)
}
function setNeural(veyra, on) {
    if (on) { if ((veyra.nrLayers || []).length === 0) veyra.addEffect("nr"); else if (veyra.nrAnyEnabled !== true) veyra.setAllNrEnabled(true) }
    else if ((veyra.nrLayers || []).length > 0 && veyra.nrAnyEnabled === true) veyra.setAllNrEnabled(false)
}
function hasUpscaleStage(veyra) { return (veyra.chain || []).some(function (n) { return n && n.type === "sr" }) }
function setUpscalerValues(veyra, t) {
    if (Number(veyra.videoSrQuality) !== t.srQuality) veyra.videoSrQuality = t.srQuality
    // The bridge takes a DLSS model only where DLSS can run; elsewhere it is left alone (no error notice).
    if (t.srQuality === 0 && veyra.dlssSrModel !== undefined && t.srModel !== null && Number(veyra.dlssSrModel) !== t.srModel) veyra.dlssSrModel = t.srModel
    if (t.srTarget >= 1 && Number(veyra.srTargetIndex) !== t.srTarget) veyra.srTargetIndex = t.srTarget
    if (veyra.dualSrEnabled !== undefined && (veyra.dualSrEnabled === true) !== (t.dualSr === true)) veyra.dualSrEnabled = t.dualSr === true
}
function setUpscaler(veyra, t) {
    if (t.sr) {
        // Family, model and target first, so the stage starts as what the profile asks for.
        setUpscalerValues(veyra, t)
        if (veyra.srEnabled !== true) {
            // No super-resolution stage in the chain yet: add one (it takes the settings above).
            if (veyra.chain !== undefined && !hasUpscaleStage(veyra) && typeof veyra.addEffect === "function") veyra.addEffect("sr")
            else veyra.srEnabled = true
        }
    } else {
        if (veyra.srEnabled === true) veyra.srEnabled = false
        // The family, model and target as the user had them (they only count while the stage is on).
        setUpscalerValues(veyra, t)
    }
}
// Writes a target, only what differs. Returns the notes of what the bridge refused.
function write(veyra, t) {
    var notes = []
    var cleanNow = Number((veyra.preferences || {}).pushClean || 0)
    if (cleanNow !== t.clean && !veyra.setPreference("pushClean", t.clean) && t.clean > 0) notes.push("clean-missing")
    setUpscaler(veyra, t)
    if (t.sr && veyra.srEnabled !== true) notes.push("sr-refused")
    setNeural(veyra, t.nr)
    if (t.nr) {
        if (veyra.nrAnyEnabled !== true) notes.push("nr-refused")
        else {
            if (t.nrPolicy !== null) setNrPolicy(veyra, t.nrPolicy)
            if (!setLook(veyra, t.look)) notes.push("look-refused")
        }
    }
    if (t.ownsFg && t.fgWrite) {
        if (t.fg) { if (veyra.fgEnabled !== true || Number(veyra.fgMultiplier) !== t.mult) veyra.fgMultiplier = t.mult }
        else if (veyra.fgEnabled === true) veyra.fgEnabled = false
        if (t.fg && (veyra.fgEnabled !== true || Number(veyra.fgMultiplier) !== t.mult)) notes.push("fg-unavailable")
    }
    if (t.ownsPresentation && t.presentation !== "leave") {
        if (veyra.nvidiaLowLatencyAvailable === true) { if (Number(veyra.nvidiaLowLatency || 0) !== t.nvidia) veyra.nvidiaLowLatency = t.nvidia }
        else if ((veyra.fgLowQueue === true) !== t.queue) veyra.fgLowQueue = t.queue
        if (Number(veyra.displaySync) !== t.display) veyra.displaySync = t.display
        if ((veyra.variableRefresh === true) !== t.vrr) veyra.variableRefresh = t.vrr
    }
    // The output cap goes with frame generation (held at what the plan makes) and with Ultra Low Latency
    // (nothing held when nothing is generated); a profile that owns neither leaves it.
    if (t.ownsFg && veyra.presentationOwned !== true && (t.fgWrite || t.presentation !== "leave")) {
        if (t.fps >= 1 && Math.abs(Number(veyra.outputCustomFps || 0) - t.fps) > 0.001) veyra.outputCustomFps = t.fps
        if (Number(veyra.outputRateMode || 0) !== t.rate) veyra.outputRateMode = t.rate
    }
    return notes
}

// Each enhancement layer back as it was (on/off, look, internal size), when the layers are the same ones.
function restoreLayers(veyra, base) {
    var now = veyra.nrLayers || []
    var same = base.layers.length === now.length && base.layers.every(function (l, i) { return Number(now[i].index) === l.index })
    if (!same) { setNeural(veyra, base.nr === true); return }
    for (var i = 0; i < base.layers.length; ++i) {
        var l = base.layers[i], n = now[i]
        if (l.tone !== null && (n.tone === undefined || Math.abs(Number(n.tone) - l.tone) >= 0.005)) veyra.setNrLayerParameter(l.index, "tone", l.tone)
        if (l.structure !== null && (n.structure === undefined || Math.abs(Number(n.structure) - l.structure) >= 0.005)) veyra.setNrLayerParameter(l.index, "structure", l.structure)
        if (l.sizePolicy !== null && (n.sizePolicy === undefined || Number(n.sizePolicy) !== l.sizePolicy)) veyra.setNrLayerParameter(l.index, "sizePolicy", l.sizePolicy)
    }
    if (typeof veyra.setEffectEnabled === "function") {
        for (var j = 0; j < base.layers.length; ++j)
            if ((now[j].enabled !== false) !== base.layers[j].enabled) veyra.setEffectEnabled(base.layers[j].index, base.layers[j].enabled)
    } else if ((veyra.nrAnyEnabled === true) !== (base.nr === true)) setNeural(veyra, base.nr === true)
}
// Puts the settings from before the first profile back. What a performance mode owns stays with the mode.
function restore(veyra, base) {
    var modeOn = String((veyra.preferences || {}).pushMode || "") !== ""
    var t = { sr: base.sr, srQuality: base.srQuality, srModel: base.srModel, srTarget: base.srTarget, dualSr: base.dualSr === true, clean: base.clean,
              fg: base.fg, mult: base.mult, rate: base.rate, fps: base.fps, nvidia: base.nvidia, queue: base.queue,
              display: base.display, vrr: base.vrr, ownsFg: !modeOn, ownsPresentation: !modeOn && veyra.presentationOwned !== true }
    if (Number((veyra.preferences || {}).pushClean || 0) !== t.clean) veyra.setPreference("pushClean", t.clean)
    setUpscaler(veyra, t)
    restoreLayers(veyra, base)
    if (t.ownsFg) {
        // The multiplier too when generation was off (setting it turns generation on, so off comes after).
        if (t.mult >= 2 && Number(veyra.fgMultiplier) !== t.mult) veyra.fgMultiplier = t.mult
        if (!t.fg && veyra.fgEnabled === true) veyra.fgEnabled = false
    }
    if (t.ownsPresentation) {
        if (veyra.nvidiaLowLatencyAvailable === true) { if (Number(veyra.nvidiaLowLatency || 0) !== t.nvidia) veyra.nvidiaLowLatency = t.nvidia }
        else if ((veyra.fgLowQueue === true) !== t.queue) veyra.fgLowQueue = t.queue
        if (Number(veyra.displaySync) !== t.display) veyra.displaySync = t.display
        if ((veyra.variableRefresh === true) !== t.vrr) veyra.variableRefresh = t.vrr
    }
    if (t.ownsFg && veyra.presentationOwned !== true) {
        if (t.fps >= 1 && Math.abs(Number(veyra.outputCustomFps || 0) - t.fps) > 0.001) veyra.outputCustomFps = t.fps
        if (Number(veyra.outputRateMode || 0) !== t.rate) veyra.outputRateMode = t.rate
    }
}

// What the planner dropped or reduced, and what the writes could not do, as one list of
// { stage, reason, from, to } (the row turns each into a sentence).
function drops(p, notes) {
    var out = (p.changes || []).map(function (c) { return { stage: String(c.stage), action: String(c.action), reason: String(c.reason), from: Number(c.from || 0), to: Number(c.to || 0) } })
    for (var i = 0; i < (notes || []).length; ++i) out.push({ stage: "note", action: "note", reason: notes[i], from: 0, to: 0 })
    return out
}

// What a profile would do now, without changing anything: { profile, settings, plan, target, drops }.
function preview(veyra, id, env) {
    if (!isProfile(id)) return null
    var s = settings(veyra, id)
    var p = plan(veyra, s, env)
    var now = current(veyra)
    var base = (now !== "none" ? saved(veyra) : null) || snapshot(veyra)
    var t = target(veyra, s, p, env, base, now !== "none" ? settings(veyra, now) : null)
    return { profile: id, settings: s, plan: p, target: t, drops: drops(p, t.notes) }
}

// The profile select() has just put in place, until PushModeDriver.qml takes it ("" = nothing waiting). One
// value for the whole interface (.pragma library): by it the driver tells a profile chosen in the row, which
// is already in place when the preference changes, from a preference that was set elsewhere.
var applied = ""
function takeApplied() { var id = applied; applied = ""; return id }

// Puts a profile in place (or "none": the settings from before the first profile back) and remembers the
// choice. Returns { profile, settings, plan, target, drops }, or null when the settings from before could
// not be saved (nothing is changed then).
// prev: only from customise() and adopt() (the settings of the profile in place before the change).
// was: only from adopt() (the profile in place before, when the preference already names the new one).
function select(veyra, id, env, prev, was) {
    var now = was !== undefined ? was : current(veyra)
    if (!isProfile(id)) {
        var before = now !== "none" ? saved(veyra) : null
        if (before) restore(veyra, before)
        veyra.setPreference("pushProfileRestore", "{}")
        applied = "none"
        veyra.setPreference("pushProfile", "none")
        veyra.logUi("push-profile", "profile=none restored=" + (before !== null))
        return { profile: "none", settings: null, plan: null, target: null, drops: [] }
    }
    var base = now !== "none" ? saved(veyra) : null
    if (!base) {
        base = snapshot(veyra)
        if (!veyra.setPreference("pushProfileRestore", JSON.stringify(base))) return null
    }
    if (prev === undefined) prev = now !== "none" ? settings(veyra, now) : null
    var s = settings(veyra, id)
    var p = plan(veyra, s, env)
    var t = target(veyra, s, p, env, base, prev)
    var notes = t.notes.concat(write(veyra, t))
    applied = id
    veyra.setPreference("pushProfile", id)
    var list = drops(p, notes)
    veyra.logUi("push-profile", "profile=" + id + " planner=" + (p.mirror === true ? "mirror" : "bridge")
                + " stages=" + p.stages.map(function (st) { return st.name }).join(",")
                + " clean=" + t.clean + " sr=" + (t.sr ? t.srTarget : 0) + " nr=" + (t.nr ? 1 : 0)
                + " fg=" + (t.ownsFg ? (t.fg ? t.mult : 0) : "mode") + " customised=" + (s.customised ? 1 : 0)
                + " changes=" + (list.length ? list.map(function (d) { return d.stage + ":" + d.reason }).join(",") : "none"))
    return { profile: id, settings: s, plan: p, target: t, drops: list }
}

// Puts in place a profile whose preference was set outside the row (the test kit's VEYRA_TEST_RP_PIPELINE
// "profile=" key writes pushProfile). was: the profile in place before that. Returns what select() returns.
function adopt(veyra, id, env, was) {
    was = isProfile(was) ? String(was) : "none"
    return select(veyra, id, env, was !== "none" ? settings(veyra, was) : null, was)
}

// Changes one value in a profile: remembered for that profile only (only what differs from its defaults is
// kept), and applied at once when it is the profile in use. Returns the select() result, true when the
// profile is not in use, or null when the value was refused.
function customise(veyra, id, key, value, env) {
    if (!isProfile(id)) return null
    var v = clean(key, value)
    if (v === undefined) return null
    var all = customAll(veyra)
    var own = all[id] && typeof all[id] === "object" ? all[id] : {}
    if (JSON.stringify(defaults(id)[key]) === JSON.stringify(v)) delete own[key]
    else own[key] = v
    if (Object.keys(own).length > 0) all[id] = own
    else delete all[id]
    var before = current(veyra) === id ? settings(veyra, id) : null
    if (!veyra.setPreference("pushProfileSettings", JSON.stringify(all))) return null
    return before ? select(veyra, id, env, before) : true
}
// "Reset this profile": its defaults again (applied at once when it is in use).
function reset(veyra, id, env) {
    if (!isProfile(id)) return null
    var all = customAll(veyra)
    delete all[id]
    var before = current(veyra) === id ? settings(veyra, id) : null
    if (!veyra.setPreference("pushProfileSettings", JSON.stringify(all))) return null
    return before ? select(veyra, id, env, before) : true
}
