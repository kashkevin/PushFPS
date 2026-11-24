// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// First-launch card on the home page: what PushFPS found on this PC, a suggestion for a first test, the
// recommended settings with Apply / Undo (PushCalibrationPanel.qml; nothing changes without Apply), and
// the way to the PS5 pairing dialog. Shown on a new install until it is dismissed once (preference
// pushWelcomeDone; an install with earlier PushFPS preferences is marked done at once, PushModes.settleWelcome); it
// sits in the page like any other card, so nothing waits for it.
//
// Everything on it is a capability the PC reported (pushHardwareSummary in PushSlipstream.cpp, and the
// display's live refresh rate and variable refresh state): which decode profiles the adapter lists, which
// frame generation methods can run, the refresh rate whatever it is, which network links are up. The GPU's
// name is shown and never compared with anything. What could not be read says "not detected".
// Unverified on Windows.
import QtQuick
import QtQuick.Layouts
import "PushModes.js" as Modes

Rectangle {
    id: card
    objectName: "pushfps-welcome"
    readonly property bool wanted: veyra.preferences.pushWelcomeDone !== true
    visible: wanted
    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + 28
    radius: 12
    color: Theme.card2
    border.width: 1
    border.color: Theme.stroke

    // pushHardwareSummary(), asked once, a moment after the card first shows (never during start-up).
    property var hardware: ({})
    property bool loaded: false
    function load() {
        card.loaded = true
        if (typeof veyra.pushHardwareSummary === "function") card.hardware = veyra.pushHardwareSummary() || ({})
        recommended.load()
    }
    Timer {
        objectName: "pushfps-welcome-timer"
        interval: 1500
        running: card.visible && !card.loaded
        onTriggered: card.load()
    }
    // The PS5 source exists in this build (the same test as the home page's source card).
    property bool pairingPossible: !!veyra.ps5 && veyra.ps5.available === true

    readonly property string none: qsTr("未检测到")
    readonly property string gpuText: String(card.hardware.gpuName || "").length > 0 ? String(card.hardware.gpuName) : card.none
    readonly property string decodeText: {
        if (card.hardware.decodeKnown !== true) return card.none
        const list = []
        if (card.hardware.decodeH264 === true) list.push("H.264")
        if (card.hardware.decodeHevc === true) list.push("HEVC")
        if (card.hardware.decodeHevc10 === true) list.push("HEVC 10-bit")
        return list.length > 0 ? list.join(" · ") : qsTr("显卡没有列出 H.264 或 HEVC 解码，串流会用处理器解码")
    }
    readonly property string fgText: {
        const list = Array.isArray(card.hardware.fgBackends) ? card.hardware.fgBackends : []
        if (list.length === 0) return card.loaded && card.hardware.fgBackends !== undefined ? qsTr("没有可用的补帧方式") : card.none
        return list.length <= 3 ? list.join(" · ") : list.slice(0, 3).join(" · ") + qsTr(" 等 %1 种").arg(list.length)
    }
    // Live: the screen PushFPS is on, at whatever rate it runs.
    readonly property real displayHz: Number(veyra.displayRefreshHz || 0)
    readonly property int vrr: Number(veyra.vrrState || 0)
    readonly property string displayText: {
        const rate = card.displayHz > 0 ? Math.round(card.displayHz) + " Hz" : card.none
        const sync = card.vrr >= 4 ? qsTr("可变刷新生效中") : card.vrr === 3 ? qsTr("可变刷新可用")
                   : card.vrr === 2 ? qsTr("可变刷新当前不可用") : card.vrr === 1 ? qsTr("不支持可变刷新") : qsTr("可变刷新无法检测")
        return rate + " · " + sync
    }
    readonly property bool wifiOnly: card.hardware.networkKnown === true && card.hardware.wifi === true && card.hardware.ethernet !== true
    readonly property string networkText: {
        if (card.hardware.networkKnown !== true) return card.none
        const link = card.hardware.ethernet === true && card.hardware.wifi === true ? qsTr("有线网络和 Wi-Fi")
                   : card.hardware.ethernet === true ? qsTr("有线网络") : card.hardware.wifi === true ? "Wi-Fi" : qsTr("其他网络")
        if (card.hardware.routeKnown !== true) return link
        return link + " · " + (card.hardware.routeDirectLan === true ? qsTr("已保存的 PS5 在同一个局域网") : qsTr("已保存的 PS5 不在同一个局域网"))
    }
    readonly property var facts: [
        { k: qsTr("显卡"), v: card.gpuText },
        { k: qsTr("硬件解码"), v: card.decodeText },
        { k: qsTr("补帧方式"), v: card.fgText },
        { k: qsTr("屏幕"), v: card.displayText },
        { k: qsTr("网络"), v: card.networkText }
    ]
    readonly property bool lowLatencyChosen: {
        const id = String(veyra.preferences.pushMode || "")
        return id === "ultra-low-latency"
    }
    function chooseLowLatency() { return Modes.select(veyra, "ultra-low-latency", Modes.chosenFps(veyra)) !== null }
    function dismiss() { veyra.setPreference("pushWelcomeDone", true) }
    // Existing installs (PushFPS preferences from an earlier run) never get the card (PushModes.settleWelcome).
    Component.onCompleted: Modes.settleWelcome(veyra)

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            spacing: 9
            VIcon { name: "info"; size: 14; color: Theme.accent }
            Text {
                Layout.fillWidth: true
                text: qsTr("这台电脑的检测结果")
                color: Theme.t1
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsH3
                font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
                wrapMode: Text.WordWrap
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 14
            rowSpacing: 4
            Repeater {
                model: card.facts.length * 2
                delegate: Text {
                    required property int index
                    readonly property var entry: card.facts[Math.floor(index / 2)]
                    readonly property bool isValue: index % 2 === 1
                    objectName: "pushfps-welcome-" + (isValue ? "value-" : "label-") + Math.floor(index / 2)
                    Layout.fillWidth: isValue
                    Layout.alignment: Qt.AlignTop
                    text: isValue ? entry.v : entry.k
                    color: isValue ? Theme.t1 : Theme.t3
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsHint
                    wrapMode: Text.WordWrap
                }
            }
        }
        Text {
            objectName: "pushfps-welcome-advice"
            Layout.fillWidth: true
            text: qsTr("第一次测试建议先用「超低延迟」：不补帧，操作反应最快。画面和手感都正常后，再在快速设置里试其他模式")
                  + (card.wifiOnly ? " · " + qsTr("电脑现在只连着 Wi-Fi：PS5 串流建议电脑和主机都用网线") : "")
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsHint
            lineHeight: 1.1
            wrapMode: Text.WordWrap
        }
        // The recommendation with Apply / Keep my settings / Details (PushCalibrationPanel.qml). Once the user
        // kept their settings it is no longer offered here; after Apply it stays for its Undo until dismissed.
        PushCalibrationPanel {
            id: recommended
            objectName: "pushfps-welcome-recommended"
            host: "welcome"
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: recommended.loaded && (!recommended.kept || recommended.record !== null)
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            VButton {
                objectName: "pushfps-welcome-mode"
                primary: !card.lowLatencyChosen
                enabled: !card.lowLatencyChosen
                text: card.lowLatencyChosen ? qsTr("已选「超低延迟」") : qsTr("使用「超低延迟」")
                onClicked: card.chooseLowLatency()
            }
            VButton {
                objectName: "pushfps-welcome-pair"
                visible: card.pairingPossible
                iconName: "gamepad"
                text: qsTr("连接 PS5")
                onClicked: veyra.navigate("ps5")
            }
            VButton {
                objectName: "pushfps-welcome-dismiss"
                ghost: true
                text: qsTr("知道了，不再显示")
                onClicked: card.dismiss()
            }
        }
    }
}
