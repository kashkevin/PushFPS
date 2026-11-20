// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Picture profiles at the top of the quick setup (pushfps-profile): "none" and the five profiles of
// include/veyra/engine/PushPerformanceMode.h, each with one line on what it does and what it costs, the
// profile in use with its own settings underneath (clean-up strength, upscaler and target, the experimental
// neural layer and its look, frame generation target) and "Reset this profile". Choosing a profile writes
// existing settings through PushProfiles.js; "none" puts back what was set before the first profile.
// The hint says which stages run, and names every stage that was dropped or changed with the reason the
// pipeline planner or the bridge gives (a missing neural runtime, no DLSS on this card, a screen smaller
// than the target): nothing is shown as running that does not run.
// Preferences: pushProfile ("none" = the default), pushProfileSettings (what the user changed, per profile),
// pushProfileRestore (the settings from before). Nothing here runs or changes anything until a profile is
// picked. Unverified on Windows.
import QtQuick
import QtQuick.Layouts
import "PushProfiles.js" as Profiles

ColumnLayout {
    id: row
    objectName: "pushfps-profile-rows"
    Layout.fillWidth: true
    spacing: 0

    readonly property var choices: [
        { id: "none", label: qsTr("不使用方案"), line: qsTr("保持自己的设置，什么都不改"), experimental: false },
        { id: "original", label: qsTr("原始画面"),
          line: qsTr("不处理：不清理、不放大、不做神经增强、不补帧。显卡负担最低，不增加延迟"), experimental: false },
        { id: "clean-1440", label: qsTr("清晰 1440p"),
          line: qsTr("清理压缩痕迹，再放大到 2560×1440，补帧目标 120 FPS。显卡负担中等；补帧会让每张真实画面晚一点显示"), experimental: false },
        { id: "enhanced-1440", label: qsTr("增强 1440p"),
          line: qsTr("在「清晰 1440p」上再加一层保守的神经增强。显卡负担高，每帧处理时间更长"), experimental: true },
        { id: "max-fidelity", label: qsTr("最高画质"),
          line: qsTr("画质参考配置：放大到 4K 后按屏幕尺寸显示，补帧目标 120 FPS。显卡负担最高"), experimental: false },
        { id: "ultra-low-latency", label: qsTr("极低延迟"),
          line: qsTr("关闭所有画面处理，显卡只排队一帧，画面一到就显示。补帧保持你原来的设置。延迟最低"), experimental: false }
    ]
    function label(id) { const c = row.choices.find(x => x.id === id); return c ? c.label : row.choices[0].label }

    // The profile in use ("none": the user's own settings).
    readonly property string profile: {
        const id = String(veyra.preferences.pushProfile || "none")
        return Profiles.isProfile(id) ? id : "none"
    }
    readonly property bool active: row.profile !== "none"
    // Home shows the short form: the chooser's cards and the profile's own settings open with the button.
    property bool compact: false
    property bool expanded: !compact
    readonly property bool settingsShown: row.active && row.expanded

    // Live numbers (the stream's size and rate), only looked at while a profile is on and something plays.
    property var stats: ({})
    Timer {
        interval: 2000; repeat: true
        running: row.visible && row.active && veyra.hasSource === true
        triggeredOnStart: true
        onTriggered: row.stats = veyra.pushLiveStats() || ({})
        onRunningChanged: if (!running) row.stats = ({})
    }
    // The screen in pixels; tests set their own.
    property var screenSize: ({ width: Math.round(Screen.width * Screen.devicePixelRatio), height: Math.round(Screen.height * Screen.devicePixelRatio) })
    function environment(live) { return Profiles.environment(veyra, live === undefined ? row.stats : live, row.screenSize) }

    // What the profile in use does on this PC now: { settings, plan, target, drops }.
    readonly property var shown: row.active ? Profiles.preview(veyra, row.profile, row.environment()) : null
    readonly property var own: row.shown !== null ? row.shown.settings : null
    // The last pick's result: what the bridge refused is only known after writing.
    property var lastResult: null

    function pick(id) {
        const live = veyra.hasSource === true ? (veyra.pushLiveStats() || ({})) : ({})
        row.stats = live
        row.lastResult = Profiles.select(veyra, Profiles.isProfile(id) ? id : "none", row.environment(live))
        return row.lastResult !== null
    }
    // One of the profile's own settings, remembered for this profile only and applied at once.
    function change(key, value) {
        if (!row.active) return false
        const result = Profiles.customise(veyra, row.profile, key, value, row.environment())
        if (result === null) return false
        if (result !== true) row.lastResult = result
        return true
    }
    function resetProfile() {
        if (!row.active) return false
        const result = Profiles.reset(veyra, row.profile, row.environment())
        if (result === null) return false
        if (result !== true) row.lastResult = result
        return true
    }

    function upscalerLabel(name) { return name === "dlss" ? "DLSS" : name === "fsr" ? "FSR" : name === "nis" ? "NIS" : qsTr("关闭", "off") }
    function lookLabel(name) {
        switch (name) {
        case "conservative": return qsTr("保守")
        case "subtle": return qsTr("柔和")
        case "balanced": return qsTr("均衡")
        case "strong": return qsTr("强烈")
        }
        return ""
    }
    // The bridge's own words for why something cannot run here, in brackets after the sentence.
    function because(reason) { return String(reason || "").length > 0 ? " (" + reason + ")" : "" }
    // Why DLSS cannot run, when DLSS is what the profile asks for.
    function upscalerReason(caps) { return row.own !== null && row.own.upscaler === "dlss" ? row.because(caps.dlssReason) : "" }

    // One sentence for a stage the planner dropped or changed, or for what the bridge refused.
    function dropText(d) {
        const caps = row.shown !== null && row.shown.plan.caps ? row.shown.plan.caps : ({})
        const hz = Math.round(Number(veyra.displayRefreshHz || 0))
        // The measured-budget rules of the planner (only with measurements from the bridge).
        if (d.reason === "gpu-budget") {
            if (d.stage === "neural") return d.action === "reduced" ? qsTr("神经增强的内部尺寸已降到 %1 行").arg(d.to) : qsTr("实测显卡处理时间不够用：这个方案不做神经增强")
            if (d.stage === "upscale") return d.action === "dropped" ? qsTr("实测显卡处理时间不够用：这个方案不放大") : qsTr("实测显卡处理时间不够用：放大已改用更省的方式")
            if (d.stage === "clean") return qsTr("实测显卡处理时间不够用：这个方案不清理压缩痕迹")
            return ""
        }
        // A stand-in the planner chose itself (the row asks for one that runs, so this is rare).
        if (d.stage === "upscale" && d.reason === "fallback-vendor-neutral")
            return qsTr("所选的放大方式不能在这台电脑上运行：改用 %1 放大").arg(row.upscalerLabel(Profiles.kUpscalers[d.to])) + row.upscalerReason(caps)
        switch (d.stage + ":" + d.reason) {
        case "neural:runtime-missing": return qsTr("神经增强不能在这台电脑上运行，所以这个方案不做神经增强") + row.because(caps.nrReason)
        case "neural:vram-budget": return qsTr("显存不够用：这个方案不做神经增强")
        case "neural:nr-size-cap":
        case "neural:nr-above-input": return qsTr("神经增强的内部尺寸已降到 %1 行").arg(d.to)
        case "note:sr-unavailable":
        case "upscale:runtime-missing": return qsTr("这台电脑不能运行所选的放大方式，也没有可以代替的，所以不放大") + row.upscalerReason(caps)
        case "upscale:scale-one": return qsTr("画面已经是目标尺寸，不需要放大")
        case "upscale:above-display": return qsTr("屏幕只有 %1 行：放大目标已降到屏幕尺寸，不做多余的更大画面").arg(d.to)
        case "upscale:double-upscaler": return qsTr("已关闭双重超分：连续放大两次是多余的")
        case "framegen:runtime-missing": return qsTr("这台电脑现在不能补帧，所以不补帧")
        case "framegen:fg-above-refresh": return qsTr("屏幕是 %1 Hz：倍率已降低，输出不会超过刷新率").arg(hz)
        case "framegen:fg-above-target": return qsTr("倍率已降低：输出不超过这个方案的目标帧率")
        case "framegen:fg-multiplier-one": return qsTr("按这个目标和屏幕没有可以生成的帧，所以不补帧")
        case "framegen:fg-multiplier-unsupported": return qsTr("当前补帧方式没有这个倍率，已改用它能提供的最高倍率")
        case "decode:invalid-request": return qsTr("还不知道画面或屏幕的尺寸，暂时不做处理")
        case "note:sr-fallback-fsr": return qsTr("所选的放大方式不能在这台电脑上运行：改用 %1 放大").arg("FSR") + row.upscalerReason(caps)
        case "note:sr-fallback-nis": return qsTr("所选的放大方式不能在这台电脑上运行：改用 %1 放大").arg("NIS") + row.upscalerReason(caps)
        case "note:display-unknown": return qsTr("还不知道屏幕的尺寸，先按方案的目标尺寸设置")
        case "note:display-rate-unknown": return qsTr("还不知道屏幕的刷新率，所以先不补帧")
        case "note:mode-owns-fg": return qsTr("已选了性能模式：补帧和显示方式由性能模式决定")
        case "note:backend-limit": return qsTr("当前补帧方式没有这个倍率，已改用它能提供的最高倍率")
        case "note:fg-unavailable": return qsTr("这台电脑现在不能补帧，所以不补帧")
        case "note:tearing": return qsTr("驱动没有确认可变刷新可用：允许撕裂，不等屏幕刷新")
        case "note:clean-missing": return qsTr("这个版本没有清理压缩痕迹的功能，所以跳过这一步")
        case "note:sr-refused": return qsTr("放大没有打开：程序没有接受这个设置")
        case "note:nr-refused": return qsTr("神经增强没有打开：程序没有接受这个设置")
        case "note:look-refused": return qsTr("神经增强的风格没有设置成功")
        }
        return ""
    }
    // What only writing shows (the bridge refused a setting), from the last pick of this profile.
    readonly property var refused: {
        const out = []
        if (row.lastResult === null || row.lastResult.profile !== row.profile) return out
        for (const d of row.lastResult.drops)
            if (d.stage === "note" && ["clean-missing", "sr-refused", "nr-refused", "look-refused", "fg-unavailable"].indexOf(d.reason) >= 0) out.push(d)
        return out
    }
    // The stages that run, in order, in plain words.
    readonly property string stageLine: {
        if (row.shown === null) return ""
        const plan = row.shown.plan, target = row.shown.target
        const parts = []
        for (const stage of plan.stages) {
            if (stage.name === "clean") parts.push(qsTr("清理压缩痕迹 %1%").arg(stage.detail))
            else if (stage.name === "upscale") parts.push(qsTr("%1 放大到 %2×%3").arg(row.upscalerLabel(plan.request.sr)).arg(stage.outWidth).arg(stage.outHeight))
            else if (stage.name === "neural") parts.push(qsTr("神经增强（%1）").arg(row.lookLabel(row.own.look)))
        }
        if (target.ownsFg) {
            if (row.own.fg === "keep") parts.push(qsTr("补帧保持原样"))
            else if (target.fg) parts.push(qsTr("%1× 补帧").arg(target.mult))
        }
        return parts.length > 0 ? parts.join(" → ") : qsTr("不做任何画面处理")
    }
    // The settings no longer are what the profile put in place (changed in another row or in Professional mode).
    readonly property bool changedElsewhere: {
        if (row.shown === null) return false
        const target = row.shown.target
        if ((veyra.srEnabled === true) !== target.sr || (veyra.nrAnyEnabled === true) !== target.nr) return true
        return target.ownsFg && target.fgWrite && (veyra.fgEnabled === true) !== target.fg
    }
    readonly property string disclosure: {
        if (row.shown === null) return qsTr("没有使用方案：下面每一项都是你自己的设置。选一个方案，一次设好清理、放大、神经增强和补帧；选「不使用方案」恢复之前的设置")
        const parts = [row.label(row.profile) + ": " + row.stageLine]
        const seen = []
        for (const d of row.shown.drops.concat(row.refused)) {
            const text = row.dropText(d)
            if (text.length > 0 && seen.indexOf(text) < 0) { seen.push(text); parts.push(text) }
        }
        if (row.shown.settings.experimental) parts.push(qsTr("含实验功能，还没有在实机上验证"))
        if (row.shown.settings.customised) parts.push(qsTr("已按你的修改调整"))
        if (row.changedElsewhere && row.refused.length === 0) parts.push(qsTr("当前设置和这个方案不一致（可能在别处改过）；再选一次方案可以重新应用"))
        return parts.join(" · ")
    }

    VRow {
        objectName: "pushfps-profile-row"
        label: qsTr("画面方案")
        help: qsTr("一次选好整条画面处理流程，不用逐项设置：清理串流压缩留下的色块和噪点、放大到 2560×1440、可选的神经增强，以及补帧目标。每个方案都可以单独修改，修改只记在那个方案里。多余的步骤会自动去掉，这台电脑不能运行的步骤会改用它能运行的方式，并在这里说明原因。选「不使用方案」恢复之前的设置")
        hint: row.disclosure
        hintBelow: true
        RowLayout {
            spacing: 8
            VTag {
                objectName: "pushfps-profile-current"
                kind: row.active ? "acc" : ""
                text: row.label(row.profile)
            }
            VButton {
                objectName: "pushfps-profile-expand"
                ghost: true
                text: row.expanded ? qsTr("收起") : qsTr("选择方案")
                onClicked: row.expanded = !row.expanded
            }
        }
    }

    // The chooser: one card for each choice, the one in use marked.
    ColumnLayout {
        objectName: "pushfps-profile"
        visible: row.expanded
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        spacing: 6
        Repeater {
            model: row.choices
            delegate: Item {
                id: choice
                required property var modelData
                readonly property bool current: row.profile === modelData.id
                signal clicked()
                onClicked: row.pick(modelData.id)
                objectName: "pushfps-profile-" + modelData.id
                Layout.fillWidth: true
                implicitHeight: choiceText.implicitHeight + 16
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.description: modelData.line
                Accessible.checkable: true
                Accessible.checked: choice.current
                Accessible.onPressAction: choice.clicked()
                Keys.onReturnPressed: choice.clicked()
                Keys.onSpacePressed: choice.clicked()
                Rectangle {
                    anchors.fill: parent
                    radius: 10
                    color: choice.current ? Theme.accentSoft : choiceHover.hovered ? Theme.card2 : Theme.card
                    border.width: choice.current || choice.activeFocus ? 2 : 1
                    border.color: choice.current || choice.activeFocus ? Theme.accent : choiceHover.hovered ? Theme.stroke2 : Theme.stroke
                }
                ColumnLayout {
                    id: choiceText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            objectName: "pushfps-profile-name"
                            text: choice.modelData.label
                            color: Theme.t1
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                            font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
                        }
                        VTag {
                            visible: choice.modelData.experimental
                            kind: "exp"
                            text: qsTr("实验")
                        }
                        Item { Layout.fillWidth: true }
                        VTag {
                            visible: choice.current && choice.modelData.id !== "none"
                            kind: "acc"
                            text: qsTr("使用中")
                        }
                    }
                    Text {
                        objectName: "pushfps-profile-line"
                        Layout.fillWidth: true
                        text: choice.modelData.line
                        color: Theme.t3
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsHint
                        wrapMode: Text.WordWrap
                    }
                }
                HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: choice.clicked() }
            }
        }
    }

    // --- the profile's own settings -------------------------------------------------------------
    VRow {
        objectName: "pushfps-profile-clean-row"
        visible: row.settingsShown
        label: qsTr("清理强度")
        help: qsTr("在放大之前去掉串流压缩留下的色块、蚊式噪点和色带的力度。0 为关闭。越高清理越多，过高会抹掉细小的纹理")
        hint: row.refused.some(d => d.reason === "clean-missing") ? row.dropText({ stage: "note", reason: "clean-missing" })
            : qsTr("只记在「%1」里").arg(row.label(row.profile))
        value: (row.own !== null ? row.own.clean : 0) + "%"
        VSlider {
            objectName: "pushfps-profile-clean"
            valueFromModel: true
            implicitWidth: 150
            from: 0; to: 100; stepSize: 5
            value: row.own !== null ? row.own.clean : 0
            resettable: true; defaultValue: row.active ? Profiles.defaults(row.profile).clean : 0
            onMoved: v => { if (row.active) row.change("clean", Math.round(v)) }
        }
    }
    VRow {
        objectName: "pushfps-profile-upscaler-row"
        visible: row.settingsShown
        label: qsTr("放大")
        help: qsTr("把 1080p 的串流画面放大到屏幕的尺寸。DLSS 需要 NVIDIA RTX 显卡；FSR 和 NIS 在任何支持 DirectX 12 的显卡上都能运行。目标高于屏幕尺寸时会先算出更大的画面再缩小，更耗显卡")
        hint: {
            if (row.shown === null) return ""
            const fallback = row.shown.drops.find(d => d.stage === "note" && (d.reason.indexOf("sr-fallback-") === 0 || d.reason === "sr-unavailable"))
            if (fallback) return row.dropText(fallback)
            const dropped = row.shown.drops.find(d => d.stage === "upscale")
            return dropped ? row.dropText(dropped) : ""
        }
        hintBelow: true
        RowLayout {
            spacing: 8
            VSeg {
                objectName: "pushfps-profile-upscaler"
                options: [{ id: "off", label: qsTr("关闭", "off") }, { id: "dlss", label: "DLSS" }, { id: "fsr", label: "FSR" }, { id: "nis", label: "NIS" }]
                current: row.own !== null ? row.own.upscaler : "off"
                onPicked: id => row.change("upscaler", id)
            }
            VSeg {
                objectName: "pushfps-profile-target"
                enabled: row.own !== null && row.own.upscaler !== "off"
                opacity: enabled ? 1 : 0.4
                options: [{ id: "qhd", label: "1440p" }, { id: "4k", label: "4K" }]
                current: row.own !== null && row.own.upscaler !== "off" ? row.own.srTarget : ""
                onPicked: id => row.change("srTarget", id)
            }
        }
    }
    VRow {
        objectName: "pushfps-profile-look-row"
        visible: row.settingsShown
        label: qsTr("神经增强（实验）")
        help: qsTr("实验功能，默认关闭。它把 NVIDIA 的神经渲染模型用在已经解码的视频画面上，没有游戏提供的运动向量、深度和遮罩，所以不是游戏内置的官方 DLSS 5，效果也不等同，也没有得到 NVIDIA 的认可。「保守」改动最小，最接近原画；觉得画面不像原作时请选更低的一档或关闭")
        hint: {
            if (row.shown === null) return ""
            const dropped = row.shown.drops.find(d => d.stage === "neural" && (d.reason === "runtime-missing" || d.reason === "vram-budget" || d.reason === "gpu-budget"))
            if (dropped) return row.dropText(dropped)
            const refusedNow = row.refused.find(d => d.reason === "nr-refused" || d.reason === "look-refused")
            if (refusedNow) return row.dropText(refusedNow)
            return row.own.neural ? qsTr("视频画面上的神经增强，不是游戏内置的官方 DLSS 5；更耗显卡") : qsTr("关闭：这个方案不做神经增强")
        }
        hintColor: row.shown !== null && row.own.neural && !row.shown.target.nr ? Theme.warn : Theme.t3
        hintBelow: true
        RowLayout {
            spacing: 8
            VSeg {
                objectName: "pushfps-profile-look"
                enabled: row.own !== null && row.own.neural
                opacity: enabled ? 1 : 0.4
                options: Profiles.looks.map(l => ({ id: l, label: row.lookLabel(l) }))
                current: row.own !== null && row.own.neural ? row.own.look : ""
                onPicked: id => row.change("look", id)
            }
            VSwitch {
                objectName: "pushfps-profile-neural"
                checked: row.own !== null && row.own.neural
                onToggled: checked => row.change("neural", checked)
            }
        }
    }
    VRow {
        objectName: "pushfps-profile-fg-row"
        visible: row.settingsShown
        label: qsTr("补帧目标")
        help: qsTr("方案只定目标帧率，补帧方式保持你原来选的；倍率取不超过目标和屏幕刷新率的最大值，不会为了数字好看而加倍率，也不会打开去除重复画面。「保持原样」不改动补帧。180 FPS 是实验目标：生成的帧越多，每张真实画面显示得越晚")
        hint: {
            if (row.shown === null) return ""
            const note = row.shown.drops.find(d => d.stage === "framegen" || (d.stage === "note" && ["mode-owns-fg", "backend-limit", "fg-unavailable", "display-rate-unknown"].indexOf(d.reason) >= 0))
            if (note) return row.dropText(note)
            return row.own.fg === "180" ? qsTr("实验目标：还没有在实机上验证") : ""
        }
        hintBelow: true
        VSeg {
            objectName: "pushfps-profile-fg"
            enabled: row.shown !== null && row.shown.target.ownsFg
            opacity: enabled ? 1 : 0.4
            options: [{ id: "off", label: qsTr("关闭", "off") }, { id: "keep", label: qsTr("保持原样") },
                      { id: "120", label: "120 FPS" }, { id: "180", label: qsTr("180 FPS（实验）") }]
            current: row.own !== null ? row.own.fg : ""
            onPicked: id => row.change("fg", id)
        }
    }
    VRow {
        objectName: "pushfps-profile-reset-row"
        visible: row.settingsShown
        label: qsTr("恢复默认")
        hint: row.own !== null && row.own.customised ? qsTr("把「%1」恢复成它原来的设置；其他方案不受影响").arg(row.label(row.profile))
                                                     : qsTr("「%1」现在就是默认设置").arg(row.label(row.profile))
        VButton {
            objectName: "pushfps-profile-reset"
            text: qsTr("恢复此方案的默认值")
            enabled: row.own !== null && row.own.customised === true
            opacity: enabled ? 1 : 0.4
            onClicked: row.resetProfile()
        }
    }
}
