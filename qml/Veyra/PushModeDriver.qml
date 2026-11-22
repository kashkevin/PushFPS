// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// Keeps the chosen performance mode (PushModeRow.qml, PushModes.js) right while something plays. One
// instance, in Main.qml; it shows nothing and emits notice(text) for the toast. It does nothing at all
// while no mode is chosen: no timer, no bridge call.
//
//   - When a source starts, the mode's settings are put in place once for that source.
//   - The mode is made for a source rate (30 or 60) and a display. When the measured source rate or the
//     display turns into another row of the table and stays there for three looks in a row (three
//     seconds), the mode is applied again for it. A measured rate the table has no row for changes
//     nothing: the last known one stays.
//   - A changed "game frame rate" preference applies at once while nothing is measured.
//   - While "Optimise for my PC", the frame quality test or the first-launch calibration runs it keeps away.
//   - At start-up it puts back what a calibration (PushCalibrationRun.qml) changed when PushFPS stopped
//     while one ran (preference pushCalibrationRestore).
//   - Automatic (preference pushModeAuto, only when PushFPS runs with VEYRA_EXP_AUTO_MODE=1): once a
//     second the bridge feeds engine::PushAutomaticMode with numbers the engine already has
//     (pushAutoModeStep) and its decision is applied. By itself it only moves towards lower latency; it
//     moves back up after its sustained healthy period and never above the chosen mode. Every change is
//     logged with the controller's machine-readable reason and shown as a short notice.
//   - Picture profiles (PushProfileRow.qml, PushProfiles.js) are put in place by the row when one is picked.
//     A profile whose preference pushProfile was set elsewhere while something plays (the test kit's
//     VEYRA_TEST_RP_PIPELINE "profile=" key) is put in place here, once. Nothing is applied at start-up
//     or while nothing plays, and nothing at all for a user who never picks a profile.
// Unverified on Windows.
import QtQuick
import "PushModes.js" as Modes
import "PushProfiles.js" as Profiles

