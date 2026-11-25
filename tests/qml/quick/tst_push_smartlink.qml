// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// SmartLink "Connection" row (PushSmartLinkRow.qml) against the fixture bridge in QuickSmokeTests.cpp, whose
// setPreference refuses unknown push* keys and out-of-range pushSmartLink values as the real validator does
// (src/ui/bridge/Preferences.cpp). The state is what pushSmartLink() reports (veyra.smartLinkState here); the
// bridge itself is src/ui/bridge/PushSmartLink.cpp. Interface logic only: nothing here runs SmartLink.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 700; height: 600
    PushSmartLinkRow { id: row; x: 10; y: 10; width: 560 }

    TestCase {
        name: "PushSmartLink"
        when: windowShown
        function init() {
            veyra.preferences = ({})
            veyra.sourceKind = ""
            veyra.hasSource = false
            veyra.smartLinkState = ({available: true, active: false, mode: "off", profile: "unknown", advice: 0})
            row.checkAvailable()
            row.link = ({})
        }
        function cleanup() {
            veyra.sourceKind = ""
            veyra.hasSource = true
            veyra.preferences = ({})
        }
        function streamWith(state) {
            veyra.sourceKind = "ps5"
            veyra.hasSource = true
            veyra.smartLinkState = Object.assign({available: true, active: true, mode: "observe", profile: "unknown", advice: 0,
                                                  reorderMs: 0, audioTargetMs: 0}, state)
            row.refresh()
        }
        function status() { return findChild(row, "pushfps-smartlink-status") }

        function test_default_is_off_and_sets_nothing() {
            verify(row.visible, "shown with nothing playing and the default PS5 console profile")
            compare(row.choice, 0)
            compare(findChild(row, "pushfps-smartlink").current, "0")
            compare(veyra.preferences.pushSmartLink, undefined, "nothing is written until a choice is made")
            verify(findChild(row, "pushfps-smartlink-experimental").visible, "marked experimental")
            verify(findChild(row, "vrow-help").text.length > 0, "the ? explains Automatic")
        }
        function test_three_choices_write_the_preference() {
            const seg = findChild(row, "pushfps-smartlink")
            compare(seg.options.map(o => o.id).join(","), "0,1,2")
            seg.picked("1")
            compare(veyra.preferences.pushSmartLink, 1)
            compare(row.choice, 1)
            seg.picked("2")
            compare(veyra.preferences.pushSmartLink, 2)
            compare(seg.current, "2")
            seg.picked("0")
            compare(veyra.preferences.pushSmartLink, 0)
            compare(row.choice, 0)
        }
        function test_fixture_refuses_unknown_keys_and_values() {
            verify(!veyra.setPreference("pushSmartLinks", 1), "an unknown push* key is refused, as by the real validator")
            verify(!veyra.setPreference("pushSmartLink", 3), "out of range")
            verify(!veyra.setPreference("pushSmartLink", -1))
            compare(veyra.preferences.pushSmartLink, undefined)
            verify(veyra.setPreference("pushSmartLink", 2))
        }
        function test_hint_per_choice_and_next_connection() {
            const hint = () => findChild(row, "pushfps-smartlink-row").hint
            const off = hint()
            findChild(row, "pushfps-smartlink").picked("1")
            const advise = hint()
            findChild(row, "pushfps-smartlink").picked("2")
            const auto = hint()
            verify(off !== advise && advise !== auto && off !== auto, "one sentence per choice")
            verify(auto.indexOf(qsTr("更改在下次连接 PS5 时生效")) < 0, "no next-connection note with nothing playing")
            streamWith({})
            verify(hint().indexOf(qsTr("更改在下次连接 PS5 时生效")) >= 0, "during a stream a change applies at the next connection")
        }
        function test_status_for_each_profile() {
            const words = {"wired": "有线", "strong-wifi": "Wi-Fi 信号良好", "unstable-wifi": "Wi-Fi 不稳定", "unknown": "正在检查…"}
            for (const id in words) {
                streamWith({profile: id})
                verify(status().visible, id)
                verify(status().text.indexOf(qsTr(words[id])) >= 0, id + ": " + status().text)
            }
        }
        function test_status_for_each_advice() {
            const texts = []
            streamWith({profile: "wired", advice: 0})
            const plain = status().text
            for (let n = 1; n <= 6; ++n) {
                streamWith({profile: "unstable-wifi", advice: n})
                const t = row.adviceText(n)
                verify(t.length > 0, "advice " + n)
                verify(status().text.indexOf(t) >= 0, "advice " + n + " shown")
                verify(texts.indexOf(t) < 0, "a sentence of its own")
                texts.push(t)
            }
            compare(row.adviceText(0), "")
            compare(row.adviceText(42), "", "an unknown advice value shows nothing")
            verify(plain.indexOf("·") < 0, "no advice: only the detected connection")
        }
        function test_changes_only_in_automatic_and_only_when_changed() {
            streamWith({mode: "apply", profile: "unstable-wifi", reorderMs: 0, audioTargetMs: 0})
            compare(row.changes.length, 0, "nothing changed: nothing said")
            streamWith({mode: "apply", profile: "unstable-wifi", reorderMs: 8, audioTargetMs: 60})
            compare(row.changes.length, 2)
            verify(status().text.indexOf(qsTr("正在稍微多等一下迟到的画面数据")) >= 0)
            verify(status().text.indexOf(qsTr("正在多预留一点声音")) >= 0)
            streamWith({mode: "observe", profile: "unstable-wifi", reorderMs: 8, audioTargetMs: 60})
            compare(row.changes.length, 0, "advise never reports a change")
        }
        function test_no_status_when_not_fresh() {
            streamWith({active: false, profile: "wired", advice: 1})
            verify(!status().visible)
            compare(row.status, "")
            streamWith({profile: "wired", advice: 1})
            verify(status().visible)
            verify(findChild(row, "pushfps-smartlink-timer").running, "asked once a second while a PS5 stream runs")
            veyra.sourceKind = ""
            veyra.hasSource = false
            verify(!status().visible, "no status without a PS5 stream")
            verify(!findChild(row, "pushfps-smartlink-timer").running, "not asked without a PS5 stream")
        }
        function test_hidden_for_other_sources_and_builds() {
            veyra.sourceKind = "capture"
            veyra.hasSource = true
            verify(!row.visible, "a capture card is not PS5 Remote Play")
            veyra.sourceKind = "moonlight"
            verify(!row.visible)
            veyra.sourceKind = ""
            veyra.hasSource = false
            veyra.preferences = ({pushConsole: "xbox"})
            verify(!row.visible, "nothing playing with an Xbox profile")
            veyra.preferences = ({pushConsole: "ps5pro"})
            verify(row.visible)
            veyra.sourceKind = "ps5"
            veyra.hasSource = true
            verify(row.visible)
            veyra.smartLinkState = ({available: false, active: false})
            row.checkAvailable()
            verify(!row.visible, "not a Slipstream build with PS5 Remote Play")
        }
    }
}
