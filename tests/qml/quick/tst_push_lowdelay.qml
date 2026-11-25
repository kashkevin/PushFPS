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
        // Review 2 MUST 4 and MUST 2: the PushWarp generator at 3X runs the mode; PushFlow only with PushFPS's frame chosen.
        function test_runs_here_with_pushwarp_and_pushflow_only_when_chosen() {
            const enabled = veyra.fgEnabled
            veyra.fgEnabled = true
            veyra.fgBackendName = "pushwarp"
            verify(row.visible && row.runsHere, "the PushWarp generator at 3X")
            veyra.fgBackendName = "pushflow"
            verify(row.runsHere && row.pushflowNeedsChoice, "PushFlow without PushFPS chosen does not run (as in Package 16)")
            sw().toggled(true)
            verify(status().text.indexOf("PushFPS") >= 0, "says the choice it needs")
            veyra.preferences = Object.assign({}, veyra.preferences, {pushLowDelaySource: "pushfps"})
            verify(!row.pushflowNeedsChoice)
            veyra.fgEnabled = enabled
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
            engineState = ({available: true, state: "active", predictedPerSecond: 29, pairsPerSecond: 30})
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
            engineState = ({available: true, state: "not-here", reason: "present-sink-fg"})
            row.refresh()
            const hud = status().text
            engineState = ({available: true, state: "not-here", reason: "format"})
            row.refresh()
            const format = status().text
            verify(hud.length > 0 && format.length > 0 && hud !== format, "each static reason has its own words")
        }
        // Review 2, SHOULD 6, and Package 16: a pair whose multiplier in use is not 3X (Smoothness Mode, adaptive, output
        // cap) pauses the mode and says it resumes by itself. Smoothness Mode and HUD Lock are no longer reasons.
        function test_not_3x_in_use_pauses_and_says_it_resumes() {
            sw().toggled(true)
            engineState = ({available: true, state: "not-here", reason: "not-3x"})
            row.refresh()
            const not3x = status().text
            const needs = qsTr("需要 NVIDIA VFG、PushFlow 或 PushWarp 补帧，倍数设为 3×")   // stream "ownfg": PushFlow and PushWarp too
            verify(not3x.length > 0 && not3x !== needs, "its own words")
            verify(not3x.indexOf("流畅模式") >= 0 && not3x.indexOf("自动继续") >= 0, "names Smoothness Mode and says it resumes by itself")
            verify(row.notHereText("hud-lock-or-exclusion") === needs && row.notHereText("smoothness-mode") === needs, "the reasons Package 16 removed have no words of their own left")
        }
        // Package 16, item 6 (forensics F5): the line is made from the live counters of the last second only.
        function test_running_only_when_predicted_pairs_ran() {
            sw().toggled(true)
            engineState = ({available: true, state: "active", predictedPerSecond: 27, pairsPerSecond: 30})
            row.refresh()
            const running = status().text
            verify(running.indexOf("27") >= 0 && running.indexOf("90") >= 0, "says how many predicted frames a second and the share of pairs")
            engineState = ({available: true, state: "active", predictedPerSecond: 27, shownPerSecond: 25, pairsPerSecond: 30})
            row.refresh()
            verify(status().text.indexOf("25") >= 0 && status().text.indexOf("90") >= 0, "counts the predicted frames shown (review 1 SHOULD 11)")
            engineState = ({available: true, state: "active", predictedPerSecond: 0, pairsPerSecond: 30, calmHave: 0, calmNeed: 0})
            row.refresh()
            verify(status().text !== running && status().text.indexOf("0/0") < 0, "'active' without predicted pairs in the last second: no '0/0'")
            engineState = ({available: true, state: "calm", predictedPerSecond: 0, pairsPerSecond: 30, calmHave: 5, calmNeed: 8})
            row.refresh()
            verify(status().text.indexOf("5/8") >= 0, "waiting shows the entry progress")
            engineState = ({available: true, state: "calm", predictedPerSecond: 0, pairsPerSecond: 30, calmHave: 2, calmNeed: 8, lastStop: "skipped", lastStopAgeS: 4})
            row.refresh()
            verify(status().text.indexOf("4") >= 0 && status().text.indexOf(row.stopText("skipped")) >= 0, "the last stop and how long ago")
            engineState = ({available: true, state: "calm", predictedPerSecond: 0, pairsPerSecond: 30, zeroSeconds: 20, lastStop: "dear", lastStopAgeS: 25})
            row.refresh()
            const notRunning = status().text
            verify(notRunning.indexOf(row.stopText("dear")) >= 0 && notRunning !== running, "20 s without predicted pairs: not running, with the reason")
            for (const r of ["skipped", "held", "refused", "unusable", "late", "dear", "ineligible", "off"])
                verify(row.stopText(r).length > 0, "every stop reason has words: " + r)
            verify(!/毫秒|ms\b/.test(running + notRunning), "never a delay figure")
        }
        function test_switch_on_never_looks_fine_without_running() {
            sw().toggled(true)
            engineState = ({})
            row.refresh()
            verify(status().visible && status().text.length > 0, "on with no engine state: it says it waits for the picture")
            engineState = ({available: true, state: "starting"})
            row.refresh()
            const starting = status().text
            row.startingSeconds = 5
            verify(status().text !== starting && status().text.length > 0, "'starting' for 5 s says it is not running")
            row.startingSeconds = 0
        }
        // Review 2, SHOULD 9: VFG refused the request: unavailable for this run, and the row says how to try once more.
        function test_checking_while_the_self_check_runs() {
            sw().toggled(true)
            engineState = ({available: true, state: "starting", checking: true})
            row.refresh()
            const checking = status().text
            verify(checking.indexOf("3") >= 0, "the check names its budget")
            engineState = ({available: true, state: "starting", checking: false})
            row.refresh()
            verify(status().text !== checking, "after the check the row reads the engine's state again")
        }
        function test_stopped_for_the_playback_says_why() {
            sw().toggled(true)
            engineState = ({available: true, state: "calm", stoppedFor: "dear"})
            row.refresh()
            const stopped = status().text
            verify(stopped.indexOf("太慢") >= 0, "the reason in words")
            engineState = ({available: true, state: "calm", stoppedFor: "none", calmHave: 3, calmNeed: 8})
            row.refresh()
            verify(status().text !== stopped, "running again reads differently")
            engineState = ({available: true, state: "not-here", reason: "flow-too-slow"})
            row.refresh()
            verify(status().text.indexOf("太慢") >= 0, "the entry guard says the motion estimation is too slow")
        }
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
            verify(help.indexOf("三分之一") < 0, "no projected saving (review 1 SHOULD 11)")
            verify(help.indexOf("太慢") >= 0 && help.indexOf("中断") >= 0, "names the dear and the interruption exits")
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
