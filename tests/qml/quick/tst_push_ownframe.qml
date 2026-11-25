// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "ownfg", step 2: the Lower delay row's frame source (PushOwnFrameRow.qml) against the fixture bridge in
// QuickSmokeTests.cpp. The fixture does not know the preference, so init() adds it to its push* keys (as
// tst_push_lowdelay.qml does). The engine's facts are set directly. Interface logic only: the decision is tested in
// tests/remoteplay/OwnFrameTests.inc, the frame itself on WARP (PushGpuTests.cpp warp-*).
import QtQuick
import QtTest
import Veyra
Item {
    id: root
    width: 700; height: 400
    PushOwnFrameRow { id: own; x: 10; y: 10; width: 560 }

    TestCase {
        name: "PushOwnFrame"
        when: windowShown
        function init() {
            if (veyra.pushKeys.indexOf("pushLowDelaySource") < 0) veyra.pushKeys = veyra.pushKeys.concat(["pushLowDelaySource"])
            if (veyra.pushKeys.indexOf("pushLowDelayCausal") < 0) veyra.pushKeys = veyra.pushKeys.concat(["pushLowDelayCausal"])
            veyra.preferences = ({})
            veyra.fgBackendName = "vfg"
            own.rowOn = true
            own.engine = ({})
        }
        function cleanup() {
            veyra.fgBackendName = "dlss"
            veyra.preferences = ({})
            own.rowOn = false
            own.engine = ({})
        }
        function seg() { return findChild(own, "pushfps-lowdelay-source") }
        function status() { return findChild(own, "pushfps-lowdelay-source-status") }

        function test_hidden_while_the_lower_delay_row_is_off() {
            own.rowOn = false
            verify(!own.visible, "nothing while the Lower delay row is off")
            own.rowOn = true
            verify(own.visible, "shown with the row on and VFG")
            veyra.fgBackendName = "dlss"
            verify(!own.visible, "not for a generator neither source serves")
        }
        function test_default_is_nvidia_with_vfg_and_writes_nothing() {
            compare(seg().current, "nvidia", "NVIDIA's frame stays the default where VFG generates")
            compare(veyra.preferences.pushLowDelaySource, undefined, "nothing is written until a choice is made")
            verify(seg().enabled, "a choice with VFG")
            verify(!status().visible, "no status before the engine reports")
        }
        function test_choice_writes_the_preference() {
            seg().picked("pushfps")
            compare(veyra.preferences.pushLowDelaySource, "pushfps")
            compare(seg().current, "pushfps")
            seg().picked("nvidia")
            compare(veyra.preferences.pushLowDelaySource, "nvidia")
            compare(seg().current, "nvidia")
        }
        function test_pushflow_has_only_pushfps() {
            veyra.fgBackendName = "pushflow"
            verify(own.visible, "shown with PushFlow")
            compare(seg().current, "pushfps", "PushFPS is the only source with PushFlow")
            verify(!seg().enabled, "no choice to make")
            verify(findChild(own, "pushfps-lowdelay-source-row").hint.length > 0, "the hint says why")
        }
        function test_status_names_the_source_in_use() {
            own.engine = ({state: "active", ownWarp: "ready", ownInUse: "chosen"})
            veyra.preferences = ({pushLowDelaySource: "pushfps"})
            verify(status().visible && status().text.indexOf("PushFPS") >= 0, "the chosen source in use")
            veyra.preferences = ({})
            own.engine = ({state: "active", ownWarp: "none", ownInUse: "none"})
            verify(!status().visible, "nothing to say while NVIDIA's frame is used as chosen")
        }
        // Review 1, MUST 3: NVIDIA refused: the mode is unavailable; this row only OFFERS the other source.
        function test_refusal_offers_pushfps_and_never_takes_over() {
            own.engine = ({state: "unavailable", reason: "vfg-refused", ownWarp: "none", ownInUse: "none"})
            verify(status().visible && status().text.indexOf("可以改选 PushFPS") >= 0, "offers the PushFPS source")
            compare(seg().current, "nvidia", "the choice stays NVIDIA")
            compare(veyra.preferences.pushLowDelaySource, undefined, "nothing is switched by itself")
        }
        // Chosen PushFPS that cannot run here: the mode does not run, never NVIDIA's frame instead.
        function test_chosen_pushfps_that_cannot_run_says_so() {
            veyra.preferences = ({pushLowDelaySource: "pushfps"})
            own.engine = ({state: "not-here", ownWarp: "check-failed"})
            verify(status().text.indexOf("不可用") >= 0 && status().text.indexOf("改选 NVIDIA") >= 0, "unavailable with PushFPS; NVIDIA offered")
            own.engine = ({state: "not-here", ownWarp: "too-slow"})
            verify(status().text.indexOf("太慢") >= 0, "too slow on this card (review 1, SHOULD 4)")
            own.engine = ({state: "starting", ownWarp: "none"})
            verify(status().text.indexOf("正在准备") >= 0, "the rebuild that creates it (review 1, SHOULD 9)")
            veyra.preferences = ({})
            own.engine = ({state: "active", ownWarp: "check-failed"})
            verify(!status().visible, "with NVIDIA chosen PushFPS's state is not shown (it is not even created)")
        }
        function test_label_is_honest() {
            const help = findChild(findChild(own, "pushfps-lowdelay-source-row"), "vrow-help").text
            verify(help.indexOf("没有在游戏画面上测试过") >= 0 && help.indexOf("没有在任何真实显卡上运行过") >= 0, "says what was not measured")
            verify(help.indexOf("细小物体") >= 0, "says where it is worse")
            verify(help.indexOf("任何显卡都能运行") < 0, "no claim that it runs on any graphics card (review 1, SHOULD 7)")
            verify(help.indexOf("不会自动改用 PushFPS") >= 0, "says a refusal does not switch the source")
            verify(help.indexOf("毫秒") < 0 && help.indexOf("ms") < 0, "no delay figure")
        }
        // Step 4: the causal switch: off by default, its text states the picture cost and that nothing was measured on a card.
        function test_causal_switch_is_off_and_honest() {
            const sw = findChild(own, "pushfps-lowdelay-causal")
            verify(sw && !sw.checked, "off by default")
            verify(!findChild(own, "pushfps-lowdelay-causal-status").visible, "no status while off")
            const help = findChild(findChild(own, "pushfps-lowdelay-causal-row"), "vrow-help").text
            verify(help.indexOf("两帧是预测的") >= 0 && help.indexOf("dB") >= 0, "the picture cost in plain words and numbers")
            verify(help.indexOf("没有在任何显卡上测量过") >= 0 && help.indexOf("延迟能减少多少也没有测量") >= 0, "nothing measured on a card, no delay claim")
            verify(help.indexOf("毫秒") < 0 && help.indexOf("ms") < 0, "no delay figure")
            verify(help.indexOf("%1") < 0 && help.indexOf("%2") < 0, "the numbers are filled in")
            sw.toggled(true)
            compare(veyra.preferences.pushLowDelayCausal, true)
        }
        function test_causal_status_steps_back_one_slot_and_says_so() {
            veyra.preferences = ({pushLowDelayCausal: true})
            const st = findChild(own, "pushfps-lowdelay-causal-status")
            own.engine = ({state: "active", ownWarp: "ready", causalLevel: "causal", causalPerSecond: 28})
            verify(st.visible && st.text.indexOf("28") >= 0, "running, with the count of the last second")
            own.engine = ({state: "active", ownWarp: "ready", causalLevel: "onepredicted", causalLastExit: "unusable", causalReturnHave: 12, causalReturnNeed: 300})
            verify(st.text.indexOf("每个真实帧配一个预测帧") >= 0 && st.text.indexOf("12/300") >= 0, "back to ONE predicted frame, with the return progress")
            own.engine = ({state: "active", ownWarp: "ready", causalLevel: "onepredicted", causalStoppedFor: "late"})
            verify(st.text.indexOf("停用到这次播放结束") >= 0, "stopped for the playback after three exits")
            own.engine = ({state: "active", ownWarp: "check-failed"})
            verify(st.text.indexOf("不会运行") >= 0, "PushWarp unusable: the causal schedule does not run")
        }
    }
}
