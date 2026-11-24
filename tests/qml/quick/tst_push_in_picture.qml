// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "present" R1 (qml/Veyra/PushInPicture.js): the delay badge and the statistics overlay hand a picture of themselves
// to the bridge (position from the picture's top left), stay cut out of the video window (videoCover) until the presenter
// has drawn that picture, stop being a cover once it has (the window keeps one rectangle), become a cover again when it
// stops drawing or the rollback switch is on, and take their picture away when hidden. Against the fixture bridge in
// QuickSmokeTests.cpp (videoOverlayCalls, videoOverlayDrawn, videoOverlayEnabled); the GPU blend itself is tested on WARP.
import QtQuick
import QtTest
import Veyra
import "../qml/Veyra/PushInPicture.js" as InPicture
Item {
    id: root
    width: 900; height: 600
    PushLatencyBadge { id: badge; x: 30; y: 40; videoOrigin: Qt.point(10, 10) }
    PushStatsOverlay { id: panel; x: 30; y: 90; videoOrigin: Qt.point(10, 10) }

    TestCase {
        name: "PushInPicture"
        when: windowShown
        function init() {
            veyra.videoOverlayCalls = []
            veyra.videoOverlayDrawn = ({})
            veyra.videoOverlayEnabled = true
            veyra.liveStats = ({running: true, latencyMs: 24, latencyP95Ms: 28, submitFps: 90, shownFpsKnown: true, shownFps: 90})
            veyra.preferences = ({pushLatencyOverlay: true, pushStatsOverlay: true})
            badge.refresh()
            panel.refresh()
        }
        function cleanup() {
            veyra.preferences = ({})
            veyra.liveStats = ({running: false})
            veyra.videoOverlayDrawn = ({})
        }
        function calls(key, withImage) {
            return veyra.videoOverlayCalls.filter(c => c.key === key && c.image === withImage)
        }

        function test_hands_over_its_picture_from_the_picture_origin() {
            verify(badge.visible && panel.visible)
            tryVerify(() => calls("badge", true).length > 0, 5000, "the badge handed over a picture of itself")
            tryVerify(() => calls("stats", true).length > 0, 5000, "the statistics overlay handed over a picture of itself")
            const last = calls("badge", true)[calls("badge", true).length - 1]
            compare(last.x, 20)
            compare(last.y, 30)
            verify(last.dpr > 0)
        }

        function test_a_cover_until_drawn_then_none() {
            // Not drawn yet: still cut out of the video window, as in Package 14.
            verify(badge.videoCover === true)
            verify(panel.videoCover === true)
            veyra.videoOverlayDrawn = ({badge: true, stats: true})
            tryCompare(badge, "videoCover", false, 2000)
            tryCompare(panel, "videoCover", false, 3000)
            verify(badge.visible, "the item itself stays (behind the picture), so the next change is grabbed again")
            // The presenter stopped drawing (paused, a provider owning Present): a cover again.
            veyra.videoOverlayDrawn = ({})
            tryCompare(badge, "videoCover", true, 2000)
            tryCompare(panel, "videoCover", true, 3000)
        }

        function test_rollback_switch_keeps_the_cut_out_and_never_grabs() {
            veyra.videoOverlayEnabled = false
            veyra.videoOverlayDrawn = ({badge: true, stats: true})
            wait(300)
            veyra.videoOverlayCalls = []
            veyra.liveStats = ({running: true, latencyMs: 31, latencyP95Ms: 33, submitFps: 90})
            badge.refresh()
            panel.refresh()
            wait(1200)
            verify(badge.videoCover === true)
            verify(panel.videoCover === true)
            compare(calls("badge", true).length, 0, "no grab with the rollback switch (review S3)")
            compare(calls("stats", true).length, 0)
        }

        function test_a_failed_grab_hands_over_nothing_and_keeps_the_cut_out() {
            // An item whose grab fails (grabToImage returns false): the bridge gets no picture, the item stays a cover.
            const fake = {visible: true, width: 40, height: 20, x: 30, y: 30, grabToImage: function(cb, size) { return false }}
            veyra.videoOverlayCalls = []
            InPicture.publish(veyra, fake, "failing", Qt.point(10, 10), 1.5)
            compare(calls("failing", true).length, 0)
            compare(calls("failing", false).length, 1, "a failed grab takes any older picture of the item away")
            verify(!InPicture.drawn(veyra, "failing"), "never reported as drawn")
        }

        function test_grabs_are_capped_at_four_a_second() {
            veyra.videoOverlayCalls = []
            // 40 changes within 400 ms (an animation moving the item every 10 ms).
            for (let i = 0; i < 40; ++i) { badge.x = 30 + (i % 2); wait(10) }
            badge.x = 30
            wait(700)
            verify(calls("badge", true).length <= 5, "at most about 4 grabs a second, got " + calls("badge", true).length)
            verify(calls("badge", true).length >= 1)
        }

        function test_hidden_takes_the_picture_away_and_is_no_cover() {
            veyra.videoOverlayDrawn = ({badge: true})
            tryCompare(badge, "videoCover", false, 2000)
            veyra.videoOverlayCalls = []
            veyra.preferences = ({pushStatsOverlay: true})
            tryCompare(badge, "visible", false, 2000)
            tryVerify(() => calls("badge", false).length > 0, 2000, "hidden: the picture is taken away")
            verify(badge.videoCover === false)
            verify(badge.inPicture === false)
        }
    }
}
