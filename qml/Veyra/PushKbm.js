// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Mouse & Keyboard page helpers (PushKbmPage.qml, PushKbmBindings.qml, PushKbmWizard.qml). Pure: no strings for the
// user, no state. Tested in tst_push_kbm.qml.
//   call()      a bridge function that may not exist in this build (the input core is another stream's work):
//               every call from the three files goes through it, so a missing function gives the fallback value;
//   curve()     the response curve as the core computes it (remoteplay/KbmInputProfile.h kbmCurve), for the plot
//               when the bridge has no pushKbmCurvePoints;
//   keyName()   a Qt key or mouse button as the profile file names it (KeyboardPadBindings::keyCode).
// Nothing here aims, repeats or times an action: the functions describe settings and name keys.
.pragma library

var profileIds = ["legacy", "ultra-low-latency", "adaptive-precision", "custom"]
var contextIds = ["walk", "aim", "sprint", "drive"]
var overrideIds = ["auto", "walk", "aim", "sprint", "drive"]
var curveIds = ["linear", "power", "precision-speed"]

function has(bridge, name) {
    return bridge !== null && bridge !== undefined && typeof bridge[name] === "function"
}
function call(bridge, name, args, fallback) {
    if (!has(bridge, name)) return fallback
    const result = bridge[name].apply(bridge, args || [])
    return result === undefined || result === null ? fallback : result
}

function clamp(v, lo, hi) {
    const n = Number(v)
    return isFinite(n) ? Math.max(lo, Math.min(hi, n)) : lo
}

// Share of full stick travel (above the dead zone) for a share u of the full mouse speed. Monotonic, 0 -> 0, 1 -> 1.
function curve(c, u) {
    if (!(u > 0)) return 0
    if (u >= 1) return 1
    const kind = c ? String(c.curve || "linear") : "linear"
    if (kind === "power") return Math.pow(u, clamp(c.exponent === undefined ? 1 : c.exponent, 0.25, 4))
    if (kind === "precision-speed") {
        const kneeIn = clamp(c.kneeIn === undefined ? 0.25 : c.kneeIn, 0.05, 0.95)
        const kneeOut = clamp(c.kneeOut === undefined ? 0.15 : c.kneeOut, 0.01, 0.99)
        return u < kneeIn ? u * kneeOut / kneeIn : kneeOut + (u - kneeIn) * (1 - kneeOut) / (1 - kneeIn)
    }
    return u
}
// [{u, out}] for the plot: the bridge's points when it has them, else the mirror above.
function curvePoints(bridge, c, count) {
    const n = Math.max(2, Math.min(256, Math.round(Number(count) || 0)))
    const fromBridge = call(bridge, "pushKbmCurvePoints", [c || ({}), n], [])
    if (fromBridge && fromBridge.length >= 2) return fromBridge
    const out = []
    for (let k = 0; k < n; ++k) {
        const u = k / (n - 1)
        out.push({ u: u, out: curve(c, u) })
    }
    return out
}
// Stick tilt (0..1) for a share of full mouse speed: the dead zone is where the smallest motion starts, and the tilt
// never passes maxTilt. With `game` on the dead zone is the game calibration's (not known here): drawn as none.
function stickFor(tuning, u) {
    const t = tuning || ({})
    const dead = t.game === true ? 0 : clamp(t.deadzone !== undefined ? t.deadzone : 0, 0, 0.5)
    const cap = clamp(t.maxTilt !== undefined ? t.maxTilt : 1, 0.1, 1)
    if (!(u > 0)) return 0
    return Math.min(cap, dead + (1 - dead) * curve(t, u))
}

