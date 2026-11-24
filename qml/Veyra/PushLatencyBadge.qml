// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// On-screen latency readout (preference pushLatencyOverlay). Measured, not
// estimated: the engine times every real frame from the capture card's callback
// to the return of Present (mean and 95th percentile over ten seconds), and counts
// the frames handed to the display (frame rate and its 1% low). It is the PC's
// share of the delay; the capture card, the display and the game add their own,
// which software cannot see - the quick setup hint says so.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: badge
    objectName: "pushfps-latency-badge"
    property var stats: ({})
    readonly property bool wanted: veyra.preferences.pushLatencyOverlay === true
    readonly property bool hasNumbers: stats.running === true && stats.latencyMs !== undefined && stats.latencyMs !== null
    visible: wanted && stats.running === true
    readonly property bool videoCover: visible
    readonly property real coverRadius: 9
    implicitWidth: row.implicitWidth + 20
    implicitHeight: 30
    radius: 9
    color: Qt.rgba(0.04, 0.05, 0.04, 0.82)
    border.width: 1
    border.color: Qt.rgba(1, 1, 1, 0.10)

    function refresh() { badge.stats = veyra.pushLiveStats() || {} }
    Timer { interval: 500; repeat: true; running: badge.wanted; triggeredOnStart: true; onTriggered: badge.refresh() }

    // Green under 30 ms of PC latency, amber to 60, red above.
    readonly property color level: !hasNumbers ? Theme.t3 : stats.latencyMs < 30 ? Theme.accent : stats.latencyMs < 60 ? Theme.warn : "#FF6B5B"

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 10
        Rectangle { implicitWidth: 7; implicitHeight: 7; radius: 3.5; color: badge.level }
        Text {
            objectName: "pushfps-latency-ms"
            text: badge.hasNumbers ? qsTr("延迟 %1 ms").arg(Number(badge.stats.latencyMs).toFixed(0)) : qsTr("延迟 测量中")
            color: Theme.t1; font.family: Theme.fontMono; font.pixelSize: 12; font.weight: Font.DemiBold
        }
        Text {
            visible: badge.hasNumbers && badge.stats.latencyP95Ms !== undefined && badge.stats.latencyP95Ms !== null
            text: qsTr("p95 %1").arg(Number(badge.stats.latencyP95Ms).toFixed(0))
            color: Theme.t3; font.family: Theme.fontMono; font.pixelSize: 11
        }
        Text {
            objectName: "pushfps-latency-fps"
            visible: badge.stats.submitFps !== undefined && badge.stats.submitFps !== null
            // Shown FPS when the presenter knows it (PushLiveCadence), otherwise named as submitted; "uneven" when
            // the presents do not reach the screen evenly (no variable refresh: holds of 1 and 2 refreshes).
            text: (badge.stats.shownFpsKnown ? qsTr("显示 %1 FPS").arg(Number(badge.stats.shownFps).toFixed(0))
                                             : qsTr("提交 %1 FPS").arg(Number(badge.stats.submitFps).toFixed(0)))
                  + (badge.stats.uneven ? qsTr(" · 不均匀") : "")
                  + (badge.stats.submitLowFps !== undefined && badge.stats.submitLowFps !== null ? qsTr(" · 1% %1").arg(Number(badge.stats.submitLowFps).toFixed(0)) : "")
            color: Theme.t2; font.family: Theme.fontMono; font.pixelSize: 11
        }
    }
}
