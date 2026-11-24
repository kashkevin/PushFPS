// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Performance mode row of the quick setup (pushfps-mode): the four modes of
// include/veyra/engine/PushPerformanceMode.h and "off", one line that says what the chosen mode does for
// the game's frame rate and how much delay its frame generation adds at the least, and the LAN profile
// switch. Choosing a mode writes existing settings through PushModes.js; "off" puts back what was set
// before. Preferences: pushMode ("" = off), pushModeRestore (the settings from before), pushLanProfile
// (the bridge turns it into VEYRA_EXP_RP_LAN_PROFILE for the next PS5 connection), pushModeAuto.
// Following a changed source rate and Automatic are PushModeDriver.qml's (one instance, in Main.qml).
// "Recommended settings" opens PushCalibrationPanel.qml below the row (the first-launch recommendation,
// applied only by its Apply). Nothing here runs or changes anything until a mode is picked. Unverified on Windows.
import QtQuick
import QtQuick.Layouts
import "PushModes.js" as Modes

ColumnLayout {
    id: row
    objectName: "pushfps-mode-rows"
    Layout.fillWidth: true
    spacing: 0

    readonly property var choices: [
        { id: "", label: qsTr("关闭（自己的设置）") },
        { id: "ultra-low-latency", label: qsTr("超低延迟") },
        { id: "competitive", label: qsTr("竞技") },
        { id: "balanced", label: qsTr("均衡") },
        { id: "maximum-smoothness", label: qsTr("最大流畅") }
    ]
    function label(id) { const c = row.choices.find(x => x.id === id); return c ? c.label : row.choices[0].label }
    // The chosen mode; with Automatic it is the ceiling.
    readonly property string mode: {
        const id = String(veyra.preferences.pushMode || "")
        return Modes.isMode(id) ? id : ""
    }
    readonly property bool lan: veyra.preferences.pushLanProfile === true
    // Automatic exists only when PushFPS was started with VEYRA_EXP_AUTO_MODE=1.
    property bool autoPossible: typeof veyra.pushAutoModeAvailable === "function" && veyra.pushAutoModeAvailable() === true
    readonly property bool autoOn: row.autoPossible && veyra.preferences.pushModeAuto === true
    property var autoState: ({})
    // The mode whose settings are in place: Automatic may have stepped below the ceiling.
    readonly property string effective: row.autoOn && Modes.isMode(String(row.autoState.mode || "")) ? String(row.autoState.mode) : row.mode

    // Live numbers, only looked at while a mode is on and something plays.
    property var stats: ({})
    Timer {
        interval: 2000; repeat: true
        running: row.visible && row.mode !== "" && veyra.hasSource === true
        triggeredOnStart: true
        onTriggered: {
            row.stats = veyra.pushLiveStats() || ({})
            if (row.autoOn && typeof veyra.pushAutoModeState === "function") row.autoState = veyra.pushAutoModeState() || ({})
        }
        onRunningChanged: if (!running) { row.stats = ({}); row.autoState = ({}) }
    }
    // The source rate the line below is written for: measured when it is a rate the table has a row for,
    // else the game frame rate chosen two rows down.
    readonly property int chosenFps: {
        const n = Number(veyra.preferences.pushGameFps || 30)
        return n === 40 || n === 60 ? n : 30
    }
    readonly property bool measured: Modes.measuredFps(row.stats) > 0
    readonly property real sourceFps: row.measured ? Modes.measuredFps(row.stats) : row.chosenFps
    // What the mode in place does for this source and this PC: { plan, multiplier, delayMs, outputFps, notes }.
    readonly property var shown: row.effective !== "" ? Modes.preview(veyra, row.effective, row.sourceFps) : null
    // Frame generation is no longer what the mode set (changed in another row or in Professional mode).
    readonly property bool changedByHand: row.shown !== null && !row.autoOn
        && ((veyra.fgEnabled === true) !== (row.shown.multiplier > 1)
            || (row.shown.multiplier > 1 && Number(veyra.fgMultiplier) !== row.shown.multiplier))

    function noteText(note) {
        const hz = Math.round(Number(veyra.displayRefreshHz || 0))
        switch (note) {
        case "bad-source-rate": return qsTr("模式只有 30 和 60 FPS 游戏的设置，所以不补帧")
        case "bad-display-rate": return qsTr("还不知道屏幕的刷新率，所以先不补帧")
        case "display-limit": return qsTr("屏幕是 %1 Hz：倍率已降低，输出不会超过刷新率").arg(hz)
        case "backend-limit": return qsTr("当前补帧方式达不到这个模式的倍率，已改用它能提供的最高倍率")
        case "fg-unavailable": return qsTr("这台电脑现在不能补帧，所以不补帧")
        case "tearing": return qsTr("驱动没有确认可变刷新可用：允许撕裂，不等屏幕刷新")
        case "uneven": return qsTr("驱动没有确认可变刷新可用：这个帧率在 %1 Hz 屏幕上每帧显示的时间不一样长").arg(hz)
        }
        return ""
    }
    // The one-line disclosure: the least delay the mode's frame generation adds for this source rate,
    // (M-1)/M of a source interval, which is arithmetic and not a measurement.
    readonly property string disclosure: {
        if (row.shown === null) return qsTr("不改动任何设置。选一个模式，自动设好补帧倍率、去重和显示方式")
        const fps = Modes.cadence(row.sourceFps) > 0 ? Modes.cadence(row.sourceFps) : Math.round(row.sourceFps)
        const head = row.shown.multiplier > 1
            ? qsTr("%1 FPS 游戏：%2× 补帧到 %3 FPS，每张真实画面至少晚 %4 毫秒显示").arg(fps).arg(row.shown.multiplier)
                  .arg(Math.round(row.shown.outputFps)).arg(Number(row.shown.delayMs).toFixed(1))
            : qsTr("%1 FPS 游戏：不补帧，补帧延迟为 0").arg(fps)
        const parts = [head + (row.measured ? qsTr("（帧率为实测）") : qsTr("（帧率来自「游戏帧率」）"))]
        for (const note of row.shown.notes) { const text = row.noteText(note); if (text.length > 0) parts.push(text) }
        if (row.changedByHand) parts.push(qsTr("补帧设置之后被手动改过；再选一次模式可以重新应用"))
        return parts.join(" · ")
    }
    readonly property string lanLine: row.lan ? qsTr("局域网优化已开启：下次连接 PS5 时生效") : ""
    // The mode's experiment switches (input poll, pair phase; Automatic: connection health) changed in the
    // environment: they are read when the next PS5 connection starts (PushModes.syncSwitches).
    property var switchState: null
    readonly property string switchLine: Modes.switchesPending(row.switchState) ? qsTr("这个模式的手柄轮询、补帧相位和连接健康检测设置从下次连接 PS5 起生效") : ""

    // result: what PushModes.select reports; null when the settings from before could not be saved.
    property var lastResult: null
    function pick(id) {
        const live = veyra.hasSource === true ? (veyra.pushLiveStats() || ({})) : ({})
        row.stats = live
        row.lastResult = Modes.select(veyra, Modes.isMode(id) ? id : "", Modes.sourceFps(veyra, live))
        if (row.lastResult !== null && Modes.switchesPending(row.lastResult.switches)) row.switchState = row.lastResult.switches
        return row.lastResult !== null
    }

    VRow {
        objectName: "pushfps-mode-row"
        label: qsTr("性能模式")
        help: qsTr("一次选好补帧倍率、去除重复画面和显示方式，不用逐项设置。「超低延迟」和「竞技」优先反应速度；「均衡」和「最大流畅」生成更多画面，但生成的帧要等下一张真实画面到达才能算出来，所以每张真实画面会晚一点显示。选「关闭」恢复之前的设置。「局域网优化」是实验功能：主机和电脑在同一个局域网时缩短等待丢失数据包的时间并加大接收缓冲，在下次连接 PS5 时生效")
        hint: row.disclosure + (row.lanLine.length > 0 ? " · " + row.lanLine : "") + (row.switchLine.length > 0 ? " · " + row.switchLine : "")
        hintBelow: true
        RowLayout {
            spacing: 8
            VSelect {
                objectName: "pushfps-mode"
                implicitWidth: 170
                options: row.choices
                value: row.label(row.mode)
                onPicked: id => row.pick(id)
            }
            Text {
                text: qsTr("局域网优化")
                color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: 12
            }
            VSwitch {
                objectName: "pushfps-mode-lan"
                checked: row.lan
                onToggled: checked => veyra.setPreference("pushLanProfile", checked)
            }
            VButton {
                objectName: "pushfps-mode-recommended"
                ghost: true
                text: row.showRecommended ? qsTr("收起推荐") : qsTr("推荐设置")
                onClicked: row.toggleRecommended()
            }
        }
    }
    // "Recommended settings" (PushCalibrationPanel.qml), opened from here at any time; the plan is asked for
    // when it opens. Nothing changes without its Apply.
    property bool showRecommended: false
    function toggleRecommended() {
        row.showRecommended = !row.showRecommended
        if (row.showRecommended) recommended.load()
    }
    PushCalibrationPanel {
        id: recommended
        objectName: "pushfps-mode-recommended-panel"
        host: "mode"
        visible: row.showRecommended
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        Layout.bottomMargin: 8
    }

    VRow {
        objectName: "pushfps-mode-auto-row"
        visible: row.autoPossible
        label: qsTr("自动调整模式")
        hint: row.mode === "" ? qsTr("先选一个模式作为上限")
            : row.autoOn && row.effective !== row.mode ? qsTr("现在是「%1」，上限「%2」。运行稳定一段时间后才会回到更高的模式").arg(row.label(row.effective)).arg(row.label(row.mode))
            : (qsTr("实验功能：显卡跟不上、画面排队或网络不稳时自动降到延迟更低的模式，最高不超过上面选的模式")
               + (row.switchLine.length > 0 ? " · " + row.switchLine : ""))
        hintBelow: true
        VSwitch {
            objectName: "pushfps-mode-auto"
            enabled: row.mode !== ""
            opacity: enabled ? 1 : 0.4
            checked: row.autoOn
            onToggled: checked => {
                veyra.setPreference("pushModeAuto", checked)
                veyra.setPreference("pushModeSwitches", true)   // a button press: the mode's switches are wanted now
                const result = Modes.syncSwitches(veyra)
                if (Modes.switchesPending(result)) row.switchState = result
            }
        }
    }
}
