// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../brand/integration/qtquick" as Brand

ApplicationWindow {
    id: window
    width: 1680
    height: 1020
    minimumWidth: 1180
    minimumHeight: 760
    visible: true
    title: "CONFLUX · Performance"
    color: Brand.ConfluxTheme.canvas
    font.family: Brand.ConfluxTheme.bodyFont
    font.pixelSize: 15
    palette.windowText: Brand.ConfluxTheme.text
    palette.window: Brand.ConfluxTheme.canvas
    palette.text: Brand.ConfluxTheme.text
    palette.buttonText: Brand.ConfluxTheme.text
    palette.button: Brand.ConfluxTheme.raised
    palette.base: Brand.ConfluxTheme.surface
    palette.highlight: Brand.ConfluxTheme.primary
    palette.highlightedText: Brand.ConfluxTheme.primaryText
    palette.placeholderText: Brand.ConfluxTheme.muted
    Component.onCompleted: Brand.ConfluxTheme.dark = initialDark

    function localUrl(text) {
        const path = text.trim()
        if (path.startsWith("file://")) return path
        if (path.startsWith("/")) return "file://" + path.split("/").map(encodeURIComponent).join("/")
        return path
    }
    function browse(deck) {
        library.selectedDeck = deck
        library.open()
        Qt.callLater(function() {
            const field = deck === 0 ? sourceA : sourceB
            field.forceActiveFocus()
        })
    }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "F6"; onActivated: Brand.ConfluxTheme.dark = !Brand.ConfluxTheme.dark }
    Shortcut { sequence: "Ctrl+O"; onActivated: window.browse(0) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            spacing: 16
            Image {
                source: Brand.ConfluxTheme.dark
                        ? "../brand/assets/logos/conflux-horizontal-dark.svg"
                        : "../brand/assets/logos/conflux-horizontal-light.svg"
                fillMode: Image.PreserveAspectFit
                Layout.preferredWidth: 164
                Layout.preferredHeight: 38
                Accessible.name: "CONFLUX"
            }
            Rectangle { width: 1; height: 24; color: Brand.ConfluxTheme.divider }
            Label {
                text: "PERFORMANCE"
                color: Brand.ConfluxTheme.muted
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 16
                font.letterSpacing: 2
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "MANUAL"
                color: Brand.ConfluxTheme.text
                font.family: Brand.ConfluxTheme.monoFont
                font.pixelSize: 12
                Accessible.name: "Manual control on both decks"
            }
            Button {
                text: "LIBRARY"
                objectName: "libraryOpen"
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 15
                Layout.minimumWidth: 104
                Layout.minimumHeight: 44
                onClicked: window.browse(0)
                background: Rectangle {
                    radius: 3
                    color: parent.down ? Brand.ConfluxTheme.raised : Brand.ConfluxTheme.surface
                    border.color: parent.activeFocus ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
                }
                Accessible.name: "Open local file library (Control O)"
            }
            Button {
                id: themeButton
                objectName: "themeToggle"
                text: Brand.ConfluxTheme.dark ? "LIGHT" : "DARK"
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 15
                Layout.minimumWidth: 80
                Layout.minimumHeight: 44
                onClicked: Brand.ConfluxTheme.dark = !Brand.ConfluxTheme.dark
                background: Rectangle {
                    radius: 3
                    color: parent.down ? Brand.ConfluxTheme.raised : Brand.ConfluxTheme.surface
                    border.color: parent.activeFocus ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
                }
                Accessible.name: "Switch color theme (F6)"
            }
        }

        RowLayout {
            id: performance
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            DeckSurface {
                deckIndex: 0
                controller: deckController
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 360
                onBrowseRequested: function(deck) { window.browse(deck) }
            }
            MixerSurface {
                controller: deckController
                Layout.preferredWidth: Math.min(400, Math.max(320, window.width * 0.235))
                Layout.fillHeight: true
            }
            DeckSurface {
                deckIndex: 1
                controller: deckController
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 360
                onBrowseRequested: function(deck) { window.browse(deck) }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: 30
            spacing: 14
            Rectangle {
                width: 6; height: 6; radius: 3
                color: deckController.outputReady ? Brand.ConfluxTheme.success : Brand.ConfluxTheme.muted
            }
            Label {
                text: deckController.busy ? "Preparing source…"
                      : (deckController.outputReady ? "AUDIO READY" : "OPEN LIBRARY TO LOAD A TRACK")
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 13
                color: Brand.ConfluxTheme.muted
            }
            Label {
                objectName: "audioError"
                text: deckController.error
                visible: text.length > 0
                elide: Text.ElideRight
                Layout.fillWidth: true
                color: Brand.ConfluxTheme.accent
                Accessible.name: "Audio error: " + text
                ToolTip.visible: errorHover.hovered && text.length > 0
                ToolTip.text: text
                HoverHandler { id: errorHover }
            }
            Item { Layout.fillWidth: true; visible: deckController.error.length === 0 }
            Button {
                text: "CONTROL STATUS"
                flat: true
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 13
                Layout.minimumHeight: 44
                onClicked: controlStatus.open()
                Accessible.name: "Show supported controls and file formats"
            }
        }
    }

    Drawer {
        id: library
        objectName: "libraryDrawer"
        property int selectedDeck: 0
        edge: Qt.BottomEdge
        width: window.width
        height: 320
        modal: true
        padding: 24
        background: Rectangle {
            color: Brand.ConfluxTheme.surface
            border.color: Brand.ConfluxTheme.divider
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "LOCAL LIBRARY"
                    color: Brand.ConfluxTheme.text
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 24
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "libraryClose"
                    text: "CLOSE  ×"
                    Layout.minimumWidth: 100
                    Layout.minimumHeight: 44
                    onClicked: library.close()
                    Accessible.name: "Close library (Escape)"
                }
            }
            Label {
                text: "Mono/stereo local files, 8–192 kHz · up to 11m39s per track. Tracks are prepared before playback. Installing a track briefly interrupts shared output."
                wrapMode: Text.WordWrap
                color: Brand.ConfluxTheme.muted
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: "DECK A"
                    color: Brand.ConfluxTheme.deckA
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 18
                    Layout.minimumWidth: 64
                }
                TextField {
                    id: sourceA
                    objectName: "deck0Path"
                    placeholderText: "/home/you/Music/track.wav"
                    placeholderTextColor: Brand.ConfluxTheme.muted
                    selectByMouse: true
                    Layout.fillWidth: true
                    Layout.minimumHeight: 44
                    onAccepted: if (loadA.enabled) deckController.loadDeck(0, window.localUrl(text))
                    Accessible.name: "Deck A local audio path"
                }
                Button {
                    id: loadA
                    objectName: "deck0Load"
                    text: "LOAD A"
                    enabled: !deckController.busy && sourceA.text.trim().length > 0
                    Layout.minimumWidth: 116
                    Layout.minimumHeight: 44
                    onClicked: deckController.loadDeck(0, window.localUrl(sourceA.text))
                    Accessible.name: "Load Deck A"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label {
                    text: "DECK B"
                    color: Brand.ConfluxTheme.deckB
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 18
                    Layout.minimumWidth: 64
                }
                TextField {
                    id: sourceB
                    objectName: "deck1Path"
                    placeholderText: "/home/you/Music/track.wav"
                    placeholderTextColor: Brand.ConfluxTheme.muted
                    selectByMouse: true
                    Layout.fillWidth: true
                    Layout.minimumHeight: 44
                    onAccepted: if (loadB.enabled) deckController.loadDeck(1, window.localUrl(text))
                    Accessible.name: "Deck B local audio path"
                }
                Button {
                    id: loadB
                    objectName: "deck1Load"
                    text: "LOAD B"
                    enabled: !deckController.busy && sourceB.text.trim().length > 0
                    Layout.minimumWidth: 116
                    Layout.minimumHeight: 44
                    onClicked: deckController.loadDeck(1, window.localUrl(sourceB.text))
                    Accessible.name: "Load Deck B"
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    Popup {
        id: controlStatus
        anchors.centerIn: parent
        width: Math.min(560, window.width - 48)
        height: statusContents.implicitHeight + topPadding + bottomPadding
        modal: true
        padding: 24
        background: Rectangle { color: Brand.ConfluxTheme.surface; border.color: Brand.ConfluxTheme.divider; radius: 6 }
        ColumnLayout {
            id: statusContents
            anchors.fill: parent
            spacing: 16
            Label { text: "CONTROL STATUS"; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 24; color: Brand.ConfluxTheme.text }
            Label {
                text: "Load, play/pause, temporary CUE, position seek, channel level and master level are active. Pause and press CUE to set a point; press again there to preview while held. CUE during playback returns to the point and pauses. Press PLAY during a preview to keep playing. Waveform overviews show actual source peaks, normalized for display.\n\nHot cues, jog manipulation, tempo, loops, sync, EQ, effects and headphone cue routing await their audio features. Their panel controls are inactive. Decks C/D are reserved."
                wrapMode: Text.WordWrap
                color: Brand.ConfluxTheme.text
                Layout.fillWidth: true
            }
            Button { text: "CLOSE"; Layout.minimumHeight: 44; onClicked: controlStatus.close() }
        }
    }
}
