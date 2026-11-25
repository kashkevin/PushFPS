// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Package 15 "Lower delay" row of the quick setup (pushfps-lowdelay-row), experimental and off by default. One switch,
// preference pushLowDelay (validated in src/ui/bridge/Preferences.cpp). On, a capture card's pairs at 3X with NVIDIA VFG
// may run as I(2/3), B, P: one shown frame in three is predicted from the motion so far, and the real frame is shown one
// output slot earlier (include/veyra/engine/PushLowDelay.h, docs/drafts/LOW_DELAY_MODE.md; not measured on any PC). A bad prediction is still shown (its unreliable parts keep the real picture); the mode
// switches itself off only when prediction is bad most of the time, predicted frames start arriving late or cost more
// than a slot on the GPU (then after a long calm run it tries again; after three such exits it stays off for the
// playback), or when skipped / held pairs become frequent (Package 16: an isolated one no longer ends it). Works with Smoothness Mode (pauses below 3X) and Live HUD Lock.
// Package 16 (forensics F5): the status line is made only from the engine's live counters of the last second
// (predictedPerSecond, pairsPerSecond, lastStop, lastStopAgeS, calmHave/calmNeed, zeroSeconds): "Running" only when
// predicted pairs ran in that second, counting the predicted frames actually shown (shownPerSecond); never a projected
// delay. Shown only while a capture card plays and NVIDIA VFG and
// 3X are available.
// One click applies in the running session, on and off. Only the FIRST switch-on of a run rebuilds the frame-generation
// graph with the predicted-frame pass (and runs the motion source's self-check once; it stops itself after about 3 s), which can pause the
// picture for a moment; the status line says "starting" / "checking" until the rebuilt graph runs. The pass is then kept
// for the run (pushLowDelayKeepPass), so later switches only start or stop asking for predicted pairs, without a rebuild.
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
        onTriggered: { row.refresh(); row.startingSeconds = String(row.engine.state || "") === "starting" ? row.startingSeconds + 1 : 0 }
        onRunningChanged: if (!running) { row.engine = ({}); row.startingSeconds = 0 }
    }
    // Package 16: seconds the engine has reported "starting" in a row (the rebuild that adds the pass did not come).
    property int startingSeconds: 0

    function notHereText(reason) {
        switch (String(reason || "")) {
        case "format": return qsTr("不能在这里运行：当前输出格式不支持（需要 SDR 或 HDR10）")
        case "present-sink-fg": return qsTr("不能在这里运行：当前补帧方式自己负责呈现")
        case "not-3x": return qsTr("暂停：当前实际倍数不是 3×（流畅模式、自适应倍率或输出上限），回到 3× 一秒后自动继续")
        case "flow-too-slow": return qsTr("不在这里运行：运动估计在这块显卡上太慢（开启时的检查测得，原因见日志）")
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
    // Package 16: why the schedule last stopped (PushLowDelayExit names), in plain words.
    function stopText(reason) {
        switch (String(reason || "")) {
        case "skipped": return qsTr("GPU 跳过了较多帧对")
        case "held": return qsTr("补帧暂时停止")
        case "refused": return qsTr("这一对帧无法预测（画面重新开始或运动尚未就绪）")
        case "unusable": return qsTr("预测多数不可靠")
        case "late": return qsTr("预测帧来不及")
        case "dear": return qsTr("预测帧在这块显卡上太慢")
        case "ineligible": return qsTr("设置改变或倍数不是 3×")
        case "off": return qsTr("这一行被关闭")
        }
        return ""
    }
    // Running / waiting / not running, from the counters of the last second only.
    function liveText() {
        const e = row.engine
        const predicted = Number(e.predictedPerSecond || 0), pairs = Number(e.pairsPerSecond || 0)
        // Review 1 SHOULD 11: the predicted frames actually shown (a frame dropped late is not counted).
        const shown = e.shownPerSecond === undefined ? predicted : Number(e.shownPerSecond || 0)
        if (predicted > 0)
            return qsTr("正在运行：每秒显示 %1 个预测帧（%2% 的帧对）").arg(shown).arg(pairs > 0 ? Math.round(100 * predicted / pairs) : 0)
        const stop = row.stopText(e.lastStop)
        if (Number(e.zeroSeconds || 0) >= 20)
            return qsTr("没有运行：%1。补帧照常。").arg(stop.length > 0 ? stop : qsTr("画面一直没有稳定"))
        // In the mode but no pair ran predicted in that second (skipped or paused pairs): no entry progress to show.
        if (String(e.state || "") === "active") return qsTr("正在运行：这一秒没有帧对用上预测")
        let text = qsTr("等待中：%1/%2 对帧稳定").arg(Number(e.calmHave || 0)).arg(Number(e.calmNeed || 0))
        if (stop.length > 0 && Number(e.lastStopAgeS) >= 0) text += qsTr("；上次停止：%1（%2 秒前）").arg(stop).arg(Number(e.lastStopAgeS))
        return text
    }
    readonly property string status: {
        if (!row.on && !row.kit) return ""
        if (!row.runsHere) return qsTr("需要 NVIDIA VFG 补帧，倍数设为 3×")
        let text = ""
        // The motion source's start-up self-check (once per run for this graphics card and size; it stops itself after about 3 s).
        if (row.engine.checking === true) return qsTr("正在检查：这块显卡上的运动估计（每次运行 PushFPS 只检查一次；约 3 秒后自行停止）")
        // Review 1 SHOULD 4 / 5: stopped for the rest of this playback after three sustained exits of the same reason.
        const stopped = String(row.engine.stoppedFor || "")
        if (stopped.length > 0 && stopped !== "none")
            return qsTr("已停用到这次播放结束：%1。补帧照常（关闭再开启这一行可再试）").arg(row.stopText(stopped))
        switch (String(row.engine.state || "")) {
        case "active": case "calm": text = row.liveText(); break
        case "idle": case "": text = qsTr("等待画面"); break
        case "starting": text = row.startingSeconds >= 5 ? qsTr("没有运行：补帧管线还没有重建（原因见日志）")
                                                        : qsTr("正在开启：补帧管线重建中（画面可能停顿片刻）"); break
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
        help: qsTr("真实画面提前一个显示帧出现。实验功能，尚未在任何电脑上实测：延迟减少多少、画面如何，都要你自己看。每三帧中有一帧是预测的：快速转动镜头、爆炸/烟雾/雨、闪光和渐变时，那一帧会停在真实画面上，看起来会有轻微顿挫；运动物体的前缘和细线（电线、栏杆）可能轻微抖动或变形。预测不好的帧会照常显示（不可靠的部分保持真实画面），只有长时间大部分不可用、预测帧开始迟到、预测帧在这块显卡上太慢，或画面经常中断（跳过或停止的帧对）时，才自动改回普通补帧；同一原因连续三次后，这次播放中不再尝试。GPU 很吃紧时，个别帧的延迟可能反而比关闭时高。只用于采集卡、NVIDIA VFG、3× 补帧。开启和关闭都立即生效：本次运行第一次开启时补帧管线重建一次并检查运动估计（约 3 秒后自行停止），画面可能停顿片刻；之后再开关不再重建。")
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
