// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "s12-settings": the short grey status line under a frame-generation row, from the SAME facts as the log line
// [settings-effect] (PushEffectBoard.facts, engine/PushSettingsEffect.h): it says when a setting does nothing with the
// running method or source, what limits it, or that it is already in force, so no row offers a dead control silently
// (SETTINGS_AUDIT.md). Nothing when the setting runs as asked or is off. `setting` is a row name of
// pushSettingsEffectLine ("multiplier", "flow", "nvidiaLowLatency"...). pauseSeconds > 0 adds that a change rebuilds the
// picture pipeline (measured in his Package 16 log: about 1 s, or about 3 s when NVIDIA VFG is re-created).
// alsoDedup: the switch also turns duplicate removal on (ProPage.qml list-fg-adaptive / list-fg-pacing).
// Review 1 (MUST 2): a reason comes from MEASURED facts; "unknown" = asked, not measured in the last window.
import QtQuick
import QtQuick.Layouts

Text {
    id: note
    property var effect: ({})
    property string setting: ""
    property int pauseSeconds: 0
    property bool alsoDedup: false
    // The optical-flow group's note sits above all of its rows (review 1 MUST 5): it names how many of them it means.
    property int groupRows: 0
    readonly property var row: (note.effect && note.effect.rows && note.effect.rows[note.setting]) || ({})
    readonly property string why: String(note.row.why || "")
    // Review 1 SHOULD 7: facts measured before a change are not shown as the new state while it applies.
    readonly property bool applying: note.effect && note.effect.applying === true

    function modelName(letter) { return letter === "L" ? qsTr("低") : letter === "M" ? qsTr("中") : letter === "H" ? qsTr("高") : letter }
    // "P": the predicted frame of Lower delay (no VFG model; review 2 MUST 1).
    function positionName(index, letter) {
        return letter === "P" ? qsTr("第 %1 个生成帧是预测帧").arg(index) : qsTr("第 %1 个生成帧 %2 模型").arg(index).arg(note.modelName(letter))
    }
    // VFG quality: what the level means per generated-frame position at the running multiplier, and the measured GPU time.
    function positionsText(e) {
        const list = e.vfgPositions || []
        if (list.length === 0) return ""
        const parts = []
        for (let i = 0; i < list.length; ++i) {
            const ms = Number(list[i].gpuMs || 0)
            parts.push(note.positionName(i + 1, String(list[i].model)) + (ms > 0 ? " " + ms.toFixed(1) + " ms" : ""))
        }
        let s = (list.length + 1) + "X · " + parts.join(" · ")   // no translatable punctuation (review 2 MUST 2)
        const models = list.map(p => String(p.model)).filter(m => m !== "P")
        if (models.some(m => m !== models[0])) s += qsTr("（相邻的生成帧来自不同模型）")
        if (e.vfgFastUsed === true) s += qsTr("（部分帧对使用了更快的模型）")
        return s
    }
    function reasonText(why, e) {
        const x = Number(e.runningX || 0)
        switch (why) {
        case "own-motion": return note.groupRows > 0 ? qsTr("下面 %1 项光流设置对 %2 没有作用（它自己估算运动）").arg(note.groupRows).arg(e.backendLabel || "NVIDIA VFG")
                                                      : qsTr("对 %1 没有作用（它自己估算运动）").arg(e.backendLabel || "NVIDIA VFG")
        case "no-flow-consumer": return qsTr("现在没有环节使用光流，这些设置不起作用")
        case "other-method": return qsTr("只对其他补帧方式有效")
        case "file-playback-only": return qsTr("只在播放视频文件时有效，采集卡和串流不使用")
        case "nothing-to-repair": return qsTr("没有需要修复的：采集画面已是均匀的 %1 FPS").arg(Number(e.contentFps || 0).toFixed(0))
        case "content-clock": return qsTr("没有重复画面可去除；仍按游戏时钟安排帧（在一次测试中：更均匀，延迟约高 2 ms）")
        case "nothing-to-choose": return qsTr("没有可选的：%1X 已最接近目标").arg(x)
        case "needs-above-2x": return qsTr("需要高于 2X 的倍率")
        case "limited-by-smoothness": return qsTr("正在运行 %1X：流畅模式（目标 %2 FPS）限制").arg(x).arg(Number(e.smoothTargetFps || 0).toFixed(0))
        case "limited-by-cap": return qsTr("正在运行 %1X：输出上限 %2 FPS 限制").arg(x).arg(Number(e.capFps || 0).toFixed(0))
        case "limited-by-adaptive": return qsTr("正在运行 %1X：自适应倍率选择").arg(x)
        case "pairs-skipped": return qsTr("正在运行 %1X，但每个真实帧之后平均只显示了 %2 个生成帧（有些被跳过）").arg(x).arg(Number(e.generatedPerReal || 0).toFixed(2))
        case "already-by-vsync": return qsTr("已生效（显示同步为垂直同步时队列本来就是 1 帧）")
        case "needs-automatic-sync": return qsTr("只在显示同步为“自动”时使用；可变刷新由驱动决定")
        case "driver-vrr-off": return qsTr("已请求；NVIDIA 驱动上次报告此显示器没有启用可变刷新")
        case "driver-reports-off": return qsTr("已开启，但 NVIDIA 驱动现在报告低延迟模式未启用（一次测试中只在 PushFPS 全屏并在最前面时启用）")
        case "needs-2x": return qsTr("只在 2X 时有效")
        case "needs-single-model": return qsTr("混合质量档不会加载更快的模型，此开关不起作用")
        case "no-faster-model": return qsTr("“低”档没有更快的模型")
        case "needs-dedup-off": return qsTr("开启去除重复帧时不起作用")
        case "needs-dedup-on": return qsTr("需要开启去除重复帧")
        case "not-loaded": return qsTr("更快的模型没有加载（显存不足）")
        case "unavailable": return qsTr("没能在这台电脑上启动")
        case "unknown": return qsTr("已开启；最近几秒没有测到它在运行")
        case "not-measurable": return qsTr("已开启；PushFPS 无法测量这一项")
        case "works-at-lower-size": return qsTr("内部以 %1×%2 处理，以 %3×%4 显示").arg(e.enhanceWidth).arg(e.enhanceHeight).arg(e.outputWidth).arg(e.outputHeight)
        }
        return ""
    }
    readonly property bool calm: why === "running" || why === "content-clock" || why === "works-at-lower-size" || why === "not-measurable"
    readonly property string reason: (note.applying || why === "running" || why === "off" || why === "not-playing") ? "" : reasonText(why, note.effect || ({}))
    readonly property var parts: {
        const p = []
        if (note.applying && note.row.requested === true) p.push(qsTr("正在应用设置…"))
        if (note.reason.length > 0) p.push(note.reason)
        if (note.setting === "vfgQuality" && !note.applying && note.row.requested === true) {
            const pos = note.positionsText(note.effect || ({}))
            if (pos.length > 0) p.push(pos)
        }
        if (note.alsoDedup && note.row.requested === true) p.push(qsTr("也会开启去除重复帧"))
        if (note.pauseSeconds > 0) p.push(qsTr("更改时画面会暂停约 %1 秒").arg(note.pauseSeconds))
        return p
    }
    visible: note.parts.length > 0
    text: note.parts.join(" · ")
    // "amber" when a reason says the setting does not do what was asked, "grey" otherwise; the colour follows (tests read tone).
    readonly property string tone: note.reason.length > 0 && !note.calm ? "amber" : "grey"
    color: note.tone === "amber" ? Theme.warn : Theme.t3
    font.family: Theme.fontUi
    font.pixelSize: Theme.fsHint
    wrapMode: Text.Wrap
    Layout.fillWidth: true
    Layout.leftMargin: 4
    Layout.rightMargin: 24
}
