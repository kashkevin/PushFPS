// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// "Recommended settings": what PushFPS would set on this PC for PS5 Remote Play, why, and what it could not
// find out. Shown in the first-launch card (PushWelcomeCard.qml) and, on request, under the performance
// mode row (PushModeRow.qml). The plan is engine/PushCalibrationPlan.h through pushCalibration("plan"),
// asked when the panel is first shown and again after a calibration; nothing is asked on a timer.
//   Apply              puts the plan in place (PushModes.js applyCalibration) and lists what it changed.
//   Undo               puts those settings back while they still hold what Apply left.
//   Keep my settings   changes nothing and remembers it (pushCalibrationKept): the first-launch card then
//                      stops offering the recommendation.
//   Details            the reasons in full, the smoother choice (never applied by Apply) and the unknowns.
//   Measure            the short calibration (PushCalibrationRun.qml), offered only when a measurement is the
//                      one thing missing before frame generation could be recommended.
// Nothing changes without Apply, and nothing is ever applied again by itself. Unverified on Windows.
import QtQuick
import QtQuick.Layouts
import "PushModes.js" as Modes

ColumnLayout {
    id: panel
    objectName: panel.prefix
    spacing: 6

    // Tests replace the bridge: planHook(args) returns a plan; codecIo { codec(), setCodec(n) } stands for
    // the PS5 source's codec setting.
    // Several panels exist at once (the first-launch card, the mode row on each page): their controls are
    // named "pushfps-calibration[-<host>]-...".
    property string host: ""
    readonly property string prefix: "pushfps-calibration" + (panel.host.length > 0 ? "-" + panel.host : "")
    property var planHook: null
    property var codecIo: null
    property var plan: null
    property bool loaded: false
    property var measured: null
    property bool showDetails: false
    property string runFailure: ""
    // The game rate the plan was made for (0 = unknown).
    property int game: 0

    function stats() { return veyra.hasSource === true && typeof veyra.pushLiveStats === "function" ? (veyra.pushLiveStats() || ({})) : ({}) }
    function load() {
        panel.loaded = true
        panel.game = Modes.calibrationGame(veyra, panel.stats())
        const args = { game: panel.game, method: String(veyra.fgBackendName || "") }
        if (panel.measured) args.measured = panel.measured
        let result = null
        if (panel.planHook) result = panel.planHook(args)
        else if (typeof veyra.pushCalibration === "function") result = veyra.pushCalibration("plan", args)
        panel.plan = result && result.valid === true ? result : null
    }
    // Asked again (never on a timer) when what the plan depends on changes: the game frame rate chosen in
    // the quick setup, or a source starting (its rate can then be measured).
    readonly property int gameChoice: Number(veyra.preferences.pushGameFps || 0)
    onGameChoiceChanged: if (panel.loaded && !calibration.running) panel.load()
    readonly property bool playing: veyra.hasSource === true
    onPlayingChanged: if (panel.loaded && !calibration.running) panel.load()
    readonly property bool usable: panel.plan !== null
    readonly property var record: { veyra.preferences; return Modes.appliedRecord(veyra) }
    readonly property bool kept: veyra.preferences.pushCalibrationKept === true
    readonly property real fps: panel.game > 0 ? panel.game : Modes.chosenFps(veyra)
    // What Apply would change now (bindings: the settings it reads).
    readonly property var pending: {
        veyra.fgEnabled; veyra.fgMultiplier; veyra.fgBackendName; veyra.vfgQuality; veyra.nrAnyEnabled; veyra.srEnabled; veyra.preferences; veyra.ps5
        return panel.usable ? Modes.calibrationChanges(veyra, panel.plan, panel.fps, panel.codecIo) : []
    }
    readonly property bool measurable: panel.usable && panel.plan.measured !== true && String(panel.plan.fgReason) === "not-measured"

    function apply() {
        if (!panel.usable || calibration.running) return null
        if (Modes.measurementBusy(veyra)) { panel.runFailure = "busy"; return null }
        return Modes.applyCalibration(veyra, panel.plan, panel.fps, panel.codecIo)
    }
    function undo() { return Modes.undoCalibration(veyra, panel.fps, panel.codecIo) }
    function keep() { veyra.setPreference("pushCalibrationKept", true) }
    function measure() {
        panel.runFailure = ""
        return panel.measurable && calibration.begin(String(panel.plan.fgMethod || ""))
    }

    PushCalibrationRun {
        id: calibration
        objectName: panel.prefix + "-run"
        onFinished: measured => { panel.measured = measured; panel.load() }
        onFailed: why => panel.runFailure = why
    }

    // --- words ---------------------------------------------------------------------------------------
    function modeLabel(id) {
        switch (id) {
        case "ultra-low-latency": return qsTr("超低延迟")
        case "competitive": return qsTr("竞技")
        case "balanced": return qsTr("均衡")
        case "maximum-smoothness": return qsTr("最大流畅")
        }
        return qsTr("关闭（自己的设置）")
    }
    function methodLabel(id) {
        const names = { vfg: "NVIDIA VFG", dlss: qsTr("DLSS 帧生成"), anr: "ANR", fsr4: "FSR 4", xess: "XeSS", fsr3: "FSR 3.1", amdfrc: "AMD FRC", pushflow: "PushFlow" }
        return names[id] || String(id)
    }
    function codecLabel(n) { return n === 2 ? "H.265 HDR" : n === 1 ? "H.265" : "H.264" }
    function onOff(on) { return on ? qsTr("开") : qsTr("关") }
    function fgReasonText(reason) {
        const p = panel.plan || ({})
        switch (reason) {
        case "generation-fits": return qsTr("已实测：显卡有足够余量，2× 补帧每张真实画面至少晚 %1 毫秒").arg(Number(p.minAddedDelayMs).toFixed(1))
        case "game-rate-unknown": return qsTr("还不知道游戏的帧率：在快速设置里选「游戏帧率」或开始游玩后再看")
        case "game-thirty": return qsTr("30 FPS 游戏：任何补帧都会让每张真实画面至少晚 16.7 毫秒，反应优先，所以不补帧")
        case "display-unknown": return qsTr("还不知道屏幕的刷新率")
        case "source-fills-display": return qsTr("游戏帧率已经达到屏幕刷新率，补出的画面显示不出来")
        case "output-above-refresh": return qsTr("补帧后的帧率超过屏幕刷新率")
        case "uneven-without-vrr": return qsTr("没有可用的可变刷新：补帧后的帧率在这块屏幕上显示时间不均匀")
        case "gpu-unknown": return qsTr("没能识别显卡")
        case "power-unknown": return qsTr("不知道电脑是否在用电池")
        case "on-battery": return qsTr("电脑正在用电池")
        case "integrated-gpu": return qsTr("集成显卡：留给解码和显示")
        case "gpu-memory-unknown": return qsTr("没能读到独立显卡的显存")
        case "fg-vram-low": return qsTr("显存不足")
        case "fg-feature-level": return qsTr("显卡不支持需要的 D3D12 功能")
        case "not-measured": return qsTr("还没有实测显卡用时；可以运行下面约 16 秒的校准")
        case "measured-other-method": return qsTr("实测用的是另一种补帧方式，请重新校准")
        case "gpu-load-high": return qsTr("实测：显卡余量不够")
        case "generated-frame-cost-high": return qsTr("实测：生成一帧的时间太长")
        case "missed-deadlines": return qsTr("实测：有生成的画面错过了显示时间")
        case "skipped-pairs": return qsTr("实测：显卡来不及，补帧经常被跳过")
        }
        return qsTr("这台电脑没有可用的补帧方式")
    }
    function codecReasonText(reason) {
        switch (reason) {
        case "hevc-decode": return qsTr("显卡有 H.265 硬件解码")
        case "hevc-decode-missing": return qsTr("显卡没有 H.265 硬件解码")
        }
        return qsTr("不知道显卡能否硬件解码 H.265")
    }
    function hdrReasonText(reason) {
        switch (reason) {
        case "hdr-on": return qsTr("屏幕已在 HDR 模式，显卡能解码 10-bit H.265")
        case "hdr-display-off": return qsTr("Windows 里屏幕没有开 HDR")
        case "hdr-needs-hevc": return qsTr("HDR 需要 H.265")
        case "hdr-needs-hevc10-decode": return qsTr("显卡没有 10-bit H.265 硬件解码")
        }
        return qsTr("不知道屏幕是否在 HDR 模式")
    }
    function unknownText(key) {
        switch (key) {
        case "game-rate": return qsTr("游戏帧率")
        case "display-refresh": return qsTr("屏幕刷新率")
        case "variable-refresh": return qsTr("可变刷新")
        case "gpu": return qsTr("显卡")
        case "gpu-memory": return qsTr("显存")
        case "video-decode": return qsTr("硬件解码")
        case "hdr-display": return qsTr("屏幕 HDR")
        case "power-source": return qsTr("电源")
        case "gpu-cost": return qsTr("显卡实际用时")
        }
        return key
    }
    function changeText(c) {
        const v = x => c.key === "backend" ? panel.methodLabel(x) : c.key === "tier" ? String(x) : c.key === "codec" ? panel.codecLabel(x)
                      : c.key === "mode" ? panel.modeLabel(x) : panel.onOff(x === true)
        const name = c.key === "backend" ? qsTr("补帧方式") : c.key === "tier" ? qsTr("PushFlow 档位") : c.key === "nr" ? qsTr("AI 画质增强")
                   : c.key === "sr" ? qsTr("超分辨率") : c.key === "codec" ? qsTr("PS5 编码（下次连接生效）") : qsTr("性能模式")
        return name + qsTr("：") + v(c.before) + " → " + v(c.after)
    }
    readonly property string hardwareText: {
        if (!panel.usable) return ""
        const p = panel.plan
        // Vendor (when it is one of the three), then the kind: "NVIDIA · dedicated GPU", "Integrated GPU".
        const vendor = p.vendor === "nvidia" ? "NVIDIA" : p.vendor === "amd" ? "AMD" : p.vendor === "intel" ? "Intel" : ""
        const kind = p.gpuClass === "integrated" ? qsTr("集成显卡") : p.gpuClass === "dedicated" ? qsTr("独立显卡") : qsTr("显卡类型未知")
        const parts = vendor.length > 0 ? [vendor, kind] : [kind]
        if (Number(p.vramGB) > 0) parts.push(qsTr("%1 GB 显存").arg(p.vramGB))
        parts.push(Number(p.refreshHz) > 0 ? Math.round(Number(p.refreshHz)) + " Hz" : qsTr("刷新率未知"))
        const v = Number(p.vrrState)
        parts.push(v >= 4 ? qsTr("可变刷新生效中") : v === 3 ? qsTr("可变刷新可用") : v === 2 ? qsTr("可变刷新当前不可用")
                   : v === 1 ? qsTr("不支持可变刷新") : qsTr("可变刷新无法检测"))
        return parts.join(" · ")
    }
    readonly property var lines: {
        if (!panel.usable) return []
        const p = panel.plan
        const fg = p.frameGeneration === true
            ? qsTr("开：%1，%2×，%3 FPS").arg(panel.methodLabel(p.fgMethod)).arg(p.multiplier).arg(Math.round(Number(p.outputFps)))
            : qsTr("关")
        return [
            { k: qsTr("性能模式"), v: panel.modeLabel(p.mode), why: p.modeReason === "generation-fits" ? qsTr("最低延迟的补帧模式") : qsTr("反应最快：不补帧，不等待") },
            { k: qsTr("补帧"), v: fg, why: panel.fgReasonText(String(p.fgReason)) },
            { k: qsTr("PS5 编码"), v: panel.codecLabel(Modes.planCodec(p)), why: panel.codecReasonText(String(p.codecReason)) + " · " + panel.hdrReasonText(String(p.hdrReason)) },
            { k: qsTr("AI 画质增强"), v: qsTr("关"), why: qsTr("每张真实画面都要占用显卡时间") }
        ]
    }
    readonly property string unknownLine: panel.usable && (panel.plan.unknown || []).length > 0
        ? qsTr("无法确定：%1").arg(panel.plan.unknown.map(k => panel.unknownText(k)).join("、")) : ""
    readonly property string estimateLine: !panel.usable ? "" : panel.plan.measured === true
        ? qsTr("显卡用时已在这台电脑上实测过一次；其余仍是估算，不是延迟测量")
        : qsTr("这是根据检测到的功能做的估算，还没有在这台电脑上实测")
    readonly property string smootherLine: panel.usable && panel.plan.smootherOffered === true
        ? qsTr("更流畅的选择（不会被「应用」设置）：「均衡」%1× 到 %2 FPS，每张真实画面至少晚 %3 毫秒，可在性能模式里自己选")
              .arg(panel.plan.smootherMultiplier).arg(Math.round(Number(panel.plan.smootherOutputFps))).arg(Number(panel.plan.smootherDelayMs).toFixed(1))
        : ""
    readonly property string failureText: {
        switch (panel.runFailure) {
        case "": return ""
        case "no-source": return qsTr("校准需要正在播放的画面：先连接 PS5 开始游玩")
        case "busy": return qsTr("另一项测试或校准正在进行：等它结束后再试，什么都没有改动")
        case "cancelled": return qsTr("校准已取消，设置已恢复")
        case "source-changed": return qsTr("画面来源变了，校准已停止，设置已恢复")
        }
        return qsTr("校准没有得到可用的数字，设置已恢复")
    }

    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-title"
        text: qsTr("推荐设置")
        color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsBody
        font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-empty"
        visible: !panel.usable
        text: panel.loaded ? qsTr("没能读到这台电脑的显卡信息，所以没有推荐，什么都不会改动") : qsTr("正在读取……")
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-hardware"
        visible: panel.usable
        text: panel.hardwareText
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Repeater {
        model: panel.lines
        delegate: Text {
            required property var modelData
            required property int index
            objectName: panel.prefix + "-line-" + index
            Layout.fillWidth: true
            text: modelData.k + qsTr("：") + modelData.v + (panel.showDetails ? " · " + modelData.why : "")
            color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
        }
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-details-text"
        visible: panel.usable && panel.showDetails
        text: [panel.smootherLine, panel.unknownLine].filter(t => t.length > 0).join("\n")
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-estimate"
        visible: panel.usable
        text: panel.estimateLine
        color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-status"
        visible: text.length > 0
        text: calibration.running ? qsTr("正在校准……%1%（画面会短暂变化，结束后恢复）").arg(Math.round(calibration.progress * 100)) : panel.failureText
        color: Theme.warn; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        objectName: panel.prefix + "-changed"
        visible: panel.record !== null
        text: panel.record ? qsTr("「应用」改动了：") + "\n" + panel.record.changes.map(c => panel.changeText(c)).join("\n") : ""
        color: Theme.t2; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint; wrapMode: Text.WordWrap
    }
    Flow {
        Layout.fillWidth: true
        spacing: 8
        VButton {
            objectName: panel.prefix + "-apply"
            visible: panel.usable
            primary: true
            enabled: panel.pending.length > 0 && !calibration.running
            text: panel.pending.length > 0 ? qsTr("应用") : qsTr("已是推荐设置")
            onClicked: panel.apply()
        }
        VButton {
            objectName: panel.prefix + "-undo"
            visible: panel.record !== null
            enabled: !calibration.running
            text: qsTr("撤销")
            onClicked: panel.undo()
        }
        VButton {
            objectName: panel.prefix + "-measure"
            visible: panel.measurable && !calibration.running
            enabled: veyra.hasSource === true
            tip: veyra.hasSource === true ? "" : qsTr("先开始游玩")
            text: qsTr("校准（约 16 秒）")
            onClicked: panel.measure()
        }
        VButton {
            objectName: panel.prefix + "-cancel"
            visible: calibration.running
            text: qsTr("取消校准")
            onClicked: calibration.cancel()
        }
        VButton {
            objectName: panel.prefix + "-details"
            visible: panel.usable
            ghost: true
            text: panel.showDetails ? qsTr("收起详情") : qsTr("详情")
            onClicked: panel.showDetails = !panel.showDetails
        }
        VButton {
            objectName: panel.prefix + "-keep"
            visible: panel.usable && !panel.kept && panel.record === null
            ghost: true
            text: qsTr("保留我的设置")
            onClicked: panel.keep()
        }
    }
}
