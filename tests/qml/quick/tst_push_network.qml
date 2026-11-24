// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// The network diagnostics panel (PushNetDiagnostics.qml): hidden unless the statistics overlay is on and a
// [slipstream-net] window exists; every row says whether it is measured, estimated or unavailable; nothing
// unknown is shown as 0. UI logic only, against the fixture bridge (pushLiveStats returns veyra.liveStats).
// The numbers are made up.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 900; height: 900
    PushNetDiagnostics { id: panel; x: 10; y: 10 }

    readonly property var window: ({
        running: true, remotePlay: true, netValid: true, netSeconds: 5, netCodec: "hevc", netWidth: 1920, netHeight: 1080,
        netResolutionChanges: 0, netFps: 59.9, netRequestedKbps: 15000, netKbps: 5230, netKbpsMin1s: 3100, netKbpsMax1s: 9600,
        netConsoleTargetKbps: null, netPackets: 6000, netLossPercent: 0.15, netMissing: 9, netLate: 4, netMissingNotLate: 5,
        netReordered: 12, netDropOther: 0, netFecRecovered: 3, netFecFailed: 0, netFecRecoveryPct: 100, netFramesLost: 0,
        netRecoveries: 0, netIngressDropped: 0, netJitterMs: 0.42, netJitterPeakMs: 3.1, netToDecodeP95Ms: 1.85,
        netUdpInErrors: null, netNicDiscards: 0, netRoute: "on-link", netRouteClass: "wired", netBottleneck: "encoder",
        netSigns: "encoder", netEvidence: "x"
    })

    TestCase {
        name: "PushNetworkDiagnostics"
        when: windowShown
        function init() { veyra.preferences = ({}); veyra.liveStats = ({running: false}); panel.stats = ({}) }
        function cleanup() { init() }
        function textOf(prefix, index) { const t = findChild(panel, prefix + index); verify(t !== null, prefix + index); return t.text }
        function rowOf(label) {
            for (let i = 0; i < panel.lines.length; ++i) if (panel.lines[i].k === label) return panel.lines[i]
            fail("no row " + label); return null
        }

        function test_hidden_and_idle_unless_the_overlay_is_on_and_a_window_exists() {
            const timer = findChild(panel, "pushfps-net-timer")
            verify(!panel.visible && !timer.running, "nothing set: not shown, not polling")
            compare(timer.interval, 1000, "at most once a second")
            veyra.liveStats = root.window
            panel.refresh()
            verify(!panel.visible, "a window without the overlay switch: still hidden")
            veyra.preferences = ({pushStatsOverlay: true})
            verify(timer.running)
            veyra.liveStats = ({running: true, remotePlay: true, netValid: false})
            panel.refresh()
            verify(!panel.visible && panel.lines.length === 0, "the overlay on, no [slipstream-net]: hidden")
            veyra.liveStats = root.window
            panel.refresh()
            verify(panel.visible)
            veyra.preferences = ({})
            verify(!panel.visible && !timer.running)
        }

        function test_rows_say_measured_estimated_or_unavailable() {
            veyra.preferences = ({pushStatsOverlay: true})
            veyra.liveStats = root.window
            panel.refresh()
            compare(rowOf(qsTr("码率")).v, qsTr("%1 / 请求 %2 kbit/s").arg("5230").arg("15000"))
            compare(rowOf(qsTr("码率")).t, panel.measured)
            // The console's own target was not reported: unavailable, not 0.
            compare(rowOf(qsTr("PS5 目标码率")).v, panel.unavailable)
            compare(rowOf(qsTr("PS5 目标码率")).t, panel.unavailable)
            // Never-arrived packets are an upper bound: estimated. Late ones are counted: measured.
            compare(rowOf(qsTr("始终没到的包（上限）")).t, panel.estimated)
            compare(rowOf(qsTr("迟到的包")).v, "4")
            compare(rowOf(qsTr("FEC 恢复")).v, qsTr("%1 % · %2 帧").arg("100.0").arg("3"))
            // The system-wide UDP counter was not read: unavailable; the adapter's 0 is a measured 0.
            compare(rowOf(qsTr("本机 UDP 接收错误（整台电脑）")).v, panel.unavailable)
            compare(rowOf(qsTr("网卡丢弃（整个网卡）")).v, "0")
            compare(rowOf(qsTr("网卡丢弃（整个网卡）")).t, panel.estimated)   // the whole adapter, not this stream alone
            compare(rowOf(qsTr("缺少的包")).v, qsTr("%1 个 · 丢包 %2 %").arg("9").arg("0.15"))
            compare(rowOf(qsTr("线路")).v, qsTr("直连（同一子网）") + " · " + qsTr("有线"))
            compare(rowOf(qsTr("主要瓶颈")).v, qsTr("PS5 编码码率低"))
            compare(rowOf(qsTr("主要瓶颈")).t, panel.estimated)
            compare(rowOf(qsTr("分辨率与编码")).v, "1920×1080 · hevc")
            // The grid shows the same rows: label, value, kind.
            compare(textOf("pushfps-net-label-", 0), qsTr("码率"))
            compare(textOf("pushfps-net-kind-", 0), panel.measured)
        }

        function test_unknowns_and_a_lossy_window() {
            veyra.preferences = ({pushStatsOverlay: true})
            veyra.liveStats = Object.assign({}, root.window, {
                netFecRecoveryPct: null, netFecRecovered: 0, netKbpsMin1s: null, netToDecodeP95Ms: null, netRoute: "unknown",
                netRouteClass: "unknown", netUdpInErrors: 2, netNicDiscards: null, netBottleneck: "network-loss", netWidth: 1280,
                netHeight: 720, netResolutionChanges: 1, netConsoleTargetKbps: 8000, netFramesLost: 2 })
            panel.refresh()
            compare(rowOf(qsTr("FEC 恢复")).v, qsTr("没有需要恢复的帧"))
            compare(rowOf(qsTr("每秒最低 / 最高")).v, panel.unavailable)
            compare(rowOf(qsTr("网络到解码延迟")).v, panel.unavailable)
            compare(rowOf(qsTr("线路")).t, panel.unavailable)
            compare(rowOf(qsTr("本机 UDP 接收错误（整台电脑）")).v, "2")
            compare(rowOf(qsTr("本机 UDP 接收错误（整台电脑）")).t, panel.estimated)
            compare(rowOf(qsTr("网卡丢弃（整个网卡）")).v, panel.unavailable)
            compare(rowOf(qsTr("主要瓶颈")).v, qsTr("网络丢包"))
            compare(rowOf(qsTr("PS5 目标码率")).v, "8000 kbit/s")
            compare(rowOf(qsTr("PS5 目标码率")).t, panel.estimated)   // the unit of the console's value is inferred
            compare(rowOf(qsTr("无法恢复的帧")).v, "2")
            compare(rowOf(qsTr("分辨率与编码")).v, "1280×720 · hevc" + qsTr(" · 变化 %1 次").arg(1))
            // Every cause the classifier can name has its own words; an unknown id is unavailable.
            for (const id of ["socket-or-adapter", "receive-thread", "app-drops", "network-loss", "packet-recovery", "decoder", "jitter",
                              "frame-generation", "enhancement", "gpu-processing", "presentation", "encoder", "none"])
                verify(panel.causeName(id) !== panel.unavailable, id)
            compare(panel.causeName("something-else"), panel.unavailable)
        }

        function test_a_wrapped_counter_and_an_empty_window_are_unavailable_not_zero() {
            veyra.preferences = ({pushStatsOverlay: true})
            veyra.liveStats = Object.assign({}, root.window, {netCounterWrap: true, netMissing: null, netLossPercent: null,
                                                              netMissingNotLate: null, netBottleneck: "unknown"})
            panel.refresh()
            compare(rowOf(qsTr("缺少的包")).v, panel.unavailable)
            compare(rowOf(qsTr("缺少的包")).t, panel.unavailable)
            compare(rowOf(qsTr("始终没到的包（上限）")).v, panel.unavailable)
            compare(rowOf(qsTr("迟到的包")).v, "4")
            // Nothing arrived in the window: no verdict, never "no clear bottleneck".
            compare(rowOf(qsTr("主要瓶颈")).v, panel.unavailable)
            compare(rowOf(qsTr("主要瓶颈")).t, panel.unavailable)
        }
    }
}
