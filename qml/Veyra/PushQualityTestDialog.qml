// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS ground-truth quality test for the current settings (dialog key "qualitytest"; Professional
// mode, streaming mode and the quick setup open it). Frame generation must be on. For a few seconds
// every second real frame is withheld, frame generation makes that moment and the result is compared
// with the original (engine/PushQualityProbe.h): PSNR, pixel errors, HUD boxes, a plain blend of the
// neighbours, real frames through the same pipeline, and whether generated frames came on time.
// Optimise for my PC runs the same test for every method.
// Stream "ownfg" (the owner's comparison at identical settings): each finished test is kept per generator (pref
// pushQualityByGenerator: score, PSNR, gain over a blend, on-time share, the settings it ran with), and the dialog shows
// every generator tested in one table, so NVIDIA VFG and PushWarp can be compared by running the test once with each.
// Nothing runs without his click; the test withholds real frames while it runs.
import QtQuick
import QtQuick.Layouts

DialogLayer {
    id: qt
    objectName: "pushfps-quality-test"
    key: "qualitytest"
    glyph: "activity"
    title: qsTr("补帧画质测试")
    sub: qsTr("每隔一帧扣下真实画面，让补帧重新生成，再和原画面比较。测量的是你自己的游戏和当前设置")
    dialogWidth: 620

    property var st: ({})
    property real seconds: 6
    readonly property bool running: st.running === true
    function begin() { if (veyra.pushQualityBegin(qt.seconds)) { qt.st = veyra.pushQualityStatus() || {}; poll.start() } }
    Timer { id: poll; interval: 150; repeat: true; onTriggered: { qt.st = veyra.pushQualityStatus() || {}; if (!qt.st.running) { stop(); qt.keepResult() } } }
    // Stream "ownfg": the result per generator, for the comparison table.
    readonly property var byGenerator: { try { return JSON.parse(String(veyra.preferences.pushQualityByGenerator || "{}")) } catch (e) { return ({}) } }
    function keepResult() {
        if (qt.st.done !== true) return
        const all = Object.assign({}, qt.byGenerator)
        all[String(veyra.fgBackendName || "?")] = { score: Number(qt.st.score || 0), psnr: Number(qt.st.psnr || 0), gain: Number(qt.st.gainOverBlend || 0),
                                                    onTime: Number(qt.st.onTime || 0), multiplier: Number(veyra.fgMultiplier || 0), at: Date.now() }
        veyra.setPreference("pushQualityByGenerator", JSON.stringify(all))
    }
    function generatorLabel(id) { return id === "vfg" ? "NVIDIA VFG" : id === "pushwarp" ? "PushWarp" : id === "pushflow" ? "PushFlow" : String(id).toUpperCase() }
    readonly property var compared: Object.keys(qt.byGenerator).sort()
    onShownChanged: if (!shown && running) { veyra.pushQualityCancel(); poll.stop() }

    actions: running ? [{ label: qsTr("取消") }] : [{ label: qsTr("关闭") }, { label: qsTr("开始测试"), primary: true }]
    onActionTriggered: label => {
        if (label === qsTr("开始测试")) qt.begin()
        else if (label === qsTr("取消")) { veyra.pushQualityCancel(); poll.stop(); qt.st = {} }
        else qt.dialogHost.close()
    }

    function db(v) { return Number(v || 0).toFixed(1) + " dB" }
    DialogGroup {
        VRow {
            visible: !veyra.fgEnabled
            label: qsTr("请先打开补帧")
            hint: qsTr("测试比较补帧生成的画面，所以需要补帧在运行（2 倍或以上）")
        }
        VRow {
            visible: qt.running
            objectName: "quality-test-progress"
            label: qsTr("正在测试…")
            hint: qsTr("已比较 %1 / %2 帧；请让游戏画面保持运动").arg(Number(qt.st.samples || 0)).arg(Number(qt.st.target || 0))
        }
        VRow {
            visible: qt.st.failed === true
            label: qsTr("测试没有完成")
            hint: String(qt.st.error || "")
        }
        VRow {
            visible: qt.st.done === true
            objectName: "quality-test-score"
            label: qsTr("画质分数")
            value: Number(qt.st.score || 0).toFixed(0) + " / 100"
            hint: qsTr("综合生成帧的准确度、相对简单混合的提升、HUD 区域和准时程度")
        }
        VRow {
            visible: qt.st.done === true
            label: qsTr("生成帧准确度")
            value: qt.db(qt.st.psnr)
            hint: qsTr("PSNR，越高越接近被扣下的真实画面；同一流程下的真实帧为 %1").arg(qt.db(qt.st.realPsnr))
        }
        VRow {
            visible: qt.st.done === true
            label: qsTr("相比简单混合")
            value: (Number(qt.st.gainOverBlend || 0) >= 0 ? "+" : "") + qt.db(qt.st.gainOverBlend)
            hint: qsTr("把前后两帧各取一半的结果为 %1；补帧应该比它好").arg(qt.db(qt.st.blendPsnr))
        }
        VRow {
            visible: qt.st.done === true
            label: qsTr("像素误差")
            value: (Number(qt.st.badShare || 0) * 100).toFixed(1) + "%"
            hint: qsTr("明显不同的像素所占比例（亮度差超过 10/255）")
        }
        VRow {
            visible: qt.st.done === true
            label: qsTr("HUD 区域")
            value: Number(qt.st.hudSamples || 0) > 0 ? qt.db(qt.st.hudPsnr) : "—"
            hint: Number(qt.st.hudSamples || 0) > 0 ? qsTr("在 HUD 保护的方框里测量；HUD 拖影最容易在这里出现")
                                                     : qsTr("没有设置 HUD 方框：在 HUD 保护里识别或画出后会一并测量")
        }
        VRow {
            visible: qt.st.done === true
            label: qsTr("准时的生成帧")
            value: (Number(qt.st.onTime || 0) * 100).toFixed(0) + "%"
            hint: qsTr("与上一帧的间隔偏差不超过半帧")
        }
        VRow {
            objectName: "quality-test-compare"
            visible: qt.compared.length > 0
            label: qsTr("按补帧方式比较")
            hint: qsTr("每种补帧方式最后一次测试的结果（同一游戏、同一设置时才可比较）：分数 · 准确度 · 相比简单混合 · 准时。换一种补帧方式再测一次，就会出现在这里")
            hintBelow: true
            Text {
                objectName: "quality-test-compare-table"
                text: qt.compared.map(id => {
                    const r = qt.byGenerator[id]
                    return qt.generatorLabel(id) + " " + Number(r.multiplier || 0) + "X：" + Number(r.score || 0).toFixed(0) + " · " + qt.db(r.psnr) + " · " +
                           (Number(r.gain || 0) >= 0 ? "+" : "") + qt.db(r.gain) + " · " + (Number(r.onTime || 0) * 100).toFixed(0) + "%"
                }).join("\n")
                color: Theme.t1; font.family: Theme.fontUi; font.pixelSize: Theme.fsHint
            }
        }
        VRow {
            visible: !qt.running
            label: qsTr("测试时长")
            VSeg {
                objectName: "quality-test-seconds"
                options: [{ id: "4", label: qsTr("%1 秒").arg(4) }, { id: "6", label: qsTr("%1 秒").arg(6) }, { id: "10", label: qsTr("%1 秒").arg(10) }]
                current: String(qt.seconds)
                onPicked: id => qt.seconds = Number(id)
            }
        }
    }
    Text {
        Layout.fillWidth: true
        Layout.topMargin: 8
        text: qsTr("测试时补帧要从一半的帧率生成，比平时更难，所以分数偏保守。需要采集卡或画面输入接口；硬件解码的串流无法测试")
        color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: 11
        wrapMode: Text.WordWrap
    }
}
