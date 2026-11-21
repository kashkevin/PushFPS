// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Smoothness Mode row of the quick setup (pushfps-smoothness-row), experimental and off by default. One choice,
// preference pushSmoothTarget: 0 off (the user's own fixed frame generation, for A/B), or the displayed rate to aim
// at: 60, 90, 120 or 180. It decides from the content rate, GPU timestamps and the scheduler's admissions; it reads no
// presentation data, so it cannot say what the panel showed. The engine's budget can still skip pairs, and
// FgCostSettling can still hold generation for seconds unless VEYRA_EXP_RP_SMOOTH_STEADY=1. The bridge hands it to the engine (applyPushSmoothTarget, src/ui/bridge/PushSlipstream.cpp);
// the engine's decision core is include/veyra/engine/PushSmoothness.h: it picks the multiplier whose output fits
// the target from the measured content rate, steps down one level only after a second of sustained trouble (GPU
// headroom or pairs the scheduler had to skip) and follows a target that asks for more again after a 0.5 s gate; a
// level lowered by trouble comes back after 5 s of headroom (or 5 s without trouble when no cost is measured) and 10 s
// after the step down, and stays lowered for that content rate otherwise; never below 2X, never off.
// It only ever chooses at or below the multiplier the user set (the ceiling). While something plays, one status
// line says what it runs and, whenever it lowered anything, why (pushSmoothState().reason).
// The user's multiplier and every other setting stay as they are; "off" is the fixed configuration.
// Unverified on Windows.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: row
    objectName: "pushfps-smoothness-rows"
    Layout.fillWidth: true
    spacing: 0

    // Where the running state comes from: the bridge (pushSmoothState) unless a test sets a function here.
    property var stateSource: null
    readonly property bool available: row.stateSource !== null || typeof veyra.pushSmoothState === "function"
    visible: row.available && veyra.fgEnabled === true

    readonly property int choice: {
        const n = Number(veyra.preferences.pushSmoothTarget || 0)
        return n === 60 || n === 90 || n === 120 || n === 180 ? n : 0
    }
    readonly property var choices: [
        { id: "0", label: qsTr("关闭", "off") },
        { id: "60", label: "60" },
        { id: "90", label: "90" },
        { id: "120", label: "120" },
        { id: "180", label: "180" }
    ]
    function pick(id) {
        const n = Number(id)
        return veyra.setPreference("pushSmoothTarget", n === 60 || n === 90 || n === 120 || n === 180 ? n : 0)
    }

    // The engine's state, once a second while something plays with the mode on.
    property var smooth: ({})
    function refresh() {
        const s = row.stateSource !== null ? row.stateSource() : typeof veyra.pushSmoothState === "function" ? veyra.pushSmoothState() : null
        row.smooth = s || ({})
    }
    Timer {
        objectName: "pushfps-smoothness-timer"
        interval: 1000; repeat: true
        running: row.visible && veyra.hasSource === true && row.choice > 0
        triggeredOnStart: true
        onTriggered: row.refresh()
        onRunningChanged: if (!running) row.smooth = ({})
    }
    readonly property int ceiling: Number(veyra.fgMultiplier || 1)

    // The controller's reasons (PushSmoothness.h pushSmoothReasonName), in a few words. The log keeps the reason itself.
    function reasonText(reason) {
        switch (String(reason)) {
        case "step-down-gpu-headroom": return qsTr("显卡余量不足，已降一档")
        case "step-down-rejected-pairs": return qsTr("有画面来不及补帧，已降一档")
        case "step-up-headroom-recovered": return qsTr("显卡余量恢复，已升一档")
        case "target-below-2x": return qsTr("游戏帧率已接近目标，保持最低的 2×")
        case "hold": return qsTr("等待显卡余量稳定后再升档")
        }
        return ""
    }
    readonly property string status: {
        const chosen = Number(row.smooth.chosen || 0)
        if (row.choice === 0 || chosen < 2) return ""
        const parts = [qsTr("当前 %1×").arg(chosen)]
        const preferred = Number(row.smooth.preferred || 0)
        if (preferred > chosen) parts.push(qsTr("目标需要 %1×").arg(preferred))
        const why = row.reasonText(row.smooth.reason)
        if (why.length > 0) parts.push(why)
        if (row.smooth.targetFromSwitch === true) parts.push(qsTr("目标由测试开关设定"))
        return parts.join(" · ")
    }
    readonly property string choiceHint: row.choice === 0 ? qsTr("使用你自己的固定补帧倍数")
        : row.ceiling < 3 ? qsTr("把补帧倍数设为 3× 或更高，流畅模式才能在其中选择")
        : qsTr("以显示 %1 FPS 为目标，在 %2× 以内自动选择补帧倍数").arg(row.choice).arg(row.ceiling)

    VRow {
        objectName: "pushfps-smoothness-row"
        label: qsTr("流畅模式")
        help: qsTr("以所选的显示帧率为目标：按游戏帧率、显卡计时和补帧调度的结果选择补帧倍数，不超过你设定的倍数。显卡持续跟不上时先降一档，模式本身从不关闭补帧；但补帧调度仍可能跳过个别画面，偶尔暂停补帧几秒。它不测量屏幕实际显示的画面。选「关闭」就是你自己的固定设置，可以用来对比。")
        hint: row.choiceHint
        hintBelow: true
        RowLayout {
            spacing: 8
            VSeg {
                objectName: "pushfps-smoothness"
                options: row.choices
                current: String(row.choice)
                onPicked: id => row.pick(id)
            }
            Text {
                objectName: "pushfps-smoothness-experimental"
                text: qsTr("实验")
                color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
            }
        }
    }
    Text {
        objectName: "pushfps-smoothness-status"
        visible: row.status.length > 0
        text: row.status
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.rightMargin: 24
        Layout.bottomMargin: 6
    }
}
