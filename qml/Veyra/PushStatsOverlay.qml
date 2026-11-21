// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Statistics overlay over the picture (preference pushStatsOverlay, off unless switched on in the quick
// setup). A larger sibling of PushLatencyBadge: the performance mode, the stream's, the game's and the
// presented frame rate, generated against real frames, PushFPS's processing latency, and for a PS5 stream
// the connection grade, loss, jitter, recovery events and the route.
//
// It measures nothing. Once a second, and only while it is wanted, it reads pushLiveStats(): numbers the
// engine has already computed (the flow window, the stream cadence report of VEYRA_EXP_RP_STREAM_DEDUP,
// the five-second connection health snapshot of VEYRA_EXP_RP_HEALTH). A number that is not there is
// shown as "not measured", never as 0 and never estimated. Unverified on Windows.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: overlay
    objectName: "pushfps-stats-panel"
    property var stats: ({})
    readonly property bool wanted: veyra.preferences.pushStatsOverlay === true
    visible: wanted && stats.running === true
    readonly property bool videoCover: visible
    readonly property real coverRadius: 9
    implicitWidth: grid.implicitWidth + 24
    implicitHeight: grid.implicitHeight + 16
    radius: 9
    color: Qt.rgba(0.04, 0.05, 0.04, 0.82)
    border.width: 1
    border.color: Qt.rgba(1, 1, 1, 0.10)

    // With Automatic on, the mode in place may be below the chosen one.
    property var autoState: ({})
    function refresh() {
        overlay.stats = veyra.pushLiveStats() || ({})
        overlay.autoState = veyra.preferences.pushModeAuto === true && typeof veyra.pushAutoModeState === "function"
            ? (veyra.pushAutoModeState() || ({})) : ({})
    }
    Timer {
        objectName: "pushfps-stats-overlay-timer"
        interval: 1000; repeat: true
        running: overlay.wanted
        triggeredOnStart: true
        onTriggered: overlay.refresh()
        onRunningChanged: if (!running) overlay.stats = ({})
    }

    readonly property string none: qsTr("未测量")
    function known(v) { return v !== undefined && v !== null && isFinite(Number(v)) }
    function number(v, digits) { return overlay.known(v) ? Number(v).toFixed(digits) : overlay.none }
    // A rate of 0 from a counter that has not counted yet is no measurement either.
    function rate(v) { return overlay.known(v) && Number(v) > 0 ? Number(v).toFixed(1) : overlay.none }

    function modeName(id) {
        switch (id) {
        case "ultra-low-latency": return qsTr("超低延迟")
        case "competitive": return qsTr("竞技")
        case "balanced": return qsTr("均衡")
        case "maximum-smoothness": return qsTr("最大流畅")
        }
        return ""
    }
    // The mode, and the multiplier the last pair of frames used when frames are being generated.
    readonly property string modeText: {
        const chosen = overlay.modeName(String(veyra.preferences.pushMode || ""))
        if (chosen === "") return qsTr("关闭（自己的设置）")
        const now = overlay.autoState.active === true ? overlay.modeName(String(overlay.autoState.mode || "")) : ""
        const multiplier = stats.fgActive === true && Number(stats.fgMultiplierNow) > 1 ? " · " + Number(stats.fgMultiplierNow) + "×" : ""
        return (now !== "" ? now : chosen) + multiplier
    }
    // What the console (or the capture card) sends, how many of those pictures are new, what is shown.
    readonly property string streamFps: stats.cadenceActive === true ? rate(stats.cadenceStreamFps)
        : stats.remotePlay === true ? rate(stats.streamDecodedFps)
        : stats.capture === true ? rate(stats.captureFps) : rate(stats.sourceFps)
    readonly property string uniqueFps: stats.cadenceActive === true ? rate(stats.cadenceSourceFps) : rate(stats.contentFps)
    readonly property string presentedFps: rate(stats.submitFps)
    readonly property string generatedFps: known(stats.generatedPresentFps) && (Number(stats.generatedPresentFps) > 0 || Number(stats.realPresentFps) > 0)
        ? Number(stats.generatedPresentFps).toFixed(1) : none
    readonly property string realFps: rate(stats.realPresentFps)
    readonly property bool health: stats.healthValid === true
    readonly property string gradeText: {
        if (!overlay.health) return overlay.none
        switch (String(stats.healthGrade || "")) {
        case "stable": return qsTr("稳定")
        case "unsteady": return qsTr("不太稳定")
        case "unstable": return qsTr("不稳定")
        }
        return overlay.none
    }
    readonly property color gradeColor: !overlay.health ? Theme.t3 : stats.healthGrade === "stable" ? Theme.accent
        : stats.healthGrade === "unsteady" ? Theme.warn : stats.healthGrade === "unstable" ? "#FF6B5B" : Theme.t3
    readonly property string routeText: {
        if (!overlay.health || stats.routeKnown !== true) return overlay.none
        const link = stats.routeLink === "ethernet" ? qsTr("有线网络") : stats.routeLink === "wifi" ? "Wi-Fi"
                   : stats.routeLink === "other" ? qsTr("其他网络") : overlay.none
        return (stats.routeDirectLan === true ? qsTr("直连局域网") : qsTr("不是直连局域网")) + " · " + link
    }
    // label, value, colour of the value ("" = the usual one)
    readonly property var lines: [
        { k: qsTr("模式"), v: overlay.modeText, c: "" },
        { k: qsTr("帧率"), v: qsTr("来源 %1 · 游戏 %2 · 显示 %3").arg(overlay.streamFps).arg(overlay.uniqueFps).arg(overlay.presentedFps), c: "" },
        { k: qsTr("显示的帧"), v: qsTr("生成 %1 · 真实 %2").arg(overlay.generatedFps).arg(overlay.realFps), c: "" },
        { k: qsTr("处理延迟"), v: overlay.known(stats.latencyMs)
              ? overlay.number(stats.latencyMs, 1) + " ms · p95 " + overlay.number(stats.latencyP95Ms, 1) : overlay.none, c: "" },
        { k: qsTr("连接质量"), v: overlay.gradeText, c: overlay.gradeColor },
        { k: qsTr("丢包"), v: overlay.health ? overlay.number(stats.healthLossPercent, 2) + " %" : overlay.none, c: "" },
        { k: qsTr("抖动"), v: overlay.health ? overlay.number(stats.healthJitterMs, 1) + " ms" : overlay.none, c: "" },
        { k: qsTr("恢复事件"), v: overlay.health ? overlay.number(stats.healthRecoveryEvents, 0) : overlay.none, c: "" },
        { k: qsTr("线路"), v: overlay.routeText, c: "" }
    ]

    GridLayout {
        id: grid
        anchors.centerIn: parent
        columns: 2
        columnSpacing: 12
        rowSpacing: 3
        Repeater {
            model: overlay.lines.length * 2
            delegate: Text {
                required property int index
                readonly property var entry: overlay.lines[Math.floor(index / 2)]
                readonly property bool isValue: index % 2 === 1
                objectName: "pushfps-stats-" + (isValue ? "value-" : "label-") + Math.floor(index / 2)
                text: isValue ? entry.v : entry.k
                color: !isValue ? Theme.t3 : entry.c !== "" ? entry.c : Theme.t1
                font.family: isValue ? Theme.fontMono : Theme.fontUi
                font.pixelSize: 11
            }
        }
    }
}
