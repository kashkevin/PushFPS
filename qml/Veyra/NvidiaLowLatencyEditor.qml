import QtQuick
import QtQuick.Layouts
ColumnLayout {
    id: root
    property string namePrefix: "list"
    property var backend: veyra
    readonly property bool available: root.backend.nvidiaLowLatencyAvailable === true
    Layout.fillWidth: true
    spacing: 4
    VRow {
        objectName: root.namePrefix+"-nvidia-low-latency-row"
        label: qsTr("英伟达低延迟")
        help: qsTr("PushFPS 在每一帧前通过 NVIDIA 驱动做一次类似 Reflex 的等待（NvAPI Sleep），限制显卡里排队的画面。这不是 NVIDIA 控制面板里的“低延迟模式”。采集卡画面的游戏不在这台电脑上运行，所以它只作用于 PushFPS 自己这一段，效果没有测出来。只在 NVIDIA 显卡上可用")
        hintBelow: true
        hint: !root.available ? qsTr("需要 NVIDIA 显卡") : ""
        VSeg {
            objectName: root.namePrefix+"-nvidia-low-latency"
            enabled: root.available
            opacity: enabled ? 1 : 0.4
            options: [{id:"0",label:qsTr("关")},{id:"1",label:qsTr("开")}]
            // Another adapter shows 关: the NVIDIA choice is kept for the next
            // NVIDIA session but does nothing there.
            current: root.available ? (root.backend.nvidiaLowLatency > 0 ? "1" : "0") : "0"
            onPicked: id => root.backend.nvidiaLowLatency=Number(id)
        }
    }
    Text {
        Layout.fillWidth: true
        text: root.backend.running ? root.backend.presentationStatus : qsTr("打开视频后生效")
        color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
        wrapMode: Text.WordWrap
    }
}
