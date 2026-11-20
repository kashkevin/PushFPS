// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Settings -> Components and licenses, the neural layer's runtime rows (PushNeuralRuntime) against the fixture
// bridge: the state shown is the bridge's, Install and Remove call it and show the new state, a refused or
// untested file is never shown as plainly installed, and the named looks reach the bridge.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 900; height: 500
    PushNeuralRuntime { id: rows; width: 860 }

    TestCase {
        name: "PushNeural"
        when: windowShown
        readonly property var absent: ({ installed: false, signedOriginal: false, tested: false, version: "none", gpuSupported: true, usable: false, blocker: "runtime-missing", text: "not installed" })
        function init() {
            veyra.neuralRuntime = absent
            veyra.neuralRuntimeAfterInstall = ({ installed: true, signedOriginal: true, tested: true, version: "310.8.0.0", gpuSupported: true, usable: true, blocker: "none", text: "installed 310.8.0.0" })
            veyra.neuralInstallCalls = 0; veyra.neuralRemoveCalls = 0; veyra.neuralLook = ""
            rows.refresh()
        }
        function tag() { return findChild(rows, "set-neural-status") }

        function test_not_installed_offers_install_only() {
            compare(tag().text, qsTr("未安装"))
            compare(tag().kind, "")
            verify(findChild(rows, "set-neural-install").primary)
            verify(!findChild(rows, "set-neural-remove").visible)
        }
        function test_install_shows_the_bridge_state_and_remove_returns() {
            findChild(rows, "set-neural-install").clicked()
            compare(veyra.neuralInstallCalls, 1)
            compare(tag().text, qsTr("已安装"))
            compare(tag().kind, "ok")
            verify(findChild(rows, "set-neural-remove").visible)
            findChild(rows, "set-neural-remove").clicked()
            compare(veyra.neuralRemoveCalls, 1)
            compare(tag().text, qsTr("未安装"))
            verify(!findChild(rows, "set-neural-remove").visible)
        }
        function test_untested_version_is_marked() {
            veyra.neuralRuntimeAfterInstall = ({ installed: true, signedOriginal: true, tested: false, version: "310.9.16.0", gpuSupported: true, usable: true, blocker: "runtime-version-untested", text: "untested" })
            findChild(rows, "set-neural-install").clicked()
            compare(tag().text, qsTr("未测试的版本"))
            compare(tag().kind, "exp")
        }
        function test_refused_file_is_never_shown_as_installed() {
            veyra.neuralRuntime = ({ installed: true, signedOriginal: false, tested: false, version: "310.9.16.0", gpuSupported: true, usable: false, blocker: "not-signed-original", text: "refused" })
            rows.refresh()
            compare(tag().text, qsTr("已拒绝"))
            compare(tag().kind, "")
        }
        function test_card_that_cannot_run_it_is_not_marked_ok() {
            veyra.neuralRuntime = ({ installed: true, signedOriginal: true, tested: true, version: "310.8.0.0", gpuSupported: false, usable: false, blocker: "gpu-not-supported", text: "not supported on this graphics card" })
            rows.refresh()
            compare(tag().kind, "")
        }
        function test_looks_reach_the_bridge() {
            const looks = veyra.pushNeuralLooks()
            compare(looks.length, 4)
            compare(looks[0].id, "conservative")
            compare(looks[3].tone, 1); compare(looks[3].structure, 1)
            verify(looks[0].tone < looks[1].tone && looks[0].structure < looks[1].structure)
            verify(veyra.pushSetNeuralLook("conservative"))
            compare(veyra.neuralLook, "conservative")
            verify(!veyra.pushSetNeuralLook("extreme"))
        }
    }
}
