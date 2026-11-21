// 首页, rebuilt from the design (pages-a.js PAGES.home + pages.css .home).
//
// The design is: a large logo, a two-line greeting, a 4-up grid of source cards
// (164x124 each), a "continue last capture" row, and a row of recent chips.
// The logo in the dock returns here; there is no other home affordance.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

VPage {
    id: root
    signal requestPage(string page)
    // Opens 画面与声音 and scrolls it into view (the screenshot run's `--tab tuning`).
    function showTuning() {
        tuning.open = true
        tuningScroll.restart()
    }
    Timer {
        id: tuningScroll
        interval: 450
        onTriggered: content.contentY = Math.max(0, Math.min(tuning.mapToItem(column, 0, 0).y - 24, content.contentHeight - content.height))
    }
    // Veyra 2.1.9's startup intro hooks (Main.qml); PushFPS has no intro, so nothing is held.
    property bool introHold: false
    // Space kept free under a pinned dock (Main.qml), so it never covers the logo.
    property real topInset: 0
    readonly property Item wordmarkItem: root.sunset ? sunsetLogo : null
    // 设置 → 外观 → 日落之城 (pushSunset, unset = on): the logo filled with a neon sunset city.
    readonly property bool sunset: veyra.preferences.pushSunset !== false

    // PushFPS home, laid out for the console player: the capture card is the main
    // action, the other sources sit under it as smaller cards, then the quick setup,
    // then the lesser actions on one line. Every source and action of the design's
    // home is still here; the content scrolls when the window is short.
    Flickable {
        id: content
        anchors.fill: parent
        anchors.bottomMargin: 44
        readonly property real columnWidthMax: 600
        readonly property real columnWidth: Math.min(columnWidthMax, width - 48)
        contentWidth: width
        contentHeight: Math.max(height, column.implicitHeight + 48 + root.topInset)
        interactive: contentHeight > height + 1
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: VScrollBar {}

        // Which sources this build includes; the cards themselves are a fixed list (see cardSub).
        QtObject {
            id: srcGrid
            function available(act) {
                if (act === "ps5") return !!veyra.ps5 && veyra.ps5.available === true
                if (act === "moonlight") return !!veyra.moonlight
                if (act === "xbox") return !!veyra.xbox
                return true
            }
            // Subtitles name what is really there: the device of the last capture
            // session and the saved PS5 host; generic wording when there is none.
            // Only the subtitle follows the live state. The cards themselves are a fixed
            // list: when the whole array was one binding, every Xbox / PC state change
            // (polling after sign-in) destroyed and rebuilt all six cards, and a click
            // landing during the rebuild was lost (field report 2026-10-02).
            function cardSub(act) {
                switch (act) {
                case "file": return qsTr("MP4 · MKV · 图片")
                case "capture": return veyra.hasCaptureSession ? veyra.captureSessionSummary.split(" · ")[0] : qsTr("HDMI 采集设备")
                case "ps5": return veyra.remotePlayHost.length > 0 ? qsTr("已保存主机 ") + veyra.remotePlayHost : qsTr("局域网串流")
                case "moonlight": return veyra.moonlight && veyra.moonlight.state.lastLabel ? veyra.moonlight.state.lastLabel : qsTr("Sunshine 主机")
                case "xbox": return veyra.xbox && veyra.xbox.state.lastLabel ? veyra.xbox.state.lastLabel : qsTr("账号登录 · 实验")
                case "frameinput": return qsTr("画面输入接口")
                default: return qsTr("窗口或显示器")
                }
            }
        }

        ColumnLayout {
            id: column
            x: Math.round((content.width - width) / 2)
            y: Math.max(24 + root.topInset, Math.round((content.height - implicitHeight) / 2))
            width: content.columnWidth
            spacing: 0

            // Sunset City logo (scripts/brand/make_sunset.py; its glow is baked into the image, 70/1340
            // of the width on each side). The startup intro glides into exactly this rect.
            Image {
                id: sunsetLogo
                objectName: "home-sunset-logo"
                visible: root.sunset
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: -6
                Layout.bottomMargin: -22
                Layout.preferredWidth: Math.min(470, content.columnWidth)
                Layout.preferredHeight: Layout.preferredWidth * 550 / 1340
                source: "sunset-logo.png"
                sourceSize.width: 940
                fillMode: Image.PreserveAspectFit
                smooth: true
                opacity: root.introHold ? 0 : 1
            }
            Item {
                id: mark
                visible: !root.sunset
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: -40
                Layout.bottomMargin: -40
                implicitWidth: logoImg.implicitWidth + 80
                implicitHeight: logoImg.implicitHeight + 80
                property real breath: 0
                scale: 1 + 0.02 * breath
                // Only while the home page shows: running behind other pages it redrew the whole
                // window every vsync during playback (~300 presents/s on a 320 Hz screen), which
                // cost GPU time and kept OBS game capture on the UI instead of the video.
                SequentialAnimation on breath {
                    running: !Theme.reduced && mark.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: 1; duration: Theme.d(2250); easing.type: Easing.InOutSine }
                    NumberAnimation { to: 0; duration: Theme.d(2250); easing.type: Easing.InOutSine }
                }
                Image {
                    id: logoImg
                    anchors.centerIn: parent
                    source: "logo.png"
                    sourceSize.width: 88
                    fillMode: Image.PreserveAspectFit
                }
                // A sibling MultiEffect, not layer.effect: Qt recreates a layer's effect item on
                // a screen DPI change while it walks the parent's children, and the walk then touched
                // the deleted item (crash moving the window to a 200 % monitor, field 2026-10-01).
                MultiEffect {
                    source: logoImg
                    anchors.fill: logoImg
                    shadowEnabled: true
                    shadowColor: "#FFFFFF"
                    shadowHorizontalOffset: 0
                    shadowVerticalOffset: 0
                    blurMax: 64
                    shadowBlur: 0.625 + 0.3125 * mark.breath
                    shadowOpacity: 0.35 + 0.2 * mark.breath
                }
            }


            ColumnLayout {
                id: hello
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 14
                spacing: 6
                PushWordmark {
                    visible: !root.sunset
                    Layout.alignment: Qt.AlignHCenter
                    pixelSize: Theme.fsH1 + 8
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("主机画面经采集卡接入，由电脑补帧到 60 或 120 FPS")
                    color: Theme.t2
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                }
            }

            Rectangle {
                id: resume
                objectName: "home-resume"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 28
                // The last source actually used: capture card, PS5 or screen capture.
                readonly property var last: veyra.lastSource
                visible: last.kind !== undefined
                implicitWidth: content.columnWidth
                implicitHeight: 54
                radius: 14
                border.width: 1
                border.color: Theme.stroke
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.accentSoft }
                    GradientStop { position: 1.0; color: Theme.ink(0.03) }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 10
                    spacing: 14
                    Rectangle {
                        implicitWidth: 34; implicitHeight: 34; radius: 10
                        color: Theme.accentSoft
                        VIcon {
                            anchors.centerIn: parent
                            name: resume.last.kind === "screen" ? "monitor" : resume.last.kind === "capture" ? "video" : "gamepad"
                            color: Theme.accent
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: qsTr("继续上次")
                            color: Theme.t1
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                            font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: resume.last.summary || ""
                            color: Theme.t2
                            font.family: Theme.fontUi
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                    VButton {
                        text: qsTr("开始")
                        iconName: "play"
                        primary: true
                        onClicked: veyra.resumeLastSource()
                    }
                }
            }


            // The main action: a wide capture card card with its own button.
            Rectangle {
                id: captureCard
                objectName: "home-capture"
                Layout.fillWidth: true
                Layout.topMargin: resume.visible ? 12 : 28
                implicitHeight: 76
                radius: Theme.rCard
                color: captureHover.hovered ? Theme.card2 : Theme.card
                border.width: 1
                border.color: captureHover.hovered ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.45) : Theme.stroke2
                Behavior on color { ColorAnimation { duration: Theme.d(Theme.durFast) } }
                Behavior on border.color { ColorAnimation { duration: Theme.d(Theme.durNormal) } }
                scale: captureTap.pressed ? 0.985 : 1.0
                Behavior on scale { NumberAnimation { duration: Theme.d(500); easing.bezierCurve: Theme.spring } }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 14
                    Rectangle {
                        implicitWidth: 44; implicitHeight: 44; radius: 12
                        color: Theme.accentSoft
                        VIcon { anchors.centerIn: parent; name: "video"; size: 20; color: Theme.accent }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        RowLayout {
                            spacing: 8
                            Text {
                                text: qsTr("采集卡")
                                color: Theme.t1
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsH3
                                font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
                            }
                            // PS5 手柄控制 is switched on for the capture card.
                            VTag {
                                visible: veyra.ps5Control.enabled === true
                                text: qsTr("PS5 手柄")
                                kind: "acc"
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: srcGrid.cardSub("capture")
                            color: Theme.t3
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsSmall
                            elide: Text.ElideRight
                        }
                    }
                    VButton {
                        objectName: "home-capture-connect"
                        text: qsTr("连接")
                        iconName: "play"
                        primary: true
                        onClicked: captureCard.open()
                    }
                }
                function open() {
                    veyra.logUi("ui-source", "home card act=capture")
                    veyra.openCaptureDialog()
                }
                HoverHandler { id: captureHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { id: captureTap; onTapped: captureCard.open() }
            }

            // The other sources: equal cards in one row (two rows when narrow).
            GridLayout {
                id: otherSources
                Layout.fillWidth: true
                Layout.topMargin: 10
                readonly property var shown: [
                    { glyph: "folder", title: qsTr("打开视频"), act: "file" },
                    { glyph: "monitor", title: qsTr("屏幕捕获"), act: "screen" },
                    { glyph: "gamepad", title: qsTr("PS5 串流"), act: "ps5" },
                    { glyph: "cast", title: qsTr("PC 串流"), act: "moonlight" },
                    { glyph: "gamepad", title: qsTr("Xbox 串流"), act: "xbox" },
                    { glyph: "import", title: qsTr("其他应用"), act: "frameinput" }
                ]
                readonly property int shownCount: shown.filter(c => srcGrid.available(c.act)).length
                columns: width >= 140 * shownCount + 10 * (shownCount - 1) ? shownCount : Math.ceil(shownCount / 2)
                columnSpacing: 10
                rowSpacing: 10
                Repeater {
                    model: otherSources.shown
                    delegate: Item {
                        id: slot
                        required property var modelData
                        required property int index
                        visible: srcGrid.available(modelData.act)
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 62
                        VRise { page: root; target: slot; d: 3 + slot.index }
                        Rectangle {
                            id: srcCard
                            width: parent.width
                            height: parent.height
                            radius: 12
                            color: cardHover.hovered ? Theme.card2 : Theme.card
                            border.width: 1
                            border.color: cardHover.hovered ? Theme.stroke2 : Theme.stroke
                            y: cardHover.hovered ? -2 : 0
                            Behavior on y { NumberAnimation { duration: Theme.d(500); easing.bezierCurve: Theme.spring } }
                            Behavior on color { ColorAnimation { duration: Theme.d(Theme.durFast) } }
                            scale: cardTap.pressed ? 0.97 : 1.0
                            Behavior on scale { NumberAnimation { duration: Theme.d(500); easing.bezierCurve: Theme.spring } }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 10
                                spacing: 10
                                Rectangle {
                                    implicitWidth: 32; implicitHeight: 32; radius: 9
                                    color: Theme.ink(0.06)
                                    VIcon { anchors.centerIn: parent; name: modelData.glyph; size: 16 }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.title
                                        color: Theme.t1
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fsBody
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: srcGrid.cardSub(modelData.act)
                                        color: Theme.t3
                                        font.family: Theme.fontUi
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                id: cardTap
                                onTapped: {
                                    veyra.logUi("ui-source", "home card act=" + modelData.act)
                                    switch (modelData.act) {
                                    case "file": veyra.openFileDialog(); break
                                    case "ps5": veyra.openPs5Dialog(); break
                                    case "moonlight": veyra.openMoonlightDialog(); break
                                    case "xbox": veyra.openXboxDialog(); break
                                    case "screen": veyra.openScreenCaptureDialog(); break
                                    case "frameinput": veyra.navigate("frameinput"); break
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // PushFPS: once, until dismissed: what was found on this PC and a suggestion for a first test.
            PushWelcomeCard { Layout.topMargin: 22 }

            // PushFPS: pick the frame rate before connecting; the same settings as
            // Professional → 补帧, in their short form.
            PushQuickSetup {
                id: quickSetup
                objectName: "home-pushfps-quick"
                compact: true
                showSetupTip: true
                Layout.fillWidth: true
                Layout.topMargin: 22
            }

            // PushFPS picture and sound: the everyday colour controls with presets and
            // the sound profile, folded away until wanted.
            VAccordion {
                id: tuning
                objectName: "home-picture-sound"
                Layout.fillWidth: true
                Layout.topMargin: 12
                glyph: "sliders"
                title: qsTr("画面与声音")
                summary: {
                    const p = homePicture.presets.find(x => x.id === homePicture.currentPreset)
                    return (p ? p.label : qsTr("已自定义")) + " · " + homeSound.presets[homeSound.preset].label
                }
                enabledSwitch: false
                PushPicture { id: homePicture; Layout.fillWidth: true }
                Rectangle { Layout.fillWidth: true; Layout.topMargin: 8; Layout.bottomMargin: 2; implicitHeight: 1; color: Theme.stroke }
                PushSoundProfile { id: homeSound; Layout.fillWidth: true; compact: true }
            }

            // The lesser actions on one line.
            RowLayout {
                id: moreActions
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 14
                spacing: 6
                VButton {
                    objectName: "home-playlist"
                    ghost: true
                    text: qsTr("播放列表 · 导入文件夹"); iconName: "list"
                    onClicked: veyra.showPlaylist()
                }
                Rectangle { implicitWidth: 1; implicitHeight: 14; color: Theme.stroke2 }
                VButton {
                    objectName: "home-network"
                    ghost: true
                    text: qsTr("播放地址…")
                    iconName: "cast"
                    onClicked: veyra.openNetworkDialog()
                }
            }

            Flow {
                id: recent
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 14
                Layout.preferredWidth: content.columnWidth
                visible: veyra.recentFiles.length > 0
                spacing: 8
                Text {
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("最近")
                    color: Theme.t3
                    font.family: Theme.fontUi
                    font.pixelSize: 12
                }
                Repeater {
                    model: veyra.recentFiles
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        readonly property bool isImage: /\.(png|jpe?g|bmp|webp|tiff?)$/i.test(modelData.path)
                        width: chipRow.implicitWidth + 22
                        height: 28
                        radius: 99
                        opacity: modelData.exists ? 1 : 0.45
                        color: chipHover.hovered && modelData.exists ? Theme.ink(0.08) : Theme.ink(0.04)
                        border.width: 1
                        border.color: Theme.stroke
                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 7
                            VIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: chip.isImage ? "image" : "film"
                                size: 14
                                color: chipText.color
                            }
                            Text {
                                id: chipText
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.label
                                color: chipHover.hovered && modelData.exists ? Theme.t1 : Theme.t2
                                font.family: Theme.fontUi
                                font.pixelSize: 12
                            }
                        }
                        ToolTip.visible: chipHover.hovered && !modelData.exists
                        ToolTip.text: qsTr("文件已不存在")
                        HoverHandler { id: chipHover; cursorShape: modelData.exists ? Qt.PointingHandCursor : Qt.ArrowCursor }
                        TapHandler { onTapped: if (modelData.exists) veyra.openPath(modelData.path) }
                    }
                }
            }
        }
    }

    // Free and open source, built on Veyra (since 3.1.3 PushFPS links no sponsor pages).
    Text {
        id: creditLine
        objectName: "home-credit"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        text: qsTr("免费开源 · 基于 Veyra 构建")
        color: Theme.t3
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsSmall
    }

    // [data-in] entrance order from pages-a.js (the source cards rise from their delegate, --d 2-5).
    VRise { target: mark; d: 0 }
    VRise { target: hello; d: 1 }
    VRise { target: captureCard; d: 2 }
    VRise { target: resume; d: 2 }
    VRise { target: quickSetup; d: 8 }
    VRise { target: tuning; d: 9 }
    VRise { target: moreActions; d: 9 }
    VRise { target: recent; d: 10 }
}
