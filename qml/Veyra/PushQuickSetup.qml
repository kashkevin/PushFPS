// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
import QtQuick
import QtQuick.Layouts
import "EffectSupport.js" as Support
import "PushCaptureCards.js" as Cards
import "PushGamePresets.js" as Presets

// PushFPS quick setup: the choices most console players make, above the detailed
// controls. Every control here only sets existing settings (frame generation on/off,
// multiplier, output cap, NR layers, NVIDIA low latency, display sync), so Professional mode always
// shows exactly what was chosen and nothing here adds engine state.
VGroup {
    id: quick
    // True once Optimise for my PC applied its choice (record pushBenchmark, applied; Undo clears it).
    readonly property bool optimized: {
        try { return JSON.parse(String(veyra.preferences.pushBenchmark || "{}")).applied === true } catch (e) { return false }
    }
    // Smooth frame pacing (row pushfps-smooth): V-Sync + a variable refresh request, with the output
    // capped (at the display when no target is set; a chosen target such as 60 FPS stays). Off returns
    // to Automatic, no VRR request, and drops only the follow-the-display cap. Before 3.1.6 the switch
    // also needed the follow-the-display cap, so choosing a 60 FPS target turned it off and live play
    // skipped frames the recording still had (field report 2026-11-17).
    readonly property bool smoothPacing: veyra.displaySync === 1 && veyra.variableRefresh === true && veyra.outputRateMode !== 0
    function setSmoothPacing(on) {
        veyra.displaySync = on ? 1 : 2
        veyra.variableRefresh = on
        if (on && veyra.outputRateMode === 0) veyra.outputRateMode = 1
        else if (!on && veyra.outputRateMode === 1) veyra.outputRateMode = 0
        veyra.setPreference("pushSmoothPacing", on)
    }
    // A capped target that does not divide the refresh rate (60 on 144 Hz) without a variable refresh
    // display: frames stay on screen for uneven times however well they are made.
    readonly property bool unevenTarget: {
        const hz = Number(veyra.displayRefreshHz || 0)
        if (veyra.outputRateMode !== 2 || hz < 1 || Number(veyra.vrrState || 0) >= 2) return false
        const ratio = hz / Math.max(1, Number(veyra.outputCustomFps))
        return Math.abs(ratio - Math.round(ratio)) > 0.05
    }
    // One-tap lowest latency (row pushfps-lowest-latency): only switches that cut waiting without
    // touching the picture: the one-frame present queue (NVIDIA low latency on NVIDIA, the low
    // queue elsewhere), fullscreen on connect (a fullscreen window skips the desktop compositor's
    // extra refresh), and on a G-SYNC/FreeSync screen smooth frame pacing with variable refresh
    // (no tearing, no skipped frames, almost no wait below the refresh rate). Frame generation,
    // AI enhancement, HUD lock and pacing repair stay as they are. Turning it off restores what
    // was set before (preference pushLowestLatency: the saved values as JSON).
    readonly property bool vrrCapable: Number(veyra.vrrState || 0) >= 2
    readonly property bool lowestLatency: (veyra.nvidiaLowLatencyAvailable === true ? Number(veyra.nvidiaLowLatency || 0) > 0 : veyra.fgLowQueue === true)
        && veyra.preferences.pushAutoFullscreen === true
        && (!quick.vrrCapable || (quick.smoothPacing && veyra.variableRefresh === true))
    function setLowestLatency(on) {
        const nvidia = veyra.nvidiaLowLatencyAvailable === true
        if (on) {
            veyra.setPreference("pushLowestLatency", JSON.stringify({
                nvidia: Number(veyra.nvidiaLowLatency || 0), queue: veyra.fgLowQueue === true,
                fullscreen: veyra.preferences.pushAutoFullscreen === true, display: veyra.displaySync,
                vrr: veyra.variableRefresh === true, rate: veyra.outputRateMode }))
            if (nvidia) veyra.nvidiaLowLatency = 1
            else veyra.fgLowQueue = true
            veyra.setPreference("pushAutoFullscreen", true)
            if (quick.vrrCapable) quick.setSmoothPacing(true)
            veyra.logUi("push-latency", "lowest latency on nvidia=" + nvidia + " vrr=" + quick.vrrCapable)
            return
        }
        let saved = null
        try { saved = JSON.parse(String(veyra.preferences.pushLowestLatency || "")) } catch (e) { saved = null }
        if (!saved || typeof saved !== "object") saved = { nvidia: 0, queue: false, fullscreen: false }
        if (nvidia) veyra.nvidiaLowLatency = Number(saved.nvidia || 0) > 0 ? 1 : 0
        else veyra.fgLowQueue = saved.queue === true
        veyra.setPreference("pushAutoFullscreen", saved.fullscreen === true)
        if (typeof saved.display === "number" && typeof saved.rate === "number") {
            veyra.displaySync = saved.display
            veyra.variableRefresh = saved.vrr === true
            veyra.outputRateMode = saved.rate
        }
        veyra.logUi("push-latency", "lowest latency off (previous settings restored)")
    }
    readonly property string lowestLatencyHint: {
        const ms = Number(quick.liveStats.latencyMs || 0)
        const now = ms > 0 ? qsTr("现在电脑处理延迟 %1 毫秒。").arg(ms.toFixed(0)) : ""
        const capture = Number(quick.liveStats.captureFps || 0)
        const tip = capture > 0 && capture < 100 && (quick.consoleId === "ps5" || quick.consoleId === "ps5pro" || quick.consoleId === "xbox")
                  ? qsTr("主机输出 120 Hz 时每帧还能早约 8 毫秒到达。") : ""
        if (!quick.lowestLatency) return qsTr("一键减少等待：画质、补帧和 AI 增强都不变")
        return now + (quick.vrrCapable ? qsTr("单帧队列、全屏、可变刷新已开启。") : qsTr("单帧队列和全屏已开启；屏幕不支持可变刷新，帧同步保持不变以免画面撕裂。")) + tip
    }
    objectName: "pushfps-quick-setup"
    // Home shows the short form: no low-latency row and no intro line.
    property bool compact: false
    // Home only: the first-run setup tip, until a capture card has been used once.
    property bool showSetupTip: false
    // Set where the picture is already visible next to the quick setup (Professional mode).
    property bool compactPro: false
    // Console games send their frames inside a 60 Hz signal: 30 FPS for most games
    // (GTA VI), 40 in some 120 Hz modes, 60 in performance modes. The player picks it
    // (设置 key pushGameFps); the multiplier is chosen from it, and the output cap holds
    // the result at the target.
    readonly property int baseFps: {
        const n = Number(veyra.preferences.pushGameFps || 30)
        return n === 40 || n === 60 ? n : 30
    }
    function setGameFps(id) {
        const n = parseInt(id)
        if (!veyra.setPreference("pushGameFps", n)) return
        // Keep the chosen target: the multiplier is worked out again for the new rate.
        // A target the game already reaches (60 FPS for a 60 FPS game) has nothing to
        // generate, so frame generation goes off instead of doubling past it.
        const t = quick.currentTarget
        if (t === "60" || t === "120" || t === "monitor") {
            if (quick.targetFps(t) <= n + 0.5) veyra.fgEnabled = false
            else quick.applyTarget(t, n)
        }
    }

    readonly property bool fgPossible: Support.available(veyra, "frame-generation")
    readonly property bool nrPossible: Support.available(veyra, "nr")

    // Which quick choice matches the current settings; "" when Professional mode
    // set something else (a custom cap, or no cap at all).
    readonly property string currentTarget: {
        if (!veyra.fgEnabled) return "off"
        if (veyra.outputRateMode === 1) return "monitor"
        if (veyra.outputRateMode === 2 && Math.abs(veyra.outputCustomFps - 60) < 0.5) return "60"
        if (veyra.outputRateMode === 2 && Math.abs(veyra.outputCustomFps - 120) < 0.5) return "120"
        return ""
    }

    function targetFps(id) {
        if (id === "60") return 60
        if (id === "120") return 120
        if (id === "monitor") return veyra.displayRefreshHz > 0 ? veyra.displayRefreshHz : 60
        return 0
    }

    // The largest multiplier the current backend offers whose output stays at or
    // under the target; the smallest offered one when even that overshoots.
    // Adaptive multiplier (EnhancementSettings::fgAdaptive): the multiplier becomes a
    // ceiling and the engine picks, per frame pair, the one closest to the target from
    // the game's measured frame rate. Only worth it when the backend offers more than 2X;
    // XeSS paces its own presentation and keeps its fixed multiplier.
    readonly property var offeredMultipliers: (veyra.fgMultiplierChoices || []).map(o => parseInt(o.id)).filter(m => m >= 2)
    readonly property bool adaptivePossible: quick.fgPossible && !veyra.presentationOwned && quick.offeredMultipliers.some(m => m > 2)
    readonly property bool adaptive: quick.adaptivePossible && veyra.fgAdaptive === true
    function setAdaptive(on) {
        veyra.fgAdaptive = on
        // The game's frame rate is measured by automatic duplicate removal.
        if (on && Number(veyra.contentRate) === 0) veyra.contentRate = 1
        const t = quick.currentTarget
        if (t === "60" || t === "120" || t === "monitor") quick.applyTarget(t)
    }
    // Live: the multiplier the last frame pair used, the measured game frame rate and, for
    // Frame Pacing Repair, the share of real frames it re-timed over the last second.
    property var liveStats: ({})
    property var lastPacing: null
    // Going live: polled while a stream runs (or every few seconds) for the tile.
    property var liveStream: ({})
    Timer {
        interval: quick.liveStream.active ? 1000 : 3000; repeat: true
        running: quick.visible && typeof veyra.pushStreamStatus === "function"
        triggeredOnStart: true
        onTriggered: quick.liveStream = veyra.pushStreamStatus() || ({})
    }
    property real pacingShare: -1
    Timer {
        interval: 1000; repeat: true
        running: (quick.adaptive || quick.pacingOn || veyra.hudLock === true || veyra.fgBackendName === "pushflow") && veyra.hasSource && veyra.fgEnabled && quick.visible
        triggeredOnStart: true
        onTriggered: {
            const st = veyra.pushLiveStats()
            const p = quick.lastPacing
            if (p && st.pacingFrames > p.pacingFrames) quick.pacingShare = (st.pacingRepaired - p.pacingRepaired) / (st.pacingFrames - p.pacingFrames)
            quick.lastPacing = { pacingFrames: Number(st.pacingFrames || 0), pacingRepaired: Number(st.pacingRepaired || 0) }
            quick.liveStats = st
        }
        onRunningChanged: if (!running) { quick.liveStats = ({}); quick.lastPacing = null; quick.pacingShare = -1 }
    }

    // Frame Pacing Repair (EnhancementSettings::pacingRepair) and Live HUD Lock (::hudLock):
    // both work on graph frame generation; XeSS paces and generates inside its presenter.
    readonly property bool ownTechPossible: quick.fgPossible && !veyra.presentationOwned
    readonly property bool pacingOn: quick.ownTechPossible && veyra.pacingRepair === true
    function setPacingRepair(on) {
        veyra.pacingRepair = on
        // The repair re-times the frames duplicate removal finds.
        if (on && Number(veyra.contentRate) === 0) veyra.contentRate = 1
    }

    function multiplierFor(fps, base) {
        const gameFps = base || quick.baseFps
        const offered = (veyra.fgMultiplierChoices || []).map(o => parseInt(o.id))
            .filter(m => m >= 2).sort((a, b) => a - b)
        if (offered.length === 0) return 0
        let pick = offered[0]
        for (let i = 0; i < offered.length; ++i)
            if (gameFps * offered[i] <= fps + 0.5) pick = offered[i]
        return pick
    }

    // base: the game's frame rate; defaults to the saved one (a profile passes its own,
    // since the preference it just wrote may not have reached the binding yet).
    function applyTarget(id, base) {
        const gameFps = base || quick.baseFps
        if (id === "off") { veyra.fgEnabled = false; return }
        const fps = targetFps(id)
        if (fps <= gameFps + 0.5) { veyra.fgEnabled = false; return }
        const m = quick.adaptive ? Presets.adaptiveCeiling(quick.offeredMultipliers, fps) : multiplierFor(fps, gameFps)
        if (m < 2) return
        veyra.fgMultiplier = m
        // A console game below the capture rate repeats every picture (30 FPS in a 60 Hz
        // signal: AABBCC). Generating between two identical pictures wastes the work and
        // smears, so the quick setup always removes duplicates first; automatic removal
        // passes a game that has none untouched. A chosen mode (60->30) stays.
        if (Number(veyra.contentRate) === 0) veyra.contentRate = 1
        // XeSS paces its own presentation (presentationOwned); the cap is then
        // greyed in Professional mode too, and the multiplier alone decides.
        if (!veyra.presentationOwned) {
            if (id === "monitor") veyra.outputRateMode = 1
            else { veyra.outputCustomFps = fps; veyra.outputRateMode = 2 }
        }
    }

    function setEnhancement(on) {
        if (on) {
            if (veyra.nrLayers.length === 0) veyra.addEffect("nr")
            else veyra.setAllNrEnabled(true)
        } else if (veyra.nrLayers.length > 0) {
            veyra.setAllNrEnabled(false)
        }
    }

    // --- the look of AI picture enhancement --------------------------------------
    // Two of the enhancement model's own controls: how far it reworks light and colour ("tone") and
    // how much fine detail it adds ("structure"), both 0..1 with 1 the model's full strength (what
    // PushFPS has always used, and what Reset returns to). Professional mode shows the same two
    // values per layer; here every enhancement layer gets the same pair, and the first one is shown.
    // Nothing is stored apart from those layer values and the look kept in each profile.
    readonly property var looks: [
        { id: "subtle", label: qsTr("柔和"), tone: 0.4, structure: 0.5 },
        { id: "balanced", label: qsTr("均衡"), tone: 0.7, structure: 0.8 },
        { id: "strong", label: qsTr("强烈"), tone: 1, structure: 1 }
    ]
    readonly property var lookLayer: quick.nrPossible && veyra.nrLayers.length > 0 ? veyra.nrLayers[0] : null
    function lookValue(v) { const n = Number(v); return isFinite(n) ? Math.max(0, Math.min(1, n)) : 1 }
    readonly property real lookTone: lookLayer && lookLayer.tone !== undefined ? lookValue(lookLayer.tone) : 1
    readonly property real lookStructure: lookLayer && lookLayer.structure !== undefined ? lookValue(lookLayer.structure) : 1
    readonly property string lookId: {
        for (const l of quick.looks)
            if (Math.abs(l.tone - quick.lookTone) < 0.005 && Math.abs(l.structure - quick.lookStructure) < 0.005) return l.id
        return ""
    }
    // The AMD enhancement runtime (4) has no such controls (Professional mode greys them out there too).
    readonly property bool lookShown: quick.nrPossible && veyra.nrAnyEnabled === true && quick.lookLayer !== null && Number(quick.lookLayer.runtime) !== 4
    // undefined leaves that value as it is.
    function setLook(tone, structure) {
        if (!quick.nrPossible) return false
        let ok = true
        const layers = veyra.nrLayers.map(l => l.index)
        for (const index of layers) {
            if (tone !== undefined) ok = veyra.setNrLayerParameter(index, "tone", Math.round(quick.lookValue(tone) * 100) / 100) && ok
            if (structure !== undefined) ok = veyra.setNrLayerParameter(index, "structure", Math.round(quick.lookValue(structure) * 100) / 100) && ok
        }
        return ok && layers.length > 0
    }
    function applyLook(id) {
        for (const l of quick.looks) if (l.id === id) return quick.setLook(l.tone, l.structure)
        return false
    }
    function resetLook() { return quick.setLook(1, 1) }
    // Split view while tuning: the picture without the enhancement on the left, with it on the right
    // (the comparison Professional mode has under Display). Frame generation pauses while it is on,
    // so a split this panel turned on never stays: it ends when the look controls go away, when this
    // panel is hidden, and after two minutes.
    readonly property bool lookSplit: veyra.compareMode === 2
    property bool lookCompare: false
    function setLookCompare(on) {
        if (on) {
            if (veyra.hasSource !== true) return
            quick.lookCompare = true
            veyra.compareMode = 2
        } else {
            quick.lookCompare = false
            if (veyra.compareMode === 2) veyra.compareMode = 0
        }
    }
    onLookSplitChanged: if (!lookSplit) lookCompare = false
    onLookShownChanged: if (!lookShown && lookCompare) setLookCompare(false)
    onVisibleChanged: if (!visible && lookCompare) setLookCompare(false)
    Timer {
        objectName: "pushfps-look-compare-timer"
        interval: 120000
        running: quick.lookCompare && quick.lookSplit
        onTriggered: quick.setLookCompare(false)
    }

    // --- console and game profiles ---------------------------------------------
    // A console profile remembers the quick choices per console: switching console
    // keeps what was set for the old one and brings back the new one's. A game
    // profile is a named snapshot (console, game frame rate, target, enhancement, low
    // latency) plus, when the current Professional settings are a saved preset, that
    // preset's name, which is applied first. Stored as JSON in the pushProfiles
    // preference (validated by the bridge); nothing here adds engine state.
    readonly property var consoles: [
        { id: "ps5", label: "PlayStation 5" }, { id: "ps5pro", label: "PS5 Pro" },
        { id: "xbox", label: "Xbox Series X|S" }, { id: "switch", label: "Nintendo Switch" },
        { id: "switch2", label: "Nintendo Switch 2" },
        { id: "other", label: qsTr("其他主机") }
    ]
    readonly property string consoleId: {
        const c = String(veyra.preferences.pushConsole || "ps5")
        return consoles.some(x => x.id === c) ? c : "ps5"
    }
    readonly property var profiles: {
        try {
            const p = JSON.parse(veyra.preferences.pushProfiles || "{}")
            return p && typeof p === "object" && !Array.isArray(p) ? p : ({})
        } catch (e) { return ({}) }
    }
    readonly property var games: Array.isArray(profiles.games) ? profiles.games.filter(g => g && typeof g.name === "string") : []
    readonly property string activeGame: typeof profiles.game === "string" && games.some(g => g.name === profiles.game) ? profiles.game : ""
    readonly property int maxGames: 50

    function snapshot() {
        const s = { fps: quick.baseFps, target: quick.currentTarget, enhance: veyra.nrAnyEnabled === true,
                 adaptive: veyra.fgAdaptive === true, pacing: veyra.pacingRepair === true, hudLock: veyra.hudLock === true,
                 lowLatency: veyra.nvidiaLowLatencyAvailable === true && (veyra.nvidiaLowLatency ?? 0) > 0 }
        // The enhancement's look, when there is an enhancement layer to read it from.
        if (quick.lookLayer !== null) s.look = { tone: quick.lookTone, structure: quick.lookStructure }
        return s
    }
    function writeProfiles(p) {
        p.v = 1
        return veyra.setPreference("pushProfiles", JSON.stringify(p))
    }
    function copyProfiles() { return JSON.parse(JSON.stringify(quick.profiles)) }
    function applySnapshot(s) {
        if (!s) return
        const fps = s.fps === 40 || s.fps === 60 ? s.fps : 30
        veyra.setPreference("pushGameFps", fps)
        if (typeof s.adaptive === "boolean" && quick.adaptivePossible) {
            veyra.fgAdaptive = s.adaptive
            if (s.adaptive && Number(veyra.contentRate) === 0) veyra.contentRate = 1
        }
        if (quick.ownTechPossible && typeof s.pacing === "boolean") quick.setPacingRepair(s.pacing)
        if (quick.ownTechPossible && typeof s.hudLock === "boolean") veyra.hudLock = s.hudLock
        if (quick.fgPossible && ["off", "60", "120", "monitor"].indexOf(s.target) >= 0)
            quick.applyTarget(s.target, fps)
        if (quick.nrPossible && typeof s.enhance === "boolean" && s.enhance !== (veyra.nrAnyEnabled === true))
            quick.setEnhancement(s.enhance)
        // Profiles saved before 3.2.1 have no look: the current one stays. So does a look that already
        // matches: layers given different values in Professional mode are only made equal when a
        // profile really asks for another look.
        if (quick.nrPossible && s.look && typeof s.look === "object" && isFinite(Number(s.look.tone)) && isFinite(Number(s.look.structure))) {
            const tone = quick.lookValue(s.look.tone), structure = quick.lookValue(s.look.structure)
            if (Math.abs(tone - quick.lookTone) >= 0.005 || Math.abs(structure - quick.lookStructure) >= 0.005)
                quick.setLook(tone, structure)
        }
        if (veyra.nvidiaLowLatencyAvailable === true && typeof s.lowLatency === "boolean")
            veyra.nvidiaLowLatency = s.lowLatency ? 1 : 0
    }
    function switchConsole(id) {
        if (id === quick.consoleId) return
        const p = copyProfiles()
        if (!p.consoles || typeof p.consoles !== "object") p.consoles = {}
        p.consoles[quick.consoleId] = snapshot()
        p.game = ""
        if (!writeProfiles(p)) return
        if (!veyra.setPreference("pushConsole", id)) return
        applySnapshot(p.consoles[id])
    }
    function saveGame(name) {
        name = String(name || "").trim().slice(0, 40)
        if (name.length === 0) return false
        const p = copyProfiles()
        const list = Array.isArray(p.games) ? p.games.filter(g => g && g.name !== name) : []
        if (list.length >= quick.maxGames) return false
        const presetName = veyra.currentPresetName || ""
        const g = snapshot()
        g.name = name
        g.console = quick.consoleId
        g.preset = (veyra.presets || []).some(x => x.name === presetName) ? presetName : ""
        list.push(g)
        list.sort((a, b) => a.name.localeCompare(b.name))
        p.games = list
        p.game = name
        return writeProfiles(p)
    }
    function applyGame(name) {
        const g = quick.games.find(x => x.name === name)
        const p = copyProfiles()
        p.game = g ? name : ""
        if (!g) { writeProfiles(p); return }
        if (g.preset) {
            const preset = (veyra.presets || []).find(x => x.name === g.preset)
            if (preset) veyra.applyPresetIndex(preset.index)
        }
        if (quick.consoles.some(c => c.id === g.console)) veyra.setPreference("pushConsole", g.console)
        applySnapshot(g)
        writeProfiles(p)
    }
    function deleteGame(name) {
        const p = copyProfiles()
        p.games = (Array.isArray(p.games) ? p.games : []).filter(g => g && g.name !== name)
        if (p.game === name) p.game = ""
        writeProfiles(p)
    }
    // A capture card that captures 120 Hz (the 4K X; the 4K S at 1080p/1440p) gets each
    // frame in 8 ms instead of 17, and lets 40 FPS (120 Hz mode) games through; Switch 2 docked
    // output tops out at 60 Hz at 4K, so the extra line is for PlayStation and Xbox. A card that only
    // passes 120 Hz through (HD60 X: 60 Hz capture) stays at 60 Hz.
    readonly property string hz120Tip: qsTr("采集卡能以 120 Hz 采集时（如 4K X，或 1080p/1440p 下的 4K S），也可输出 120 Hz：每帧早约 8 毫秒到达，40 FPS 的 120 Hz 模式也能使用。只支持 120 Hz 直通的采集卡请保持 60 Hz。")
    readonly property string setupTip: quick.consoleTip + (quick.consoleId === "ps5" || quick.consoleId === "xbox" ? " " + quick.hz120Tip : "")
    readonly property string consoleTip: {
        switch (quick.consoleId) {
        case "xbox": return qsTr("Xbox 请在 设置 → 常规 → 电视和显示选项 中选择 60 Hz。游戏没有 HDCP，可以直接采集。")
        case "switch2": return qsTr("Switch 2 请放在底座上，输出 60 Hz。游戏没有 HDCP，可以直接采集。")
        case "switch": return qsTr("Switch 请放在底座上，输出 1080p 60 Hz。游戏没有 HDCP，可以直接采集。")
        case "other": return qsTr("主机请输出 60 Hz，并关闭 HDCP（如有此选项），否则采集画面可能为黑屏。")
        default: return qsTr("主机请输出 60 Hz。PlayStation 需在 设置 → 系统 → HDMI 中关闭 HDCP，否则采集画面为黑屏。")
        }
    }

    readonly property string targetHint: {
        if (!quick.fgPossible) return Support.status(veyra, "frame-generation").reason || ""
        if (quick.currentTarget === "") return qsTr("已在专业模式中手动设置")
        if (quick.currentTarget === "off") return qsTr("主机游戏保持原有帧率")
        if (quick.adaptive) return qsTr("自适应：按游戏的实时帧率在 2× 到 %1× 之间选择，尽量接近 %2 FPS")
            .arg(veyra.fgMultiplier).arg(Math.round(quick.targetFps(quick.currentTarget)))
        const m = veyra.fgMultiplier
        const reach = quick.baseFps * m
        const want = quick.targetFps(quick.currentTarget)
        if (reach + 0.5 < want)
            return qsTr("当前补帧方式最高 %1×，%2 FPS 游戏约为 %3 FPS").arg(m).arg(quick.baseFps).arg(reach)
        return qsTr("%1 FPS 游戏将以 %2 FPS 运行（%3× 补帧）").arg(quick.baseFps)
            .arg(Math.round(Math.min(reach, want))).arg(m)
    }

    // Header: what this card is, and the one-line explanation in the full form.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: 12
        Layout.bottomMargin: 2
        spacing: 3
        Text {
            text: qsTr("快速设置")
            color: Theme.t1
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsH3
            font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
        }
        Text {
            visible: !quick.compact
            Layout.fillWidth: true
            text: qsTr("为主机游戏选好帧率和画面，其他细节在专业模式里")
            color: Theme.t3
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsHint
            wrapMode: Text.WordWrap
        }
    }

    // First-run tip, as a quiet info strip rather than a paragraph above the rows.
    Rectangle {
        objectName: "pushfps-setup-tip"
        visible: quick.showSetupTip && !veyra.hasCaptureSession
        Layout.fillWidth: true
        Layout.topMargin: 8
        Layout.bottomMargin: 2
        implicitHeight: tipRow.implicitHeight + 16
        radius: 9
        color: Theme.ink(0.035)
        border.width: 1
        border.color: Theme.stroke
        RowLayout {
            id: tipRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 9
            VIcon { Layout.alignment: Qt.AlignTop; name: "info"; size: 14; color: Theme.accent }
            Text {
                Layout.fillWidth: true
                text: quick.setupTip
                color: Theme.t2
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsHint
                lineHeight: 1.1
                wrapMode: Text.WordWrap
            }
        }
    }

    // Picture profiles (PushProfileRow.qml): one choice for the whole picture pipeline (clean-up, upscaling,
    // the experimental neural layer, a frame generation target) from the table in
    // engine/PushPerformanceMode.h. "None" (the default) leaves every row below to the user.
    PushProfileRow { objectName: "pushfps-profile-rows"; compact: quick.compact }

    // PushFPS tools: measure this PC, start from a game's preset, protect the HUD,
    // record a before/after clip. They wrap onto a second line in narrow panels.
    readonly property bool compareRecording: veyra.recording && veyra.recordingState.compare === true
    function openOnPicture(dialog) {
        // The picture has to be on screen while it is sampled (a hidden window presents differently).
        if (veyra.hasSource && !quick.compactPro) veyra.navigate("pro")
        veyra.navigate(dialog)
    }
    GridLayout {
        id: toolsGrid
        objectName: "pushfps-tools-row"
        Layout.fillWidth: true
        Layout.topMargin: 8
        Layout.bottomMargin: 6
        // Two equal columns; one in a very narrow panel.
        columns: width >= 360 ? 2 : 1
        columnSpacing: 8
        rowSpacing: 8
        PushToolTile {
            objectName: "pushfps-optimize"
            // Grey until Optimise for my PC has chosen and applied settings (green would read as
            // "already on"); green while that result is kept, grey again after Undo.
            primary: quick.optimized
            iconName: "zap"
            text: qsTr("为我的电脑优化")
            sub: qsTr("测试每种组合，选出最好的")
            tip: qsTr("在实际画面上测试每种组合，自动选出最好的一种")
            onClicked: quick.openOnPicture("optimize")
        }
        PushToolTile {
            objectName: "pushfps-game-presets"
            iconName: "gamepad"
            text: qsTr("游戏预设")
            sub: qsTr("GTA VI 等，HUD 保持清晰")
            tip: qsTr("GTA VI 等游戏的推荐设置，小地图和 HUD 保持清晰")
            onClicked: veyra.navigate("games")
        }
        PushToolTile {
            objectName: "pushfps-hud"
            iconName: "scan"
            text: qsTr("HUD 保护")
            sub: qsTr("自动识别，可手动修正")
            tip: qsTr("自动找出小地图和 HUD，让它们不补帧；方框可以手动修正")
            onClicked: quick.openOnPicture("hud")
        }
        PushToolTile {
            objectName: "pushfps-compare-clip"
            iconName: "split"
            readonly property int clipSeconds: {
                const n = Number(veyra.preferences.pushCompareSeconds || 15)
                return [10, 15, 30, 60].indexOf(n) >= 0 ? n : 15
            }
            readonly property string clipLayout: {
                const n = Number(veyra.preferences.pushCompareLayout || 0)
                return n === 2 ? qsTr("竖屏") : n === 1 ? qsTr("并排") : qsTr("分屏")
            }
            text: quick.compareRecording ? qsTr("停止对比片段") : qsTr("对比片段")
            sub: quick.compareRecording ? qsTr("正在录制…") : veyra.recording ? qsTr("录制中不可用")
                 : qsTr("%1 秒").arg(clipSeconds) + " · " + clipLayout
            tip: qsTr("录制前后对比视频：一边是主机原本的帧率，一边是 PushFPS。可选分屏、并排或竖屏（TikTok、Shorts、Reels），以及时长")
            // Opens with nothing playing too, so the layout and length can be set beforehand;
            // the dialog says a picture is needed and only its start button records.
            enabled: quick.compareRecording || !veyra.recording
            onClicked: {
                if (quick.compareRecording) veyra.stopRecording("compare-button")
                else veyra.navigate("compare")
            }
        }
        PushToolTile {
            objectName: "pushfps-stream"
            // Fifth tile: the whole row, so the grid stays even.
            Layout.columnSpan: toolsGrid.columns
            Layout.fillWidth: true
            iconName: "cast"
            readonly property var streamState: quick.liveStream
            text: streamState.active ? qsTr("直播中") : qsTr("直播")
            sub: streamState.active ? (streamState.live ? qsTr("%1 个平台").arg((streamState.outputs || []).filter(o => o.state === "live").length) : qsTr("正在连接平台…"))
                             : qsTr("Twitch、YouTube、Kick、TikTok，可同时推")
            tip: qsTr("把 PushFPS 的画面直接推到直播平台，可以同时推多个平台")
            onClicked: veyra.navigate("stream")
        }
    }

    // The capture card in use, and whether it is on PushFPS's tested list.
    VRow {
        id: cardRow
        objectName: "pushfps-card-row"
        readonly property var card: Cards.find(veyra.captureDeviceLabel)
        visible: String(veyra.captureDeviceLabel || "").length > 0
        label: qsTr("采集卡")
        hint: card && card.status === "tested" ? qsTr("PushFPS 已实测")
            : card && card.status === "veyra" ? qsTr("Veyra 作者已实测")
            : qsTr("还没有人报告这张卡的结果，欢迎在 GitHub 上分享")
        hintBelow: true
        RowLayout {
            spacing: 6
            Text {
                text: veyra.captureDeviceLabel || ""
                color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: 12
                elide: Text.ElideRight; Layout.maximumWidth: 180
            }
            VTag {
                objectName: "pushfps-card-status"
                kind: cardRow.card && cardRow.card.status !== "untested" ? "ok" : ""
                text: cardRow.card && cardRow.card.status !== "untested" ? qsTr("已测试") : qsTr("未测试")
            }
            VButton {
                visible: !(cardRow.card && cardRow.card.status === "tested")
                ghost: true
                text: qsTr("报告")
                onClicked: Qt.openUrlExternally(Cards.reportUrl)
            }
        }
    }

    VRow {
        objectName: "pushfps-console-row"
        label: qsTr("游戏主机")
        VSelect {
            objectName: "pushfps-console"
            implicitWidth: 190
            options: quick.consoles
            value: (quick.consoles.find(c => c.id === quick.consoleId) || quick.consoles[0]).label
            onPicked: id => quick.switchConsole(id)
        }
    }

    VRow {
        id: gameRow
        objectName: "pushfps-game-row"
        label: qsTr("游戏配置")
        hint: quick.activeGame.length > 0 ? "" : qsTr("把当前设置存为某个游戏的配置，之后一键切换")
        property bool naming: false
        RowLayout {
            spacing: 4
            VSelect {
                objectName: "pushfps-game"
                visible: !gameRow.naming
                implicitWidth: 150
                options: [{ id: "", label: qsTr("无") }].concat(quick.games.map(g => ({ id: g.name, label: g.name })))
                value: quick.activeGame.length > 0 ? quick.activeGame : qsTr("无")
                onPicked: id => quick.applyGame(id)
            }
            DialogTextField {
                id: gameName
                objectName: "pushfps-game-name"
                visible: gameRow.naming
                implicitWidth: 150
                placeholder: qsTr("游戏名称")
            }
            VButton {
                objectName: "pushfps-game-save"
                icon: true
                iconName: gameRow.naming ? "check" : "plus"
                tip: gameRow.naming ? qsTr("保存") : (quick.activeGame.length > 0 ? qsTr("更新或另存为游戏配置") : qsTr("存为游戏配置"))
                enabled: gameRow.naming ? gameName.text.trim().length > 0 : quick.games.length < quick.maxGames || quick.activeGame.length > 0
                onClicked: {
                    if (gameRow.naming) { if (quick.saveGame(gameName.text)) gameRow.naming = false; return }
                    gameName.text = quick.activeGame
                    gameRow.naming = true
                }
            }
            VButton {
                objectName: "pushfps-game-delete"
                icon: true
                iconName: gameRow.naming ? "x" : "trash"
                tip: gameRow.naming ? qsTr("取消") : qsTr("删除此游戏配置")
                visible: gameRow.naming || quick.activeGame.length > 0
                onClicked: {
                    if (gameRow.naming) { gameRow.naming = false; return }
                    quick.deleteGame(quick.activeGame)
                }
            }
        }
    }

    // Performance modes (PushModeRow.qml): one choice that sets frame generation, duplicate removal and
    // presentation from the table in engine/PushPerformanceMode.h. "Off" leaves every row below to the user.
    PushModeRow { objectName: "pushfps-mode-rows" }
    // SmartLink "Connection" (PushSmartLinkRow.qml), experimental: shown only for PS5 Remote Play in a Slipstream
    // build. Off by default; the choice applies from the next PS5 connection.
    PushSmartLinkRow { objectName: "pushfps-smartlink-rows" }

    VRow {
        objectName: "pushfps-game-fps-row"
        label: qsTr("游戏帧率")
        help: qsTr("游戏自己每秒画出多少张新画面。主机输出固定的 60 Hz 或 120 Hz 信号，30 FPS 的游戏在 60 Hz 下每张画面重复一次。PushFPS 去掉重复的画面，再根据游戏帧率决定要生成几帧")
        hint: qsTr("大多数主机游戏为 30 FPS；120 Hz 模式的游戏常为 40；性能模式选 60")
        VSeg {
            objectName: "pushfps-game-fps"
            enabled: quick.fgPossible
            opacity: enabled ? 1 : 0.4
            options: [{ id: "30", label: "30" }, { id: "40", label: "40" }, { id: "60", label: "60" }]
            current: String(quick.baseFps)
            onPicked: id => quick.setGameFps(id)
        }
    }

    // The game is set to 30 FPS but every 60 Hz capture frame looks new to the automatic duplicate
    // removal (a capture card that adds noise to repeated pictures, field report 2026-11-17: GTA V
    // fidelity mode measured as 60). Then frame generation works from 60 and the console side of a
    // comparison clip reads 60. One tap switches to the fixed 60 -> 30 mode (every second sample).
    readonly property bool cadenceMismatch: quick.liveStats.running === true && veyra.fgEnabled === true && quick.baseFps === 30
        && Math.abs(Number(quick.liveStats.captureFps || 0) - 60) < 3 && Number(quick.liveStats.contentFps || 0) > 45
        && Number(veyra.contentRate) !== 5
    VRow {
        objectName: "pushfps-cadence-row"
        visible: quick.cadenceMismatch
        label: qsTr("检测到 60 FPS")
        hint: qsTr("游戏帧率设为 30，但每一帧采集画面都被当作新画面（%1 FPS）。采集卡可能给重复画面加了噪点。改用固定的「60→30」去重，按 30 FPS 补帧").arg(Math.round(Number(quick.liveStats.contentFps || 0)))
        hintColor: Theme.warn
        hintBelow: true
        VButton {
            objectName: "pushfps-cadence-fix"
            text: qsTr("改为 60→30")
            onClicked: { veyra.contentRate = 5; veyra.logUi("push-quick", "content rate 60->30 from the cadence hint") }
        }
    }

    VRow {
        objectName: "pushfps-target-row"
        label: qsTr("目标帧率")
        help: qsTr("补帧后每秒显示的画面数。生成的帧插在两张真实画面之间，让运动更顺滑；手柄的反应速度不会因此变快")
        hint: quick.targetHint
        hintBelow: true
        hintColor: quick.fgPossible ? Theme.t3 : Theme.warn
        VSeg {
            objectName: "pushfps-target"
            enabled: quick.fgPossible
            opacity: enabled ? 1 : 0.4
            // 60 FPS is only a target when the game runs below it.
            options: quick.baseFps < 60
                ? [{ id: "off", label: qsTr("关闭", "off") }, { id: "60", label: "60 FPS" },
                   { id: "120", label: "120 FPS" }, { id: "monitor", label: qsTr("匹配显示器") }]
                : [{ id: "off", label: qsTr("关闭", "off") },
                   { id: "120", label: "120 FPS" }, { id: "monitor", label: qsTr("匹配显示器") }]
            current: quick.currentTarget
            onPicked: id => quick.applyTarget(id)
        }
    }

    // Streaming mode has no Professional page beside it: the frame generation method (PushFlow, DLSS, FSR,
    // ...) and PushFlow's tier are chosen here too.
    VRow {
        objectName: "pushfps-fg-backend-row"
        visible: quick.compactPro
        label: qsTr("补帧方式")
        hint: veyra.fgProviderText
        hintBelow: true
        VSelect {
            objectName: "pushfps-fg-backend"
            options: veyra.fgBackendChoices
            value: (options.find(o => o.id === veyra.fgBackendName) || options[0]).label
            onPicked: id => veyra.fgBackendName = id
        }
    }

    VRow {
        objectName: "pushfps-pushflow-tier-row"
        visible: quick.compactPro && veyra.fgBackendName === "pushflow"
        label: qsTr("PushFlow 档位")
        // While it runs: the precision this graphics card passed with and the network's time per frame.
        hint: quick.liveStats.pushFlowP50Ms > 0
              ? qsTr("正在运行：%1 · 每帧 %2 ms（95%：%3 ms）").arg(quick.liveStats.pushFlowPrecision === "fp16" ? qsTr("半精度") : qsTr("全精度"))
                    .arg(Number(quick.liveStats.pushFlowP50Ms).toFixed(1)).arg(Number(quick.liveStats.pushFlowP95Ms).toFixed(1))
              : ""
        hintBelow: true
        VSeg {
            objectName: "pushfps-pushflow-tier"
            options: [{ id: "0", label: qsTr("快速") }, { id: "1", label: qsTr("均衡") }, { id: "2", label: qsTr("高质量") }]
            current: String(Math.min(veyra.vfgQuality, 2))
            onPicked: id => veyra.vfgQuality = Number(id)
        }
    }

    VRow {
        objectName: "pushfps-adaptive-row"
        label: qsTr("自适应倍率")
        help: qsTr("很多主机游戏的帧率并不固定：30 FPS 的游戏会掉到 25，40 FPS 模式会在 35 到 45 之间波动。开启后 PushFPS 实时测量游戏帧率，每一对画面都选择最接近目标帧率的补帧倍率，类似 NVIDIA DLSS 4.5 的动态多帧生成")
        hint: !quick.fgPossible ? (Support.status(veyra, "frame-generation").reason || "")
            : !quick.adaptivePossible ? qsTr("当前补帧方式只有 2×，无需自适应")
            : quick.adaptive && quick.liveStats.fgMultiplierNow > 1 && quick.liveStats.contentFps > 0
              ? qsTr("现在 %1×：游戏 %2 FPS → 约 %3 FPS").arg(quick.liveStats.fgMultiplierNow)
                    .arg(Math.round(quick.liveStats.contentFps)).arg(Math.round(quick.liveStats.contentFps * quick.liveStats.fgMultiplierNow))
              : qsTr("游戏帧率波动时自动调整倍率，输出尽量贴近目标")
        VSwitch {
            objectName: "pushfps-adaptive"
            enabled: quick.adaptivePossible && veyra.fgEnabled
            opacity: enabled ? 1 : 0.4
            checked: quick.adaptive
            onToggled: checked => quick.setAdaptive(checked)
        }
    }

    VRow {
        objectName: "pushfps-pacing-row"
        label: qsTr("帧节奏修复")
        help: qsTr("很多锁 30 FPS 的主机游戏出帧并不均匀：有的画面停留 17 毫秒，有的停留 50 毫秒，即使帧率没错，画面也会顿挫。PushFPS 把每一帧放回均匀的位置再补帧，主机自己做不到这一点。每帧最多晚一次刷新显示")
        hint: !quick.ownTechPossible ? (quick.fgPossible ? qsTr("XeSS 自己控制节奏，不能同时使用") : (Support.status(veyra, "frame-generation").reason || ""))
            : quick.pacingOn && quick.pacingShare >= 0
              ? (quick.pacingShare > 0.05 ? qsTr("正在修复：%1% 的画面重新对齐").arg(Math.round(quick.pacingShare * 100))
                                          : qsTr("这款游戏的节奏已经均匀，无需修复"))
              : qsTr("修复忽快忽慢的出帧（如 17/50 毫秒交替），让 30 FPS 游戏动起来更顺")
        VSwitch {
            objectName: "pushfps-pacing"
            enabled: quick.ownTechPossible && veyra.fgEnabled
            opacity: enabled ? 1 : 0.4
            checked: quick.pacingOn
            onToggled: checked => quick.setPacingRepair(checked)
        }
    }

    VRow {
        objectName: "pushfps-hudlock-row"
        label: qsTr("实时 HUD 锁定")
        help: qsTr("补出来的帧会把画面上静止的文字、准星和血条跟着背景一起拉动，看起来发糊或抖动。开启后 PushFPS 每一帧都会找出静止的 HUD 像素，补帧时直接用主机原来的像素，不需要画框")
        // What actually runs (EnhanceGraph::hudLockState): a pass that could not start says so.
        readonly property int hudState: Number(quick.liveStats.hudLockState || 0)
        hint: !quick.ownTechPossible ? (quick.fgPossible ? qsTr("XeSS 自己生成画面，不能同时使用") : (Support.status(veyra, "frame-generation").reason || ""))
            : veyra.hudLock === true && hudState === 2 ? qsTr("实时 HUD 锁定没能在这台电脑上启动，补帧照常运行（原因见日志）")
            : veyra.hudLock === true && hudState === 1 ? qsTr("正在运行：文字和准星在补出来的帧里保持清晰")
            : qsTr("每一帧自动找出 HUD，文字和准星在补出来的帧里保持清晰")
        VSwitch {
            objectName: "pushfps-hudlock"
            enabled: quick.ownTechPossible && veyra.fgEnabled
            opacity: enabled ? 1 : 0.4
            checked: quick.ownTechPossible && veyra.hudLock === true
            onToggled: checked => veyra.hudLock = checked
        }
    }

    VRow {
        objectName: "pushfps-enhance-row"
        label: qsTr("AI 画面增强")
        help: qsTr("用 AI 去掉采集卡压缩造成的色块和噪点，让细节更清楚。和补帧无关，可以单独开关")
        hint: quick.nrPossible ? qsTr("关闭时只做补帧。开启后每帧都经过 NVIDIA 神经渲染（DLSS 5 NR）增强，画面更清晰，但更耗 GPU")
                               : (Support.status(veyra, "nr").reason || "")
        VSwitch {
            objectName: "pushfps-enhance"
            enabled: quick.nrPossible
            opacity: enabled ? 1 : 0.4
            checked: veyra.nrAnyEnabled
            onToggled: checked => quick.setEnhancement(checked)
        }
    }

    // The enhancement's look (shown while AI picture enhancement is on).
    VRow {
        objectName: "pushfps-look-row"
        visible: quick.lookShown
        label: qsTr("增强风格")
        help: qsTr("AI 画面增强改动画面的力度。「强烈」是完整效果，也是一直以来的默认值；觉得画面不像原作、过于锐利或人脸不自然时，选「均衡」或「柔和」。也可以用下面两个滑块自己调")
        hint: quick.lookId === "" ? qsTr("自定义：已用滑块调整") : quick.lookId === "strong" ? qsTr("完整效果（默认）") : qsTr("比默认更接近原画")
        RowLayout {
            spacing: 8
            VSeg {
                objectName: "pushfps-look"
                options: quick.looks.map(l => ({ id: l.id, label: l.label }))
                current: quick.lookId
                onPicked: id => quick.applyLook(id)
            }
            VButton {
                objectName: "pushfps-look-reset"
                text: qsTr("重置")
                enabled: quick.lookId !== "strong"
                opacity: enabled ? 1 : 0.4
                onClicked: quick.resetLook()
            }
        }
    }
    VRow {
        objectName: "pushfps-look-tone-row"
        visible: quick.lookShown
        label: qsTr("光影")
        hint: qsTr("AI 重新处理明暗和色彩的程度。0% 保持原画的光影和颜色")
        value: Math.round(quick.lookTone * 100) + "%"
        VSlider {
            objectName: "pushfps-look-tone"
            valueFromModel: true
            implicitWidth: 150
            from: 0; to: 1; stepSize: 0.01
            inputScale: 100
            value: quick.lookTone
            resettable: true; defaultValue: 1
            onMoved: v => { if (quick.lookShown) quick.setLook(v, undefined) }
        }
    }
    VRow {
        objectName: "pushfps-look-detail-row"
        visible: quick.lookShown
        label: qsTr("细节")
        hint: qsTr("AI 补充细小纹理、接触阴影和反光的程度。0% 不添加细节")
        value: Math.round(quick.lookStructure * 100) + "%"
        VSlider {
            objectName: "pushfps-look-detail"
            valueFromModel: true
            implicitWidth: 150
            from: 0; to: 1; stepSize: 0.01
            inputScale: 100
            value: quick.lookStructure
            resettable: true; defaultValue: 1
            onMoved: v => { if (quick.lookShown) quick.setLook(undefined, v) }
        }
    }
    VRow {
        objectName: "pushfps-look-compare-row"
        visible: quick.lookShown
        label: qsTr("与原画对比")
        help: qsTr("把画面分成两半：左边是没有 AI 画面增强的原画，右边是增强后的画面，方便边调边看。在专业模式的画面上可以拖动分割线")
        hint: quick.lookSplit ? qsTr("对比时补帧暂停；关闭对比后自动恢复。两分钟后自动关闭")
                              : veyra.hasSource === true ? qsTr("左边原画，右边增强后") : qsTr("先连接画面")
        VSwitch {
            objectName: "pushfps-look-compare"
            enabled: veyra.hasSource === true || quick.lookSplit
            opacity: enabled ? 1 : 0.4
            checked: quick.lookSplit
            onToggled: checked => quick.setLookCompare(checked)
        }
    }

    VRow {
        objectName: "pushfps-lowest-latency-row"
        label: qsTr("最低延迟")
        help: qsTr("一键打开所有不影响画质的低延迟设置：显卡只排队一帧（NVIDIA 显卡用 NVIDIA 低延迟），连接后进入全屏（绕过桌面合成，少等一次刷新），屏幕支持 G-SYNC/FreeSync 时用可变刷新同步（不撕裂、不跳帧）。补帧、AI 增强、HUD 锁定和帧节奏修复保持不变。关闭后恢复之前的设置")
        hint: quick.lowestLatencyHint
        VSwitch {
            objectName: "pushfps-lowest-latency"
            checked: quick.lowestLatency
            onToggled: checked => quick.setLowestLatency(checked)
        }
    }

    VRow {
        objectName: "pushfps-latency-overlay-row"
        label: qsTr("在画面上显示延迟")
        hint: qsTr("实测电脑处理延迟（采集卡画面到达到交给显示器）和帧率；采集卡、显示器和游戏的延迟另计")
        RowLayout {
            spacing: 8
            VButton {
                objectName: "pushfps-latency-guide-open"
                ghost: true
                text: qsTr("测量总延迟")
                iconName: "gauge"
                onClicked: veyra.navigate("latencyguide")
            }
            VSwitch {
                objectName: "pushfps-latency-overlay"
                checked: veyra.preferences.pushLatencyOverlay === true
                onToggled: checked => veyra.setPreference("pushLatencyOverlay", checked)
            }
        }
    }

    // Statistics overlay (PushStatsOverlay.qml): reads numbers the engine already has, once a second.
    VRow {
        objectName: "pushfps-stats-overlay-row"
        label: qsTr("在画面上显示详细统计")
        hint: qsTr("模式、来源和显示帧率、生成与真实的帧、处理延迟，PS5 串流还有连接质量、丢包、抖动和线路。只读取已有的数字，没有的显示「未测量」")
        VSwitch {
            objectName: "pushfps-stats-overlay"
            checked: veyra.preferences.pushStatsOverlay === true
            onToggled: checked => veyra.setPreference("pushStatsOverlay", checked)
        }
    }

    // Plug and play: Veyra's 启动后自动继续上次内容 (autoResume: the last capture card with its
    // settings, or the last stream) and PushFPS's fullscreen-on-connect (Main.qml).
    VRow {
        objectName: "pushfps-auto-connect-row"
        visible: !quick.compact
        label: qsTr("启动时自动连接")
        hint: qsTr("打开 PushFPS 时直接连接上次的采集卡（沿用上次的设置）或串流")
        VSwitch {
            objectName: "pushfps-auto-connect"
            checked: !!veyra.preferences.autoResume
            onToggled: v => veyra.setPreference("autoResume", v)
        }
    }
    VRow {
        objectName: "pushfps-auto-fullscreen-row"
        visible: !quick.compact
        label: qsTr("连接后自动全屏")
        hint: qsTr("采集卡或串流画面出现时进入全屏；按 F11 或 Esc 退出")
        VSwitch {
            objectName: "pushfps-auto-fullscreen"
            checked: veyra.preferences.pushAutoFullscreen === true
            onToggled: v => veyra.setPreference("pushAutoFullscreen", v)
        }
    }

    // Smooth frame pacing: every frame waits for a refresh (V-Sync; variable refresh where the monitor
    // has it) and the output is capped at the refresh rate, so each generated frame is shown once.
    // Off: Veyra's Automatic, Present(0,0): the newest frame replaces one not shown yet (lowest
    // latency, but frame generation output can skip a frame and repeat the next).
    VRow {
        objectName: "pushfps-smooth-row"
        visible: !quick.compact
        label: qsTr("流畅帧同步")
        help: qsTr("等屏幕刷新再显示每一帧，并把输出限制在屏幕刷新率：补出来的每一帧都完整显示，玩起来和录像一样流畅。屏幕支持 G-SYNC/FreeSync 时会请求可变刷新，几乎不增加延迟")
        hint: quick.smoothPacing && quick.unevenTarget
              ? qsTr("%1 FPS 不能整除 %2 Hz：每帧显示时间不均匀。请选「匹配显示器」，或把屏幕设为 %1 的整数倍").arg(Math.round(veyra.outputCustomFps)).arg(Math.round(veyra.displayRefreshHz))
              : quick.smoothPacing ? qsTr("每一帧都完整显示；没有可变刷新时最多多等一次屏幕刷新")
                                   : qsTr("延迟最低，但补帧画面可能偶尔跳帧，直播画面比录像卡顿")
        VSwitch {
            objectName: "pushfps-smooth"
            checked: quick.smoothPacing
            onToggled: checked => quick.setSmoothPacing(checked)
        }
    }

    // G-SYNC set-up. The screen can do variable refresh and PushFPS asks for it, but the NVIDIA driver
    // has not confirmed it for this program: usually the driver only allows G-SYNC in full screen, or
    // not for PushFPS at all, and frames then stay on screen for uneven times (a 60 FPS output on a
    // 144 Hz screen: 2-3-2-3 refreshes). The button runs the driver-profile check and, when the profile
    // can be set for PushFPS alone, the repair (NvidiaVrrProfileController; a restart is needed after).
    // Field report 2026-11-18: an RTX 5090 user could not get G-SYNC until he found "Repair" under
    // Professional -> Display.
    readonly property var gsyncTools: ("nvidiaVrrProfile" in veyra) ? veyra.nvidiaVrrProfile : null
    property bool gsyncWanted: false     // the button was pressed; the check is running
    property bool gsyncChecked: false    // a check has finished since the button was pressed
    readonly property bool gsyncUnconfirmed: quick.gsyncTools !== null && veyra.hasSource === true && quick.smoothPacing
                                             && Number(veyra.vrrState || 0) === 3
    Connections {
        target: quick.gsyncTools
        ignoreUnknownSignals: true
        function onChanged() {
            if (quick.gsyncTools.busy) return
            if (quick.gsyncWanted) {
                quick.gsyncWanted = false
                quick.gsyncChecked = true
                if (quick.gsyncTools.canRepair) { quick.gsyncTools.repair(); return }
            }
            // Checked and nothing (left) to set: remembered, so the row does not come back at every
            // start on screens whose driver never reports variable refresh as active (G-SYNC Compatible).
            if (quick.gsyncChecked && !quick.gsyncTools.canRepair) veyra.setPreference("pushGsyncChecked", true)
        }
    }
    VRow {
        objectName: "pushfps-gsync-row"
        // Once checked with nothing left to set, the row goes: the driver's report is then all there is.
        visible: !quick.compact && quick.gsyncUnconfirmed && veyra.preferences.pushGsyncChecked !== true
                 && (!quick.gsyncChecked || quick.gsyncWanted || quick.gsyncTools.busy || quick.gsyncTools.canRepair)
        label: "G-SYNC"
        help: qsTr("屏幕支持可变刷新时，画面一准备好屏幕就刷新：不撕裂，每一帧显示的时间都一样长。NVIDIA 驱动要为每个程序单独允许窗口和全屏 G-SYNC")
        hint: quick.gsyncWanted || (quick.gsyncTools && quick.gsyncTools.busy) ? qsTr("正在检查驱动配置…")
            : qsTr("屏幕支持可变刷新，但驱动还没有确认对 PushFPS 生效，画面可能不如录像流畅。点「一键设置」为 PushFPS 允许窗口和全屏 G-SYNC，然后重启 PushFPS")
        hintBelow: true
        VButton {
            objectName: "pushfps-gsync-fix"
            text: qsTr("一键设置")
            enabled: quick.gsyncTools !== null && !quick.gsyncTools.busy && !quick.gsyncWanted
            onClicked: { quick.gsyncWanted = true; quick.gsyncTools.check() }
        }
    }

    VRow {
        objectName: "pushfps-low-latency-row"
        visible: !quick.compact
        label: qsTr("英伟达低延迟")
        help: qsTr("减少在显卡里排队等待的画面，从按下按键到画面反应更快。只在 NVIDIA 显卡上可用")
        hint: veyra.nvidiaLowLatencyAvailable === true ? qsTr("减少处理排队，降低输入延迟") : qsTr("需要 NVIDIA 显卡")
        VSwitch {
            objectName: "pushfps-low-latency"
            enabled: veyra.nvidiaLowLatencyAvailable === true
            opacity: enabled ? 1 : 0.4
            checked: veyra.nvidiaLowLatencyAvailable === true && (veyra.nvidiaLowLatency ?? 0) > 0
            onToggled: checked => veyra.nvidiaLowLatency = checked ? 1 : 0
        }
    }
}
