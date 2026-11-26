// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "s12-settings": one reader of the engine's requested / applied / executing facts (veyra.pushSettingsEffect(),
// engine/PushSettingsEffect.h; the same facts as the log line [settings-effect]) for every PushEffectNote and the
// PushRunningNow block of a page: one call a second while the page shows, never one per row. stateSource lets a test
// hand in the facts. Unverified on Windows.
import QtQuick

Item {
    id: board
    objectName: "pushfps-effect-board"
    visible: false
    property bool active: true
    property var stateSource: null
    property var facts: ({})
    function refresh() {
        const s = board.stateSource !== null ? board.stateSource()
                : typeof veyra.pushSettingsEffect === "function" ? veyra.pushSettingsEffect() : null
        board.facts = s || ({})
    }
    Timer {
        objectName: "pushfps-effect-board-timer"
        interval: 1000; repeat: true; triggeredOnStart: true
        running: board.active
        onTriggered: board.refresh()
    }
    Connections {
        target: veyra
        ignoreUnknownSignals: true
        function onSettingsChanged() { board.refresh() }
        function onPresentationChanged() { board.refresh() }
    }
}
