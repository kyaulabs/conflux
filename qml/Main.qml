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
                objectName: "audioSettingsOpen"
                text: deckController.recording.active ? "AUDIO / REC ●" : "AUDIO / REC"
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 15
                Layout.minimumWidth: 116
                Layout.minimumHeight: 44
                onClicked: audioSettings.open()
                Accessible.name: "Audio outputs and stereo recording"
                background: Rectangle {
                    radius: 3
                    color: parent.down ? Brand.ConfluxTheme.raised : Brand.ConfluxTheme.surface
                    border.color: parent.activeFocus || deckController.recording.active
                                  ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
                }
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
                text: deckController.busy ? (library.preparation.status === "Preparing" ? "Preparing source…"
                                             : library.preparation.status === "Cancelling" ? "Cancelling preparation…"
                                             : library.preparation.status === "Installing" ? "Installing source…"
                                             : library.preparation.status === "Saving" ? "Saving library…"
                                             : "Updating audio…")
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
        property string selectedId: ""
        readonly property var selectedTrack: {
            const tracks = deckController.libraryTracks
            for (let index = 0; index < tracks.length; ++index)
                if (tracks[index].id === selectedId) return tracks[index]
            return null
        }
        readonly property var preparation: deckController.preparation
        readonly property string preparationText: {
            if (preparation.status === "Idle") return ""
            const target = preparation.deck === 0 ? "Deck A" : preparation.deck === 1 ? "Deck B" : "Library import"
            const parts = [preparation.status, target, preparation.name]
                .filter(function(part) { return Boolean(part) })
            if (preparation.cacheHit && preparation.status === "Complete") parts.push("Reused prepared audio")
            return parts.join(" · ")
        }
        function selectTrack(track) {
            selectedId = track.id
            sourceA.text = track.path
            sourceB.text = track.path
        }
        function canLoad(path) {
            if (deckController.busy) return false
            if (selectedId.length > 0)
                return deckController.libraryReady && selectedTrack !== null && selectedTrack.available
            return path.trim().length > 0
        }
        function load(deck) {
            const stems = deckController.stemsAvailable && stemPreparation.checked
            if (selectedId.length > 0) {
                if (stems) deckController.loadLibraryStemTrack(deck, selectedId)
                else deckController.loadLibraryTrack(deck, selectedId)
            } else {
                const url = window.localUrl(deck === 0 ? sourceA.text : sourceB.text)
                if (stems) deckController.loadStemDeck(deck, url)
                else deckController.loadDeck(deck, url)
            }
        }
        edge: Qt.BottomEdge
        width: window.width
        height: Math.min(window.height - 48, 650)
        modal: true
        padding: 24
        background: Rectangle {
            color: Brand.ConfluxTheme.surface
            border.color: Brand.ConfluxTheme.divider
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "LOCAL LIBRARY"
                    color: Brand.ConfluxTheme.text
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 24
                }
                Item { Layout.fillWidth: true }
                CheckBox {
                    id: stemPreparation
                    objectName: "libraryPreparedStems"
                    text: "PREPARED STEMS"
                    visible: deckController.stemsAvailable
                    enabled: deckController.stemsAvailable && !deckController.busy
                    checked: false
                    Layout.minimumHeight: 44
                    Accessible.name: "Load prepared stems"
                    Accessible.description: "Prepare four local stems when loading a deck. Import still saves the original track."
                }
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
                text: "Import a local file to save it in the library, or load a path directly. Mono/stereo, 8–192 kHz · up to 11m39s per track. Replacing a track keeps the other deck and recording running."
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
                    onTextEdited: library.selectedId = ""
                    onAccepted: if (loadA.enabled) library.load(0)
                    Accessible.name: "Deck A local audio path"
                }
                Button {
                    objectName: "deck0Import"
                    text: "IMPORT"
                    enabled: deckController.libraryReady && !deckController.busy && sourceA.text.trim().length > 0
                    Layout.minimumWidth: 100
                    Layout.minimumHeight: 44
                    onClicked: deckController.importTrack(window.localUrl(sourceA.text))
                    Accessible.name: "Import Deck A path into local library"
                }
                Button {
                    id: loadA
                    objectName: "deck0Load"
                    text: "LOAD A"
                    enabled: library.canLoad(sourceA.text)
                    Layout.minimumWidth: 116
                    Layout.minimumHeight: 44
                    onClicked: library.load(0)
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
                    onTextEdited: library.selectedId = ""
                    onAccepted: if (loadB.enabled) library.load(1)
                    Accessible.name: "Deck B local audio path"
                }
                Button {
                    objectName: "deck1Import"
                    text: "IMPORT"
                    enabled: deckController.libraryReady && !deckController.busy && sourceB.text.trim().length > 0
                    Layout.minimumWidth: 100
                    Layout.minimumHeight: 44
                    onClicked: deckController.importTrack(window.localUrl(sourceB.text))
                    Accessible.name: "Import Deck B path into local library"
                }
                Button {
                    id: loadB
                    objectName: "deck1Load"
                    text: "LOAD B"
                    enabled: library.canLoad(sourceB.text)
                    Layout.minimumWidth: 116
                    Layout.minimumHeight: 44
                    onClicked: library.load(1)
                    Accessible.name: "Load Deck B"
                }
            }
            Label {
                objectName: "libraryStatus"
                text: deckController.libraryError.length > 0 ? deckController.libraryError
                      : !deckController.libraryReady ? "Loading saved library…"
                      : deckController.libraryTracks.length === 0 ? "Library is empty. Import a path to save your first track."
                      : library.selectedId.length > 0 && (library.selectedTrack === null || !library.selectedTrack.available)
                        ? "Selected source is missing. Edit a path to load another file."
                      : "Select a saved track, then LOAD A or LOAD B. Analysis: unknown."
                textFormat: Text.PlainText
                color: deckController.libraryError.length > 0 ? Brand.ConfluxTheme.accent : Brand.ConfluxTheme.muted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.name: text
            }
            ListView {
                id: savedTracks
                objectName: "libraryTracks"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 100
                clip: true
                model: deckController.libraryTracks
                spacing: 4
                ScrollBar.vertical: ScrollBar { }
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    objectName: "libraryTrack" + index
                    width: savedTracks.width
                    height: 56
                    highlighted: library.selectedId === modelData.id
                    onClicked: library.selectTrack(modelData)
                    Accessible.name: modelData.name + (modelData.available ? "" : ", source missing")
                    Accessible.description: "Select this track for Load A or Load B. Analysis unknown."
                    background: Rectangle {
                        radius: 3
                        color: parent.highlighted ? Brand.ConfluxTheme.raised : Brand.ConfluxTheme.canvas
                        border.color: parent.activeFocus || parent.highlighted
                                      ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.divider
                    }
                    contentItem: RowLayout {
                        spacing: 16
                        Label {
                            objectName: "libraryTrack" + index + "Name"
                            text: modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Brand.ConfluxTheme.text
                            Layout.fillWidth: true
                        }
                        Label {
                            text: modelData.available
                                  ? Math.floor(modelData.durationSeconds / 60) + ":"
                                    + String(Math.floor(modelData.durationSeconds % 60)).padStart(2, "0")
                                  : "MISSING"
                            color: modelData.available ? Brand.ConfluxTheme.muted : Brand.ConfluxTheme.accent
                            font.family: Brand.ConfluxTheme.monoFont
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumHeight: 44
                spacing: 16
                visible: library.preparation.status !== "Idle"
                Label {
                    objectName: "preparationStatus"
                    text: library.preparationText
                    textFormat: Text.PlainText
                    color: Brand.ConfluxTheme.muted
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    Accessible.name: text
                    ToolTip.visible: preparationHover.hovered && text.length > 0
                    ToolTip.text: text
                    HoverHandler { id: preparationHover }
                }
                Button {
                    objectName: "preparationCancel"
                    text: "CANCEL PREPARATION"
                    visible: library.preparation.status === "Preparing"
                             || library.preparation.status === "Cancelling"
                             || library.preparation.status === "Installing"
                             || library.preparation.status === "Saving"
                    enabled: library.preparation.cancellable
                    Layout.minimumWidth: 220
                    Layout.minimumHeight: 44
                    onClicked: deckController.cancelPreparation()
                    Accessible.name: library.preparation.deck < 0 ? "Cancel library import"
                                     : "Cancel audio preparation for " + (library.preparation.deck === 0 ? "Deck A" : "Deck B")
                    Accessible.description: "Stops preparation before installation or saving. Current playback continues."
                }
            }
        }
    }

    Drawer {
        id: audioSettings
        objectName: "audioSettingsDrawer"
        edge: Qt.RightEdge
        width: Math.min(620, window.width - 24)
        height: window.height
        modal: true
        padding: 20
        property string masterLeft: ""
        property string masterRight: ""
        property string cueLeft: ""
        property string cueRight: ""
        readonly property var masterOptions: [{key: "", label: "Default master"}].concat(deckController.audioRouting.ports)
        readonly property var cueOptions: [{key: "", label: "Cue disabled"}].concat(deckController.audioRouting.ports)
        readonly property bool completePairs: (masterLeft.length === 0) === (masterRight.length === 0)
                                             && (cueLeft.length === 0) === (cueRight.length === 0)
        readonly property bool distinctPorts: {
            const keys = [masterLeft, masterRight, cueLeft, cueRight].filter(function(key) { return key.length > 0 })
            return keys.every(function(key, index) { return keys.indexOf(key) === index })
        }
        function optionIndex(options, key) {
            for (let i = 0; i < options.length; ++i)
                if (options[i].key === key) return i
            return -1
        }
        onOpened: {
            masterLeft = deckController.audioRouting.masterLeft
            masterRight = deckController.audioRouting.masterRight
            cueLeft = deckController.audioRouting.cueLeft
            cueRight = deckController.audioRouting.cueRight
            deckController.refreshAudioRouting()
        }
        background: Rectangle {
            color: Brand.ConfluxTheme.surface
            border.color: Brand.ConfluxTheme.divider
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "AUDIO / RECORDING"
                    color: Brand.ConfluxTheme.text
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 24
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "audioSettingsClose"
                    text: "CLOSE ×"
                    Layout.minimumHeight: 44
                    onClicked: audioSettings.close()
                    Accessible.name: "Close audio and recording settings (Escape)"
                }
            }
            ScrollView {
                id: audioScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: audioScroll.availableWidth
                    spacing: 12
                    Label {
                        text: "OUTPUTS"
                        font.family: Brand.ConfluxTheme.displayFont
                        font.pixelSize: 18
                        color: Brand.ConfluxTheme.text
                    }
                    Label {
                        text: "Refresh playback ports, then choose your outputs. You can save outputs before loading a track. Applying during playback briefly interrupts audio, then restores transport. Headphone cue needs four distinct ports across master and cue; it never falls back to the default master."
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                    }
                    Label {
                        text: "Default master: " + (deckController.audioRouting.defaultSink || "not discovered")
                        wrapMode: Text.WrapAnywhere
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 8
                        Layout.fillWidth: true
                        Label { text: "MASTER LEFT"; color: Brand.ConfluxTheme.text; font.family: Brand.ConfluxTheme.displayFont }
                        ComboBox {
                            objectName: "masterLeftPort"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 40
                            textRole: "label"
                            model: audioSettings.masterOptions
                            currentIndex: audioSettings.optionIndex(model, audioSettings.masterLeft)
                            onActivated: function(index) { audioSettings.masterLeft = model[index].key }
                            Accessible.name: "Master left playback port"
                        }
                        Label { text: "MASTER RIGHT"; color: Brand.ConfluxTheme.text; font.family: Brand.ConfluxTheme.displayFont }
                        ComboBox {
                            objectName: "masterRightPort"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 40
                            textRole: "label"
                            model: audioSettings.masterOptions
                            currentIndex: audioSettings.optionIndex(model, audioSettings.masterRight)
                            onActivated: function(index) { audioSettings.masterRight = model[index].key }
                            Accessible.name: "Master right playback port"
                        }
                        Label { text: "CUE LEFT"; color: Brand.ConfluxTheme.text; font.family: Brand.ConfluxTheme.displayFont }
                        ComboBox {
                            objectName: "cueLeftPort"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 40
                            textRole: "label"
                            model: audioSettings.cueOptions
                            currentIndex: audioSettings.optionIndex(model, audioSettings.cueLeft)
                            onActivated: function(index) { audioSettings.cueLeft = model[index].key }
                            Accessible.name: "Headphone cue left playback port"
                        }
                        Label { text: "CUE RIGHT"; color: Brand.ConfluxTheme.text; font.family: Brand.ConfluxTheme.displayFont }
                        ComboBox {
                            objectName: "cueRightPort"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 40
                            textRole: "label"
                            model: audioSettings.cueOptions
                            currentIndex: audioSettings.optionIndex(model, audioSettings.cueRight)
                            onActivated: function(index) { audioSettings.cueRight = model[index].key }
                            Accessible.name: "Headphone cue right playback port"
                        }
                    }
                    Label {
                        objectName: "routingValidation"
                        visible: !audioSettings.completePairs || !audioSettings.distinctPorts
                        text: !audioSettings.completePairs ? "Select both channels of each pair, or leave both empty."
                                                          : "Master and cue must use distinct playback ports."
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.accent
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            objectName: "audioRoutingRefresh"
                            text: "REFRESH"
                            Layout.minimumHeight: 44
                            enabled: !deckController.busy
                            onClicked: deckController.refreshAudioRouting()
                            Accessible.name: "Refresh playback ports"
                        }
                        Button {
                            objectName: "audioRoutingApply"
                            text: "APPLY OUTPUTS"
                            Layout.minimumHeight: 44
                            enabled: !deckController.busy && !deckController.recording.active
                                     && audioSettings.completePairs && audioSettings.distinctPorts
                            onClicked: deckController.configureAudioRouting(audioSettings.masterLeft, audioSettings.masterRight,
                                                                           audioSettings.cueLeft, audioSettings.cueRight)
                            Accessible.name: "Apply master and headphone cue outputs"
                        }
                    }
                    Label {
                        objectName: "routingStatus"
                        text: "Master " + (deckController.audioRouting.masterReady ? "ready" : "unavailable")
                              + " · Cue " + (deckController.audioRouting.cueReady ? "ready" : "unrouted")
                        color: Brand.ConfluxTheme.text
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "routingError"
                        text: [deckController.audioRouting.masterError, deckController.audioRouting.cueError, deckController.error]
                              .filter(function(error, index, all) { return Boolean(error) && all.indexOf(error) === index }).join("\n")
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.accent
                        Layout.fillWidth: true
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Brand.ConfluxTheme.divider }
                    Label {
                        text: "STEREO RECORDING"
                        font.family: Brand.ConfluxTheme.displayFont
                        font.pixelSize: 18
                        color: Brand.ConfluxTheme.text
                    }
                    Label {
                        text: "Records the post-master mix to a new stereo WAV file. Headphone cue is excluded. Enter a new absolute .wav path; existing files are never overwritten. Stop recording before applying outputs."
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                    }
                    TextField {
                        id: recordingPath
                        objectName: "recordingPath"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        placeholderText: "/home/you/Music/my-set.wav"
                        selectByMouse: true
                        enabled: !deckController.recording.active
                        onAccepted: if (recordStart.enabled) deckController.startRecording(window.localUrl(text))
                        Accessible.name: "New absolute stereo WAV recording path"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            id: recordStart
                            objectName: "recordingStart"
                            text: "RECORD MASTER"
                            Layout.minimumHeight: 44
                            enabled: deckController.outputReady && !deckController.busy && !deckController.recording.active
                                     && (recordingPath.text.trim().startsWith("/") || recordingPath.text.trim().startsWith("file:///"))
                                     && recordingPath.text.trim().toLowerCase().endsWith(".wav")
                            onClicked: deckController.startRecording(window.localUrl(recordingPath.text))
                            Accessible.name: "Start stereo master recording to a new file"
                        }
                        Button {
                            objectName: "recordingStop"
                            text: "STOP / SAVE"
                            Layout.minimumHeight: 44
                            enabled: deckController.recording.active && deckController.recording.status !== "Stopping"
                                     && deckController.recording.status !== "Finalizing"
                            onClicked: deckController.stopRecording()
                            Accessible.name: "Stop and finalize stereo recording"
                        }
                    }
                    Label {
                        objectName: "recordingStatus"
                        text: "Recording: " + deckController.recording.status
                        color: deckController.recording.active ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.text
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "recordingSavedPath"
                        text: deckController.recording.path
                        visible: text.length > 0
                        wrapMode: Text.WrapAnywhere
                        color: Brand.ConfluxTheme.text
                        Layout.fillWidth: true
                        Accessible.name: "Recording file: " + text
                    }
                    Label {
                        text: "Captured " + (deckController.recording.acceptedFrames / 48000).toFixed(1) + " s · saved "
                              + (deckController.recording.writtenFrames / 48000).toFixed(1) + " s"
                              + (deckController.recording.overflowFrames > 0 ? " · stopped: recording incomplete" : "")
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "recordingError"
                        text: deckController.recording.error
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.accent
                        Layout.fillWidth: true
                        Accessible.name: "Recording error: " + text
                    }
                }
            }
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
                text: "Load, play/pause, temporary CUE, position seek and jog, channel/master levels, headphone cue selection and peak/RMS meters are active. Pause and press deck CUE to set a point; press again there to preview while held. CUE during playback returns to the point and pauses. Press PLAY during a preview to keep playing. Mixer CUE selects the pre-fader headphone bus. Choose separate outputs and record the post-master mix in AUDIO / REC. Waveform overviews show actual source peaks, normalized for display. Jog moves the track position; it does not scratch or change playback speed.\n\nHot cues, tempo, loops, sync, EQ and effects await their audio features. Their panel controls are inactive. Decks C/D are reserved."
                wrapMode: Text.WordWrap
                color: Brand.ConfluxTheme.text
                Layout.fillWidth: true
            }
            Button { text: "CLOSE"; Layout.minimumHeight: 44; onClicked: controlStatus.close() }
        }
    }
}
