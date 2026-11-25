// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Settings -> Components and licenses: the runtime of the experimental neural layer ("AI picture enhancement").
// Only NVIDIA's signed original nvngx_dlssnr.dll is accepted; it is installed from a file the user already has
// (checked before and after the copy by src/ui/bridge/PushNeural.cpp) and never downloaded by the app.
// docs/NEURAL_RENDERING_RUNTIME.md. Tests: tests/qml/quick/tst_push_neural.qml.
import QtQuick
import QtQuick.Layouts

VGroup {
    id: neural
    objectName: "set-neural"
    property var runtimeState: ({ installed: false, signedOriginal: false, tested: false, usable: false, gpuSupported: true, text: "" })
    readonly property bool installed: runtimeState.installed === true
    readonly property bool accepted: installed && runtimeState.signedOriginal === true
    function refresh() { neural.runtimeState = veyra.pushNeuralRuntimeState() }
    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()
    VRow {
        label: qsTr("神经渲染运行库（实验）")
        hint: qsTr("把 NVIDIA 的神经渲染模型用于解码后的视频画面，没有游戏的运动矢量、深度和遮罩。这不是 NVIDIA 集成在游戏里的 DLSS 5，也未获 NVIDIA 认可；HDR 画面下的色彩准确性尚未验证。只接受 NVIDIA 签名的原版文件，应用不会下载它。NVIDIA 没有公布涵盖其他程序使用此文件的条款：是否使用由你决定")
        VTag {
            objectName: "set-neural-status"
            text: !neural.installed ? qsTr("未安装") : !neural.accepted ? qsTr("已拒绝") : neural.runtimeState.tested === true ? qsTr("已安装") : qsTr("未测试的版本")
            kind: !neural.accepted || neural.runtimeState.usable !== true ? "" : neural.runtimeState.tested === true ? "ok" : "exp"
        }
    }
    VRow {
        label: qsTr("安装与移除")
        hint: String(neural.runtimeState.text || "")
        RowLayout {
            spacing: 6
            VButton {
                objectName: "set-neural-install"
                primary: !neural.installed
                text: neural.installed ? qsTr("更新…") : qsTr("从已有文件安装…")
                onClicked: { veyra.pushInstallNeuralRuntime(); neural.refresh() }
            }
            VButton {
                objectName: "set-neural-remove"
                ghost: true
                visible: neural.installed
                text: qsTr("移除")
                onClicked: { veyra.pushRemoveNeuralRuntime(); neural.refresh() }
            }
        }
    }
}