Item {
    id: driver
    objectName: "pushfps-mode-driver"
    visible: false
    signal notice(string text)

    readonly property string mode: {
        const id = String(veyra.preferences.pushMode || "")
        return Modes.isMode(id) ? id : ""
    }
    readonly property int chosenFps: {
        const n = Number(veyra.preferences.pushGameFps || 30)
        return n === 40 || n === 60 ? n : 30
    }
    // Tests turn the timer off and call look() themselves, and replace the two Automatic hooks; the
    // program uses the timer and the bridge.
    property bool timed: true
    property bool autoAvailable: typeof veyra.pushAutoModeAvailable === "function" && veyra.pushAutoModeAvailable() === true
    property var autoStep: null
    readonly property bool autoOn: driver.autoAvailable && veyra.preferences.pushModeAuto === true

    property string applied: ""       // the mode whose settings are in place (Automatic may be below `mode`)
    property string planKey: ""       // what they were made for; "" = not looked at this source yet
    property string pendingKey: ""
    property int pendingLooks: 0
    property real measured: 0         // the last measured source rate with a row in the table, this source
    readonly property int looksNeeded: 3

    function label(id) {
        switch (id) {
        case "ultra-low-latency": return qsTr("超低延迟")
        case "competitive": return qsTr("竞技")
        case "balanced": return qsTr("均衡")
        case "maximum-smoothness": return qsTr("最大流畅")
        }
        return ""
    }
    // The controller's reasons (PushPerformanceMode.h), in a few words. The log keeps the reason itself.
    function reasonText(reason) {
        switch (reason) {
        case "step-down-gpu-busy": return qsTr("显卡跟不上")
        case "step-down-queue-depth": return qsTr("画面在排队")
        case "step-down-network": return qsTr("网络不稳定")
        case "step-down-no-benefit": return qsTr("更高的模式不会多出画面")
        case "step-up-sustained-healthy": return qsTr("已稳定运行一段时间")
        }
        return ""
    }
    function fps() { return driver.measured > 0 ? driver.measured : driver.chosenFps }
    function key(id, rate) {
        const p = Modes.plan(id, rate, Number(veyra.displayRefreshHz || 0))
        return id + "|" + Modes.cadence(rate) + "|" + (p.valid ? p.multiplier : 0)
    }
    function forget() { driver.planKey = ""; driver.pendingKey = ""; driver.pendingLooks = 0 }
    // Puts `id` in place for the current source rate. why: "" (first look at a source, a preference;
    // logged as "apply"), "source" (the source rate or the display moved to another row of the table) or
    // the controller's reason.
    function apply(id, why) {
        const rate = driver.fps()
        const result = Modes.engage(veyra, id, rate)
        if (!result) return false
        const before = driver.applied
        driver.applied = id
        driver.planKey = driver.key(id, rate)
        driver.pendingKey = ""
        driver.pendingLooks = 0
        veyra.logUi("push-mode", "applied mode=" + id + " from=" + (before.length ? before : "none") + " ceiling=" + driver.mode
                    + " reason=" + (why.length ? why : "apply") + " sourceFps=" + Number(rate).toFixed(2)
                    + " multiplier=" + result.multiplier + " minAddedDelayMs=" + Number(result.delayMs).toFixed(2)
                    + " notes=" + (result.notes.length ? result.notes.join(",") : "none"))
        const what = result.multiplier > 1 ? qsTr("%1× 补帧，至少晚 %2 毫秒").arg(result.multiplier).arg(Number(result.delayMs).toFixed(1)) : qsTr("不补帧")
        if (why === "source") driver.notice(qsTr("%1：已按 %2 FPS 游戏重新设置，%3").arg(driver.label(id)).arg(Modes.cadence(rate)).arg(what))
        else if (why.length > 0) {
            const text = driver.reasonText(why)
            driver.notice(qsTr("自动模式：改为「%1」，%2").arg(driver.label(id)).arg(what) + (text.length > 0 ? " (" + text + ")" : ""))
        }
        return true
    }
    // "Optimise for my PC" and the frame quality test set and measure frame generation themselves:
    // nothing is changed under them (the PushFlow budget watch keeps away for the same reason).
    function busy() {
        const optimizing = String(veyra.preferences.pushOptimizeRestore || "")
        if (optimizing.length > 0 && optimizing !== "{}") return true
        if (Modes.calibrationRunning(veyra)) return true
        return typeof veyra.pushQualityStatus === "function" && (veyra.pushQualityStatus() || ({})).running === true
    }
    function look() {
        if (driver.mode === "") return
        if (driver.busy()) { driver.pendingKey = ""; driver.pendingLooks = 0; return }
        const live = veyra.pushLiveStats() || ({})
        const now = Modes.measuredFps(live)
        if (now > 0) driver.measured = now
        let want = driver.mode
        let why = ""
        if (driver.autoOn) {
            const decision = driver.autoStep ? driver.autoStep(driver.mode) : veyra.pushAutoModeStep(driver.mode)
            if (decision && decision.available === true && Modes.isMode(String(decision.mode || ""))) {
                want = String(decision.mode)
                if (decision.changed === true && decision.started !== true) why = String(decision.reason || "")
            }
        }
        const k = driver.key(want, driver.fps())
        if (driver.planKey === "") { driver.apply(want, ""); return }
        if (want !== driver.applied) {
            // The controller has done its own waiting; a ceiling lowered or Automatic switched off applies too.
            driver.apply(want, why)
            return
        }
        if (k === driver.planKey) { driver.pendingKey = ""; driver.pendingLooks = 0; return }
        if (k !== driver.pendingKey) { driver.pendingKey = k; driver.pendingLooks = 1; return }
        if (++driver.pendingLooks >= driver.looksNeeded) driver.apply(want, "source")
    }

    Timer {
        objectName: "pushfps-mode-driver-timer"
        interval: 1000; repeat: true
        running: driver.timed && driver.mode !== "" && veyra.hasSource === true
        triggeredOnStart: true
        onTriggered: driver.look()
        // Another source: measured again from nothing, and the mode is put in place for it once.
        onRunningChanged: if (!running) { driver.measured = 0; driver.forget() }
    }
    // The picture profile as the preference has it ("" = never set), and the one in place before its last change.
    readonly property string profileRaw: String(veyra.preferences.pushProfile || "")
    property string profileBefore: "none"
    property bool adoptPending: false
    // The screen in pixels; tests set their own.
    property var screenSize: ({ width: Math.round(Screen.width * Screen.devicePixelRatio), height: Math.round(Screen.height * Screen.devicePixelRatio) })
    property string adoptFrom: "none"
    onProfileRawChanged: {
        const id = driver.profileRaw
        const was = driver.profileBefore
        const inPlace = Profiles.takeApplied()
        driver.profileBefore = Profiles.isProfile(id) ? id : "none"
        // Picked in the row (already in place), never set, or nothing plays: nothing to do here.
        if (id === "" || id === inPlace || veyra.hasSource !== true) return
        if (id !== "none" && !Profiles.isProfile(id)) return
        // Putting a profile in place writes preferences, so not from inside this preference's own change.
        if (!driver.adoptPending) { driver.adoptPending = true; driver.adoptFrom = was }
        Qt.callLater(driver.adoptProfile)
    }
    function adoptProfile() {
        if (!driver.adoptPending) return
        driver.adoptPending = false
        const id = driver.profileRaw
        if (id !== "none" && !Profiles.isProfile(id)) return
        const result = Profiles.adopt(veyra, id, Profiles.environment(veyra, veyra.pushLiveStats() || ({}), driver.screenSize), driver.adoptFrom)
        Profiles.takeApplied()   // the preference already names it: no change follows that would take it
        veyra.logUi("push-profile", "adopted profile=" + id + " from=" + driver.adoptFrom + " applied=" + (result !== null) + " (preference set outside the quick setup)")
    }
    // The row applied the newly chosen mode itself: start from it.
    onModeChanged: {
        // A mode turned off after "None" was chosen under it: the frame generation / presentation a profile
        // still owed come back now, after the mode's own restore (PushProfiles.settlePending).
        if (driver.mode === "") Profiles.settlePending(veyra)
        driver.applied = driver.mode
        driver.forget()
        if (driver.mode === "" && driver.autoAvailable && !driver.autoStep && typeof veyra.pushAutoModeStep === "function") veyra.pushAutoModeStep("")
    }
    Component.onCompleted: {
        driver.profileBefore = Profiles.current(veyra)
        // A mode chosen in an earlier run: its experiment switches back in the environment for the first connection.
        // Only for a mode chosen with this version (pref pushModeSwitches): one chosen before changes nothing.
        if (driver.mode !== "" && veyra.preferences.pushModeSwitches === true) Modes.syncSwitches(veyra)
        if (!Modes.calibrationRunning(veyra)) return
        const restored = Modes.calibrationRestore(veyra)
        veyra.logUi("push-calibration", "start-up: a calibration did not finish; settings restored=" + restored)
    }
    // The game frame rate chosen in the quick setup counts while nothing is measured.
    onChosenFpsChanged: if (driver.mode !== "" && driver.measured === 0 && !driver.busy()) driver.apply(driver.applied.length > 0 ? driver.applied : driver.mode, "")
}
