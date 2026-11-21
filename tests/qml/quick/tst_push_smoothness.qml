// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Smoothness Mode row (PushSmoothnessRow.qml) against the fixture bridge in QuickSmokeTests.cpp. The fixture does not
// know the preference, so init() adds it to its push* keys (as tst_push_profiles.qml does for pushClean) and the row's
// own pick() keeps the value in the validator's set (src/ui/bridge/Preferences.cpp: 0, 60, 90, 120, 180). The engine's
// state comes from row.stateSource (the bridge's pushSmoothState() in the program). Interface logic only: nothing here
// runs the engine's decision core (tests/remoteplay/SmoothnessTests.inc does).
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 700; height: 600
    PushSmoothnessRow { id: row; x: 10; y: 10; width: 560 }

    TestCase {
        name: "PushSmoothness"
        when: windowShown
        property var engineState: ({})
        function init() {
            if (veyra.pushKeys.indexOf("pushSmoothTarget") < 0) veyra.pushKeys = veyra.pushKeys.concat(["pushSmoothTarget"])
            veyra.preferences = ({})
            veyra.fgMultiplier = 4
            veyra.hasSource = false
            engineState = ({})
            row.stateSource = () => engineState
            row.smooth = ({})
        }
        function cleanup() {
            veyra.fgMultiplier = 1
            veyra.hasSource = true
            veyra.preferences = ({})
        }
        function status() { return findChild(row, "pushfps-smoothness-status") }

        function test_default_is_off_and_writes_nothing() {
            verify(row.visible, "shown while frame generation is on")
            compare(row.choice, 0)
            compare(findChild(row, "pushfps-smoothness").current, "0")
            compare(veyra.preferences.pushSmoothTarget, undefined, "nothing is written until a choice is made")
            verify(findChild(row, "pushfps-smoothness-experimental").visible, "marked experimental")
            verify(findChild(row, "vrow-help").text.length > 0, "the ? explains the mode")
            verify(!status().visible, "no status while off")
        }
        function test_hidden_without_frame_generation() {
            veyra.fgMultiplier = 1
            verify(!row.visible)
            veyra.fgMultiplier = 3
            verify(row.visible)
        }
        function test_choices_write_the_preference() {
            const seg = findChild(row, "pushfps-smoothness")
            compare(seg.options.map(o => o.id).join(","), "0,60,90,120,180")
            seg.picked("120")
            compare(veyra.preferences.pushSmoothTarget, 120)
            compare(row.choice, 120)
            seg.picked("180")
            compare(veyra.preferences.pushSmoothTarget, 180)
            seg.picked("0")
            compare(veyra.preferences.pushSmoothTarget, 0)
            compare(row.choice, 0)
            // Anything else is written as off, never as an unknown rate.
            row.pick("75")
            compare(veyra.preferences.pushSmoothTarget, 0)
        }
        function test_hint_names_the_target_and_the_ceiling() {
            const hint = () => findChild(row, "pushfps-smoothness-row").hint
            const off = hint()
            findChild(row, "pushfps-smoothness").picked("120")
            const on = hint()
            verify(off !== on, "one sentence for off, another for a target")
            verify(on.indexOf("120") >= 0 && on.indexOf("4") >= 0, "the target and the ceiling: " + on)
            veyra.fgMultiplier = 2
            verify(hint() !== on, "a 2X ceiling leaves nothing to choose, and the hint says so")
        }
        function test_status_shows_the_reason_when_it_lowered() {
            findChild(row, "pushfps-smoothness").picked("180")
            engineState = ({available: true, target: 180, chosen: 2, preferred: 3, reason: "step-down-gpu-headroom", stepDowns: 1, loadPercent: 95})
            row.refresh()
            verify(status().visible)
            verify(status().text.indexOf("2") >= 0 && status().text.indexOf("3") >= 0, status().text)
            verify(status().text.indexOf(row.reasonText("step-down-gpu-headroom")) >= 0, "the reason is shown")
            engineState = ({available: true, target: 180, chosen: 3, preferred: 3, reason: "at-ceiling"})
            row.refresh()
            compare(status().text, qsTr("当前 %1×").arg(3), "at the target: no reason, no second number")
            engineState = ({available: true, target: 120, chosen: 2, preferred: 2, reason: "at-ceiling", targetFromSwitch: true})
            row.refresh()
            verify(status().text.indexOf(qsTr("目标由测试开关设定")) >= 0, "a target from the test switch is named")
        }
        function test_no_status_when_the_engine_is_not_deciding() {
            findChild(row, "pushfps-smoothness").picked("120")
            engineState = ({available: true, target: 120, chosen: 0, preferred: 0, reason: "off"})
            row.refresh()
            verify(!status().visible)
        }
        function test_timer_runs_only_while_playing_with_a_target() {
            const timer = findChild(row, "pushfps-smoothness-timer")
            verify(!timer.running)
            findChild(row, "pushfps-smoothness").picked("90")
            verify(!timer.running, "nothing plays")
            veyra.hasSource = true
            verify(timer.running)
            findChild(row, "pushfps-smoothness").picked("0")
            verify(!timer.running)
        }
        function test_every_reason_has_words() {
            for (const r of ["step-down-gpu-headroom", "step-down-rejected-pairs", "step-up-headroom-recovered", "target-below-2x", "hold"])
                verify(row.reasonText(r).length > 0, r)
            compare(row.reasonText("at-ceiling"), "")
        }
    }
}
