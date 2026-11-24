// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// SmartLink "Connection" row of the quick setup (pushfps-smartlink-row), experimental. Shown only in a
// Slipstream build with PS5 Remote Play (bridge pushSmartLink().available) and only for a PS5 Remote Play
// source, or with nothing playing while the console profile is a PS5. One choice, preference pushSmartLink:
// 0 off (the default: the app sets nothing), 1 advise, 2 automatic. The bridge turns it into
// VEYRA_EXP_RP_SMARTLINK ("1" / "apply") for the NEXT PS5 connection (src/ui/bridge/PushSmartLink.cpp); a
// running stream keeps what it started with. While a PS5 stream runs and SmartLink's state is fresh, one status
// line says which connection was detected in plain words, one sentence of advice (SmartLinkTypes.h Advice), and
// in Automatic what it changed, only when it changed something. Nothing here changes a Windows, adapter or
// router setting, and nothing here claims an improvement. Unverified on Windows and on a PS5.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: row
    objectName: "pushfps-smartlink-rows"
    Layout.fillWidth: true
    spacing: 0

    // The build has SmartLink's interface (Slipstream build with PS5 Remote Play). Read once: the row must not
    // come and go while the program runs.
    property bool available: false
    function checkAvailable() {
        const s = typeof veyra.pushSmartLink === "function" ? (veyra.pushSmartLink() || ({})) : ({})
        row.available = s.available === true
    }
    Component.onCompleted: row.checkAvailable()

    readonly property bool streaming: veyra.sourceKind === "ps5" && veyra.hasSource === true
    // A PS5 Remote Play source, or nothing playing with a PS5 console profile (the choice is made before connecting).
    readonly property bool ps5: {
        if (veyra.sourceKind === "ps5") return true
        if (veyra.hasSource === true) return false
        const c = String(veyra.preferences.pushConsole || "ps5")
        return c === "ps5" || c === "ps5pro"
    }
    visible: row.available && row.ps5

    readonly property int choice: {
        const n = Number(veyra.preferences.pushSmartLink || 0)
        return n === 1 || n === 2 ? n : 0
    }
    readonly property var choices: [
        { id: "0", label: qsTr("关闭", "off") },
        { id: "1", label: qsTr("建议") },
        { id: "2", label: qsTr("自动") }
    ]
    function pick(id) {
        const n = Number(id)
        return veyra.setPreference("pushSmartLink", n === 1 || n === 2 ? n : 0)
    }

    // What the running stream's SmartLink reports (bridge pushSmartLink), once a second while a PS5 stream runs.
    property var link: ({})
    function refresh() { row.link = typeof veyra.pushSmartLink === "function" ? (veyra.pushSmartLink() || ({})) : ({}) }
    Timer {
        objectName: "pushfps-smartlink-timer"
        interval: 1000; repeat: true
        running: row.visible && row.streaming
        triggeredOnStart: true
        onTriggered: row.refresh()
        onRunningChanged: if (!running) row.link = ({})
    }
    readonly property bool active: row.streaming && row.link.active === true

    function profileText(id) {
        switch (String(id)) {
        case "wired": return qsTr("有线")
        case "strong-wifi": return qsTr("Wi-Fi 信号良好")
        case "unstable-wifi": return qsTr("Wi-Fi 不稳定")
        }
        return qsTr("正在检查…")
    }
    // SmartLinkTypes.h Advice: 0 None, 1 LinkIsFine, 2 UseCable, 3 MoveCloserOrFasterBand,
    // 4 LowerPictureRateNextTime, 5 RaiseRateNextTime, 6 BackgroundActivity.
    function adviceText(n) {
        switch (Number(n)) {
        case 1: return qsTr("连接没有问题。")
        case 2: return qsTr("用网线连接会让串流更稳定。")
        case 3: return qsTr("靠近路由器，或改用路由器的 5 GHz 网络。")
        case 4: return qsTr("下次可以先用较低的画质开始。")
        case 5: return qsTr("这个连接下次可以承载更高的画质。")
        case 6: return qsTr("这台电脑或网络上的某个程序每隔几秒会让 Wi-Fi 暂停一下。")
        }
        return ""
    }
    // Automatic only, and only when SmartLink changed something.
    readonly property var changes: {
        if (!row.active || row.link.mode !== "apply") return []
        const out = []
        if (Number(row.link.reorderMs || 0) > 0) out.push(qsTr("正在稍微多等一下迟到的画面数据"))
        if (Number(row.link.audioTargetMs || 0) > 0) out.push(qsTr("正在多预留一点声音"))
        return out
    }
    readonly property string status: {
        if (!row.active) return ""
        const parts = [qsTr("检测到的连接：%1").arg(row.profileText(row.link.profile))]
        const advice = row.adviceText(row.link.advice)
        if (advice.length > 0) parts.push(advice)
        for (const c of row.changes) parts.push(c)
        return parts.join(" · ")
    }

    readonly property string choiceHint: row.choice === 2 ? qsTr("检查连接，并可以调整等待时间")
        : row.choice === 1 ? qsTr("检查连接并给出建议，不改变任何设置")
        : qsTr("不检查连接，一切保持原样")
    readonly property string nextLine: row.streaming ? qsTr("更改在下次连接 PS5 时生效") : ""

    VRow {
        objectName: "pushfps-smartlink-row"
        label: qsTr("网络连接")
        help: qsTr("「自动」检查串流的连接，并可以调整等待迟到的画面数据和预留声音的时间。它从不改变 Windows、网卡或路由器的任何设置。")
        hint: row.choiceHint + (row.nextLine.length > 0 ? " · " + row.nextLine : "")
        hintBelow: true
        RowLayout {
            spacing: 8
            VSeg {
                objectName: "pushfps-smartlink"
                options: row.choices
                current: String(row.choice)
                onPicked: id => row.pick(id)
            }
            Text {
                objectName: "pushfps-smartlink-experimental"
                text: qsTr("实验")
                color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
            }
        }
    }
    Text {
        objectName: "pushfps-smartlink-status"
        visible: row.status.length > 0
        text: row.status
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.rightMargin: 24
        Layout.bottomMargin: 6
    }
}
