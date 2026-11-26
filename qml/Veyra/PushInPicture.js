// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "present" R1: the readouts that sit over the picture while it plays (PushLatencyBadge, PushStatsOverlay,
// PushNetDiagnostics) are drawn INSIDE the swap chain, so the native video window keeps one rectangle (no cut-out: the
// research report's finding was that a cut-out region and the slow present path came together in his logs). Each item
// stays a normal QML item behind the video window (where nobody sees it) and hands a picture of itself to the bridge
// when it changes, at most 4 times a second (the items' grab timer, also during animations); the presenter blends it after
// the picture (never into recordings or streams). The item stays a cover, the old cut-out, until the bridge says a frame
// carrying its picture was presented, and becomes one again up to about 2 s after that stops (the presenter's 1 s plus the
// item's refresh period: paused without changes, a provider that owns Present). With the rollback switch
// VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY=1 it never grabs. A failed grab hands over nothing: the item stays a cut-out. Without
// the bridge functions (an older bridge, the test fixture without them) nothing changes.
.pragma library

function available(veyra) {
    return !!veyra && typeof veyra.pushSetVideoOverlay === "function" && typeof veyra.pushVideoOverlayState === "function"
}

// The bridge has it AND it is not switched off (VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY / _PACKAGE14 / _PACKAGE13): with the
// rollback the items never grab a picture of themselves (review S3).
function enabled(veyra) {
    if (!available(veyra)) return false
    const s = veyra.pushVideoOverlayState() || ({})
    return s.enabled === true
}

// Hands the item's picture to the bridge (or takes it away when the item is hidden). origin: the picture's top left in
// the window (Main.qml's videoHost), in the same coordinates as item.x / item.y; dpr: the window's pixel ratio.
function publish(veyra, item, key, origin, dpr) {
    if (!enabled(veyra)) return
    if (!item.visible || item.width <= 0 || item.height <= 0) {
        veyra.pushSetVideoOverlay(key, null, 0, 0, 1)
        return
    }
    const ratio = dpr > 0 ? dpr : 1
    const ox = item.x - (origin ? origin.x : 0), oy = item.y - (origin ? origin.y : 0)
    const ok = item.grabToImage(function(result) {
        // The item may have gone in the meantime: never publish a picture of a hidden item.
        if (item.visible) veyra.pushSetVideoOverlay(key, result.image, ox, oy, ratio)
    }, Qt.size(Math.ceil(item.width * ratio), Math.ceil(item.height * ratio)))
    if (!ok) veyra.pushSetVideoOverlay(key, null, 0, 0, 1)
}

// True when the presenter has drawn this item's latest picture within the last second.
function drawn(veyra, key) {
    if (!available(veyra)) return false
    const s = veyra.pushVideoOverlayState() || ({})
    return s.enabled === true && !!s.drawn && s.drawn[key] === true
}
