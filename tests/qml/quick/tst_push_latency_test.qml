// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// PS5 latency test dialog (PushLatencyTestDialog.qml) against the fixture bridge in QuickSmokeTests.cpp:
// the original test is asked for exactly as before (PS button, 8 presses by default, pushLatencyTestBegin),
// latency test v2 offers up to 500 presses and the OPTIONS / D-pad buttons (pushLatencyTestBeginWith), and the
// split results appear only when the bridge reports them. The bridge's own test logic is
// src/ui/bridge/PushLatencyTest.cpp; its statistics are tested in tests/remoteplay/LatencyStatsTests.inc.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 900; height: 1200
    Item { id: host; anchors.fill: parent; property string dialog: ""; property real motionScale: 1; function close() { dialog = "" } }
    PushLatencyTestDialog { id: lt; dialogHost: host }
    Component { id: freshDialog; PushLatencyTestDialog { dialogHost: host } }

    TestCase {
        name: "PushLatencyTest"
        when: windowShown
        function init() {
            veyra.sourceKind = "ps5"
            veyra.latencyCalls = []
            veyra.latencyOriginalBegins = 0
            veyra.latencyState = ({state: "idle"})
            lt.trials = 8
            lt.button = "ps"
            lt.st = ({})
            host.dialog = "latencytest"
            verify(lt.shown)
            tryVerify(() => lt.visible, 2000, "the dialog has faded in")
            lt.idleStatus = ({})
        }
        function cleanup() { veyra.latencyState = ({state: "idle"}) }
        function options(name) { return findChild(lt, name).options.map(o => o.id) }

        function test_defaults_run_the_original_test() {
            compare(lt.trials, 8, "the default number of presses is unchanged")
            compare(lt.button, "ps")
            lt.begin()
            compare(veyra.latencyCalls.length, 1)
            compare(veyra.latencyCalls[0].trials, 8)
            compare(veyra.latencyCalls[0].button, "ps", "the PS button goes through pushLatencyTestBegin, as before")
            verify(lt.running)
        }
        // Review S5 (orchestrator's decision): the 50/200/500 and button choices stay visible without a switch, so
        // the default of a NEW dialog must be exactly the old test: 8 presses of the PS button through the original
        // entry point pushLatencyTestBegin.
        function test_new_dialog_default_is_exactly_the_old_test() {
            const d = freshDialog.createObject(root)
            verify(d !== null)
            compare(d.trials, 8)
            compare(d.button, "ps")
            compare(findChild(d, "latency-test-trials").current, "8")
            compare(findChild(d, "latency-test-button").current, "ps")
            d.begin()
            compare(veyra.latencyOriginalBegins, 1, "the original entry point, not pushLatencyTestBeginWith")
            compare(veyra.latencyCalls.length, 1)
            compare(veyra.latencyCalls[0].trials, 8)
            compare(veyra.latencyCalls[0].button, "ps")
            veyra.pushLatencyTestCancel()
            d.destroy()
        }
        function test_up_to_500_presses_are_offered() {
            const ids = options("latency-test-trials")
            for (const n of ["5", "8", "12", "50", "200", "500"]) verify(ids.indexOf(n) >= 0, n)
            findChild(lt, "latency-test-trials").picked("500")
            compare(lt.trials, 500)
            lt.begin()
            compare(veyra.latencyCalls[0].trials, 500)
        }
        function enableButtons() { lt.idleStatus = ({state: "idle", buttonsAvailable: true}) }
        // OPTIONS / D-pad are an experiment (VEYRA_EXP_RP_LATENCY_BUTTONS=1, status buttonsAvailable): hidden by
        // default, and then the PS button is used whatever was chosen before.
        function test_button_choice_hidden_unless_the_bridge_offers_it() {
            verify(!findChild(lt, "latency-test-button-row").visible, "hidden without buttonsAvailable")
            verify(findChild(lt, "latency-test-trials").visible, "50/200/500 stay offered for the PS button")
            lt.button = "options"
            lt.begin()
            compare(veyra.latencyOriginalBegins, 1, "the PS button through the original entry point")
            compare(veyra.latencyCalls[0].button, "ps")
            veyra.pushLatencyTestCancel()
            lt.st = ({state: "idle", buttonsAvailable: false})
            verify(!findChild(lt, "latency-test-button-row").visible)
            lt.st = ({state: "idle", buttonsAvailable: true})
            verify(findChild(lt, "latency-test-button-row").visible, "shown when the bridge reports buttonsAvailable")
            verify(findChild(lt, "latency-test-button-experimental").visible, "marked experimental")
        }
        function test_button_choice_reaches_the_bridge() {
            enableButtons()
            compare(options("latency-test-button").join(","), "ps,options,dpad-right-left")
            findChild(lt, "latency-test-button").picked("dpad-right-left")
            findChild(lt, "latency-test-trials").picked("200")
            compare(lt.secondsPerPress, 1)
            lt.begin()
            compare(veyra.latencyCalls[0].button, "dpad-right-left")
            compare(veyra.latencyCalls[0].trials, 200)
            veyra.pushLatencyTestCancel()
            findChild(lt, "latency-test-button").picked("options")
            lt.begin()
            compare(veyra.latencyCalls[1].button, "options")
        }
        function test_split_results_only_when_reported() {
            lt.st = ({state: "done", answered: 12, trials: 12, medianMs: 556, minMs: 512, maxMs: 584, pushfpsMs: 10, remoteMs: 546})
            verify(findChild(lt, "latency-test-total").visible)
            verify(!findChild(lt, "latency-test-console").visible, "the original test shows no split")
            verify(!findChild(lt, "latency-test-pc").visible)
            verify(!findChild(lt, "latency-test-detectable").visible)
            lt.st = ({state: "done", answered: 200, trials: 200, medianMs: 120, minMs: 90, maxMs: 160, pushfpsMs: 10, remoteMs: 110,
                      split: true, consoleMs: 105.25, pcMs: 9.4, pcBasis: "present", detectableMs: 3.2})
            verify(findChild(lt, "latency-test-console").visible)
            compare(findChild(lt, "latency-test-console").value, "105.3 ms")
            compare(findChild(lt, "latency-test-pc").value, "9.4 ms")
            compare(findChild(lt, "latency-test-detectable").value, "3.2 ms")
            lt.st = ({state: "done", answered: 3, trials: 3, medianMs: 120, split: true, consoleMs: -1, pcMs: -1, detectableMs: -1})
            verify(!findChild(lt, "latency-test-console").visible, "unknown values stay hidden")
            verify(!findChild(lt, "latency-test-detectable").visible)
            // Review S6: with fewer than 30 answered presses the figure is unstable: a plain sentence instead.
            lt.st = ({state: "done", answered: 12, trials: 12, medianMs: 120, split: true, consoleMs: 105, pcMs: 9, detectableMs: 40})
            verify(!findChild(lt, "latency-test-detectable").visible, "hidden under 30 presses")
            verify(findChild(lt, "latency-test-detectable-few").visible)
            lt.st = ({state: "done", answered: 30, trials: 30, medianMs: 120, split: true, consoleMs: 105, pcMs: 9, detectableMs: 20})
            verify(findChild(lt, "latency-test-detectable").visible, "shown from 30 presses")
            verify(!findChild(lt, "latency-test-detectable-few").visible)
        }
        function test_presses_not_measurable_because_the_picture_moved() {
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, split: true, consoleMs: 105, pcMs: 9, detectableMs: 20})
            verify(!findChild(lt, "latency-test-unmeasurable").visible, "absent count: nothing shown")
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, unknownPresses: 0})
            verify(!findChild(lt, "latency-test-unmeasurable").visible)
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, unknownPresses: 10})
            verify(findChild(lt, "latency-test-unmeasurable").visible)
            verify(findChild(lt, "latency-test-unmeasurable").hint.indexOf("10") >= 0)
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, split: true, unmeasurable: 7, stopReason: ""})
            verify(findChild(lt, "latency-test-unmeasurable").hint.indexOf("7") >= 0, "the bridge's key (exp/s4-autofix)")
            lt.st = ({state: "failed", error: "", trial: 6, trials: 50, answered: 1, split: true, unmeasurable: 5, stopReason: "picture-moving"})
            verify(findChild(lt, "latency-test-unmeasurable").visible, "also when the test stopped early")
            verify(findChild(lt, "latency-test-failed").hint.length > 0, "a stop for a moving picture is explained")
        }
        function test_menu_buttons_ask_for_a_still_picture() {
            enableButtons()
            findChild(lt, "latency-test-button").picked("options")
            lt.st = ({})
            const hint = findChild(lt, "latency-test-before").hint
            verify(hint.indexOf(qsTr("进入游戏画面")) < 0, "OPTIONS is not tested over moving gameplay")
            verify(hint.indexOf(qsTr("静止")) >= 0, hint)
            findChild(lt, "latency-test-button").picked("dpad-right-left")
            verify(findChild(lt, "latency-test-before").hint.indexOf(qsTr("静止")) >= 0)
            findChild(lt, "latency-test-button").picked("ps")
            verify(findChild(lt, "latency-test-before").hint.indexOf(qsTr("进入游戏画面")) === 0, "the PS button text is unchanged")
        }
        function test_not_valid_when_the_picture_changes_by_itself() {
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120})
            verify(!findChild(lt, "latency-test-not-valid").visible, "no sham counts: nothing said")
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, shams: 5, shamAnswered: 0})
            verify(!findChild(lt, "latency-test-not-valid").visible)
            lt.st = ({state: "done", answered: 40, trials: 50, medianMs: 120, shams: 5, shamAnswered: 2})
            verify(findChild(lt, "latency-test-not-valid").visible)
        }
        function test_early_stop_always_says_why() {
            lt.st = ({state: "failed", error: "PS5 串流已停止，延迟测试已取消"})
            compare(findChild(lt, "latency-test-failed").hint, "PS5 串流已停止，延迟测试已取消", "the bridge's reason")
            lt.st = ({state: "failed", error: ""})
            verify(findChild(lt, "latency-test-failed").hint.length > 0, "a plain fallback when the reason is empty")
        }
        function test_choices_hidden_while_running() {
            lt.begin()
            lt.st = veyra.pushLatencyTestStatus()
            verify(lt.running)
            verify(!findChild(lt, "latency-test-button").visible, "button and presses cannot change while it runs")
        }
    }
}
