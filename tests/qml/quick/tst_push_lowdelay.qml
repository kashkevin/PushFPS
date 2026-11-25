// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Package 15 "Lower delay" row (PushLowDelayRow.qml) against the fixture bridge in QuickSmokeTests.cpp. In the CI lists
// (build-windows.yml, checks.yml) since TASK 3: before that it was never run. The fixture does
// not know the preference, so init() adds it to its push* keys (as tst_push_smoothness.qml does). The engine's state
// comes from row.stateSource (the bridge's pushLowDelayState() in the program). Interface logic only: the schedule
// itself is tested in tests/remoteplay/LowDelayTests.inc and LowDelaySimTests.inc.
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 700; height: 600
    PushLowDelayRow { id: row; x: 10; y: 10; width: 560 }

    TestCase {
        name: "PushLowDelay"
        when: windowShown
        property var engineState: ({})
        property var savedCaps: null
        function init() {
            if (veyra.pushKeys.indexOf("pushLowDelay") < 0) veyra.pushKeys = veyra.pushKeys.concat(["pushLowDelay"])
            veyra.preferences = ({})
            savedCaps = veyra.effectCapabilities
            veyra.effectCapabilities = Object.assign({}, savedCaps, {vfg: {available: true}})
            veyra.sourceKind = "capture"
            veyra.fgBackendName = "vfg"
            veyra.fgMultiplier = 3
            veyra.hasSource = true
            engineState = ({})
            row.stateSource = () => engineState
            row.engine = ({})
        }
        function cleanup() {
            veyra.effectCapabilities = savedCaps
            veyra.sourceKind = ""
            veyra.fgBackendName = "dlss"
            veyra.fgMultiplier = 1
            veyra.preferences = ({})
        }
        function sw() { return findChild(row, "pushfps-lowdelay") }
        function status() { return findChild(row, "pushfps-lowdelay-status") }

        function test_default_is_off_and_writes_nothing() {
            verify(row.visible, "shown for a capture card with VFG and 3X")
            verify(!sw().checked, "off by default")
            compare(veyra.preferences.pushLowDelay, undefined, "nothing is written until it is switched")
            verify(findChild(row, "pushfps-lowdelay-experimental").visible, "marked experimental")
            verify(findChild(row, "vrow-help").text.length > 0, "the ? explains the trade")
            verify(findChild(row, "pushfps-lowdelay-row").hint.length > 0, "the hint says what to watch for")
            verify(!status().visible, "no status while off")
        }
        function test_shown_only_for_a_capture_card_with_vfg_and_3x() {
            veyra.sourceKind = "ps5"
            verify(!row.visible, "not for a PS5 stream")
            veyra.sourceKind = "file"
            verify(!row.visible, "not for a file")
            veyra.sourceKind = "capture"
            verify(row.visible)
            veyra.effectCapabilities = Object.assign({}, savedCaps, {vfg: {available: false}})
            verify(!row.visible, "not without VFG")
            veyra.effectCapabilities = Object.assign({}, savedCaps, {vfg: {available: true}})
            const choices = veyra.fgMultiplierChoices
            veyra.fgMultiplierChoices = [{id: "1"}, {id: "2"}]
            verify(!row.visible, "not without 3X")
            veyra.fgMultiplierChoices = choices
            verify(row.visible)
        }
        function test_switch_writes_the_preference() {
            sw().toggled(true)
            compare(veyra.preferences.pushLowDelay, true)
            verify(row.on)
            sw().toggled(false)
            compare(veyra.preferences.pushLowDelay, false)
            verify(!row.on)
        }
        function test_status_follows_the_engine() {
            sw().toggled(true)
            engineState = ({available: true, state: "calm"})
            row.refresh()
            verify(status().visible)
            const calm = status().text
            engineState = ({available: true, state: "active"})
            row.refresh()
            const active = status().text
            verify(active !== calm && active.length > 0, "running and waiting read differently")
            engineState = ({available: true, state: "unavailable"})
            row.refresh()
            const unavailable = status().text
            verify(unavailable !== active && unavailable !== calm && unavailable.length > 0, "built for it but no pass: it cannot run here")
        }
        function test_one_click_applies_in_the_running_session() {
            // Switching on: the bridge reports "starting" while the graph is rebuilt with the pass, then the engine's
            // own state. Nothing anywhere says "from the next start": the row never waits for a restart.
            sw().toggled(true)
            engineState = ({available: true, state: "starting"})
            row.refresh()
            verify(status().visible, "a status while the graph is rebuilt")
            const starting = status().text
            verify(starting.length > 0)
            engineState = ({available: true, state: "calm"})
            row.refresh()
            verify(status().text !== starting, "the rebuilt graph runs: the starting line goes")
            verify(!/(下次启动|next start)/i.test(status().text), "no restart is asked for")
            // Switching off: the status goes at once, whatever the engine last reported.
            sw().toggled(false)
            verify(!status().visible, "off: no status line")
            compare(veyra.preferences.pushLowDelay, false)
            // The ? text says it applies at once both ways and does not ask for a restart.
            const help = findChild(row, "vrow-help").text
            verify(help.indexOf("下次启动") < 0, "the help no longer says 'from the next start'")
        }
        function test_cannot_run_here_says_why() {
            sw().toggled(true)
            engineState = ({available: true, state: "not-here", reason: "hud-lock-or-exclusion"})
            row.refresh()
            const hud = status().text
            engineState = ({available: true, state: "not-here", reason: "format"})
            row.refresh()
            const format = status().text
            verify(hud.length > 0 && format.length > 0 && hud !== format, "each static reason has its own words")
        }
        // Review 2, SHOULD 4 and 6: Smoothness Mode deciding, and a pair whose multiplier in use is not 3X (adaptive,
        // output cap) while the setting says 3X, each have their own words (not "needs VFG at 3X").
        function test_not_3x_in_use_and_smoothness_mode_say_why() {
            sw().toggled(true)
            engineState = ({available: true, state: "not-here", reason: "not-3x"})
            row.refresh()
            const not3x = status().text
            engineState = ({available: true, state: "not-here", reason: "smoothness-mode"})
            row.refresh()
            const smooth = status().text
            const needs = qsTr("需要 NVIDIA VFG 补帧，倍数设为 3×")
            verify(not3x.length > 0 && smooth.length > 0, "both say something")
            verify(not3x !== needs && smooth !== needs && not3x !== smooth, "each has its own words")
        }
        // Review 2, SHOULD 9: VFG refused the request: unavailable for this run, and the row says how to try once more.
        function test_vfg_refused_says_so() {
            sw().toggled(true)
            engineState = ({available: true, state: "unavailable", reason: "vfg-refused"})
            row.refresh()
            const refused = status().text
            engineState = ({available: true, state: "unavailable", reason: "pass-unavailable"})
            row.refresh()
            verify(refused.length > 0 && refused !== status().text, "VFG's refusal has its own words")
            verify(refused.indexOf("VFG") >= 0, "it names VFG")
        }
        function test_paused_says_why() {
            sw().toggled(true)
            const seen = []
            for (const p of ["quality-test", "benchmark", "hud-detection", "comparison-clip"]) {
                engineState = ({available: true, state: "suspended", paused: p})
                row.refresh()
                verify(status().visible && status().text.length > 0, "paused: " + p)
                verify(seen.indexOf(status().text) < 0, "each pause reason reads differently")
                seen.push(status().text)
            }
        }
        function test_kit_switch_shows_on_with_the_row_off() {
            engineState = ({available: true, state: "active", fromSwitch: true})
            row.refresh()
            verify(row.kit, "the kit's switch is seen")
            verify(sw().checked, "the switch shows on while the test switch runs the mode")
            verify(status().visible && status().text.length > 0, "the status says the test switch runs it")
            compare(veyra.preferences.pushLowDelay, undefined, "nothing is written")
        }
        function test_help_states_no_number_as_fact() {
            const help = findChild(row, "vrow-help").text
            verify(help.indexOf("11 毫秒") < 0, "no millisecond figure stated as fact")
            verify(help.indexOf("尚未在任何电脑上实测") >= 0, "says it has not been measured on any PC")
            verify(help.indexOf("预测不好的帧会照常显示") >= 0, "says bad predictions are still shown (review 2, MUST 2)")
            verify(help.indexOf("个别帧的延迟可能反而比关闭时高") >= 0, "says some frames can be later")
            verify(findChild(row, "pushfps-lowdelay-row").hint.indexOf("关掉这一行") >= 0, "the hint says to switch it off if it looks wrong")
        }
        // Review 2, MUST 3: when the engine refuses the change, the bridge puts the preference back (and shows a notice);
        // the row then shows off with no status, whatever state the engine reported last.
        function test_refused_change_goes_back_to_off() {
            sw().toggled(true)
            engineState = ({available: true, state: "starting"})
            row.refresh()
            verify(status().visible, "starting while the rebuild is on its way")
            veyra.preferences = Object.assign({}, veyra.preferences, {pushLowDelay: false})   // what applyPushLowDelay does on refusal
            verify(!sw().checked, "the switch shows off again")
            verify(!status().visible, "no 'starting' left behind")
            // A refused write of the preference itself changes nothing either.
            const keys = veyra.pushKeys
            veyra.pushKeys = keys.filter(k => k !== "pushLowDelay")
            sw().toggled(true)
            verify(!sw().checked && !status().visible, "refused preference: still off")
            veyra.pushKeys = keys
        }
        function test_old_next_start_state_is_not_shown() {
            sw().toggled(true)
            engineState = ({available: true, state: "next-start", passPossible: false})
            row.refresh()
            verify(!status().visible, "the retired next-start state shows nothing (no restart prompt)")
        }
        function test_needs_vfg_at_3x_when_settings_differ() {
            sw().toggled(true)
            engineState = ({available: true, state: "active"})
            row.refresh()
            const active = status().text
            veyra.fgMultiplier = 2
            verify(status().text !== active, "2X: the status asks for 3X")
            veyra.fgMultiplier = 3
            veyra.fgBackendName = "dlss"
            verify(status().text !== active, "another method: the status asks for VFG")
            verify(row.visible, "the row stays, so it can be switched off")
        }
    }
}