// Qt.Key_* values used below (qnamespace.h); kept as numbers so this file stays a plain library.
var kKeyNames = {
    0x20: "Space", 0x01000020: "Shift", 0x01000021: "Ctrl", 0x01000023: "Alt", 0x01000001: "Tab", 0x01000000: "Esc",
    0x01000004: "Enter", 0x01000005: "Enter", 0x01000003: "Back", 0x01000024: "Caps", 0x01000013: "Up", 0x01000015: "Down",
    0x01000012: "Left", 0x01000014: "Right", 0x01000010: "Home", 0x01000011: "End", 0x01000006: "Insert", 0x01000007: "Delete",
    0x01000016: "PageUp", 0x01000017: "PageDown", 0x60: "Grave", 0x7e: "Grave", 0x2d: "Minus", 0x5f: "Minus", 0x3d: "Plus", 0x2b: "Plus",
    0x2c: "Comma", 0x3c: "Comma", 0x2e: "Period", 0x3e: "Period", 0x2f: "Slash", 0x3f: "Slash", 0x3b: "Semicolon", 0x3a: "Semicolon",
    0x27: "Quote", 0x22: "Quote", 0x5b: "LBracket", 0x7b: "LBracket", 0x5d: "RBracket", 0x7d: "RBracket", 0x5c: "Backslash", 0x7c: "Backslash"
}
var kKeypadModifier = 0x20000000
// The profile file's name of a pressed key, "" when it has none. `key` is Qt's key code, `modifiers` its modifiers.
function keyName(key, modifiers) {
    const k = Number(key)
    const keypad = (Number(modifiers) & kKeypadModifier) !== 0
    if (k >= 0x30 && k <= 0x39) return (keypad ? "Num" : "") + String.fromCharCode(k)
    if (k >= 0x41 && k <= 0x5a) return String.fromCharCode(k)
    if (k >= 0x01000030 && k <= 0x0100003b) return "F" + (k - 0x01000030 + 1)
    const name = kKeyNames[k]
    return name === undefined ? "" : name
}
// Qt.LeftButton 1, RightButton 2, MiddleButton 4, BackButton 8, ForwardButton 16 -> Mouse1 (left), Mouse3 (right),
// Mouse2 (middle), Mouse4, Mouse5 (the numbering of the capture and the profile file).
function mouseName(button) {
    switch (Number(button)) {
    case 1: return "Mouse1"
    case 2: return "Mouse3"
    case 4: return "Mouse2"
    case 8: return "Mouse4"
    case 16: return "Mouse5"
    default: return ""
    }
}
function wheelName(deltaY) { return Number(deltaY) > 0 ? "WheelUp" : Number(deltaY) < 0 ? "WheelDown" : "" }

// ---- Binding lists ([{key, action}]) ---------------------------------------------------------------------------------
function actionOf(list, key) {
    const wanted = String(key).toLowerCase()
    for (const b of (list || [])) if (String(b.key).toLowerCase() === wanted) return String(b.action)
    return "none"
}
function keysOf(list, action) {
    const out = []
    for (const b of (list || [])) if (String(b.action) === String(action)) out.push(String(b.key))
    return out
}
// The aiming profile's keys: walking's, with aiming's own differences on top (`aimOwn`: only where it differs;
// action "none" there means the key does nothing while aiming).
function aimList(walk, aimOwn) {
    const out = []
    const own = {}
    for (const b of (aimOwn || [])) own[String(b.key).toLowerCase()] = String(b.action)
    for (const b of (walk || [])) {
        const k = String(b.key).toLowerCase()
        if (own[k] === undefined) out.push({ key: String(b.key), action: String(b.action) })
    }
    for (const b of (aimOwn || [])) if (String(b.action) !== "none") out.push({ key: String(b.key), action: String(b.action) })
    return out
}
function listFor(bindings, context) {
    const b = bindings || ({})
    if (context === "drive") return b.drive || []
    if (context === "aim") return aimList(b.walk || [], b.aim || [])
    return b.walk || []
}

// A number for a readout: "-" when the core does not know it (-1).
function shown(value, decimals) {
    const n = Number(value)
    return isFinite(n) && n >= 0 ? n.toFixed(decimals === undefined ? 1 : decimals) : "-"
}
// A file name's slug as the bridge makes it (lower-case letters, digits and '-'): what a typed name becomes.
function slug(name) {
    let out = ""
    const text = String(name || "").trim().toLowerCase()
    for (let i = 0; i < text.length && out.length < 48; ++i) {
        const c = text[i]
        if ((c >= "a" && c <= "z") || (c >= "0" && c <= "9")) out += c
        else if (out.length > 0 && out[out.length - 1] !== "-") out += "-"
    }
    while (out.length > 0 && out[out.length - 1] === "-") out = out.slice(0, -1)
    return out
}
