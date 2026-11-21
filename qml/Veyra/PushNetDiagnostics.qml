// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Network diagnostics panel for a PS5 stream, beside the statistics overlay (shown only when the overlay
// is switched on AND the [slipstream-net] window exists, i.e. with VEYRA_EXP_RP_STATS=1; with nothing set
// it never appears). It measures nothing itself: once a second, while wanted, it reads the net* keys of
// pushLiveStats() (include/veyra/remoteplay/NetDiagnostics.h, StreamBottleneck.h). Every row says what
// kind of number it is: measured (a count or time this PC took), estimated (an upper bound or a rule's
// verdict), or unavailable (Windows or the console did not say). Unverified on Windows.
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: panel
    objectName: "pushfps-net-panel"
    property var stats: ({})
    readonly property bool wanted: veyra.preferences.pushStatsOverlay === true
    readonly property bool available: stats.netValid === true
    visible: wanted && available
    readonly property bool videoCover: visible
    readonly property real coverRadius: 9
    implicitWidth: layout.implicitWidth + 24
    implicitHeight: layout.implicitHeight + 16
    radius: 9
    color: Qt.rgba(0.04, 0.05, 0.04, 0.82)
    border.width: 1
    border.color: Qt.rgba(1, 1, 1, 0.10)

    function refresh() { panel.stats = veyra.pushLiveStats() || ({}) }
    Timer {
        objectName: "pushfps-net-timer"
        interval: 1000; repeat: true
        running: panel.wanted
        triggeredOnStart: true
        onTriggered: panel.refresh()
        onRunningChanged: if (!running) panel.stats = ({})
    }

    readonly property string measured: qsTr("实测")
    readonly property string estimated: qsTr("估计")
    readonly property string unavailable: qsTr("不可用")
    function known(v) { return v !== undefined && v !== null && isFinite(Number(v)) }
    function fixed(v, digits) { return Number(v).toFixed(digits) }
    // One row: label, value, kind. A value that is not known is "unavailable", never 0.
    function row(label, value, kind) { return { k: label, v: value, t: kind } }
    function causeName(id) {
        switch (String(id || "")) {
        case "socket-or-adapter": return qsTr("本机接收丢弃")
        case "receive-thread": return qsTr("接收线程唤醒晚")
        case "app-drops": return qsTr("程序队列丢弃")
        case "network-loss": return qsTr("网络丢包")
        case "packet-recovery": return qsTr("包恢复（重排等待）")
        case "decoder": return qsTr("解码")
        case "jitter": return qsTr("网络抖动")
        case "frame-generation": return qsTr("补帧")
        case "enhancement": return qsTr("画面增强")
        case "gpu-processing": return qsTr("GPU 处理")
        case "presentation": return qsTr("显示")
        case "encoder": return qsTr("PS5 编码码率低")
        case "none": return qsTr("没有明显瓶颈")
        case "unknown": return panel.unavailable   // nothing arrived in the window: nothing to judge
        }
        return panel.unavailable
    }
    readonly property string routeText: {
        const hop = stats.netRoute === "on-link" ? qsTr("直连（同一子网）") : stats.netRoute === "gateway" ? qsTr("经过路由器") : panel.unavailable
        const link = stats.netRouteClass === "wired" ? qsTr("有线") : stats.netRouteClass === "wifi" ? "Wi-Fi" : ""
        return link === "" ? hop : hop + " · " + link
    }
    readonly property var lines: !panel.available ? [] : [
        panel.row(qsTr("码率"), qsTr("%1 / 请求 %2 kbit/s").arg(panel.fixed(stats.netKbps, 0)).arg(panel.fixed(stats.netRequestedKbps, 0)), panel.measured),
        panel.known(stats.netKbpsMin1s) && panel.known(stats.netKbpsMax1s)
            ? panel.row(qsTr("每秒最低 / 最高"), panel.fixed(stats.netKbpsMin1s, 0) + " / " + panel.fixed(stats.netKbpsMax1s, 0) + " kbit/s", panel.measured)
            : panel.row(qsTr("每秒最低 / 最高"), panel.unavailable, panel.unavailable),
        panel.known(stats.netConsoleTargetKbps)
            ? panel.row(qsTr("PS5 目标码率"), panel.fixed(stats.netConsoleTargetKbps, 0) + " kbit/s", panel.estimated)   // its unit is inferred
            : panel.row(qsTr("PS5 目标码率"), panel.unavailable, panel.unavailable),
        Number(stats.netWidth) > 0
            ? panel.row(qsTr("分辨率与编码"), Number(stats.netWidth) + "×" + Number(stats.netHeight) + " · " + String(stats.netCodec || "") +
                  (Number(stats.netResolutionChanges) > 0 ? qsTr(" · 变化 %1 次").arg(Number(stats.netResolutionChanges)) : ""), panel.measured)
            : panel.row(qsTr("分辨率与编码"), panel.unavailable, panel.unavailable),
        panel.known(stats.netMissing) && panel.known(stats.netLossPercent)
            ? panel.row(qsTr("缺少的包"), qsTr("%1 个 · 丢包 %2 %").arg(panel.fixed(stats.netMissing, 0)).arg(panel.fixed(stats.netLossPercent, 2)), panel.measured)
            : panel.row(qsTr("缺少的包"), panel.unavailable, panel.unavailable),
        panel.row(qsTr("迟到的包"), panel.fixed(stats.netLate, 0), panel.measured),
        panel.known(stats.netMissingNotLate)
            ? panel.row(qsTr("始终没到的包（上限）"), panel.fixed(stats.netMissingNotLate, 0), panel.estimated)
            : panel.row(qsTr("始终没到的包（上限）"), panel.unavailable, panel.unavailable),
        panel.row(qsTr("乱序的包"), panel.fixed(stats.netReordered, 0), panel.measured),
        panel.known(stats.netFecRecoveryPct)
            ? panel.row(qsTr("FEC 恢复"), qsTr("%1 % · %2 帧").arg(panel.fixed(stats.netFecRecoveryPct, 1)).arg(panel.fixed(stats.netFecRecovered, 0)), panel.measured)
            : panel.row(qsTr("FEC 恢复"), qsTr("没有需要恢复的帧"), panel.measured),
        panel.row(qsTr("无法恢复的帧"), panel.fixed(stats.netFramesLost, 0), panel.measured),
        panel.row(qsTr("关键帧恢复"), panel.fixed(stats.netRecoveries, 0), panel.measured),
        panel.row(qsTr("抖动"), qsTr("%1 ms · 峰值 %2 ms").arg(panel.fixed(stats.netJitterMs, 2)).arg(panel.fixed(stats.netJitterPeakMs, 1)), panel.measured),
        panel.known(stats.netNicDiscards)
            ? panel.row(qsTr("网卡丢弃（整个网卡）"), panel.fixed(stats.netNicDiscards, 0), panel.estimated)
            : panel.row(qsTr("网卡丢弃（整个网卡）"), panel.unavailable, panel.unavailable),
        panel.known(stats.netUdpInErrors)
            ? panel.row(qsTr("本机 UDP 接收错误（整台电脑）"), panel.fixed(stats.netUdpInErrors, 0), panel.estimated)
            : panel.row(qsTr("本机 UDP 接收错误（整台电脑）"), panel.unavailable, panel.unavailable),
        panel.known(stats.netToDecodeP95Ms)
            ? panel.row(qsTr("网络到解码延迟"), "p95 " + panel.fixed(stats.netToDecodeP95Ms, 2) + " ms", panel.measured)
            : panel.row(qsTr("网络到解码延迟"), panel.unavailable, panel.unavailable),
        panel.row(qsTr("线路"), panel.routeText, stats.netRoute === "on-link" || stats.netRoute === "gateway" ? panel.measured : panel.unavailable),
        panel.row(qsTr("主要瓶颈"), panel.causeName(stats.netBottleneck), stats.netBottleneck === "unknown" ? panel.unavailable : panel.estimated)
    ]

    ColumnLayout {
        id: layout
        anchors.centerIn: parent
        spacing: 4
        Text {
            objectName: "pushfps-net-title"
            text: qsTr("网络诊断 · 最近 %1 秒").arg(panel.known(stats.netSeconds) ? panel.fixed(stats.netSeconds, 0) : "5")
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
        GridLayout {
            columns: 3
            columnSpacing: 10
            rowSpacing: 3
            Repeater {
                model: panel.lines.length * 3
                delegate: Text {
                    required property int index
                    readonly property var entry: panel.lines[Math.floor(index / 3)]
                    readonly property int part: index % 3
                    objectName: "pushfps-net-" + (part === 0 ? "label-" : part === 1 ? "value-" : "kind-") + Math.floor(index / 3)
                    text: part === 0 ? entry.k : part === 1 ? entry.v : entry.t
                    color: part === 0 ? Theme.t3 : part === 2 ? (entry.t === panel.measured ? Theme.accent : entry.t === panel.estimated ? Theme.warn : Theme.t3) : Theme.t1
                    font.family: part === 1 ? Theme.fontMono : Theme.fontUi
                    font.pixelSize: part === 2 ? 10 : 11
                }
            }
        }
    }
}
