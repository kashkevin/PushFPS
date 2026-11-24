// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// First-launch wizard of the Mouse & Keyboard page (shown inside PushKbmPage.qml the first time the switch is turned
// on there, and from the page's buttons later; preference pushKbmWizardDone). Three short steps, each can be skipped:
//   1. pick the key layout (keep, the built-in GTA-style layout, or a saved profile file);
//   2. dead zone and sensitivity: the core's guided calibration in front of the game (the PC moves the right stick,
//      the player taps while watching the picture shown beside the instructions; nothing reads the picture);
//   3. maximum turning speed: the player's own test motions, from which the core fits how fast a hand motion means
//      "full stick" (the stick stays neutral while a motion is recorded).
// Nothing chosen here is applied before "Finish and apply" (finish()); cancel() applies nothing and leaves the
// preference alone. One exception is stated on the page before it starts: the game calibration of step 2 is the
// core's own procedure and writes its numbers to the profile file when it runs to its end.
// The mouse stays with the interface on this page; for a calibration or a recording the page asks the capture to
// take it (bridge pushKbmHold, renewed while it runs) and gives it back at the end. It never starts by itself.
// Bridge: src/ui/bridge/PushKbm.cpp through PushKbm.js call(): a build without a function says so here.
// Nothing here aims, repeats or times an action for the player. Unverified on Windows and on a PS5.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "PushKbm.js" as Kbm

