// G1.6: component smoke tests through Qt Quick Test (tests/qml/quick/tst_*.qml).
// The test source and Veyra module are resolved from the executable's staging
// directory. This prevents a staging run from silently reading the source tree.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QtQml/qqml.h>
#include <QtQuickTest/quicktest.h>
#include <QGuiApplication>
#include <QTest>
#include <QWindow>
#include <QScreen>
#include <QPixmap>

// Quick Test's keyClick targets its QQuickView host. Multi-window ownership
// tests need events delivered to the actual focused QML Window instead.
class ActiveWindowKeys : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE bool focused(QObject* window) { return QGuiApplication::focusWindow() == window; }
    Q_INVOKABLE bool savePresented(QObject* object,const QString& path) {
        auto* window=qobject_cast<QWindow*>(object);
        if(!window||!window->screen())return false;
        // Read already presented pixels, without triggering a fresh QML render.
        const auto image=window->screen()->grabWindow(window->winId());
        return !image.isNull()&&image.save(path);
    }
    Q_INVOKABLE bool press(int key) {
        auto* window = QGuiApplication::focusWindow();
        if (!window) return false;
        QTest::keyClick(window, Qt::Key(key));
        QCoreApplication::processEvents();
        return true;
    }
};

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("testWindowKeys"), new ActiveWindowKeys(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("exportTestOutputDirectory"),qEnvironmentVariable("VEYRA_EXPORT_TEST_ARTIFACTS"));
        engine->addImportPath(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qml")));
        // The registered inline NR card has the engine's creation context,
        // not the fixture item's lexical context. Supply its explicit UI-only
        // snapshot here. The separate real-player probes test the C++ bridge.
        QQmlComponent snapshot(engine);
        snapshot.setData(R"qml(import QtQml
QtObject {
    property var effectCapabilities: ({nr2:{available:true},nr3:{available:true},nr4:{available:true}})
    property var nrRuntimeChoices: [{id:"2",label:"RTX 20–50 · SF 310.9.16"},{id:"3",label:"RTX 50 · NVIDIA original"},{id:"4",label:"RX 7000 · RedDuke (experimental)"}]
    property var nrLayers: [{index:1}]
    property int nrMotionSource: 1
    property string nrAutoStatus: ""
    property var nodeTimings: ({})
    property var preferences: ({exportStopsPlayback:false})
    property real playbackRate: 1
    property real duration: 100
    property bool hasSource: true
    property bool isCapture: false
    // Off by default, as before these existed: only the low latency test turns them on.
    property bool nvidiaLowLatencyAvailable: false
    property int nvidiaLowLatency: 0
    property bool fgLowQueue: false
    property int vrrState: 0
    property bool running: false
    property string presentationStatus: ""
    // PushFPS quick setup (tst_push_quick_setup.qml): frame generation, output cap,
    // NR and presets as the bridge exposes them. Frame generation follows the
    // multiplier the way setFgMultiplier does (above 1 = on).
    property bool fgEnabled: false
    property int fgMultiplier: 1
    property var fgMultiplierChoices: [{id:"1"},{id:"2"},{id:"3"},{id:"4"}]
    // PushFPS adaptive multiplier (QmlPlayerBridge::fgAdaptive) and duplicate removal (contentRate).
    property bool fgAdaptive: false
    property bool pacingRepair: false
    property bool hudLock: false
    property int contentRate: 0
    property string version: ""
    property var nvidiaVrrProfile: null
    property int outputRateMode: 0
    property int displaySync: 2
    property bool variableRefresh: false
    property real outputCustomFps: 0
    property bool presentationOwned: false
    property real displayRefreshHz: 240
    property bool nrAnyEnabled: false
    property bool hasCaptureSession: false
    property var presets: []
    property string currentPresetName: "自定义"
    property int lastAppliedPreset: -1
    onFgMultiplierChanged: fgEnabled = fgMultiplier > 1
    onFgEnabledChanged: if (!fgEnabled && fgMultiplier > 1) fgMultiplier = 1
    function addEffect(type) { if (type === "nr") nrAnyEnabled = true }
    function setAllNrEnabled(on) { nrAnyEnabled = on }
    property int compareMode: 0
    function setNrLayerParameter(index, key, value) {
        if (!nrLayers.some(l => l.index === index)) return false
        nrLayers = nrLayers.map(l => { if (l.index !== index) return l; const c = Object.assign({}, l); c[key] = value; return c })
        return true
    }
    function applyPresetIndex(index) { lastAppliedPreset = index; return true }
    // PushFPS picture editor, sound profile and recording card (tst_push_picture_sound.qml).
    property var colourState: ({index: 0, enabled: false, exposure: 0, contrast: 0, highlights: 0, shadows: 0,
        saturation: 0, vibrance: 0, texture: 0, temperature: 0,
        curves: [[{x:0,y:0},{x:1,y:1}],[{x:0,y:0},{x:1,y:1}],[{x:0,y:0},{x:1,y:1}],[{x:0,y:0},{x:1,y:1}],[{x:0,y:0},{x:1,y:1}]]})
    property bool colorEnabled: false
    onColorEnabledChanged: { const s = Object.assign({}, colourState); s.enabled = colorEnabled; colourState = s }
    property bool colourCanUndo: false
    property int colourUndoCount: 0
    function setColourParameter(name, value) { const s = Object.assign({}, colourState); s[name] = value; colourState = s; colourCanUndo = true; return true }
    function setColourCurve(channel, points) {
        const s = Object.assign({}, colourState); const c = s.curves.slice(); c[channel] = points.map(p => ({x: p.x, y: p.y}))
        s.curves = c; colourState = s; return true
    }
    function colourUndo() { colourUndoCount++; return true }
    function soundResponseDb(preset, eq, hz) { return preset === 0 ? 0 : 3 }
    property var recordingState: ({saved: 0, active: false, path: ""})
    // `recording` (below) declares recordingChanged() as well.
    property var replayState: ({saved: 0, active: false, path: ""})
    signal replayChanged()
    // PushFPS optimiser, game presets, latency readout and sharing (src/ui/bridge/PushFps.cpp).
    property string fgBackendName: "dlss"
    property var fgBackendChoices: [{id: "dlss", label: "DLSS"}]
    property string fgProviderText: ""
    property int vfgQuality: 1
    property bool srEnabled: false
    property string captureDeviceLabel: ""
    property var exclusionState: ({enabled: false, feather: 6, regions: []})
    property var liveStats: ({running: false})
    property var benchmarkModel: null
    property int benchmarkWindows: 0
    property var lastRegions: null
    property string lastCopiedText: ""
    property string lastCopiedFile: ""
    property int discordOpened: 0
    property string lastSharedFile: ""
    property string importFileText: ""
    property string lastExportText: ""
    signal pushShareFinished(bool ok, string message)
)qml"
    // MSVC limits one string literal to about 16 KB (C2026); adjacent literals are joined.
    R"qml(    function pushLiveStats() { return liveStats }
    property int halfTests: 0
    function pushTestPushFlowHalf() { halfTests++; return true }
    function pushFlowHalfTesting() { return false }
    function pushBenchmarkBegin() { benchmarkWindows++ }
    function pushBenchmarkEnd() { return benchmarkModel ? benchmarkModel() : ({valid: false}) }
    // PushFPS ground-truth quality test (tst_push_optimize.qml). qualityModel(backend) returns the status
    // the test ends with; null: the source can't be tested.
    property var qualityModel: null
    property int qualityStarts: 0
    property int qualityCancels: 0
    property var qualityState: ({running: false, done: false, failed: false})
    function pushQualityBegin(seconds) {
        qualityStarts++
        const r = qualityModel ? qualityModel(String(fgBackendName || "")) : null
        qualityState = r ? Object.assign({running: false, done: true, failed: false, samples: 20}, r)
                         : ({running: false, done: false, failed: true, error: "unsupported source"})
        return true
    }
    function pushQualityCancel() { qualityCancels++ }
    function pushQualityStatus() { return qualityState }
    // PushFPS latency test (tst_push_latency_test.qml): what the dialog asked for; latencyState is the status it reads.
    property var latencyCalls: []
    property var latencyState: ({state: "idle"})
    property int latencyOriginalBegins: 0   // calls of the original entry point (pushLatencyTestBegin)
    function pushLatencyTestBegin(trials) { latencyOriginalBegins++; return pushLatencyTestBeginWith(trials, "ps") }
    function pushLatencyTestBeginWith(trials, button) {
        latencyCalls = latencyCalls.concat([{trials: trials, button: button}])
        latencyState = ({state: "running", trial: 0, trials: trials})
        return true
    }
    function pushLatencyTestCancel() { latencyState = ({state: "idle"}) }
    function pushLatencyTestStatus() { return latencyState }
    // PushFPS SmartLink row (tst_push_smartlink.qml): what pushSmartLink() reports (src/ui/bridge/PushSmartLink.cpp).
    property var smartLinkState: ({available: true, active: false, mode: "off", profile: "unknown", advice: 0})
    function pushSmartLink() { return smartLinkState }
    function pushSetExclusionRegions(regions, feather) {
        lastRegions = regions
        exclusionState = ({enabled: regions.length > 0, feather: feather, regions: regions})
        return true
    }
    function pushCopyText(text) { lastCopiedText = text; return true }
    function pushCopyFile(path) { lastCopiedFile = path; return true }
    function pushOpenDiscord() { discordOpened++; return true }
    function pushShareToDiscord(path) { lastSharedFile = path; return true }
    property bool anrInstalled: false
    property int anrInstallCalls: 0
    property string lastOpenedLink: ""
    function pushAnrState() { return { installed: anrInstalled, folder: "C:/PushFPS/runtime/experimental/anr", name: "Apex Neural Rendering (ANR)", maker: "Apex Compute", url: "anr-url" } }
    function pushInstallAnr() { anrInstallCalls++; anrInstalled = true; return true }
    function pushOpenAnrFolder() { return true }
    function pushLinkUrl(key) { return ["repo", "veyra", "anr", "anrReleases", "apex"].indexOf(key) >= 0 ? "link-" + key : "" }
    function pushOpenLink(key) { lastOpenedLink = key; return pushLinkUrl(key).length > 0 }
    function pushImportTextFile(title) { return importFileText }
    function pushExportTextFile(title, name, text) { lastExportText = text; return true }
    // PushFPS HUD detection, comparison clips and the wordmark (tst_push_hud.qml).
    property string appName: "PushFPS"
    property bool recording: false
    property var hudState: ({running: false, done: false, failed: false, taken: 0, target: 0, regions: [], preview: "", tooStill: false, motion: 0})
    property int hudStarts: 0
    property int hudLastSamples: 0
    property int hudCancels: 0
    property int compareStarts: 0
    property int compareSeconds: 0
    property int compareLayout: -1
    property int recordingStops: 0
    function pushStartHudDetection(samples) {
        if (!hasSource) return false
        hudStarts++; hudLastSamples = samples
        hudState = ({running: true, done: false, failed: false, taken: 0, target: samples, regions: [], preview: "", tooStill: false, motion: 0})
        return true
    }
    function pushCancelHudDetection() { hudCancels++; hudState = ({running: false, done: false, failed: false, taken: 0, target: 0, regions: [], preview: ""}) }
    function pushHudDetection() { return hudState }
    function pushStartCompareClip(seconds, layout) { compareStarts++; compareSeconds = seconds; compareLayout = layout === undefined ? 0 : layout; return true }
    // PushFPS going live (src/ui/bridge/PushStream.cpp, tst_push_stream.qml).
    property var streamKeys: ({})
    property var streamState: ({active: false, live: false, outputs: []})
    property int streamStarts: 0
    property int streamStops: 0
    function pushStreamPlatforms() {
        return [
            {id: "twitch", name: "Twitch", servers: [{name: "Auto", url: "twitch-auto"}], maxVideoKbps: 6000, maxAudioKbps: 320, needsRtmps: false, keyPage: "dashboard.twitch.tv"},
            {id: "youtube", name: "YouTube", servers: [{name: "Primary (RTMPS)", url: "youtube-primary"}], maxVideoKbps: 51000, maxAudioKbps: 160, needsRtmps: false, keyPage: "studio.youtube.com"},
            {id: "kick", name: "Kick", servers: [{name: "Default", url: "kick-default"}], maxVideoKbps: 8000, maxAudioKbps: 160, needsRtmps: true, keyPage: "kick.com"},
            {id: "tiktok", name: "TikTok", servers: [], maxVideoKbps: 6000, maxAudioKbps: 160, needsRtmps: false, keyPage: "tiktok.com"},
            {id: "custom", name: "Custom", servers: [], maxVideoKbps: 50000, maxAudioKbps: 320, needsRtmps: false, keyPage: ""}
        ]
    }
    function pushSetStreamKey(id, key) {
        if (String(key).trim().length === 0) return false
        const k = Object.assign({}, streamKeys); k[id] = true; streamKeys = k; return true
    }
    function pushHasStreamKey(id) { return streamKeys[id] === true }
    function pushForgetStreamKey(id) { const k = Object.assign({}, streamKeys); delete k[id]; streamKeys = k }
)qml"
    R"qml(    function pushStartStream() {
        let list = []
        try { list = JSON.parse(String(preferences.streamTargets || "[]")) } catch (e) {}
        const on = list.filter(t => t.enabled !== false && streamKeys[t.id] === true)
        if (!hasSource || on.length === 0) return false
        streamStarts++
        streamState = ({active: true, live: true, seconds: 5, outputs: on.map(t => ({name: t.name || t.platform, state: "live", kbps: 6100, framesDropped: 0, reconnects: 0, error: ""}))})
        return true
    }
    function pushStopStream() { streamStops++; streamState = ({active: false, live: false, outputs: []}) }
    function pushStreamStatus() { return streamState }
    // PushFPS streaming mode (tst_push_studio.qml): mixer meters, recording and monitor controls.
    property var studioLevels: ({gamePeakDb: -100, gameRmsDb: -100, micPeakDb: -100, micRmsDb: -100, micOpen: false, micDevice: "", micError: "",
                                  desktopPeakDb: -100, desktopRmsDb: -100, desktopOpen: false, desktopError: "", auxPeakDb: -100, auxRmsDb: -100, auxOpen: false, auxDevice: "", auxError: ""})
    property bool studioMeters: false
    property int studioMeterCalls: 0
    function pushStudioLevels() { return studioLevels }
    function pushSetStudioMeters(on) { studioMeters = on; studioMeterCalls++ }
    property int microphoneRefreshes: 0
    function refreshMicrophones() { microphoneRefreshes++ }
    property var microphoneDevices: []
    readonly property bool recordingMicrophone: preferences.recordMic === true
    readonly property string recordingMicrophoneId: String(preferences.recordMicDevice || "")
    readonly property bool recordingAudio: preferences.recordAudio !== false
    property string currentPage: ""
    property real volume: 1
    property bool muted: false
    property int recordingToggles: 0
    function toggleRecording(source) { recordingToggles++; recording = !recording }
    readonly property bool replayEnabled: preferences.replayEnabled === true
    property int replaySeconds: 60
    property int replaySaves: 0
    function saveReplay(source) { replaySaves++ }
    property int screenshots: 0
    function takeScreenshot() { screenshots++ }
    property string sourceTitle: ""
    property string sourceKind: ""
    // PushFPS streaming mode scenes and video playback mode (tst_push_studio.qml, tst_push_watch.qml).
    property var lastScene: null
    property int sceneCalls: 0
    function pushSetStreamScene(scene) { lastScene = scene; sceneCalls++; return true }
    property string pickedImage: ""
    function pushPickImage() { return pickedImage }
    // Streaming mode sources (webcam, window, video file).
    property var cameras: [{id: "cam1", name: "USB Webcam"}]
    property var windowList: [{id: "4242", name: "Chat - Browser"}]
    property string pickedMedia: ""
    property var lastSources: []
    property int sourceCalls: 0
    property var sourceStatus: []
    function pushCameras() { return cameras }
    function pushWindows() { return windowList }
    function pushPickMedia() { return pickedMedia }
    function pushSetSceneSources(list) { lastSources = list; sourceCalls++ }
    function pushSourceStatus() { return sourceStatus }
    function pushSourcePreview(id) { return "" }
    // PushFPS Frame Input API (tst_push_frame_input.qml).
    property var frameInputSenders: []
    property string frameInputStarted: ""
    function pushFrameInputSenders() { return frameInputSenders }
    function pushStartFrameInput(channel) {
        if (!frameInputSenders.some(s => s.channel === channel && s.alive)) return false
        frameInputStarted = channel; return true
    }
    property var screenOptions: ({kind: 0, passthrough: false, method: 0, fps: 0, cursor: true, left: 0, top: 0, right: 0, bottom: 0, fill: false})
    property var screenTargets: [{id: "w1", label: "Browser - YouTube"}]
    property string screenTargetId: "w1"
    property string screenTargetLabel: "Browser - YouTube"
    function setScreenOption(key, value) { const o = Object.assign({}, screenOptions); o[key] = value; screenOptions = o; return true }
    property int screenStarts: 0
    function startScreenCapture() { screenStarts++; hasSource = true; return true }
    property int targetRefreshes: 0
    function refreshCaptureTargets() { targetRefreshes++ }
    property int fileDialogs: 0
    function openFileDialog() { fileDialogs++ }
    property int playbackStops: 0
    function stopPlayback() { playbackStops++; hasSource = false }
    property bool videoHdr: false
    function stopRecording(source) { recordingStops++; recording = false }
    // PushFPS super-resolution editor (SrStageEditor): ids as QmlPlayerBridge::srBackendChoices.
    property int videoSrQuality: 2
    property int dualSrQuality: 2
    property bool dualSrEnabled: false
    property int dualSrTargetIndex: 2
    property int srTargetIndex: 2
    property int srMotionSource: 1
    property int srPrescale: 0
    property int srOutput: 0
    property string srStageSummary: ""
    property string srStageNote: ""
    property var srBackendChoices: [{id: "0", label: "DLSS SR"}, {id: "rtx", label: "RTX"}, {id: "5", label: "FSR 3.1"},
                                    {id: "6", label: "NVIDIA Image Scaling (NIS)"}]
    function srAlgorithmOf(q) { return q >= 1 && q <= 4 ? "rtx" : String(q) }
    function srTierOf(q) { return q >= 1 && q <= 4 ? q : 0 }
    function srQualityFor(algorithm, tier) { return algorithm === "rtx" ? (tier >= 1 && tier <= 4 ? tier : 2) : Number(algorithm) }
    function srTierChoices(q) { return [{id: "1", label: "1"}, {id: "2", label: "2"}, {id: "3", label: "3"}, {id: "4", label: "4"}] }
    signal navigate(string page)
    property string lastOpenedPath: ""
    property var lastExportFiles: []
    property bool recordingFolderOpened: false
    function openPath(path) { lastOpenedPath = path }
    function addExportFiles(paths) { lastExportFiles = paths }
    function openRecordingDirectory() { recordingFolderOpened = true }
    signal settingsChanged()
    signal chainChanged()
    signal snapshotChanged()
    function nrAutoSelectionReason(index) { return "Auto is unavailable in this four-layer UI fixture" }
    function effectAvailability(id) { return effectCapabilities[id] || {available:true, reason:""} }
    function resetNrLayer(index) {}
    // PushFPS keys the real validator (src/ui/bridge/Preferences.cpp setPreference) accepts; any other
    // push* key is refused as there. The performance-mode and calibration JSON keys must hold a JSON object or
    // array (older tests still clear other JSON keys with "", which the real validator refuses).
    property var pushKeys: ["pushAlwaysOnTop","pushAutoFullscreen","pushBenchmark","pushCalibrationApplied","pushCalibrationKept","pushCalibrationRestore","pushClean","pushCleanHud","pushCleanParts","pushCompareLayout","pushCompareSeconds","pushConsole","pushDiscordWebhook","pushFlowHalf","pushFlowHalfProbe","pushGameFps","pushGamePresets","pushGsyncChecked","pushLanProfile","pushLatencyOverlay","pushLowestLatency","pushMode","pushModeAuto","pushModeRestore","pushModeSwitches","pushOptimizeGoal","pushOptimizeRestore","pushOptimizeSkip","pushProfile","pushProfileRestore","pushProfileSettings","pushProfiles","pushSmartLink","pushSmoothPacing","pushSmoothPacing2","pushStatsOverlay","pushSunset","pushTheme","pushWelcomeChecked","pushWelcomeDone"]
    property var pushJsonKeys: ["pushModeRestore","pushCalibrationRestore","pushCalibrationApplied","pushProfileRestore","pushProfileSettings"]
    function setPreference(key, value) {
        if (String(key).indexOf("push") === 0) {
            if (pushKeys.indexOf(key) < 0) return false
            if (key === "pushSmartLink" && [0, 1, 2].indexOf(Number(value)) < 0) return false   // range {0, 2} as Preferences.cpp
            if (key === "pushClean" && !(Number(value) >= 0 && Number(value) <= 100)) return false   // range {0, 100} as Preferences.cpp
            if (key === "pushCleanParts" && !(Number(value) >= 0 && Number(value) <= 31)) return false   // range {0, 31} as Preferences.cpp
            if (key === "pushProfile" && ["none","original","clean-1440","enhanced-1440","max-fidelity","ultra-low-latency"].indexOf(String(value)) < 0) return false
            if (pushJsonKeys.indexOf(key) >= 0) {
                let doc = null
                try { doc = JSON.parse(String(value)) } catch (e) { return false }
                if (doc === null || typeof doc !== "object") return false
            }
        }
        const copy = Object.assign({}, preferences)
        copy[key] = value
        preferences = copy
        return true
    }
    function logUi(category, text) {}
    function observeHdrCoverage(on) {}
})qml", QUrl());
        auto* model = snapshot.create(engine->rootContext());
        if (!model) qFatal("Cannot create explicit component snapshot: %s", qPrintable(snapshot.errorString()));
        model->setParent(engine);
        engine->rootContext()->setContextProperty(QStringLiteral("veyra"), model);
        qmlRegisterType(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qml/Veyra/NrLayerEditor.qml"))), "VeyraTest", 1, 0, "NrLayerEditor");
        // ProPage is private to the production shell's directory, not exported
        // by Veyra/qmldir. Register it only here to test its real inline controls.
        qmlRegisterType(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qml/Veyra/ProPage.qml"))), "VeyraTest", 1, 0, "ProPage");
        qmlRegisterType(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qml/Veyra/Ps5SourceDialog.qml"))), "VeyraTest", 1, 0, "Ps5SourceDialog");
        qmlRegisterType(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qml/Veyra/ExportPage.qml"))), "VeyraTest", 1, 0, "ExportPage");
        qmlRegisterType(QUrl::fromLocalFile(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qml/Veyra/NvidiaLowLatencyEditor.qml"))), "VeyraTest", 1, 0, "NvidiaLowLatencyEditor");
    }
};

int main(int argc, char** argv) {
    const QString applicationDirectory = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    const QString sourceDirectory = QDir(applicationDirectory).filePath(QStringLiteral("qml-tests"));
    if (!QFileInfo::exists(QDir(sourceDirectory).filePath(QStringLiteral("tst_components.qml")))) {
        fprintf(stderr, "missing staged Qt Quick Test source directory: %s\n", qPrintable(sourceDirectory));
        return 2;
    }
    const QByteArray sourceDirectoryBytes = sourceDirectory.toLocal8Bit();
    Setup setup;
    return quick_test_main_with_setup(argc, argv, "veyra_qml_quick_tests", sourceDirectoryBytes.constData(), &setup);
}

#include "QuickSmokeTests.moc"
