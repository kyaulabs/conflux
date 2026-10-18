// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../brand/integration/qtquick" as Brand

ApplicationWindow {
    id: window
    width: 1080
    height: 760
    minimumWidth: 760
    minimumHeight: 560
    visible: true
    title: "CONFLUX · M1"
    color: Brand.ConfluxTheme.canvas
    font.family: Brand.ConfluxTheme.bodyFont
    font.pixelSize: 16
    palette.windowText: Brand.ConfluxTheme.text
    palette.window: Brand.ConfluxTheme.canvas
    palette.text: Brand.ConfluxTheme.text
    palette.buttonText: Brand.ConfluxTheme.text
    palette.button: Brand.ConfluxTheme.raised
    palette.base: Brand.ConfluxTheme.surface
    palette.highlight: Brand.ConfluxTheme.primary
    palette.highlightedText: Brand.ConfluxTheme.primaryText
    palette.mid: Brand.ConfluxTheme.controlBorder
    palette.dark: Brand.ConfluxTheme.controlBorder
    palette.midlight: Brand.ConfluxTheme.controlBorder
    palette.light: Brand.ConfluxTheme.raised
    Component.onCompleted: Brand.ConfluxTheme.dark = initialDark

    function timeText(seconds) {
        const value = Math.max(0, Math.floor(Number(seconds) || 0))
        return Math.floor(value / 60) + ":" + String(value % 60).padStart(2, "0")
    }

    function localUrl(text) {
        const path = text.trim()
        if (path.startsWith("file://"))
            return path
        if (path.startsWith("/"))
            return "file://" + path.split("/").map(encodeURIComponent).join("/")
        return path
    }

    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "F6"; onActivated: Brand.ConfluxTheme.dark = !Brand.ConfluxTheme.dark }

    ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: 20
                spacing: 20
                Image {
                    source: Brand.ConfluxTheme.dark
                            ? "../brand/assets/logos/conflux-horizontal-dark.svg"
                            : "../brand/assets/logos/conflux-horizontal-light.svg"
                    Layout.preferredWidth: 196
                    Layout.preferredHeight: 52
                    fillMode: Image.PreserveAspectFit
                    Accessible.name: "CONFLUX"
                }
                Item { Layout.fillWidth: true }
                Switch {
                    objectName: "themeToggle"
                    text: "Dark theme"
                    checked: Brand.ConfluxTheme.dark
                    onToggled: Brand.ConfluxTheme.dark = checked
                    Accessible.name: "Dark theme (F6)"
                    Layout.minimumHeight: 48
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 8
                Label {
                    text: "M1 · Manual audio"
                    font.pixelSize: 28
                    font.bold: true
                    color: Brand.ConfluxTheme.text
                }
                Label {
                    text: "Load stereo 48 kHz local audio. Each deck accepts up to 174 seconds / 64 MiB of decoded audio."
                    color: Brand.ConfluxTheme.muted
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            GridLayout {
                columns: 2
                columnSpacing: 20
                rowSpacing: 20
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24

                Repeater {
                    model: 2
                    delegate: Frame {
                        id: deckCard
                        required property int index
                        readonly property var deck: deckController.decks[index]
                        readonly property string deckLabel: index === 0 ? "Deck A" : "Deck B"
                        readonly property color deckColor: index === 0 ? Brand.ConfluxTheme.deckA : Brand.ConfluxTheme.deckB
                        readonly property bool canControl: Boolean(deck.loaded) && deckController.outputReady
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 480
                        implicitHeight: deckContents.implicitHeight + topPadding + bottomPadding
                        padding: 20

                        function loadFile() {
                            if (loadButton.enabled)
                                deckController.loadDeck(index, window.localUrl(pathField.text))
                        }

                        background: Rectangle {
                            color: Brand.ConfluxTheme.surface
                            radius: Brand.ConfluxTheme.panelRadius
                            border.color: Brand.ConfluxTheme.divider
                            border.width: 1
                        }

                        ColumnLayout {
                            id: deckContents
                            anchors.fill: parent
                            spacing: 12
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Rectangle {
                                    width: 8
                                    height: 24
                                    radius: 4
                                    color: deckCard.deckColor
                                }
                                Label {
                                    text: deckCard.deckLabel
                                    color: Brand.ConfluxTheme.text
                                    font.pixelSize: 22
                                    font.bold: true
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: deckCard.deck.loaded ? (deckCard.deck.playing ? "Playing" : "Paused") : "Empty"
                                    color: deckCard.deck.loaded ? deckCard.deckColor : Brand.ConfluxTheme.muted
                                }
                            }
                            Label {
                                text: deckCard.deck.loaded ? deckCard.deck.name : "Choose a local audio file"
                                color: Brand.ConfluxTheme.text
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                                Accessible.name: deckCard.deckLabel + " loaded file: " + text
                            }
                            Label {
                                text: "Local file path"
                                color: Brand.ConfluxTheme.muted
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                TextField {
                                    id: pathField
                                    objectName: "deck" + deckCard.index + "Path"
                                    placeholderText: "/home/you/Music/track.wav"
                                    placeholderTextColor: Brand.ConfluxTheme.muted
                                    selectByMouse: true
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 100
                                    Layout.minimumHeight: 48
                                    Accessible.name: deckCard.deckLabel + " local audio path"
                                    onAccepted: deckCard.loadFile()
                                }
                                Button {
                                    id: loadButton
                                    objectName: "deck" + deckCard.index + "Load"
                                    text: "Load"
                                    enabled: !deckController.busy && pathField.text.trim().length > 0
                                    Layout.minimumWidth: 76
                                    Layout.minimumHeight: 48
                                    Accessible.name: "Load " + deckCard.deckLabel
                                    onClicked: deckCard.loadFile()
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                Button {
                                    objectName: "deck" + deckCard.index + "Play"
                                    text: deckCard.deck.playing ? "Pause" : "Play"
                                    highlighted: true
                                    enabled: deckCard.canControl
                                    Layout.minimumWidth: 100
                                    Layout.minimumHeight: 48
                                    Accessible.name: text + " " + deckCard.deckLabel
                                    onClicked: deckController.setPlaying(deckCard.index, !deckCard.deck.playing)
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: window.timeText(seekSlider.pressed ? seekSlider.value : deckCard.deck.positionSeconds)
                                            + " / " + window.timeText(deckCard.deck.durationSeconds)
                                    font.family: Brand.ConfluxTheme.monoFont
                                    color: Brand.ConfluxTheme.text
                                }
                            }
                            Slider {
                                id: seekSlider
                                objectName: "deck" + deckCard.index + "Seek"
                                property real pendingSeek: 0
                                property bool dragged: false
                                from: 0
                                to: Math.max(0.001, Number(deckCard.deck.durationSeconds) || 0)
                                stepSize: 0.1
                                enabled: deckCard.canControl
                                Layout.fillWidth: true
                                Layout.minimumHeight: 48
                                Accessible.name: deckCard.deckLabel + " position"
                                onMoved: {
                                    pendingSeek = value
                                    if (pressed)
                                        dragged = true
                                    else if (deckCard.canControl)
                                        deckController.seekSeconds(deckCard.index, value)
                                }
                                onPressedChanged: {
                                    if (pressed) {
                                        dragged = false
                                    } else if (dragged) {
                                        if (deckCard.canControl)
                                            deckController.seekSeconds(deckCard.index, pendingSeek)
                                        dragged = false
                                    }
                                }
                                Binding {
                                    target: seekSlider
                                    property: "value"
                                    value: Number(deckCard.deck.positionSeconds) || 0
                                    when: !seekSlider.pressed
                                    restoreMode: Binding.RestoreNone
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "Deck gain"; color: Brand.ConfluxTheme.muted }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: Math.round(Number(deckCard.deck.gain) * 100) + "%"
                                    color: Brand.ConfluxTheme.text
                                    font.family: Brand.ConfluxTheme.monoFont
                                }
                            }
                            Slider {
                                objectName: "deck" + deckCard.index + "Gain"
                                from: 0
                                to: 1
                                stepSize: 0.01
                                value: Number(deckCard.deck.gain) || 0
                                enabled: deckCard.canControl
                                Layout.fillWidth: true
                                Layout.minimumHeight: 48
                                Accessible.name: deckCard.deckLabel + " gain"
                                onMoved: deckController.setDeckGain(deckCard.index, value)
                            }
                        }
                    }
                }
            }

            Frame {
                implicitHeight: masterControls.implicitHeight + topPadding + bottomPadding
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                padding: 20
                background: Rectangle {
                    color: Brand.ConfluxTheme.surface
                    radius: Brand.ConfluxTheme.panelRadius
                    border.color: Brand.ConfluxTheme.divider
                    border.width: 1
                }
                RowLayout {
                    id: masterControls
                    anchors.fill: parent
                    spacing: 20
                    Label {
                        text: "Master gain"
                        font.bold: true
                        color: Brand.ConfluxTheme.text
                    }
                    Slider {
                        objectName: "masterGain"
                        from: 0
                        to: 1
                        stepSize: 0.01
                        value: deckController.masterGain
                        enabled: deckController.outputReady
                        Layout.fillWidth: true
                        Layout.minimumHeight: 48
                        Accessible.name: "Master gain"
                        onMoved: deckController.setMasterGain(value)
                    }
                    Label {
                        text: Math.round(deckController.masterGain * 100) + "%"
                        color: Brand.ConfluxTheme.text
                        font.family: Brand.ConfluxTheme.monoFont
                        Layout.minimumWidth: 48
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 24
                spacing: 8
                Label {
                    text: deckController.busy
                          ? "Loading audio… Shared playback pauses briefly when the file is installed."
                          : (deckController.outputReady ? "Audio output ready" : "Load a file to start the audio output.")
                    color: Brand.ConfluxTheme.muted
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Accessible.name: text
                }
                Label {
                    objectName: "audioError"
                    text: deckController.error
                    visible: text.length > 0
                    color: Brand.ConfluxTheme.accent
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Accessible.name: "Audio error: " + text
                }
            }
        }
    }
}
