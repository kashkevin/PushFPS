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

    TestCase {
        name: "PushLatencyTest"
        when: windowShown
        function init() {
            veyra.sourceKind = "ps5"
            veyra.latencyCalls = []
            veyra.latencyState = ({state: "idle"})
            lt.trials = 8
            lt.button = "ps"
            lt.st = ({})
            host.dialog = "latencytest"
            verify(lt.shown)
            tryVerify(() => lt.visible, 2000, "the dialog has faded in")
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
        function test_up_to_500_presses_are_offered() {
            const ids = options("latency-test-trials")
            for (const n of ["5", "8", "12", "50", "200", "500"]) verify(ids.indexOf(n) >= 0, n)
            findChild(lt, "latency-test-trials").picked("500")
            compare(lt.trials, 500)
            lt.begin()
            compare(veyra.latencyCalls[0].trials, 500)
        }
        function test_button_choice_reaches_the_bridge() {
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
        }
        function test_choices_hidden_while_running() {
            lt.begin()
            lt.st = veyra.pushLatencyTestStatus()
            verify(lt.running)
            verify(!findChild(lt, "latency-test-button").visible, "button and presses cannot change while it runs")
        }
    }
}
