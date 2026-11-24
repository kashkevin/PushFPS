// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Package 15 "Lower delay" row of the quick setup (pushfps-lowdelay-row), experimental and off by default. One switch,
// preference pushLowDelay (validated in src/ui/bridge/Preferences.cpp). On, a capture card's pairs at 3X with NVIDIA VFG
// may run as I(2/3), B, P: one shown frame in three is predicted from the motion so far, and the real frame is shown one
// output slot earlier (a third of a game frame; estimated, not measured: include/veyra/engine/PushLowDelay.h,
// docs/drafts/LOW_DELAY_MODE.md). A bad prediction is still shown (its unreliable parts keep the real picture); the mode
// switches itself off only when prediction is bad most of the time or predicted frames start arriving late (then after
// a long calm run it tries again), and at once for a skipped or held pair or a history reset. Shown only while a capture
// card plays and NVIDIA VFG and 3X are available.
// One click applies in the running session, on and off: the engine stops asking for predicted pairs at once, and with
// VFG at 3X the frame-generation graph is rebuilt with or without the predicted-frame pass (as HUD Lock is), which can
// pause the picture for a moment. The status line says "starting" until the rebuilt graph runs.
// The bridge hands it to the engine (applyPushLowDelay, src/ui/bridge/PushSlipstream.cpp). Unverified on Windows.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: row
    objectName: "pushfps-lowdelay-rows"
    Layout.fillWidth: true
    spacing: 0

    // Where the running state comes from: the bridge (pushLowDelayState) unless a test sets a function here.
    property var stateSource: null
    readonly property bool available: row.stateSource !== null || typeof veyra.pushLowDelayState === "function"
    readonly property bool vfgAvailable: (((veyra.effectCapabilities || {}).vfg) || {}).available === true
    readonly property bool threeXOffered: (veyra.fgMultiplierChoices || []).some(o => parseInt(o.id) === 3)
    visible: row.available && veyra.sourceKind === "capture" && row.vfgAvailable && row.threeXOffered
    readonly property bool on: veyra.preferences.pushLowDelay === true
    // The kit's switch (VEYRA_EXP_RP_FG_SCHEDULE=onepredicted set before PushFPS started) runs the mode with the row off.
    readonly property bool kit: row.engine.fromSwitch === true
    readonly property bool runsHere: veyra.fgEnabled === true && veyra.fgBackendName === "vfg" && Number(veyra.fgMultiplier || 1) === 3

    property var engine: ({})
    function refresh() {
        const s = row.stateSource !== null ? row.stateSource() : typeof veyra.pushLowDelayState === "function" ? veyra.pushLowDelayState() : null
        row.engine = s || ({})
    }
    Timer {
        objectName: "pushfps-lowdelay-timer"
        interval: 1000; repeat: true
        running: row.visible && veyra.hasSource === true
        triggeredOnStart: true
        onTriggered: row.refresh()
        onRunningChanged: if (!running) row.engine = ({})
    }

    function notHereText(reason) {
        switch (String(reason || "")) {
        case "hud-lock-or-exclusion": return qsTr("不能在这里运行：实时 HUD 锁定或排除区域已开启")
        case "format": return qsTr("不能在这里运行：当前输出格式不支持（需要 SDR 或 HDR10）")
        case "present-sink-fg": return qsTr("不能在这里运行：当前补帧方式自己负责呈现")
        case "not-3x": return qsTr("当前实际倍数不是 3×（自适应倍率或输出上限），这时不运行")
        case "smoothness-mode": return qsTr("不能在这里运行：流畅模式正在决定倍数，两者不能同时使用")
        }
        return qsTr("需要 NVIDIA VFG 补帧，倍数设为 3×")
    }
    function pausedText(paused) {
        switch (String(paused || "")) {
        case "quality-test": return qsTr("已暂停：正在测试补帧画质")
        case "benchmark": return qsTr("已暂停：正在为这台电脑做优化测试")
        case "hud-detection": return qsTr("已暂停：正在检测 HUD")
        case "comparison-clip": return qsTr("已暂停：正在录制对比片段")
        }
        return qsTr("已暂停")
    }
    readonly property string status: {
        if (!row.on && !row.kit) return ""
        if (!row.runsHere) return qsTr("需要 NVIDIA VFG 补帧，倍数设为 3×")
        let text = ""
        switch (String(row.engine.state || "")) {
        case "active": text = qsTr("正在运行：每个真实帧后面显示一个预测帧"); break
        case "calm": text = qsTr("已开启：画面稳定后开始预测（不可靠的部分保持真实画面）"); break
        case "starting": text = qsTr("正在开启：补帧管线重建中（画面可能停顿片刻）"); break
        case "unavailable": text = String(row.engine.reason || "") === "vfg-refused"
                                   ? qsTr("这台电脑的 VFG 不接受单独生成这一帧：本次运行中已停用，补帧照常（关闭再开启这一行可再试一次）")
                                   : qsTr("这台电脑或当前设置不能运行，补帧照常（原因见日志）"); break
        case "not-here": text = row.notHereText(row.engine.reason); break
        case "suspended": text = row.pausedText(row.engine.paused); break
        }
        if (row.kit && !row.on) return text.length > 0 ? qsTr("由测试开关开启（关闭这一行不会停止它）") + " · " + text : qsTr("由测试开关开启（关闭这一行不会停止它）")
        return text
    }

    VRow {
        objectName: "pushfps-lowdelay-row"
        label: qsTr("更低延迟（每个真实帧配一个预测帧）")
        help: qsTr("真实画面提前一个显示帧出现：少等约三分之一个游戏帧。实验功能，尚未在任何电脑上实测：延迟减少多少、画面如何，都要你自己看。每三帧中有一帧是预测的：快速转动镜头、爆炸/烟雾/雨、闪光和渐变时，那一帧会停在真实画面上，看起来会有轻微顿挫；运动物体的前缘和细线（电线、栏杆）可能轻微抖动或变形。预测不好的帧会照常显示（不可靠的部分保持真实画面），只有长时间大部分不可用或帧开始迟到时才自动改回普通补帧。GPU 很吃紧时，个别帧的延迟可能反而比关闭时高。只用于采集卡、NVIDIA VFG、3× 补帧。开启和关闭都立即生效：补帧管线重建一次，画面可能停顿片刻。")
        hint: qsTr("看到抖动、撕裂或拖影就关掉这一行：关闭立即恢复原来的补帧方式。")
        hintBelow: true
        RowLayout {
            spacing: 8
            VSwitch {
                objectName: "pushfps-lowdelay"
                checked: row.on || row.kit
                onToggled: checked => veyra.setPreference("pushLowDelay", checked)
            }
            Text {
                objectName: "pushfps-lowdelay-experimental"
                text: qsTr("实验")
                color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
            }
        }
    }
    Text {
        objectName: "pushfps-lowdelay-status"
        visible: row.status.length > 0
        text: row.status
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.rightMargin: 24
        Layout.bottomMargin: 6
    }
}