Item {
    id: wizard
    signal finished()
    signal cancelled()
    signal requestBindings()

    property var bridge: veyra
    // 0 layout, 1 dead zone and sensitivity, 2 maximum turning speed, 3 summary.
    property int step: 0
    property var st: ({})
    property var files: []

    // ---- What the user chose; applied only by finish() --------------------------------------------------------------
    // "keep", "builtin" or "file:<name>".
    property string layoutChoice: "keep"
    // The fitted numbers the user asked to use (step 3); null = leave sensitivity alone.
    property var tuning: null

    // ---- Step 2: the game calibration ---------------------------------------------------------------------------------
    // "idle", "arming" (waiting for the capture), "running", "done", "failed".
    property string calPhase: "idle"
    property string calStep: "intro"
    // The profile file a calibration that ran to its end was kept as ("" = nothing was written).
    property string calKept: ""
    // This run's calibration ran to its end with a measurement: the engine wrote and uses its numbers; the whole
    // profile is kept as the file for the next start only by "Finish and apply" (finish()).
    property bool calFinished: false
    property var calLines: []
    property int calWaited: 0
    // ---- Step 3: the test motions --------------------------------------------------------------------------------------
    readonly property var motionKinds: ["turn", "turn", "turn", "flick", "flick", "flick", "slow", "slow", "slow"]
    property var motions: []            // [{kind, counts, seconds}]
    // "idle", "arming", "recording".
    property string motionPhase: "idle"
    property int motionWaited: 0
    property var fit: null
    // Seconds for a full turn at full stick, when step 2 measured it in this run (0 = not known).
    property real gameTurnSeconds: 0

    readonly property bool streaming: wizard.st.streaming === true && wizard.st.active === true
    readonly property bool canCalibrate: wizard.st.calibration === true
    readonly property bool canFit: wizard.st.fit === true
    readonly property bool busy: wizard.calPhase === "arming" || wizard.calPhase === "running" || wizard.motionPhase !== "idle"
    // The page shows the game picture beside step 2 (PushKbmPage.videoViewport).
    readonly property bool wantsPicture: wizard.visible && wizard.step === 1 && wizard.streaming && wizard.canCalibrate
    readonly property int textWidth: wizard.wantsPicture ? 400 : 760
    readonly property bool pictureFits: wizard.width >= 900

    function refresh() {
        wizard.st = Kbm.call(wizard.bridge, "pushKbmState", [], ({}))
        wizard.files = Kbm.call(wizard.bridge, "pushKbmProfileFiles", [], [])
    }
    function begin(step) {
        wizard.release()
        wizard.layoutChoice = "keep"
        wizard.tuning = null
        wizard.calPhase = "idle"; wizard.calStep = "intro"; wizard.calLines = []; wizard.calKept = ""; wizard.calFinished = false; wizard.motionNote = ""
        wizard.motions = []; wizard.motionPhase = "idle"; wizard.fit = null; wizard.gameTurnSeconds = 0
        wizard.step = Math.max(0, Math.min(2, Number(step) || 0))
        wizard.refresh()
    }
    // Gives the mouse back and ends whatever the page started (a stopped calibration writes nothing).
    function release() {
        if (wizard.calPhase === "running") Kbm.call(wizard.bridge, "pushKbmCalibration", ["stop"], ({}))
        if (wizard.motionPhase === "recording") Kbm.call(wizard.bridge, "pushKbmFit", ["stop", ({})], ({}))
        if (wizard.calPhase === "arming" || wizard.calPhase === "running") wizard.calPhase = "idle"
        wizard.motionPhase = "idle"
        Kbm.call(wizard.bridge, "pushKbmHold", [false], false)
    }
    function next() { if (!wizard.busy) { wizard.step = Math.min(3, wizard.step + 1); wizard.refresh() } }
    function back() { if (!wizard.busy) { wizard.step = Math.max(0, wizard.step - 1); wizard.refresh() } }
    // Skipping drops what this step would have applied.
    function skip() {
        if (wizard.busy) wizard.release()
        if (wizard.step === 0) wizard.layoutChoice = "keep"
        if (wizard.step === 1) wizard.calFinished = false
        if (wizard.step === 2) wizard.tuning = null
        wizard.step = Math.min(3, wizard.step + 1)
        wizard.refresh()
    }
    function cancel() {
        wizard.release()
        wizard.layoutChoice = "keep"
        wizard.tuning = null
        wizard.cancelled()
    }
    // The only place that applies anything. Returns what was applied, for the log and the tests.
    function finish() {
        wizard.release()
        const applied = []
        // First, so that the layout chosen in step 1 is applied on top of the calibrated numbers, as before.
        if (wizard.calFinished) {
            const kept = Kbm.call(wizard.bridge, "pushKbmCalibration", ["keep"], ({ ok: false }))
            wizard.calKept = kept.ok === true ? String(kept.file || "") : ""
            if (wizard.calKept.length > 0) applied.push("calibration:" + wizard.calKept)
        }
        if (wizard.layoutChoice === "builtin") {
            if (Kbm.call(wizard.bridge, "pushKbmResetBindings", [], false) === true) applied.push("layout:builtin")
        } else if (wizard.layoutChoice.indexOf("file:") === 0) {
            if (Kbm.call(wizard.bridge, "pushKbmLoadProfile", [wizard.layoutChoice.slice(5)], false) === true) applied.push(wizard.layoutChoice)
        }
        if (wizard.tuning !== null) {
            const r = Kbm.call(wizard.bridge, "pushKbmFit", ["apply", ({ context: "all", tuning: wizard.tuning })], ({ ok: false }))
            if (r.ok === true) applied.push("tuning")
        }
        if (wizard.bridge && typeof wizard.bridge.setPreference === "function") wizard.bridge.setPreference("pushKbmWizardDone", true)
        if (wizard.bridge && typeof wizard.bridge.logUi === "function") wizard.bridge.logUi("push-kbm", "wizard finished applied=" + (applied.length ? applied.join(",") : "nothing"))
        wizard.finished()
        return applied
    }
    onVisibleChanged: if (!visible) wizard.release()

    // ---- Step 2 ------------------------------------------------------------------------------------------------------
    function startCalibration() {
        if (wizard.busy || !wizard.streaming || !wizard.canCalibrate) return false
        wizard.calLines = []
        wizard.calKept = ""
        wizard.calFinished = false
        wizard.calWaited = 0
        wizard.calPhase = "arming"
        Kbm.call(wizard.bridge, "pushKbmHold", [true], false)
        return true
    }
    function pollCalibration() {
        if (wizard.calPhase !== "arming" && wizard.calPhase !== "running") return
        Kbm.call(wizard.bridge, "pushKbmHold", [true], false)   // renewed while the page is alive
        if (wizard.calPhase === "arming") {
            const s = Kbm.call(wizard.bridge, "pushKbmCalibration", ["status"], ({}))
            if (s.captured === true) {
                // Started only once the capture has the mouse: taking it would end a calibration begun earlier.
                const r = Kbm.call(wizard.bridge, "pushKbmCalibration", ["start"], ({ ok: false }))
                wizard.addLines(r.messages)
                if (r.ok === true && r.running === true) { wizard.calPhase = "running"; wizard.calStep = String(r.step || "intro") }
                else { wizard.calPhase = "failed"; Kbm.call(wizard.bridge, "pushKbmHold", [false], false) }
            } else if (++wizard.calWaited > 12) {
                wizard.calPhase = "failed"
                Kbm.call(wizard.bridge, "pushKbmHold", [false], false)
            }
            return
        }
        const s = Kbm.call(wizard.bridge, "pushKbmCalibration", ["status"], ({ running: false }))
        wizard.addLines(s.messages)
        if (s.running === true) { wizard.calStep = String(s.step || "intro"); return }
        wizard.calPhase = "done"
        Kbm.call(wizard.bridge, "pushKbmHold", [false], false)
        // Ran to its end with a measurement (the engine's own closing line): the engine wrote and uses the numbers;
        // keeping the whole profile as the file for the next start waits for "Finish and apply". Stopped or without
        // a measurement: nothing was written.
        wizard.calFinished = wizard.calLines.indexOf("calibration finished") >= 0
        wizard.refresh()
    }
    function addLines(lines) {
        if (!lines || lines.length === 0) return
        const all = wizard.calLines.slice()
        for (const line of lines) {
            all.push(String(line))
            // The measured time of a full turn on foot, for the fit of step 3 (the engine's own log line).
            const turn = /walk full turn ([0-9]+(?:\.[0-9]+)?) s/.exec(String(line))
            if (turn !== null && Number(turn[1]) > 0) wizard.gameTurnSeconds = Number(turn[1])
        }
        wizard.calLines = all.slice(-40)
    }
    function calStageName(id) {
        return id === "deadzone-up" || id === "deadzone-down" ? qsTr("死区")
             : id === "turn-time" ? qsTr("全速转一圈") : id === "response" ? qsTr("响应曲线")
             : id === "ratio-up" || id === "ratio-down" ? qsTr("垂直 / 水平比例") : qsTr("准备")
    }
    function calInstruction(id) {
        switch (String(id)) {
        case "deadzone-up": return qsTr("摇杆正在从中心慢慢推出：镜头一开始移动就按空格。")
        case "deadzone-down": return qsTr("摇杆正在慢慢收回：镜头一停下就按空格。")
        case "turn-time": return qsTr("摇杆推到底向右转：选一个参照物，每次它经过画面中心就按空格，共三次。")
        case "response": return qsTr("和上一步一样，但摇杆只推一部分：参照物每次经过画面中心就按空格。")
        case "ratio-up": return qsTr("镜头反复向右、再向上摆动，向上的幅度逐渐变大：看起来和向右一样大时按空格。")
        case "ratio-down": return qsTr("同样的摆动，向上的幅度逐渐变小：看起来和向右一样大时按空格。")
        default: return qsTr("站在空旷的地方，按空格开始；每个情境（步行、瞄准、驾驶）开始前都会先问一次，Enter 跳过该情境。")
        }
    }
    Timer {
        objectName: "pushfps-kbm-wizard-cal-timer"
        interval: 250; repeat: true
        running: wizard.visible && (wizard.calPhase === "arming" || wizard.calPhase === "running")
        onTriggered: wizard.pollCalibration()
    }

    // ---- Step 3 ------------------------------------------------------------------------------------------------------
    readonly property string motionKind: wizard.motions.length < wizard.motionKinds.length ? wizard.motionKinds[wizard.motions.length] : ""
    function motionName(kind) {
        return kind === "flick" ? qsTr("最快的舒适甩动") : kind === "slow" ? qsTr("最慢的稳定移动")
                                : qsTr("转四分之一圈：按你希望的距离移动鼠标")
    }
    property string motionNote: ""
    function fitReason(id) {
        return id === "need-turns" ? qsTr("需要至少 3 次有效的转向动作") : id === "need-flicks" ? qsTr("需要至少 3 次有效的甩动")
             : id === "bad-motion" ? qsTr("有一次动作没有移动，或时间为零") : String(id)
    }
    function startMotion() {
        if (wizard.busy || !wizard.streaming || !wizard.canFit || wizard.motionKind === "") return false
        wizard.motionNote = ""
        wizard.motionWaited = 0
        wizard.motionPhase = "arming"
        Kbm.call(wizard.bridge, "pushKbmHold", [true], false)
        return true
    }
    function pollMotion() {
        if (wizard.motionPhase === "idle") return
        Kbm.call(wizard.bridge, "pushKbmHold", [true], false)
        if (wizard.motionPhase === "arming") {
            const s = Kbm.call(wizard.bridge, "pushKbmState", [], ({}))
            if (s.capture === "captured") {
                const r = Kbm.call(wizard.bridge, "pushKbmFit", ["record", ({ kind: wizard.motionKind })], ({ ok: false }))
                if (r.ok === true) { wizard.motionPhase = "recording"; wizard.motionWaited = 0 }
                else { wizard.motionPhase = "idle"; Kbm.call(wizard.bridge, "pushKbmHold", [false], false) }
            } else if (++wizard.motionWaited > 30) {
                wizard.motionPhase = "idle"
                wizard.motionNote = qsTr("没能开始：请确认 PushFPS 窗口在最前面，然后再试一次。")
                Kbm.call(wizard.bridge, "pushKbmHold", [false], false)
            }
            return
        }
        // Recording: three seconds, then the motion is taken and the mouse given back.
        if (++wizard.motionWaited < 30) return
        wizard.endMotion()
    }
    function endMotion() {
        const kind = wizard.motionKind
        const r = Kbm.call(wizard.bridge, "pushKbmFit", ["stop", ({})], ({ ok: false }))
        wizard.motionPhase = "idle"
        Kbm.call(wizard.bridge, "pushKbmHold", [false], false)
        if (r.ok !== true || !(Number(r.counts) > 0) || !(Number(r.seconds) > 0)) {
            wizard.motionNote = qsTr("没有检测到鼠标移动，请再记录一次")
            return false
        }
        const all = wizard.motions.slice()
        all.push({ kind: kind, counts: Number(r.counts), seconds: Number(r.seconds) })
        wizard.motions = all
        if (all.length >= wizard.motionKinds.length) wizard.runFit()
        return true
    }
    function runFit() {
        const of = kind => wizard.motions.filter(m => m.kind === kind)
        const args = { context: "walk", gameFullTurnSeconds: wizard.gameTurnSeconds, turnCounts: of("turn").map(m => m.counts),
                       flicks: of("flick").map(m => ({ counts: m.counts, seconds: m.seconds })),
                       slow: of("slow").map(m => ({ counts: m.counts, seconds: m.seconds })) }
        wizard.fit = Kbm.call(wizard.bridge, "pushKbmFit", ["fit", args], ({ ok: false, reason: "unavailable" }))
        wizard.tuning = null
    }
    function restartMotions() { if (!wizard.busy) { wizard.motions = []; wizard.fit = null; wizard.tuning = null; wizard.motionNote = "" } }
    function useFit(on) { wizard.tuning = on === true && wizard.fit && wizard.fit.ok === true ? wizard.fit.tuning : null }
    Timer {
        objectName: "pushfps-kbm-wizard-motion-timer"
        interval: 100; repeat: true
        running: wizard.visible && wizard.motionPhase !== "idle"
        onTriggered: wizard.pollMotion()
    }

    readonly property var summary: {
        const lines = []
        lines.push(wizard.layoutChoice === "builtin" ? qsTr("按键：恢复内置的 GTA 风格布局")
                 : wizard.layoutChoice.indexOf("file:") === 0 ? qsTr("按键：载入方案“%1”").arg(wizard.layoutChoice.slice(5))
                 : qsTr("按键：保持不变"))
        lines.push(wizard.tuning !== null ? qsTr("灵敏度：使用测得的数值，并改用“自定义”输入方案") : qsTr("灵敏度：不更改"))
        if (wizard.calFinished) lines.push(qsTr("游戏内校准：引擎已写入并正在使用测得的数值；完成后整个方案也会保存，下次连接 PS5 时继续使用"))
        return lines
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
    // One choice of step 1.
    component Choice: Rectangle {
        id: choice
        property string choiceId: ""
        property string title: ""
        property string line: ""
        readonly property bool selected: wizard.layoutChoice === choiceId
        Layout.fillWidth: true
        implicitHeight: choiceColumn.implicitHeight + 16
        radius: 9
        color: selected ? Theme.accentSoft : choiceHover.hovered ? Theme.ink(0.04) : "transparent"
        border.width: activeFocus ? 2 : 1
        border.color: selected || activeFocus ? Theme.accent : Theme.stroke
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: title
        Keys.onSpacePressed: wizard.layoutChoice = choiceId
        Keys.onReturnPressed: wizard.layoutChoice = choiceId
        ColumnLayout {
            id: choiceColumn
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 12; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: choice.title
                color: Theme.t1
                font.family: Theme.fontUi; font.pixelSize: Theme.fsBody; font.weight: Font.DemiBold
            }
            Line { visible: choice.line.length > 0; text: choice.line }
        }
        HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: wizard.layoutChoice = choice.choiceId }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: body.implicitHeight + 44
        clip: true
        ScrollBar.vertical: VScrollBar { }

        ColumnLayout {
            id: body
            x: 26
            y: 22
            width: Math.min(parent.width - 52, wizard.textWidth)
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                VH1 { text: qsTr("键盘和鼠标向导") }
                Item { Layout.fillWidth: true }
                Text {
                    objectName: "pushfps-kbm-wizard-step"
                    visible: wizard.step < 3
                    text: qsTr("第 %1 步，共 3 步").arg(wizard.step + 1)
                    color: Theme.t3
                    font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
                }
            }
            Line { text: qsTr("每一步都可以跳过；在最后点“完成并应用”之前不会更改任何设置。唯一的例外：你开始的游戏内校准跑完时，引擎会自己写入并立即使用测得的数值。") }

            // ---- 1 ----------------------------------------------------------------------------------------------------
            ColumnLayout {
                objectName: "pushfps-kbm-wizard-layout"
                visible: wizard.step === 0
                Layout.fillWidth: true
                spacing: 8
                VH3 { text: qsTr("1. 选择按键布局") }
                Choice { objectName: "pushfps-kbm-wizard-layout-keep"; choiceId: "keep"; title: qsTr("保持现在的按键") }
                Choice {
                    objectName: "pushfps-kbm-wizard-layout-builtin"
                    choiceId: "builtin"
                    title: qsTr("内置的 GTA 风格布局")
                    line: qsTr("来自公开的按键表，未调校，未在游戏里验证：WASD 移动，Shift 奔跑，空格跳跃，鼠标左键射击，右键瞄准")
                }
                Repeater {
                    model: wizard.files
                    delegate: Choice {
                        required property var modelData
                        objectName: "pushfps-kbm-wizard-layout-file-" + String(modelData.name)
                        choiceId: "file:" + String(modelData.name)
                        title: qsTr("方案文件：%1").arg(String(modelData.name))
                    }
                }
                Line { text: qsTr("以后可以在“按键设置”里逐个修改。") }
            }

            // ---- 2 ----------------------------------------------------------------------------------------------------
            ColumnLayout {
                objectName: "pushfps-kbm-wizard-calibration"
                visible: wizard.step === 1
                Layout.fillWidth: true
                spacing: 8
                VH3 { text: qsTr("2. 死区和灵敏度校准") }
                Line { text: qsTr("在游戏里测量游戏怎样响应右摇杆：电脑只推动右摇杆，你看着画面按键记下。没有任何东西读取画面。") }
                Line { text: qsTr("跑完后数值由引擎写入按键方案文件并立即使用（旧文件保留为 .bak）；中途停止则不做任何更改。") }
                Line {
                    objectName: "pushfps-kbm-wizard-cal-needs"
                    visible: wizard.canCalibrate && !wizard.streaming
                    text: qsTr("需要先连接 PS5 串流、打开键盘和鼠标，并让角色站在空旷的地方。可以先跳过：串流时按 %1 放开鼠标，再回到这个页面。")
                              .arg(String(wizard.st.releaseKeys || "Ctrl+Alt+Shift+Z"))
                    color: Theme.warn
                }
                Line {
                    objectName: "pushfps-kbm-wizard-cal-hotkey"
                    visible: !wizard.canCalibrate
                    text: qsTr("这个版本请在游戏画面里按 Ctrl+Alt+Shift+K 开始校准：空格或鼠标左键记下，Enter 跳过，Esc 停止。")
                    color: Theme.warn
                }
                Line {
                    objectName: "pushfps-kbm-wizard-cal-narrow"
                    visible: wizard.wantsPicture && !wizard.pictureFits
                    text: qsTr("窗口太窄，放不下游戏画面：请先放大窗口。")
                    color: Theme.warn
                }
                KButton {
                    objectName: "pushfps-kbm-wizard-cal-start"
                    visible: wizard.canCalibrate && wizard.streaming && wizard.calPhase !== "running" && wizard.calPhase !== "arming"
                    enabled: wizard.pictureFits && !wizard.busy
                    primary: true
                    text: wizard.calPhase === "done" || wizard.calPhase === "failed" ? qsTr("再校准一次") : qsTr("开始校准")
                    onClicked: wizard.startCalibration()
                }
                Line {
                    objectName: "pushfps-kbm-wizard-cal-state"
                    visible: wizard.calPhase !== "idle"
                    color: wizard.calPhase === "failed" ? Theme.err : Theme.t1
                    text: wizard.calPhase === "arming" ? qsTr("正在接管鼠标…")
                        : wizard.calPhase === "running" ? qsTr("正在进行：%1").arg(wizard.calStageName(wizard.calStep))
                        : wizard.calPhase === "failed" ? qsTr("没能开始：请确认 PushFPS 窗口在最前面，然后再试一次。")
                        : qsTr("校准已结束，鼠标已交还。下面是引擎记录的原文（英文）：")
                }
                Text {
                    objectName: "pushfps-kbm-wizard-cal-instruction"
                    visible: wizard.calPhase === "running"
                    Layout.fillWidth: true
                    text: wizard.calInstruction(wizard.calStep)
                    color: Theme.t1
                    font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                    wrapMode: Text.WordWrap
                }
                Line {
                    visible: wizard.calPhase === "running"
                    text: qsTr("空格或鼠标左键：记下 · Enter：跳过这一步 · Esc：停止（不做更改）")
                }
                Line {
                    objectName: "pushfps-kbm-wizard-cal-kept"
                    visible: wizard.calPhase === "done"
                    color: Theme.t1
                    text: wizard.calFinished ? qsTr("引擎已写入并正在使用这些数值；点“完成并应用”后整个方案也会保存，下次连接 PS5 时继续使用")
                                             : qsTr("这一次没有写入任何数值")
                }
                Text {
                    objectName: "pushfps-kbm-wizard-cal-lines"
                    visible: wizard.calLines.length > 0 && wizard.calPhase !== "arming"
                    Layout.fillWidth: true
                    text: wizard.calLines.slice(-12).join("\n")
                    color: Theme.t2
                    font.family: Theme.fontMono; font.pixelSize: 11
                    wrapMode: Text.WrapAnywhere
                }
            }

            // ---- 3 ----------------------------------------------------------------------------------------------------
            ColumnLayout {
                objectName: "pushfps-kbm-wizard-turn"
                visible: wizard.step === 2
                Layout.fillWidth: true
                spacing: 8
                VH3 { text: qsTr("3. 最大转向速度") }
                Line { text: qsTr("主机只认识摇杆：摇杆推到底时游戏转多快，镜头最快就转多快，鼠标甩得再快也不会更快。") }
                Line { text: qsTr("用你自己的手感来定：做几次测试动作，电脑据此算出多快的鼠标移动算“摇杆推到底”，以及灵敏度和曲线。记录时摇杆保持不动，游戏里不会有任何动作。") }
                Line { text: qsTr("游戏本身的最快转速在第 2 步的“全速转一圈”里测量；这里测的是你的手。") }
                Line {
                    objectName: "pushfps-kbm-wizard-fit-unavailable"
                    visible: !wizard.canFit
                    text: qsTr("这个版本的输入引擎还没有这项测量。")
                    color: Theme.warn
                }
                Line {
                    objectName: "pushfps-kbm-wizard-fit-needs"
                    visible: wizard.canFit && !wizard.streaming
                    text: qsTr("需要正在运行的 PS5 串流并打开键盘和鼠标。可以先跳过。")
                    color: Theme.warn
                }
                ColumnLayout {
                    visible: wizard.canFit && wizard.streaming
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        objectName: "pushfps-kbm-wizard-motion"
                        visible: wizard.motionKind !== ""
                        Layout.fillWidth: true
                        text: qsTr("第 %1 次，共 %2 次：%3").arg(wizard.motions.length + 1).arg(wizard.motionKinds.length).arg(wizard.motionName(wizard.motionKind))
                        color: Theme.t1
                        font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        spacing: 8
                        KButton {
                            objectName: "pushfps-kbm-wizard-record"
                            visible: wizard.motionKind !== ""
                            enabled: !wizard.busy
                            primary: true
                            text: wizard.motionPhase === "recording" ? qsTr("正在记录：现在移动鼠标…")
                                : wizard.motionPhase === "arming" ? qsTr("正在接管鼠标…") : qsTr("记录这一次（约 3 秒）")
                            onClicked: wizard.startMotion()
                        }
                        KButton {
                            objectName: "pushfps-kbm-wizard-motions-restart"
                            visible: wizard.motions.length > 0
                            enabled: !wizard.busy
                            ghost: true
                            text: qsTr("重新开始")
                            onClicked: wizard.restartMotions()
                        }
                    }
                    Line {
                        objectName: "pushfps-kbm-wizard-motion-note"
                        visible: wizard.motionNote.length > 0
                        text: wizard.motionNote
                        color: Theme.warn
                    }
                    Line {
                        objectName: "pushfps-kbm-wizard-fit-failed"
                        visible: wizard.fit !== null && wizard.fit.ok !== true
                        text: wizard.fit !== null ? qsTr("测试动作不足以算出数值：%1").arg(wizard.fitReason(String(wizard.fit.reason || ""))) : ""
                        color: Theme.err
                    }
                    ColumnLayout {
                        id: fitResult
                        objectName: "pushfps-kbm-wizard-fit-result"
                        visible: wizard.fit !== null && wizard.fit.ok === true
                        Layout.fillWidth: true
                        spacing: 4
                        readonly property var t: wizard.fit !== null && wizard.fit.tuning ? wizard.fit.tuning : ({})
                        Text {
                            text: qsTr("算出的数值（步行、瞄准和奔跑共用）：")
                            color: Theme.t1
                            font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
                        }
                        Line { text: qsTr("水平灵敏度 %1 · 垂直灵敏度 %2").arg(Number(fitResult.t.sensX || 0).toFixed(2)).arg(Number(fitResult.t.sensY || 0).toFixed(2)) }
                        Line { text: qsTr("摇杆推到底：%1 计数/秒").arg(Math.round(Number(fitResult.t.fullSpeed || 0))) }
                        Line { text: qsTr("死区补偿 %1").arg(Number(fitResult.t.deadzone || 0).toFixed(2)) }
                        Line { visible: text.length > 0; text: wizard.fit !== null ? String(wizard.fit.explanation || "") : "" }
                        VRow {
                            label: qsTr("完成时使用这些数值")
                            hint: qsTr("会改用“自定义”输入方案；不打开就不更改灵敏度")
                            VSwitch {
                                objectName: "pushfps-kbm-wizard-use-fit"
                                checked: wizard.tuning !== null
                                activeFocusOnTab: true
                                Keys.onSpacePressed: toggled(!checked)
                                onToggled: checked => wizard.useFit(checked)
                            }
                        }
                    }
                }
            }

            // ---- Summary ----------------------------------------------------------------------------------------------
            ColumnLayout {
                objectName: "pushfps-kbm-wizard-summary"
                visible: wizard.step === 3
                Layout.fillWidth: true
                spacing: 6
                VH3 { text: qsTr("点“完成并应用”后：") }
                Repeater {
                    model: wizard.summary
                    delegate: Line {
                        required property string modelData
                        required property int index
                        objectName: "pushfps-kbm-wizard-summary-" + index
                        text: modelData
                        color: Theme.t1
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                KButton { objectName: "pushfps-kbm-wizard-cancel"; text: qsTr("取消"); ghost: true; onClicked: wizard.cancel() }
                Item { Layout.fillWidth: true }
                KButton { objectName: "pushfps-kbm-wizard-back"; text: qsTr("上一步"); visible: wizard.step > 0; enabled: !wizard.busy; onClicked: wizard.back() }
                KButton { objectName: "pushfps-kbm-wizard-skip"; text: qsTr("跳过这一步"); visible: wizard.step < 3; ghost: true; onClicked: wizard.skip() }
                KButton { objectName: "pushfps-kbm-wizard-next"; text: qsTr("下一步"); visible: wizard.step < 3; enabled: !wizard.busy; primary: true; onClicked: wizard.next() }
                KButton { objectName: "pushfps-kbm-wizard-finish"; text: qsTr("完成并应用"); visible: wizard.step === 3; primary: true; onClicked: wizard.finish() }
            }
        }
    }
}
