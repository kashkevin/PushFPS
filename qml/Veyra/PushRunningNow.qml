// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "s12-settings": "what is running now", 3-5 plain lines at the top of the Frame Generation card and in quick
// setup, from the facts of [settings-effect] (PushEffectBoard.facts) and the delay readout (veyra.pushLiveStats():
// latencyMs / latencyWaitMs, the same numbers as the badge): the method, the multiplier that RUNS and why, generated
// frames shown per real frame in the last seconds, which NVIDIA model makes each generated frame, the share of the delay
// that is the interpolation schedule, and the HUD lock state. liveSource lets a test hand in the live statistics.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: now
    objectName: "pushfps-running-now"
    property var effect: ({})
    property var liveSource: null
    property var live: ({})
    spacing: 2
    Layout.fillWidth: true
    Timer {
        interval: 1000; repeat: true; triggeredOnStart: true
        running: now.visible
        onTriggered: now.live = now.liveSource !== null ? now.liveSource()
                     : (typeof veyra.pushLiveStats === "function" ? (veyra.pushLiveStats() || {}) : {})
    }
    readonly property bool playing: now.effect && now.effect.playing === true
    readonly property var multiplierRow: (now.effect && now.effect.rows && now.effect.rows.multiplier) || ({})
    readonly property int runningX: Number(now.effect.runningX || 0)
    function modelName(letter) { return letter === "L" ? qsTr("低") : letter === "M" ? qsTr("中") : letter === "H" ? qsTr("高") : letter === "P" ? qsTr("预测帧") : letter }
    readonly property bool applying: now.effect && now.effect.applying === true
    readonly property string line1: {
        if (!now.playing) return qsTr("开始播放后，这里显示实际运行的补帧")
        if (now.applying) return qsTr("正在应用设置…")
        if (now.multiplierRow.requested !== true) return qsTr("补帧关闭")
        const why = String(now.multiplierRow.why || "")
        let s = qsTr("%1 · 正在运行 %2X").arg(now.effect.backendLabel || "").arg(now.runningX)
        if (why === "limited-by-smoothness") s += " · " + qsTr("设定 %1X，流畅模式（目标 %2 FPS）限制").arg(now.effect.requestedX).arg(Number(now.effect.smoothTargetFps || 0).toFixed(0))
        else if (why === "limited-by-cap") s += " · " + qsTr("设定 %1X，输出上限 %2 FPS 限制").arg(now.effect.requestedX).arg(Number(now.effect.capFps || 0).toFixed(0))
        else if (why === "limited-by-adaptive") s += " · " + qsTr("设定 %1X，自适应倍率选择").arg(now.effect.requestedX)
        else if (why === "unknown") s += " · " + qsTr("最近几秒没有显示生成帧")
        return s
    }
    // Review 1 SHOULD 7: only from a window with enough real frames (the window restarts every 10 s).
    readonly property string line2: {
        if (!now.playing || now.applying || now.multiplierRow.requested !== true) return ""
        const g = Number(now.effect.generatedPerReal)
        let s = g >= 0 && Number(now.effect.realPresented || 0) >= 30 ? qsTr("最近几秒：每个真实帧之后显示 %1 个生成帧").arg(g.toFixed(2)) : ""
        const positions = now.effect.vfgPositions || []
        if (now.effect.vfg === true && positions.length > 0) {
            const parts = positions.map(p => Number(p.gpuMs || 0) > 0 ? now.modelName(p.model) + " " + Number(p.gpuMs).toFixed(1) + " ms" : now.modelName(p.model))
            s += (s.length ? " · " : "") + qsTr("各生成帧的模型（按质量档）：%1").arg(parts.join(" / "))
        }
        return s
    }
    // Review 1 SHOULD 9: the wait is (M-1)/M of the game frame only for the interpolation schedule (not Lower delay, not XeSS).
    readonly property string line3: {
        if (!now.playing || now.applying || now.multiplierRow.requested !== true || now.runningX < 2) return ""
        if (now.effect.lowDelayActive === true || now.effect.presentSinkFg === true) return ""
        const total = Number(now.live.latencyMs), wait = Number(now.live.latencyWaitMs)
        if (!(total > 0) || !(wait >= 0) || typeof now.live.latencyWaitMs !== "number") return ""
        return qsTr("延迟 %1 ms，其中 %2 ms 是真实帧排在 %3 个生成帧之后等待显示：%4X 时固定").arg(total.toFixed(0)).arg(wait.toFixed(0)).arg(now.runningX - 1).arg(now.runningX)
    }
    readonly property string line4: {
        if (!now.playing || now.applying) return ""
        const r = (now.effect.rows && now.effect.rows.hudLock) || ({})
        if (r.requested !== true) return ""
        const why = String(r.why || "")
        // "running" only when its compose dispatches were counted in the window (review 1 MUST 2).
        return why === "running" ? qsTr("实时 HUD 锁定：运行中") : why === "unavailable" ? qsTr("实时 HUD 锁定：没能启动") : qsTr("实时 HUD 锁定：未运行")
    }
    Repeater {
        model: [now.line1, now.line2, now.line3, now.line4].filter(t => t.length > 0)
        Text {
            objectName: "pushfps-running-now-line-" + index
            required property string modelData
            required property int index
            text: modelData
            color: index === 0 ? Theme.t1 : Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: index === 0 ? Theme.fsSmall : Theme.fsHint
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
