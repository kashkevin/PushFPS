// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS automatic Remote Play latency test (dialog key "latencytest"; the PS5 streaming dialog,
// streaming mode and Professional mode open it). PushFPS presses the PS button on the PS5 through
// Remote Play, waits for the control centre to appear in the picture, closes it and repeats
// (bridge pushLatencyTestBegin, engine/PushInputLatency.h). Splits the time into PushFPS's own
// processing and the rest (PS5, network, decoding). Not a controller-to-screen measurement.
import QtQuick
import QtQuick.Layouts

DialogLayer {
    id: lt
    objectName: "pushfps-latency-test"
    key: "latencytest"
    glyph: "gauge"
    title: qsTr("PS5 串流延迟测试")
    sub: qsTr("PushFPS 通过串流按下 PS 键，等控制中心出现在画面上，计算用了多久，并分出 PushFPS 自己的处理时间")
    dialogWidth: 600

    property var st: ({})
    property int trials: 8
    // Latency test v2 (docs/LATENCY_MEASUREMENT.md): the button pressed. "ps" is the original test;
    // "options" and "dpad-right-left" answer faster than the PS5's control centre.
    property string button: "ps"
    readonly property bool running: st.state === "running"
    readonly property bool streaming: veyra.sourceKind === "ps5"
    // Seconds per press: open, answer, close and settle for PS / OPTIONS; press and settle for the D-pad.
    readonly property real secondsPerPress: button === "dpad-right-left" ? 1 : 3
    function begin() {
        const ok = lt.button === "ps" ? veyra.pushLatencyTestBegin(lt.trials) : veyra.pushLatencyTestBeginWith(lt.trials, lt.button)
        if (ok) { lt.st = veyra.pushLatencyTestStatus() || {}; poll.start() }
    }
    Timer { id: poll; interval: 200; repeat: true; onTriggered: { lt.st = veyra.pushLatencyTestStatus() || {}; if (lt.st.state !== "running") stop() } }
    onShownChanged: if (!shown && running) { veyra.pushLatencyTestCancel(); poll.stop() }

    actions: running ? [{ label: qsTr("取消") }] : [{ label: qsTr("关闭") }, { label: qsTr("开始测试"), primary: true }]
    onActionTriggered: label => {
        if (label === qsTr("开始测试")) lt.begin()
        else if (label === qsTr("取消")) { veyra.pushLatencyTestCancel(); poll.stop(); lt.st = veyra.pushLatencyTestStatus() || {} }
        else lt.dialogHost.close()
    }

    function ms(v) { return Math.round(Number(v || 0)) + " ms" }
    function ms2(v) { return Number(v || 0).toFixed(1) + " ms" }
    DialogGroup {
        VRow {
            visible: !lt.streaming
            label: qsTr("需要 PS5 串流")
            hint: qsTr("先在首页连接 PS5 串流，并进入一个游戏")
        }
        VRow {
            visible: lt.streaming && !lt.running && lt.st.state !== "done"
            label: qsTr("开始前")
            hint: lt.button === "ps"
                  ? qsTr("进入游戏画面，放下手柄。测试约 %1 秒，会打开和关闭 PS5 的控制中心 %2 次").arg(lt.trials * 3).arg(lt.trials)
                  : lt.button === "options"
                    ? qsTr("进入游戏画面，放下手柄。测试约 %1 秒，会按 OPTIONS 键打开和关闭游戏菜单 %2 次").arg(Math.round(lt.trials * lt.secondsPerPress)).arg(lt.trials)
                    : qsTr("打开一个菜单（例如 PS5 主页），放下手柄。测试约 %1 秒，会按方向键右、左交替移动选中项 %2 次").arg(Math.round(lt.trials * lt.secondsPerPress)).arg(lt.trials)
        }
        VRow {
            visible: lt.running
            objectName: "latency-test-progress"
            label: qsTr("正在测试…")
            hint: qsTr("第 %1 / %2 次；请不要碰手柄").arg(Math.min(Number(lt.st.trial || 0) + 1, lt.trials)).arg(lt.trials)
        }
        VRow {
            visible: lt.st.state === "failed"
            label: qsTr("测试没有完成")
            hint: String(lt.st.error || "")
        }
        VRow {
            visible: lt.st.state === "done"
            objectName: "latency-test-total"
            label: qsTr("按键到画面")
            value: Number(lt.st.answered || 0) > 0 ? lt.ms(lt.st.medianMs) : "—"
            hint: Number(lt.st.answered || 0) > 0
                  ? qsTr("中位数；最快 %1，最慢 %2，%3 / %4 次看到了响应").arg(lt.ms(lt.st.minMs)).arg(lt.ms(lt.st.maxMs)).arg(lt.st.answered).arg(lt.st.trials)
                  : qsTr("没有看到控制中心出现：请确认在游戏里，并且 PS 键能打开控制中心")
        }
        VRow {
            visible: lt.st.state === "done" && Number(lt.st.answered || 0) > 0
            label: qsTr("PS5、网络和解码")
            value: lt.ms(lt.st.remoteMs)
        }
        VRow {
            visible: lt.st.state === "done" && Number(lt.st.answered || 0) > 0
            label: qsTr("PushFPS 处理")
            value: lt.ms(lt.st.pushfpsMs)
            hint: qsTr("画面到达电脑到交给显示器，与延迟显示相同")
        }
        // Latency test v2: every press split at the network arrival of the frame that showed the answer.
        VRow {
            visible: lt.st.state === "done" && lt.st.split === true && Number(lt.st.consoleMs) >= 0
            objectName: "latency-test-console"
            label: qsTr("按键到画面到达电脑")
            value: lt.ms2(lt.st.consoleMs)
            hint: qsTr("中位数：PS5 的响应、编码和网络")
        }
        VRow {
            visible: lt.st.state === "done" && lt.st.split === true && Number(lt.st.pcMs) >= 0
            objectName: "latency-test-pc"
            label: qsTr("画面到达到呈现")
            value: lt.ms2(lt.st.pcMs)
            hint: qsTr("中位数：每次按键的同一帧在这台电脑上的处理")
        }
        VRow {
            visible: lt.st.state === "done" && lt.st.split === true && Number(lt.st.detectableMs) >= 0
            objectName: "latency-test-detectable"
            label: qsTr("可分辨的差异")
            value: lt.ms2(lt.st.detectableMs)
            hint: qsTr("两次同样次数的测试，差别至少这么大才能可靠地看出来。次数越多，这个值越小")
        }
        VRow {
            visible: !lt.running
            label: qsTr("次数")
            VSeg {
                objectName: "latency-test-trials"
                options: [{ id: "5", label: "5" }, { id: "8", label: "8" }, { id: "12", label: "12" },
                          { id: "50", label: "50" }, { id: "200", label: "200" }, { id: "500", label: "500" }]
                current: String(lt.trials)
                onPicked: id => lt.trials = Number(id)
            }
        }
        VRow {
            visible: !lt.running
            label: qsTr("按键")
            hint: qsTr("方向键和 OPTIONS 键的响应比控制中心快，更容易测出几毫秒的差别")
            VSeg {
                objectName: "latency-test-button"
                options: [{ id: "ps", label: qsTr("PS 键") }, { id: "options", label: qsTr("OPTIONS 键") },
                          { id: "dpad-right-left", label: qsTr("方向键 右/左") }]
                current: lt.button
                onPicked: id => lt.button = id
            }
        }
    }
    Text {
        Layout.fillWidth: true
        Layout.topMargin: 8
        text: qsTr("这是自动诊断，不是从手柄到屏幕的物理测量：不含手柄、显示器和游戏本身的延迟。PS5 的控制中心动画本身也需要一点时间")
        color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
        wrapMode: Text.WordWrap
    }
}
