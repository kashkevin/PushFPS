// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// On-screen latency readout (preference pushLatencyOverlay). Measured, not
// estimated: the engine times every real frame from the capture card's callback
// to the return of Present (mean and 95th percentile over the last second,
// FrameFlowWindow::latency), and counts the frames handed to the display (frame
// rate and its 1% low). It is the PC's share of the delay; the capture card, the
// display and the game add their own, which software cannot see.
// Package 15: the tooltip splits the number into picture + first generated frame +
// the schedule's wait + margin over the same frames (engine/PushDelaySplit.h, pushLiveStats
// latency*Ms); the shown parts are rounded so they add up to the shown total; the wait part is a formula from the
// pair's schedule (said in the text), picture and first generated frame are readiness as the engine saw it. splitText /
// tipText are the text model (another component may draw the badge; keep them here).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "PushInPicture.js" as InPicture

Rectangle {
    id: badge
    objectName: "pushfps-latency-badge"
    property var stats: ({})
    readonly property bool wanted: veyra.preferences.pushLatencyOverlay === true
    readonly property bool hasNumbers: stats.running === true && stats.latencyMs !== undefined && stats.latencyMs !== null
    visible: wanted && stats.running === true
    // Stream "present" R1 (PushInPicture.js): while it plays this is drawn inside the swap chain, so the video window keeps
    // one rectangle; it is cut out of the video window (a cover) only until the presenter has drawn it, and whenever it
    // has not for up to about 2 s. videoOrigin: the picture's top left in the window (Main.qml).
    property point videoOrigin: Qt.point(0, 0)
    property bool inPicture: false
    readonly property bool videoCover: visible && !inPicture
    function publishPicture() { InPicture.publish(veyra, badge, "badge", badge.videoOrigin, Screen.devicePixelRatio) }
    // At most 4 grabs a second, coalesced (review S4: never one per Qt frame while the item moves); hiding is immediate.
    function schedulePicture() { if (!badgeGrabTimer.running) badgeGrabTimer.start() }
    Timer { id: badgeGrabTimer; objectName: "pushfps-latency-grab-timer"; interval: 250; onTriggered: badge.publishPicture() }
    onVisibleChanged: { if (!visible) { badge.inPicture = false; badgeGrabTimer.stop(); badge.publishPicture() } else badge.schedulePicture() }
    onXChanged: badge.schedulePicture()
    onYChanged: badge.schedulePicture()
    onWidthChanged: badge.schedulePicture()
    onHeightChanged: badge.schedulePicture()
    onStatsChanged: badge.schedulePicture()
    readonly property real coverRadius: 9
    implicitWidth: row.implicitWidth + 20
    implicitHeight: 30
    radius: 9
    // Drawn in the picture the backing covers the game, not the dark interface: 0.94 there (6 % of the game shows through),
    // 0.82 as before where the items stay cut-outs (review S4).
    color: Qt.rgba(0.04, 0.05, 0.04, InPicture.enabled(veyra) ? 0.94 : 0.82)
    border.width: 1
    border.color: Qt.rgba(1, 1, 1, 0.10)

    function refresh() { badge.stats = veyra.pushLiveStats() || {} }
    Timer { interval: 500; repeat: true; running: badge.wanted; triggeredOnStart: true; onTriggered: { badge.refresh(); badge.inPicture = InPicture.drawn(veyra, "badge") } }

    // Package 15: where the number goes, when most of the last second's real frames were split.
    readonly property bool splitKnown: hasNumbers && typeof stats.latencyPictureMs === "number" && typeof stats.latencyWaitMs === "number" &&
                                       typeof stats.latencyFirstGeneratedMs === "number" && Number(stats.latencySplitShare || 0) >= 0.5
    readonly property string splitText: {
        if (!splitKnown) return ""
        const total = Math.round(Number(stats.latencyMs))
        const picture = Math.round(Number(stats.latencyPictureMs)), first = Math.round(Number(stats.latencyFirstGeneratedMs))
        const wait = Math.round(Number(stats.latencyWaitMs))
        const margin = total - picture - first - wait   // so the shown parts add up to the shown total
        return qsTr("%1 ms = %2 画面处理 + %3 第一个生成帧 + %4 等待前面要显示的帧（按排程计算，不是测量） + %5 余量").arg(total).arg(picture).arg(first).arg(wait).arg(margin)
    }
    readonly property string tipText: hasNumbers
        ? (splitText.length > 0 ? splitText + "\n" : "") + qsTr("从采集卡送来画面到这一帧交给显示（Present 返回）的时间，最近 1 秒真实帧的平均值。") + "\n"
          + qsTr("不包括游戏本身、主机的视频输出、采集卡和显示器的延迟。")
        : ""
    HoverHandler { id: badgeHover }
    ToolTip.visible: badgeHover.hovered && badge.tipText.length > 0
    ToolTip.text: badge.tipText

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
