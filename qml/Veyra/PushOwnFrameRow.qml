// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "ownfg": the "Lower delay" row's two own-frame choices (inside PushLowDelayRow.qml, shown while that row is on).
//
// 1. Frame source (step 2): who makes the first frame of a predicted pair, the interpolated I(2/3): NVIDIA (VFG, the
//    default) or PushFPS (PushWarp, the project's own motion-compensated frame from the same motion the predicted frame
//    uses). Preference pushLowDelaySource: "auto" (unset: NVIDIA where VFG generates), "nvidia", "pushfps"; validated in
//    src/ui/bridge/Preferences.cpp, applied by src/ui/bridge/PushOwnFrame.cpp (the first choice of PushFPS in a run
//    rebuilds the graph once so that PushWarp exists: review 1, SHOULD 9). The source is the USER'S choice and nothing
//    else (review 1, MUST 3): when NVIDIA refuses, the mode is unavailable and this row only OFFERS the other source; when
//    PushFPS is chosen but cannot run here, the mode does not run (never NVIDIA's frame instead). With PushFlow at 3X
//    PushFPS is the only source and the row says so (experimental, not tested end to end).
// 2. Fully causal schedule (step 4), an EXPERIMENTAL switch, never a default: preference pushLowDelayCausal
//    (pipeline/PushCausal.h). Its text states the measured picture cost in plain words (offline, synthetic scenes) and
//    that nothing has been measured on a graphics card; no delay figure.
// Picture numbers: OFFLINE, synthetic 4K scenes, CPU, the implemented PushWarp rules (tools/pushwarp_eval, review 1
// SHOULD 7). Unverified on Windows.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: own
    objectName: "pushfps-lowdelay-source-rows"
    Layout.fillWidth: true
    spacing: 0

    property bool rowOn: false          // the Lower delay row (or the kit's switch) is on
    property var engine: ({})           // the row's engine facts (pushLowDelayState: state, reason, ownInUse, ownWarp, causal*)
    readonly property bool vfg: veyra.fgBackendName === "vfg"
    readonly property bool pushflow: veyra.fgBackendName === "pushflow"
    visible: own.rowOn && (own.vfg || own.pushflow)
    readonly property string stored: String(veyra.preferences.pushLowDelaySource || "auto")
    // What the choice shows: auto is NVIDIA with VFG (the generator's own frame stays the default).
    readonly property string shown: own.pushflow ? "pushfps" : own.stored === "pushfps" ? "pushfps" : "nvidia"
    readonly property bool causalOn: veyra.preferences.pushLowDelayCausal === true
    // PushWarp is in use by the user's choice: PushFPS chosen, PushFlow (its only source), or the causal schedule.
    readonly property bool warpChosen: own.shown === "pushfps" || own.causalOn

    readonly property string status: {
        const warp = String(own.engine.ownWarp || "none"), use = String(own.engine.ownInUse || "none")
        if (own.warpChosen && warp === "check-failed")
            return own.pushflow ? qsTr("PushFPS 帧在这块显卡上没有通过开启时的检查：更低延迟在这里不可用（原因见日志）")
                                : qsTr("PushFPS 帧在这块显卡上没有通过开启时的检查：选择 PushFPS 时更低延迟不可用（原因见日志）。改选 NVIDIA 可以继续使用。")
        if (own.warpChosen && warp === "too-slow")
            return own.pushflow ? qsTr("PushFPS 帧在这块显卡上太慢：更低延迟在这里不可用（原因见日志）")
                                : qsTr("PushFPS 帧在这块显卡上太慢：选择 PushFPS 时更低延迟不可用（原因见日志）。改选 NVIDIA 可以继续使用。")
        if (own.warpChosen && warp === "none" && String(own.engine.state || "") !== "")
            return qsTr("正在准备 PushFPS 帧：补帧管线重建并检查（连同运动估计最多 3 秒）")
        // NVIDIA chosen and refused: the mode is unavailable (PushLowDelayRow.qml says so); here only the offer.
        if (own.shown === "nvidia" && String(own.engine.state || "") === "unavailable" && String(own.engine.reason || "") === "vfg-refused")
            return qsTr("NVIDIA 的补帧不接受这个请求；你可以改选 PushFPS 自己的帧。")
        if (use === "chosen" || use === "not-vfg") return qsTr("现在由 PushFPS 生成")
        return ""
    }

    VRow {
        objectName: "pushfps-lowdelay-source-row"
        label: qsTr("插值帧来源")
        help: qsTr("预测帧对中的第一帧（两个真实帧之间的插值帧）由谁生成。NVIDIA（默认）：由 VFG 生成。PushFPS：PushFPS 自己的运动补偿帧，使用与预测帧相同的运动估计，不需要 NVIDIA。PushFPS 帧只在合成测试画面上比较过（离线测量）：在那里它平均比学习型插帧模型略差（约 0.6 dB），细小物体前后有运动时（栏杆后面的车辆）差约 4 dB，闪光和渐变时差约 3 dB。没有在游戏画面上测试过，也没有在任何真实显卡上运行过，它在你的显卡上的开销未知。只在想测试它时使用；看到细小物体撕裂或画面变晚，就改回 NVIDIA。第一次选择 PushFPS 时补帧管线重建一次并检查它（连同运动估计最多 3 秒）。NVIDIA 拒绝单独生成这一帧时，不会自动改用 PushFPS。")
        hint: own.pushflow ? qsTr("当前补帧方式下只有 PushFPS 可用（实验性，没有做过端到端测试）。") : ""
        hintBelow: true
        VSeg {
            objectName: "pushfps-lowdelay-source"
            enabled: own.vfg
            opacity: enabled ? 1 : 0.4
            options: [{ id: "nvidia", label: "NVIDIA" }, { id: "pushfps", label: "PushFPS" }]
            current: own.shown
            onPicked: id => veyra.setPreference("pushLowDelaySource", id)
        }
    }
    Text {
        objectName: "pushfps-lowdelay-source-status"
        visible: own.status.length > 0
        text: own.status
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.rightMargin: 24
        Layout.bottomMargin: 6
    }

    // ---- Step 4: the fully causal schedule (experimental) ----
    function causalExitText(reason) {
        switch (String(reason || "")) {
        case "unusable": return qsTr("预测多数不可靠")
        case "late": return qsTr("预测帧来不及")
        case "dear": return qsTr("预测帧在这块显卡上太慢")
        }
        return ""
    }
    readonly property string causalStatus: {
        if (!own.causalOn) return ""
        const e = own.engine, warp = String(e.ownWarp || "none")
        if (warp === "check-failed" || warp === "too-slow") return qsTr("PushFPS 帧在这里不可用：完全预测不会运行（原因见日志）")
        const stopped = String(e.causalStoppedFor || "none")
        if (stopped !== "none" && stopped !== "")
            return qsTr("完全预测已停用到这次播放结束（%1）：现在每个真实帧配一个预测帧（关闭再开启可再试）").arg(own.causalExitText(stopped))
        if (String(e.causalLevel || "") === "onepredicted")
            return qsTr("暂时退回到每个真实帧配一个预测帧：%1（%2/%3 对稳定后再试）").arg(own.causalExitText(e.causalLastExit))
                       .arg(Number(e.causalReturnHave || 0)).arg(Number(e.causalReturnNeed || 0))
        if (Number(e.causalPerSecond || 0) > 0) return qsTr("正在运行：每秒 %1 个帧对是真实帧在前、两帧预测").arg(Number(e.causalPerSecond))
        return qsTr("等待：更低延迟运行时才会使用")
    }
    VRow {
        objectName: "pushfps-lowdelay-causal-row"
        label: qsTr("完全预测（实验）")
        help: qsTr("真实画面最先显示，同一帧对里另外的帧都从它预测出来，不再等任何生成的帧（3× 时两个预测帧，流畅模式 2× 时一个）。画面代价是实际存在的：3× 时每三帧中有两帧是预测的。在合成测试画面上（离线测量，不是游戏画面），按真实帧之后的位置，第一个预测帧比学习型插帧模型的插值帧差约 %1 dB，第二个差约 %2 dB，物体位置偏差更大；从遮挡后面出现的东西无法预测，会停在真实画面上。没有在任何显卡上测量过，延迟能减少多少也没有测量。预测多数不可靠、预测帧来不及或在这块显卡上太慢时，先退回到每个真实帧配一个预测帧并在这里说明，不会直接跳回普通补帧；同一原因三次后，这次播放中不再尝试。使用 PushFPS 帧：第一次开启时补帧管线重建一次。").arg(own.causalCostP1).arg(own.causalCostP2)
        hintBelow: true
        RowLayout {
            spacing: 8
            VSwitch {
                objectName: "pushfps-lowdelay-causal"
                checked: own.causalOn
                onToggled: checked => veyra.setPreference("pushLowDelayCausal", checked)
            }
            Text {
                objectName: "pushfps-lowdelay-causal-experimental"
                text: qsTr("实验")
                color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
            }
        }
    }
    // The offline measurement of the implemented scheme (tools/pushwarp_eval; numbers rounded to 0.1 dB, no delay figure).
    readonly property string causalCostP1: "1.8"
    readonly property string causalCostP2: "3.9"
    Text {
        objectName: "pushfps-lowdelay-causal-status"
        visible: own.causalStatus.length > 0
        text: own.causalStatus
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.rightMargin: 24
        Layout.bottomMargin: 6
    }
}
