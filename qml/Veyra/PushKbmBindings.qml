// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Key editor of the Mouse & Keyboard page (shown inside PushKbmPage.qml): every action of the controller with the
// keys and mouse buttons that do it, per context (walking, aiming, driving) and per profile file.
//   - "Add a key" waits for one key or mouse button ("press a key"); Esc cancels, and the list beside it holds every
//     key name for keyboard-only use and for binding Esc itself;
//   - one key does one thing: a key that already does something else in that context is NAMED and refused, and is
//     moved only when the user says so (bridge pushKbmSetBinding(..., replace));
//   - reset to the built-in Grand Theft Auto style layout (asked twice), load and save profile files.
// A change of bindings releases every held key in a running capture (the core's rule: bindings change outside
// play). Bridge: src/ui/bridge/PushKbm.cpp; every call goes through PushKbm.js call(), so a build without the
// functions shows "not available" instead of breaking. Nothing here repeats or times an action: one key press is
// one button press. Unverified on Windows.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "PushKbm.js" as Kbm

Item {
    id: editor
    signal closed()

    property var bridge: veyra
    property var bindings: ({ available: false })
    property var files: []
    // "walk", "aim" or "drive".
    property string context: "walk"
    // The action waiting for a key ("" = none).
    property string capturing: ""
    // { key, action, existing } while a refused key waits for the user's decision; null otherwise.
    property var conflict: null
    property string status: ""
    property bool resetArmed: false
    property string fileName: ""

    readonly property bool available: editor.bindings.available === true
    readonly property var list: Kbm.listFor(editor.bindings, editor.context)
    readonly property var actions: editor.bindings.actions || []
    readonly property var keyNames: editor.bindings.keys || []

    function refreshBindings() { editor.bindings = Kbm.call(editor.bridge, "pushKbmBindings", [], ({ available: false })) }
    function reload() {
        editor.refreshBindings()
        editor.files = Kbm.call(editor.bridge, "pushKbmProfileFiles", [], [])
        editor.fileName = String(editor.bindings.file || "")
        editor.capturing = ""
        editor.conflict = null
        editor.resetArmed = false
        editor.status = ""
    }
    // Loaded when the page opens the editor (PushKbmPage.openBindings), not when the program starts: reload() reads
    // the profile folder.
    onVisibleChanged: if (!visible) { editor.capturing = ""; editor.conflict = null; editor.resetArmed = false }

    function keysFor(action) { return Kbm.keysOf(editor.list, action) }
    function beginCapture(action) {
        if (!editor.available) return
        editor.conflict = null
        editor.resetArmed = false
        editor.status = ""
        editor.capturing = String(action)
        catcher.forceActiveFocus()
    }
    function cancelCapture() { editor.capturing = "" }
    // A key or mouse button for the action that is waiting. True when it was bound; a key that already does
    // something else is refused and named (editor.conflict), never silently moved.
    function captureKey(name) {
        const action = editor.capturing
        if (action === "" || String(name) === "") return false
        const r = Kbm.call(editor.bridge, "pushKbmSetBinding", [editor.context, String(name), action, false], ({ ok: false, reason: "unavailable" }))
        editor.capturing = ""
        if (r.ok === true) {
            editor.status = qsTr("%1 现在用于“%2”").arg(editor.keyLabel(String(name))).arg(editor.actionLabel(action))
            editor.refreshBindings()
            return true
        }
        if (String(r.reason) === "conflict") {
            editor.conflict = { key: String(name), action: action, existing: String(r.conflict || "") }
            editor.status = ""
            return false
        }
        editor.status = String(r.reason) === "unknown-key" ? qsTr("这个键不能用于按键方案") : qsTr("没有更改：这个版本不能修改按键")
        return false
    }
    function resolveConflict(replace) {
        const c = editor.conflict
        if (!c) return false
        editor.conflict = null
        if (replace !== true) { editor.status = qsTr("没有更改"); return false }
        const r = Kbm.call(editor.bridge, "pushKbmSetBinding", [editor.context, c.key, c.action, true], ({ ok: false }))
        editor.status = r.ok === true ? qsTr("%1 现在用于“%2”").arg(editor.keyLabel(c.key)).arg(editor.actionLabel(c.action)) : qsTr("没有更改")
        editor.refreshBindings()
        return r.ok === true
    }
    function unbind(key) {
        const r = Kbm.call(editor.bridge, "pushKbmSetBinding", [editor.context, String(key), "none", true], ({ ok: false }))
        editor.conflict = null
        editor.status = r.ok === true ? qsTr("%1 不再做任何事").arg(editor.keyLabel(String(key))) : qsTr("没有更改")
        editor.refreshBindings()
        return r.ok === true
    }
    // Asked twice: the first call arms, the second resets.
    function resetAll() {
        if (!editor.resetArmed) { editor.resetArmed = true; return false }
        editor.resetArmed = false
        const ok = Kbm.call(editor.bridge, "pushKbmResetBindings", [], false) === true
        editor.status = ok ? qsTr("已恢复内置布局") : qsTr("没有更改")
        editor.conflict = null
        editor.refreshBindings()
        return ok
    }
    function loadFile(name) {
        const ok = Kbm.call(editor.bridge, "pushKbmLoadProfile", [String(name)], false) === true
        editor.reload()
        editor.status = ok ? qsTr("已载入方案“%1”").arg(String(name)) : qsTr("没能读取这个按键方案文件")
        return ok
    }
    function saveFile(name) {
        const slug = Kbm.slug(name)
        if (slug === "") { editor.status = qsTr("请先输入方案名称（字母或数字）"); return false }
        const ok = Kbm.call(editor.bridge, "pushKbmSaveProfile", [slug], false) === true
        editor.reload()
        editor.status = ok ? qsTr("已保存为“%1”，下次连接 PS5 时使用").arg(slug) : qsTr("按键方案没有保存")
        return ok
    }

    function contextLabel(id) { return id === "aim" ? qsTr("瞄准") : id === "drive" ? qsTr("驾驶") : id === "menu" ? qsTr("菜单") : qsTr("步行") }
    // The contexts the editor offers: the menu keys only when this build's core has them.
    readonly property var contextOptions: {
        const out = [{ id: "walk", label: qsTr("步行") }, { id: "aim", label: qsTr("瞄准") }, { id: "drive", label: qsTr("驾驶") }]
        if (editor.bindings.menu !== undefined) out.push({ id: "menu", label: qsTr("菜单") })
        return out
    }
    // A preset of the core (stream "kbm"): keys, menu keys and its low-latency input in one step.
    function applyPreset(id) {
        const r = Kbm.call(editor.bridge, "pushKbmApplyPreset", [String(id)], ({ ok: false }))
        editor.status = r.ok === true ? qsTr("已套用 GTA V 风格键位。GTA VI 的换弹键没有公开资料：如果游戏里换弹是别的按键，把 R 设到那个按键上（R 就不再是近战）") : qsTr("没有更改")
        editor.conflict = null
        editor.refreshBindings()
        return r.ok === true
    }
    function actionLabel(id) {
        switch (String(id)) {
        case "cross": return qsTr("✕ 键")
        case "circle": return qsTr("○ 键")
        case "square": return qsTr("□ 键")
        case "triangle": return qsTr("△ 键")
        case "l1": return "L1"
        case "r1": return "R1"
        case "l2": return "L2"
        case "r2": return "R2"
        case "l3": return qsTr("L3（按下左摇杆）")
        case "r3": return qsTr("R3（按下右摇杆）")
        case "options": return qsTr("OPTIONS 键")
        case "share": return qsTr("创建键")
        case "touchpad": return qsTr("触摸板按下")
        case "ps": return qsTr("PS 键")
        case "dup": return qsTr("方向键 上")
        case "ddown": return qsTr("方向键 下")
        case "dleft": return qsTr("方向键 左")
        case "dright": return qsTr("方向键 右")
        case "lup": return qsTr("左摇杆 上")
        case "ldown": return qsTr("左摇杆 下")
        case "lleft": return qsTr("左摇杆 左")
        case "lright": return qsTr("左摇杆 右")
        case "walk": return qsTr("慢走（摇杆推一半）")
        case "fine": return qsTr("精细瞄准（按住时鼠标变慢）")
        case "drive": return qsTr("切换步行 / 驾驶")
        case "menu": return qsTr("暂停（OPTIONS）并切换菜单按键")
        default: return String(id)
        }
    }
    function keyLabel(name) {
        switch (String(name)) {
        case "Mouse1": return qsTr("鼠标左键")
        case "Mouse2": return qsTr("鼠标中键")
        case "Mouse3": return qsTr("鼠标右键")
        case "Mouse4": return qsTr("鼠标侧键 1")
        case "Mouse5": return qsTr("鼠标侧键 2")
        case "WheelUp": return qsTr("滚轮向上")
        case "WheelDown": return qsTr("滚轮向下")
        case "Space": return qsTr("空格")
        default: return String(name)
        }
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

    Flickable {
        id: flick
        anchors.fill: parent
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
                spacing: 12
                KButton { objectName: "pushfps-kbm-bindings-back"; text: qsTr("返回"); iconName: "left"; onClicked: editor.closed() }
                VH1 { text: qsTr("按键设置") }
                Item { Layout.fillWidth: true }
            }
            Line { text: qsTr("点“添加按键”后按下键盘按键或鼠标按键。一个键只能做一件事；更改按键会松开所有按住的键。") }
            Line {
                objectName: "pushfps-kbm-bindings-unavailable"
                visible: !editor.available
                text: qsTr("这个版本不能修改按键：没有 PS5 串流，或引擎还没有这项功能。")
                color: Theme.warn
            }
            Line {
                objectName: "pushfps-kbm-bindings-next"
                visible: editor.available && editor.bindings.live !== true
                text: qsTr("这个版本在下次连接 PS5 时才会用上保存的按键；请保存为方案文件。")
            }

            // ---- Profile file and reset ---------------------------------------------------------------------------
            VGroup {
                visible: editor.available
                VRow {
                    objectName: "pushfps-kbm-file-row"
                    label: qsTr("按键方案文件")
                    hint: qsTr("保存在设置文件夹里（slipstream-kbm-名称.txt），旧文件保留为 .bak；下次连接 PS5 时使用这里选的方案")
                    hintBelow: true
                    RowLayout {
                        spacing: 8
                        VSelect {
                            objectName: "pushfps-kbm-file"
                            implicitWidth: 150
                            title: qsTr("按键方案文件")
                            options: (editor.files || []).map(f => ({ id: String(f.name), label: String(f.name) }))
                            value: String(editor.bindings.file || "").length > 0 ? String(editor.bindings.file) : qsTr("内置布局")
                            enabled: (editor.files || []).length > 0
                            opacity: enabled ? 1 : 0.5
                            onPicked: id => editor.loadFile(id)
                        }
                        DialogTextField {
                            id: nameField
                            objectName: "pushfps-kbm-file-name"
                            Layout.preferredWidth: 140
                            placeholder: qsTr("方案名称")
                            text: editor.fileName
                        }
                        KButton { objectName: "pushfps-kbm-file-save"; text: qsTr("保存"); onClicked: editor.saveFile(nameField.text) }
                    }
                }
                VRow {
                    objectName: "pushfps-kbm-preset-row"
                    visible: Kbm.has(editor.bridge, "pushKbmApplyPreset")
                    label: qsTr("GTA V 风格")
                    hint: qsTr("WASD 移动，鼠标转镜头，左键开火，右键瞄准，Shift 奔跑，空格跳跃，F 上下车，R 近战，Q 掩体，Ctrl 蹲下，Tab 武器轮盘，E 互动，Esc 暂停和菜单按键；按媒体整理的 GTA VI PS5 按键表（未在游戏里验证）。同时设定最低延迟的鼠标处理，保留你自定义的灵敏度。换弹键没有公开资料：如果游戏里换弹是别的按键，把 R 设到那个按键上（R 就不再是近战）")
                    hintBelow: true
                    KButton {
                        objectName: "pushfps-kbm-preset-gta-pc"
                        text: qsTr("套用 GTA V 风格键位")
                        onClicked: editor.applyPreset("gta-pc")
                    }
                }
                VRow {
                    label: qsTr("恢复默认")
                    hint: qsTr("恢复内置的 GTA 风格布局（来自公开的按键表，未调校，未在游戏里验证）")
                          + (String(editor.bindings.layoutName || "").length > 0 ? "\n" + String(editor.bindings.layoutName) : "")
                    hintBelow: true
                    KButton {
                        objectName: "pushfps-kbm-bindings-reset"
                        text: editor.resetArmed ? qsTr("再点一次确认") : qsTr("恢复内置布局")
                        onClicked: editor.resetAll()
                    }
                }
                VRow {
                    label: qsTr("情境")
                    hint: editor.context === "aim" ? qsTr("瞄准沿用步行的按键；这里只改瞄准时不一样的键")
                        : editor.context === "drive" ? qsTr("驾驶有自己的一套按键")
                        : editor.context === "menu" ? qsTr("用“暂停并切换菜单按键”的键进入和离开菜单按键；菜单按键生效时鼠标不转动镜头")
                        : qsTr("步行的按键也用于奔跑和瞄准")
                    VSeg {
                        objectName: "pushfps-kbm-bindings-context"
                        options: editor.contextOptions
                        current: editor.context
                        onPicked: id => { editor.capturing = ""; editor.conflict = null; editor.context = id }
                    }
                }
            }

            Line {
                objectName: "pushfps-kbm-bindings-status"
                visible: editor.status.length > 0
                text: editor.status
                color: Theme.t1
            }

            // ---- A refused key: named, and moved only when the user says so -----------------------------------------
            Rectangle {
                objectName: "pushfps-kbm-conflict"
                visible: editor.conflict !== null
                Layout.fillWidth: true
                implicitHeight: conflictColumn.implicitHeight + 20
                radius: 10
                color: Theme.warnGlow
                border.width: 1; border.color: Theme.warn
                ColumnLayout {
                    id: conflictColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 8
                    Text {
                        objectName: "pushfps-kbm-conflict-text"
                        Layout.fillWidth: true
                        text: editor.conflict ? qsTr("%1 已经用于“%2”，没有更改。一个键只能做一件事。")
                                                    .arg(editor.keyLabel(editor.conflict.key)).arg(editor.actionLabel(editor.conflict.existing)) : ""
                        color: Theme.t1
                        font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        spacing: 8
                        KButton {
                            objectName: "pushfps-kbm-conflict-replace"
                            text: editor.conflict ? qsTr("改为用于“%1”").arg(editor.actionLabel(editor.conflict.action)) : ""
                            onClicked: editor.resolveConflict(true)
                        }
                        KButton { objectName: "pushfps-kbm-conflict-keep"; text: qsTr("保持原样"); ghost: true; onClicked: editor.resolveConflict(false) }
                    }
                }
            }

            // ---- "Press a key" ---------------------------------------------------------------------------------------
            Rectangle {
                objectName: "pushfps-kbm-capture"
                visible: editor.capturing !== ""
                Layout.fillWidth: true
                implicitHeight: captureColumn.implicitHeight + 20
                radius: 10
                color: Theme.accentSoft
                border.width: catcher.activeFocus ? 2 : 1; border.color: Theme.accent
                Item {
                    id: catcher
                    objectName: "pushfps-kbm-capture-focus"
                    anchors.fill: parent
                    focus: editor.capturing !== ""
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("请按下一个键")
                    Keys.onPressed: event => {
                        if (editor.capturing === "") return
                        event.accepted = true
                        if (event.isAutoRepeat) return
                        if (event.key === Qt.Key_Escape) { editor.cancelCapture(); return }
                        const name = Kbm.keyName(event.key, event.modifiers)
                        if (name === "") { editor.status = qsTr("这个键不能用于按键方案"); return }
                        editor.captureKey(name)
                    }
                }
                ColumnLayout {
                    id: captureColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 8
                    Text {
                        objectName: "pushfps-kbm-capture-text"
                        Layout.fillWidth: true
                        text: qsTr("请按下要用于“%1”的键盘按键，或在下面的方框里按鼠标按键、滚动滚轮（Esc 取消）").arg(editor.actionLabel(editor.capturing))
                        color: Theme.t1
                        font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        spacing: 8
                        // Any mouse button or a wheel notch inside this box is taken as the key (the buttons beside
                        // it keep their own clicks).
                        Rectangle {
                            objectName: "pushfps-kbm-capture-mouse"
                            implicitWidth: 170
                            implicitHeight: 34
                            radius: 8
                            color: Theme.ink(0.06)
                            border.width: 1; border.color: Theme.stroke2
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("鼠标按键按这里")
                                color: Theme.t2
                                font.family: Theme.fontUi; font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.AllButtons
                                onPressed: mouse => editor.captureKey(Kbm.mouseName(mouse.button))
                                onWheel: wheel => editor.captureKey(Kbm.wheelName(wheel.angleDelta.y))
                            }
                        }
                        VSelect {
                            objectName: "pushfps-kbm-key-list"
                            implicitWidth: 170
                            title: qsTr("从列表选择")
                            value: qsTr("从列表选择")
                            options: editor.keyNames.map(k => ({ id: String(k), label: editor.keyLabel(String(k)) }))
                            onPicked: id => editor.captureKey(id)
                        }
                        KButton { objectName: "pushfps-kbm-capture-cancel"; text: qsTr("取消"); ghost: true; onClicked: editor.cancelCapture() }
                    }
                }
            }

            // ---- Every action with its keys ---------------------------------------------------------------------------
            VGroup {
                objectName: "pushfps-kbm-actions"
                visible: editor.available
                Repeater {
                    model: editor.actions
                    delegate: VRow {
                        id: actionRow
                        required property string modelData
                        readonly property var keys: editor.keysFor(modelData)
                        objectName: "pushfps-kbm-action-" + modelData
                        label: editor.actionLabel(modelData)
                        RowLayout {
                            spacing: 6
                            Repeater {
                                model: actionRow.keys
                                delegate: Rectangle {
                                    id: chip
                                    required property string modelData
                                    objectName: "pushfps-kbm-key-" + actionRow.modelData + "-" + modelData
                                    implicitWidth: chipRow.implicitWidth + 14
                                    implicitHeight: 26
                                    radius: 7
                                    color: Theme.ink(0.07)
                                    border.width: activeFocus ? 2 : 1
                                    border.color: activeFocus ? Theme.accent : Theme.stroke2
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("%1：按 Delete 取消这个键").arg(editor.keyLabel(modelData))
                                    Keys.onDeletePressed: editor.unbind(modelData)
                                    Keys.onPressed: event => { if (event.key === Qt.Key_Backspace) { editor.unbind(chip.modelData); event.accepted = true } }
                                    Row {
                                        id: chipRow
                                        anchors.centerIn: parent
                                        spacing: 6
                                        Text {
                                            text: editor.keyLabel(chip.modelData)
                                            color: Theme.t1
                                            font.family: Theme.fontUi; font.pixelSize: 12
                                        }
                                        Text {
                                            objectName: "pushfps-kbm-unbind-" + actionRow.modelData + "-" + chip.modelData
                                            text: "×"
                                            color: removeHover.hovered ? Theme.err : Theme.t3
                                            font.family: Theme.fontUi; font.pixelSize: 13
                                            HoverHandler { id: removeHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler { onTapped: editor.unbind(chip.modelData) }
                                        }
                                    }
                                }
                            }
                            KButton {
                                objectName: "pushfps-kbm-bind-" + actionRow.modelData
                                text: editor.capturing === actionRow.modelData ? qsTr("请按键…") : qsTr("添加按键")
                                ghost: true
                                onClicked: editor.beginCapture(actionRow.modelData)
                            }
                        }
                    }
                }
            }
        }
    }
}
