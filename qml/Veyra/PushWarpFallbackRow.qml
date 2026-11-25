// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Stream "ownfg", review 2 SHOULD 4: when the PushWarp generator could not start (its start-up check failed or ran over
// its budget; EngineController's recovery turned frame generation off and says so in backendWarning), this row says so
// and offers NVIDIA VFG with one click where VFG is available. It never switches by itself: only the click does.
import QtQuick
import QtQuick.Layouts

VRow {
    id: fallback
    objectName: "pushfps-pushwarp-fallback-row"
    // The recovery text names PushWarp in every language (a product name, never translated).
    readonly property bool warpFailed: String(veyra.backendWarning || "").indexOf("PushWarp") >= 0
    readonly property bool vfgAvailable: (((veyra.effectCapabilities || {}).vfg) || {}).available === true
    visible: fallback.warpFailed && veyra.fgBackendName !== "vfg"
    label: qsTr("PushWarp 没有启动")
    hint: fallback.vfgAvailable ? qsTr("开启时的检查没有通过或超过时间，补帧已关闭（原因见日志）。可以改回 NVIDIA VFG。")
                                : qsTr("开启时的检查没有通过或超过时间，补帧已关闭（原因见日志）。")
    hintBelow: true
    VButton {
        objectName: "pushfps-pushwarp-fallback-vfg"
        visible: fallback.vfgAvailable
        text: qsTr("改回 NVIDIA VFG")
        onClicked: { veyra.fgBackendName = "vfg"; veyra.fgEnabled = true }
    }
}
