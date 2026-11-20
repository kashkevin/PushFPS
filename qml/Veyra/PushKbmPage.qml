// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Mouse & Keyboard page (page "kbm"; dock button 键盘和鼠标, and a row in Settings → PS5): keyboard and mouse on a PS5
// Remote Play stream. The console only ever sees a controller; this page is the switch, the input performance profile,
// the per-context sensitivity and curve, the maximum turning speed, a live readout and the limits. The key editor
// (PushKbmBindings.qml) and the first-launch wizard (PushKbmWizard.qml) open inside the page.
//
// Every bridge call goes through PushKbm.js call(): the input core is another stream's work, so a function that this
// build does not have gives a fallback value and the page says what is not available instead of breaking
// (bridge: src/ui/bridge/PushKbm.cpp). `bridge` is the object the calls go to (veyra; a stub in tst_push_kbm.qml).
// Nothing here aims, repeats or times an action for the player: the page changes how the player's own motion is
// converted. "As before" (legacy) is the default; the other profiles are the user's choice and experimental.
// Unverified on Windows and on a PS5.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "PushKbm.js" as Kbm

VPage {
    id: page
    objectName: "pushfps-kbm-page"
    signal requestPage(string page)

    property var bridge: veyra
    // "main", "bindings" (PushKbmBindings) or "wizard" (PushKbmWizard).
    property string view: "main"
    // The wizard opens by itself the first time the switch is turned on here (never during play: this page has no picture).
    property bool autoWizard: true

    // Step 2 of the wizard shows the game picture beside its instructions (Main.qml videoRect); no picture otherwise.
    readonly property int pictureLeft: 14 + 26 + 400 + 24
    readonly property int pictureTop: 14 + 22 + 52
    readonly property rect videoViewport: page.view === "wizard" && wizard.wantsPicture && wizard.pictureFits
        ? Qt.rect(pictureLeft, pictureTop, Math.max(1, width - pictureLeft - 40),
                  Math.max(1, Math.min(height - pictureTop - 36, Math.round((width - pictureLeft - 40) * 9 / 16))))
        : Qt.rect(0, 0, 0, 0)

    readonly property var noState: ({ available: false, core: false, live: false, liveBindings: false, calibration: false, fit: false,
        forced: false, enabled: false, streaming: false, active: false, canRelease: false, capture: "off",
        releaseKeys: "Ctrl+Alt+Shift+Z", inputProfile: "legacy", contextOverride: "auto", inUse: "", context: "none",
        fallback: "none", source: "none" })
    property var st: noState
    // The bridge has the page's functions at all (an older engine has none of them).
    property bool hasBridge: false
    property var settings: ({ available: false })
    property var readout: ({ active: false })
    readonly property var prefs: page.bridge && page.bridge.preferences ? page.bridge.preferences : ({})

    function refresh() {
        const s = Kbm.call(page.bridge, "pushKbmState", [], null)
        page.hasBridge = s !== null
        page.st = s !== null ? Object.assign({}, page.noState, s) : page.noState
        page.settings = Kbm.call(page.bridge, "pushKbmInputSettings", [""], ({ available: false }))
        page.readout = page.st.streaming === true ? Kbm.call(page.bridge, "pushKbmReadout", [], ({ active: false })) : ({ active: false })
    }
    Component.onCompleted: page.refresh()
    onCurrentChanged: if (current) page.refresh(); else if (page.view !== "main") page.closeView()
    onBridgeChanged: page.refresh()
    Timer {
        objectName: "pushfps-kbm-timer"
        interval: 1000; repeat: true
        running: page.current && page.view === "main"
        onTriggered: page.refresh()
    }

    readonly property bool usable: page.hasBridge && page.st.available === true
    readonly property bool on: page.st.enabled === true
    readonly property string profile: Kbm.profileIds.indexOf(String(page.st.inputProfile)) >= 0 ? String(page.st.inputProfile) : "legacy"
    readonly property bool custom: page.profile === "custom"
    readonly property bool legacy: page.profile === "legacy"
    readonly property bool haveSettings: page.settings.available === true && !!page.settings.contexts
    readonly property bool editable: page.custom && page.haveSettings
    property string shownContext: "walk"
    readonly property var tuning: page.haveSettings && page.settings.contexts[page.shownContext] ? page.settings.contexts[page.shownContext] : ({})
    readonly property var plotPoints: page.haveSettings ? Kbm.curvePoints(page.bridge, page.tuning, 33) : []

    function setEnabled(on) {
        if (!page.usable || page.st.forced === true) return false
        const ok = Kbm.call(page.bridge, "pushKbmSetEnabled", [on === true], false) === true
        page.refresh()
        if (ok && on === true && page.autoWizard && page.prefs.pushKbmWizardDone !== true) page.openWizard(0)
        return ok
    }
    function setProfile(id) {
        if (!page.usable || page.st.core !== true || Kbm.profileIds.indexOf(String(id)) < 0) return false
        const ok = Kbm.call(page.bridge, "pushKbmSetInputProfile", [String(id)], false) === true
        page.refresh()
        return ok
    }
    function setCustom(key, value) {
        if (!page.editable) return false
        const ok = Kbm.call(page.bridge, "pushKbmSetCustom", [String(key), value], false) === true
        page.refresh()
        return ok
    }
    function customize() {
        const ok = Kbm.call(page.bridge, "pushKbmCustomFrom", [page.profile], false) === true
        page.refresh()
        return ok
    }
    function setOverride(id) {
        if (Kbm.overrideIds.indexOf(String(id)) < 0) return false
        const ok = Kbm.call(page.bridge, "pushKbmSetContextOverride", [String(id)], false) === true
        page.refresh()
        return ok
    }
    function recapture() {
        const ok = Kbm.call(page.bridge, "pushKbmAllowCapture", [], false) === true
        page.refresh()
        return ok
    }
    function openWizard(step) {
        wizard.begin(step === undefined ? 0 : step)
        page.view = "wizard"
    }
    function openBindings() {
        bindings.reload()
        page.view = "bindings"
    }
    function closeView() {
        page.view = "main"
        page.refresh()
    }

    // ---- Words ----------------------------------------------------------------------------------------------------
    function profileName(id) {
        return id === "ultra-low-latency" ? qsTr("超低延迟输入") : id === "adaptive-precision" ? qsTr("自适应精准")
             : id === "custom" ? qsTr("自定义") : qsTr("和以前一样")
    }
    function profileLine(id) {
        return id === "ultra-low-latency" ? qsTr("原始输入立即换算：没有平滑，也不等后面的采样；发送节奏用已验证的那一套")
             : id === "adaptive-precision" ? qsTr("步行、瞄准、奔跑、驾驶各用自己的灵敏度、曲线和死区补偿；默认不加任何缓冲")
             : id === "custom" ? qsTr("灵敏度、曲线、平滑和发送方式都由你决定")
             : qsTr("默认。和之前的版本完全相同：数值来自按键方案文件，带其中的平滑")
    }
    function contextName(id) {
        return id === "aim" ? qsTr("瞄准") : id === "sprint" ? qsTr("奔跑") : id === "drive" ? qsTr("驾驶")
             : id === "walk" ? qsTr("步行") : id === "auto" ? qsTr("自动") : qsTr("无")
    }
    function fallbackReason(id) {
        return id === "translate-slower" ? qsTr("自适应换算比简单路径慢")
             : id === "queue-older" ? qsTr("输入在发送前等待得更久")
             : id === "user" ? qsTr("由你选择") : String(id)
    }
    function sourceName(id) {
        return id === "kbm" ? qsTr("键盘和鼠标") : id === "pad" ? qsTr("手柄") : id === "mixed" ? qsTr("两者") : qsTr("无")
    }
    readonly property string stateText: {
        if (!page.hasBridge) return qsTr("这个版本的引擎还没有键盘和鼠标页面需要的功能")
        if (page.st.available !== true) return qsTr("这个版本不含 PS5 串流，无法使用键盘和鼠标")
        if (!page.on) return qsTr("已关闭")
        if (page.st.capture === "captured") return qsTr("鼠标已被游戏占用：按 %1 释放").arg(String(page.st.releaseKeys))
        if (page.st.capture === "suspended") return qsTr("已释放：离开再回到串流窗口后重新接管")
        if (page.st.active === true) return qsTr("已开启：回到串流画面时会接管键盘和鼠标")
        return qsTr("已开启：连接 PS5 串流后生效")
    }
    readonly property var readoutRows: {
        const r = page.readout || ({})
        if (r.active !== true) return []
        const unknown = qsTr("未知")
        const value = (v, decimals, unit) => Kbm.shown(v, decimals) === "-" ? unknown : unit.arg(Kbm.shown(v, decimals))
        return [
            { id: "translate", label: qsTr("按键换算 P95"), value: value(r.translateUsP95, 0, qsTr("%1 微秒")) },
            { id: "raw", label: qsTr("鼠标事件到换算 P95"), value: value(r.rawToTranslateUsP95, 0, qsTr("%1 微秒")) },
            { id: "state", label: qsTr("状态到发送 P95"), value: value(r.stateToSendMsP95, 2, qsTr("%1 毫秒")) },
            { id: "event", label: qsTr("事件到发送 P95"), value: value(r.eventToSendMsP95, 2, qsTr("%1 毫秒")) },
            { id: "gap", label: qsTr("发送间隔 P95"), value: value(r.sendGapMsP95, 2, qsTr("%1 毫秒")) },
            { id: "hz", label: qsTr("鼠标回报率"), value: Number(r.mouseHz) > 0 ? qsTr("%1 Hz").arg(Number(r.mouseHz)) : unknown },
            { id: "saturated", label: qsTr("摇杆打满的比例"), value: value(r.saturatedPct, 1, qsTr("%1%")) },
            { id: "source", label: qsTr("输入来源"), value: page.sourceName(String(r.source)) },
            { id: "context", label: qsTr("当前情境"), value: page.contextName(String(r.context)) },
            { id: "fallback", label: qsTr("退回状态"), value: String(r.fallback || "none") === "none" ? qsTr("无") : page.fallbackReason(String(r.fallback)) }
        ]
    }

    component KButton: VButton {
        activeFocusOnTab: true
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Keys.onSpacePressed: clicked()
        Rectangle {
            anchors.fill: parent; anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2; border.color: Theme.accent
            visible: parent.activeFocus
        }
    }
    component Line: Text {
        Layout.fillWidth: true
        color: Theme.t2
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsHint
        lineHeight: 1.15
        wrapMode: Text.WordWrap
    }
    // A slider row for one Custom value; read-only (dimmed) unless Custom is chosen.
    component TuneRow: VRow {
        id: tune
        property string key: ""
        property real shownValue: 0
        property real from: 0
        property real to: 1
        property int decimals: 2
        value: Number(tune.shownValue).toFixed(tune.decimals)
        VSlider {
            objectName: "pushfps-kbm-" + tune.key.replace(".", "-")
            implicitWidth: 150
            valueFromModel: true
            live: false
            from: tune.from; to: tune.to
            value: tune.shownValue
            enabledControl: page.editable
            opacity: page.editable ? 1 : 0.45
            onMoved: value => page.setCustom(tune.key, Number(value.toFixed(tune.decimals + 1)))
        }
    }

    Rectangle {
        id: bodyCard
        anchors.fill: parent
        anchors.margins: 14
        radius: Theme.rCard
        color: Theme.card
        border.width: 1
        border.color: Theme.stroke
        clip: true

        Flickable {
            id: flick
            objectName: "pushfps-kbm-flick"
            anchors.fill: parent
            visible: page.view === "main"
            contentHeight: body.implicitHeight + 44
            clip: true
            ScrollBar.vertical: VScrollBar { }

            ColumnLayout {
                id: body
                x: 26
                y: 22
                width: Math.min(parent.width - 52, 760)
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    VH1 { text: qsTr("键盘和鼠标") }
                    VTag { objectName: "pushfps-kbm-experimental"; text: qsTr("实验"); kind: "exp" }
                    Item { Layout.fillWidth: true }
                }
                Line { text: qsTr("电脑把按键和鼠标移动变成手柄的按键和摇杆；主机看到的始终是一个手柄。") }

                // ---- Switch and capture ------------------------------------------------------------------------
                VGroup {
                    VRow {
                        objectName: "pushfps-kbm-enabled-row"
                        label: qsTr("用键盘和鼠标玩 PS5 串流")
                        hint: page.stateText
                        hintColor: page.st.capture === "captured" ? Theme.accent : Theme.t3
                        VSwitch {
                            objectName: "pushfps-kbm-enabled"
                            checked: page.on
                            enabled: page.usable && page.st.forced !== true
                            opacity: enabled ? 1 : 0.4
                            activeFocusOnTab: true
                            Keys.onSpacePressed: toggled(!checked)
                            Keys.onReturnPressed: toggled(!checked)
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: qsTr("用键盘和鼠标玩 PS5 串流")
                            onToggled: checked => page.setEnabled(checked)
                        }
                    }
                    Line {
                        objectName: "pushfps-kbm-forced"
                        visible: page.st.forced === true
                        text: qsTr("由启动变量 VEYRA_EXP_RP_KBM 打开，这里无法关闭")
                        Layout.bottomMargin: 8
                    }
                    VRow {
                        objectName: "pushfps-kbm-release-row"
                        visible: page.usable
                        label: qsTr("释放鼠标")
                        hint: qsTr("游戏中随时可以按 %1 把鼠标和键盘交还给电脑；打开这个页面时，鼠标和键盘始终留给界面").arg(String(page.st.releaseKeys))
                    }
                    VRow {
                        objectName: "pushfps-kbm-recapture-row"
                        visible: page.on && page.st.capture === "suspended" && page.st.canRelease === true
                        label: qsTr("重新接管")
                        hint: qsTr("回到串流窗口时再次接管键盘和鼠标")
                        KButton { objectName: "pushfps-kbm-recapture"; text: qsTr("立即重新接管"); onClicked: page.recapture() }
                    }
                    VRow {
                        objectName: "pushfps-kbm-controller-row"
                        label: qsTr("同时连接手柄")
                        hint: qsTr("手柄可以一直连着：按键相加，扳机取更深的一边，摇杆听最后动的那个设备")
                    }
                    VRow {
                        label: qsTr("按键和校准")
                        hint: qsTr("每个动作用哪个键；第一次使用时的三步向导")
                        RowLayout {
                            spacing: 8
                            KButton { objectName: "pushfps-kbm-bindings-open"; text: qsTr("按键设置"); enabled: page.usable; onClicked: page.openBindings() }
                            KButton { objectName: "pushfps-kbm-wizard-open"; text: qsTr("校准向导"); enabled: page.usable; onClicked: page.openWizard(0) }
                        }
                    }
                }

                // ---- Input performance profile -----------------------------------------------------------------
                VH3 { text: qsTr("输入性能方案"); Layout.topMargin: 6 }
                VGroup {
                    Line {
                        objectName: "pushfps-kbm-profile-unavailable"
                        visible: page.st.core !== true
                        text: qsTr("这个版本的输入引擎还没有新的输入方案：实际使用的是“和以前一样”")
                        color: Theme.warn
                    }
                    Repeater {
                        model: Kbm.profileIds
                        delegate: Rectangle {
                            id: choice
                            required property string modelData
                            readonly property bool selected: page.profile === modelData
                            objectName: "pushfps-kbm-profile-" + modelData
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            implicitHeight: choiceColumn.implicitHeight + 16
                            radius: 9
                            color: selected ? Theme.accentSoft : choiceHover.hovered ? Theme.ink(0.04) : "transparent"
                            border.width: activeFocus ? 2 : 1
                            border.color: selected || activeFocus ? Theme.accent : Theme.stroke
                            enabled: page.usable && page.st.core === true
                            opacity: enabled || selected ? 1 : 0.5
                            activeFocusOnTab: true
                            Accessible.role: Accessible.RadioButton
                            Accessible.name: page.profileName(modelData)
                            Accessible.description: page.profileLine(modelData)
                            Keys.onSpacePressed: page.setProfile(modelData)
                            Keys.onReturnPressed: page.setProfile(modelData)
                            ColumnLayout {
                                id: choiceColumn
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: 12; anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Text {
                                    text: page.profileName(choice.modelData)
                                    color: Theme.t1
                                    font.family: Theme.fontUi; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold
                                }
                                Line { objectName: "pushfps-kbm-profile-line-" + choice.modelData; text: page.profileLine(choice.modelData) }
                            }
                            HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: page.setProfile(choice.modelData) }
                        }
                    }
                    Line {
                        Layout.topMargin: 8
                        Layout.bottomMargin: 4
                        text: qsTr("新方案还没有在真机上验证，属于实验功能；“和以前一样”保持默认。")
                    }
                    Line {
                        objectName: "pushfps-kbm-inuse"
                        visible: page.st.active === true && String(page.st.inUse).length > 0 && String(page.st.inUse) !== page.profile
                        text: qsTr("当前串流实际使用：%1").arg(page.profileName(String(page.st.inUse)))
                        color: Theme.warn
                    }
                    Line {
                        objectName: "pushfps-kbm-fallback"
                        visible: String(page.st.fallback) !== "none"
                        text: qsTr("已自动退回超低延迟：%1").arg(page.fallbackReason(String(page.st.fallback)))
                        color: Theme.warn
                        Layout.bottomMargin: 4
                    }
                    VRow {
                        objectName: "pushfps-kbm-smoothing-row"
                        label: qsTr("平滑")
                        hint: page.profile === "ultra-low-latency" ? qsTr("关闭，且不可更改：平滑就是延迟")
                            : page.profile === "adaptive-precision" ? qsTr("关闭：这个方案不加时间缓冲")
                            : page.custom ? qsTr("0 = 关闭。平滑把鼠标移动平均到这段时间里，因此会增加延迟")
                            : qsTr("由按键方案文件决定")
                        value: page.legacy || !page.haveSettings ? "" : Number(page.settings.smoothingMs) > 0 ? qsTr("%1 毫秒").arg(Number(page.settings.smoothingMs).toFixed(1)) : qsTr("关闭", "off")
                        VSlider {
                            objectName: "pushfps-kbm-smoothing"
                            visible: !page.legacy && page.haveSettings
                            implicitWidth: 150
                            valueFromModel: true
                            live: false
                            from: 0; to: 50
                            value: page.haveSettings ? Number(page.settings.smoothingMs || 0) : 0
                            enabledControl: page.editable
                            opacity: page.editable ? 1 : 0.45
                            onMoved: value => page.setCustom("smoothingMs", Math.round(value * 2) / 2)
                        }
                    }
                    VRow {
                        objectName: "pushfps-kbm-transmit-row"
                        visible: page.editable
                        label: qsTr("发送方式")
                        hint: qsTr("已验证：按现在的固定节奏发送手柄状态。变化即发：输入一变就发送，实验性，可能让主机收到更多数据包")
                        VSeg {
                            objectName: "pushfps-kbm-transmit"
                            options: [{ id: "proven", label: qsTr("已验证") }, { id: "leading-edge", label: qsTr("变化即发") }]
                            current: String(page.settings.transmit || "proven")
                            onPicked: id => page.setCustom("transmit", id)
                        }
                    }
                }

                // ---- Per context ---------------------------------------------------------------------------------
                VH3 { text: qsTr("各情境的灵敏度和曲线"); Layout.topMargin: 6 }
                VGroup {
                    VRow {
                        objectName: "pushfps-kbm-override-row"
                        label: qsTr("情境切换")
                        hint: qsTr("自动：按住瞄准键用瞄准，切到驾驶用驾驶，移动时按住奔跑键用奔跑。手动选择只改变镜头手感，不改变按键")
                        hintBelow: true
                        VSeg {
                            objectName: "pushfps-kbm-override"
                            enabled: page.usable && page.st.core === true
                            opacity: enabled ? 1 : 0.45
                            options: Kbm.overrideIds.map(id => ({ id: id, label: page.contextName(id) }))
                            current: Kbm.overrideIds.indexOf(String(page.st.contextOverride)) >= 0 ? String(page.st.contextOverride) : "auto"
                            onPicked: id => page.setOverride(id)
                        }
                    }
                    Line {
                        objectName: "pushfps-kbm-legacy-note"
                        visible: page.legacy || !page.haveSettings
                        text: page.st.core === true ? qsTr("“和以前一样”的数值在按键方案文件里，这里不显示。选择其他方案后可以查看每个情境的数值。")
                                                    : qsTr("这个版本的输入引擎没有可显示的情境数值。")
                        Layout.topMargin: 6; Layout.bottomMargin: 8
                    }
                    ColumnLayout {
                        objectName: "pushfps-kbm-tuning"
                        visible: !page.legacy && page.haveSettings
                        Layout.fillWidth: true
                        spacing: 0
                        VRow {
                            label: qsTr("查看和修改")
                            VSeg {
                                objectName: "pushfps-kbm-context"
                                options: Kbm.contextIds.map(id => ({ id: id, label: page.contextName(id) }))
                                current: page.shownContext
                                onPicked: id => page.shownContext = id
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 4; Layout.bottomMargin: 6
                            visible: !page.editable
                            spacing: 10
                            Line { objectName: "pushfps-kbm-readonly"; text: qsTr("选择“自定义”才能修改这些数值") }
                            KButton { objectName: "pushfps-kbm-customize"; text: qsTr("以当前方案为起点自定义"); onClicked: page.customize() }
                        }
                        TuneRow { label: qsTr("水平灵敏度"); key: page.shownContext + ".sensX"; shownValue: Number(page.tuning.sensX || 0); from: 0.05; to: 5 }
                        TuneRow { label: qsTr("垂直灵敏度"); key: page.shownContext + ".sensY"; shownValue: Number(page.tuning.sensY || 0); from: 0.05; to: 5 }
                        TuneRow {
                            objectName: "pushfps-kbm-ads-row"
                            visible: page.shownContext === "aim"
                            label: qsTr("瞄准灵敏度倍率 (ADS)")
                            hint: qsTr("瞄准情境下两个方向的灵敏度都乘以它")
                            key: "ads"; shownValue: Number(page.settings.ads || 0); from: 0.05; to: 4
                        }
                        VRow {
                            label: qsTr("响应曲线")
                            VSeg {
                                objectName: "pushfps-kbm-curve"
                                enabled: page.editable
                                opacity: enabled ? 1 : 0.45
                                options: [{ id: "linear", label: qsTr("线性") }, { id: "power", label: qsTr("幂次") }, { id: "precision-speed", label: qsTr("精准＋速度") }]
                                current: Kbm.curveIds.indexOf(String(page.tuning.curve)) >= 0 ? String(page.tuning.curve) : "linear"
                                onPicked: id => page.setCustom(page.shownContext + ".curve", id)
                            }
                        }
                        TuneRow {
                            visible: String(page.tuning.curve) === "power"
                            label: qsTr("曲线指数"); hint: qsTr("大于 1：慢速更细腻；小于 1：慢速更快")
                            key: page.shownContext + ".exponent"; shownValue: Number(page.tuning.exponent || 0); from: 0.25; to: 4
                        }
                        TuneRow {
                            visible: String(page.tuning.curve) === "precision-speed"
                            label: qsTr("精准区范围"); hint: qsTr("鼠标速度低于这个比例时使用精准段")
                            key: page.shownContext + ".kneeIn"; shownValue: Number(page.tuning.kneeIn || 0); from: 0.05; to: 0.95
                        }
                        TuneRow {
                            visible: String(page.tuning.curve) === "precision-speed"
                            label: qsTr("精准区输出"); hint: qsTr("精准段结束时的摇杆输出比例")
                            key: page.shownContext + ".kneeOut"; shownValue: Number(page.tuning.kneeOut || 0); from: 0.01; to: 0.99
                        }
                        TuneRow {
                            label: qsTr("死区补偿"); hint: qsTr("最小的鼠标移动从这个摇杆倾斜开始，用来抵消游戏自己的摇杆死区")
                            key: page.shownContext + ".deadzone"; shownValue: Number(page.tuning.deadzone || 0); from: 0; to: 0.5
                        }
                        // Stick output against mouse speed for the context shown: an immediate function of the
                        // current sample (dead zone, then the curve), nothing averaged over time.
                        Canvas {
                            id: plot
                            objectName: "pushfps-kbm-curve-plot"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 130
                            Layout.topMargin: 6
                            property var points: page.plotPoints
                            property var tuning: page.tuning
                            property color lineColor: Theme.accent
                            property color gridColor: Theme.stroke2
                            onPointsChanged: requestPaint()
                            onTuningChanged: requestPaint()
                            onLineColorChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                const c = getContext("2d"); c.reset()
                                const w = width, h = height, pad = 6
                                if (w <= 2 * pad || h <= 2 * pad) return
                                c.strokeStyle = plot.gridColor; c.lineWidth = 1
                                c.strokeRect(pad + 0.5, pad + 0.5, w - 2 * pad - 1, h - 2 * pad - 1)
                                const pts = plot.points || []
                                if (pts.length < 2) return
                                c.strokeStyle = plot.lineColor; c.lineWidth = 2
                                c.beginPath()
                                for (let k = 0; k < pts.length; ++k) {
                                    const u = Number(pts[k].u)
                                    const x = pad + u * (w - 2 * pad)
                                    const y = h - pad - Kbm.stickFor(plot.tuning, u) * (h - 2 * pad)
                                    if (k === 0) c.moveTo(x, h - pad); else if (k === 1) { c.lineTo(pad, y); c.lineTo(x, y) } else c.lineTo(x, y)
                                }
                                c.stroke()
                            }
                        }
                        Line { text: qsTr("摇杆输出（纵轴）随鼠标速度（横轴）的变化"); Layout.bottomMargin: 8 }
                    }
                }

                // ---- Maximum turning speed -----------------------------------------------------------------------
                VH3 { text: qsTr("最大转向速度"); Layout.topMargin: 6 }
                VGroup {
                    VRow {
                        objectName: "pushfps-kbm-turn-row"
                        label: qsTr("摇杆推到底时的鼠标速度")
                        hint: page.haveSettings && !page.legacy ? qsTr("%1 的校准值").arg(page.contextName(page.shownContext))
                                                               : qsTr("还没有可显示的校准值")
                        RowLayout {
                            spacing: 10
                            Text {
                                objectName: "pushfps-kbm-turn-value"
                                visible: page.haveSettings && !page.legacy
                                text: qsTr("%1 计数/秒").arg(Math.round(Number(page.tuning.fullSpeed || 0)))
                                color: Theme.t1
                                font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                            }
                            KButton { objectName: "pushfps-kbm-calibrate"; text: qsTr("校准"); enabled: page.usable; onClicked: page.openWizard(2) }
                        }
                    }
                    Line {
                        objectName: "pushfps-kbm-turn-why"
                        text: qsTr("主机只认识摇杆：摇杆推到底时游戏转多快，镜头最快就转多快，鼠标甩得再快也不会更快。")
                        Layout.bottomMargin: 8
                    }
                }

                // ---- Live readout (only while a PS5 stream runs) -------------------------------------------------
                VH3 { text: qsTr("实时读数"); Layout.topMargin: 6 }
                VGroup {
                    objectName: "pushfps-kbm-readout"
                    Line {
                        objectName: "pushfps-kbm-readout-note"
                        text: page.readoutRows.length > 0 ? qsTr("这些是电脑内部的处理时间，不是从输入到画面的延迟。")
                                                          : qsTr("PS5 串流运行并开启键盘和鼠标后，这里显示电脑内部的处理时间。")
                        Layout.bottomMargin: page.readoutRows.length > 0 ? 0 : 8
                    }
                    Repeater {
                        model: page.readoutRows
                        delegate: VRow {
                            required property var modelData
                            objectName: "pushfps-kbm-readout-" + modelData.id
                            label: modelData.label
                            Text {
                                objectName: "pushfps-kbm-readout-value-" + modelData.id
                                text: modelData.value
                                color: Theme.t1
                                font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                            }
                        }
                    }
                }

                // ---- Limits ---------------------------------------------------------------------------------------
                VH3 { text: qsTr("限制"); Layout.topMargin: 6 }
                VGroup {
                    objectName: "pushfps-kbm-limits"
                    Line { objectName: "pushfps-kbm-limit-1"; text: qsTr("主机看到的是模拟摇杆：鼠标移动被换算成摇杆倾斜，永远不是真正的鼠标输入。") }
                    Line { objectName: "pushfps-kbm-limit-2"; text: qsTr("镜头转速有上限：摇杆推到底就是游戏允许的最快转速。"); Layout.topMargin: 4 }
                    Line { objectName: "pushfps-kbm-limit-3"; text: qsTr("游戏自己的摇杆加速度仍然生效，手感会和电脑游戏不同。"); Layout.topMargin: 4 }
                    Line { objectName: "pushfps-kbm-limit-4"; text: qsTr("游戏的辅助瞄准是为手柄设计的；这里不提供、也不增强任何瞄准辅助。"); Layout.topMargin: 4 }
                }
            }
        }

        PushKbmBindings {
            id: bindings
            objectName: "pushfps-kbm-bindings"
            anchors.fill: parent
            visible: page.view === "bindings"
            bridge: page.bridge
            onClosed: page.closeView()
        }
        PushKbmWizard {
            id: wizard
            objectName: "pushfps-kbm-wizard"
            anchors.fill: parent
            visible: page.view === "wizard"
            bridge: page.bridge
            onFinished: page.closeView()
            onCancelled: page.closeView()
            onRequestBindings: page.openBindings()
        }
    }
}
