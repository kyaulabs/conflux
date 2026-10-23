// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../brand/integration/qtquick" as Brand

ApplicationWindow {
    id: window
    width: 1680
    height: 1020
    minimumWidth: 1180
    minimumHeight: 760
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    title: "CONFLUX · Performance"
    color: Brand.ConfluxTheme.canvas
    font.family: Brand.ConfluxTheme.bodyFont
    font.pixelSize: Brand.ConfluxTheme.textPixels(15)
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

    FileDialog {
        id: localFileDialog
        objectName: "libraryFileDialog"
        property int targetDeck: -1
        parentWindow: window
        modality: Qt.WindowModal
        title: targetDeck === 0 ? "Choose Deck A audio" : "Choose Deck B audio"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Audio files (*.wav *.flac *.mp3 *.ogg *.opus *.aiff *.aif)", "All files (*)"]
        onAccepted: {
            const deck = targetDeck
            targetDeck = -1
            if (!library.opened || deckController.busy || (deck !== 0 && deck !== 1)) return
            const url = selectedFile.toString()
            if (!url.startsWith("file:///") && !url.startsWith("file://localhost/")) return
            const field = deck === 0 ? sourceA : sourceB
            field.text = url
            library.selectedId = ""
            field.forceActiveFocus()
        }
        onRejected: targetDeck = -1
    }

    FileDialog {
        id: cueFileDialog
        objectName: "libraryCueFileDialog"
        property string targetSourceId: ""
        property bool pendingSelection: false
        parentWindow: window
        modality: Qt.WindowModal
        title: library.selectedTrack === null ? "Choose CUE sheet" : "Choose CUE sheet for " + library.selectedTrack.name
        fileMode: FileDialog.OpenFile
        nameFilters: ["CUE sheets (*.cue *.CUE)", "All files (*)"]
        onAccepted: {
            const sourceId = targetSourceId
            const pending = pendingSelection
            pendingSelection = false
            targetSourceId = ""
            if (!pending || !library.opened || deckController.busy || !deckController.libraryReady
                || sourceId !== library.selectedId
                || (sourceId.length > 0 && (library.selectedTrack === null || !library.selectedTrack.available))) return
            const url = selectedFile.toString()
            if (!url.startsWith("file:///") && !url.startsWith("file://localhost/")) return
            cuePath.text = url
            library.cuePathSourceId = sourceId
            cuePath.forceActiveFocus()
        }
        onRejected: { pendingSelection = false; targetSourceId = "" }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            spacing: 16
            Item {
                objectName: "windowDragArea"
                Layout.fillWidth: true
                Layout.minimumWidth: 304
                Layout.fillHeight: true
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 16
                    Image {
                        source: Brand.ConfluxTheme.dark
                        ? "../brand/assets/logos/conflux-horizontal-dark.svg"
                        : "../brand/assets/logos/conflux-horizontal-light.svg"
                        fillMode: Image.PreserveAspectFit
                        width: 164
                        height: 38
                        Accessible.name: "CONFLUX"
                    }
                    Rectangle { width: 1; height: 24; y: 7; color: Brand.ConfluxTheme.divider }
                    Label {
                        y: 8
                        text: "PERFORMANCE"
                        color: Brand.ConfluxTheme.muted
                        font.family: Brand.ConfluxTheme.displayFont
                        font.pixelSize: Brand.ConfluxTheme.textPixels(16)
                        font.letterSpacing: 2
                    }
                }
                DragHandler {
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: if (active) window.startSystemMove()
                }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        if (window.visibility === Window.Maximized) window.showNormal()
                        else window.showMaximized()
                    }
                }
            }
            Label {
                text: "MANUAL"
                color: Brand.ConfluxTheme.text
                font.family: Brand.ConfluxTheme.monoFont
                font.pixelSize: Brand.ConfluxTheme.textPixels(12)
                Accessible.name: "Manual control on both decks"
            }
            Button {
                text: "LIBRARY"
                objectName: "libraryOpen"
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: Brand.ConfluxTheme.textPixels(15)
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
                id: audioSettingsButton
                objectName: "audioSettingsOpen"
                text: "AUDIO / REC"
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: Brand.ConfluxTheme.textPixels(15)
                Layout.minimumWidth: 116
                Layout.minimumHeight: 44
                onClicked: audioSettings.open()
                Accessible.name: "Audio outputs and stereo recording"
                Accessible.description: deckController.recording.active ? "Stereo master recording is active." : "Configure outputs or record the stereo master."
                contentItem: Item {
                    implicitWidth: recordingHeaderRow.implicitWidth
                    implicitHeight: recordingHeaderRow.height
                    Row {
                        id: recordingHeaderRow
                        anchors.centerIn: parent
                        height: Math.max(recordingHeaderLabel.implicitHeight, recordingHeaderDot.height)
                        spacing: 8
                        Label {
                            id: recordingHeaderLabel
                            objectName: "audioSettingsRecordingLabel"
                            anchors.verticalCenter: parent.verticalCenter
                            text: audioSettingsButton.text
                            font: audioSettingsButton.font
                            color: Brand.ConfluxTheme.text
                        }
                        Rectangle {
                            id: recordingHeaderDot
                            objectName: "audioSettingsRecordingIndicator"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: width / 2
                            visible: deckController.recording.active
                            color: Brand.ConfluxTheme.primary
                            Accessible.ignored: true
                        }
                    }
                }
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
                font.pixelSize: Brand.ConfluxTheme.textPixels(15)
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
            WindowActions { targetWindow: window }
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
                font.pixelSize: Brand.ConfluxTheme.textPixels(13)
                color: Brand.ConfluxTheme.muted
            }
            Label {
                id: audioError
                objectName: "audioError"
                text: deckController.error
                textFormat: Text.PlainText
                visible: text.length > 0
                elide: Text.ElideRight
                Layout.fillWidth: true
                color: Brand.ConfluxTheme.accent
                Accessible.name: "Audio error: " + text
                HoverHandler { id: errorHover }
                ToolTip {
                    id: errorTooltip
                    objectName: "audioErrorTooltip"
                    visible: errorHover.hovered && audioError.text.length > 0
                    text: audioError.text
                    contentItem: Text {
                        objectName: "audioErrorTooltipText"
                        text: errorTooltip.text
                        textFormat: Text.PlainText
                        font: errorTooltip.font
                        wrapMode: Text.Wrap
                        color: errorTooltip.palette.toolTipText
                    }
                }
            }
            Item { Layout.fillWidth: true; visible: deckController.error.length === 0 }
            Button {
                text: "CONTROL STATUS"
                flat: true
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: Brand.ConfluxTheme.textPixels(13)
                Layout.minimumHeight: 44
                onClicked: controlStatus.open()
                Accessible.name: "Show supported controls and file formats"
            }
        }
    }

    // Let Wayland/the window manager own resizing and its pointer grab.
    // The thin border sits outside the performance controls' 12px margin.
    Repeater {
        model: [Qt.LeftEdge, Qt.RightEdge, Qt.TopEdge, Qt.BottomEdge,
                Qt.LeftEdge | Qt.TopEdge, Qt.RightEdge | Qt.TopEdge,
                Qt.LeftEdge | Qt.BottomEdge, Qt.RightEdge | Qt.BottomEdge]
        MouseArea {
            required property int modelData
            readonly property bool onLeftEdge: Boolean(modelData & Qt.LeftEdge)
            readonly property bool onRightEdge: Boolean(modelData & Qt.RightEdge)
            readonly property bool onTopEdge: Boolean(modelData & Qt.TopEdge)
            readonly property bool onBottomEdge: Boolean(modelData & Qt.BottomEdge)
            readonly property bool corner: (onLeftEdge || onRightEdge) && (onTopEdge || onBottomEdge)
            objectName: "windowResize" + modelData
            z: 100
            enabled: window.visibility !== Window.Maximized && window.visibility !== Window.FullScreen
            x: onRightEdge ? window.width - 5 : onLeftEdge ? 0 : 5
            y: onBottomEdge ? window.height - 5 : onTopEdge ? 0 : 5
            width: onLeftEdge || onRightEdge ? 5 : window.width - 10
            height: onTopEdge || onBottomEdge ? 5 : window.height - 10
            cursorShape: corner ? (onLeftEdge === onTopEdge ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                                : onLeftEdge || onRightEdge ? Qt.SizeHorCursor : Qt.SizeVerCursor
            acceptedButtons: Qt.LeftButton
            onPressed: window.startSystemResize(modelData)
        }
    }

    Drawer {
        id: library
        objectName: "libraryDrawer"
        property int selectedDeck: 0
        property string selectedId: ""
        property string cuePathSourceId: ""
        property string recoverySourceId: ""
        property bool recoveryApproved: false
        property real recoveryInSeconds: 0
        property real recoveryOutSeconds: 0
        function clearRecoveryApproval() {
            recoveryApproved = false
            recoverySourceId = ""
        }
        onSelectedIdChanged: {
            clearRecoveryApproval()
            recoveryRangeDialog.close()
            cancelFileBrowse()
            cancelCueBrowse()
            if (cuePathSourceId.length > 0 && cuePathSourceId !== selectedId) {
                cuePath.text = ""
                cuePathSourceId = ""
            }
        }
        readonly property var selectedTrack: {
            const tracks = deckController.libraryTracks
            for (let index = 0; index < tracks.length; ++index)
                if (tracks[index].id === selectedId) return tracks[index]
            return null
        }
        readonly property var selectedCues: selectedTrack !== null && selectedTrack.cues !== undefined
                                            ? selectedTrack.cues : []
        readonly property var selectedLevels: selectedTrack !== null && selectedTrack.levels !== undefined
                                              ? selectedTrack.levels : ({})
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
            cancelFileBrowse()
            cancelCueBrowse()
            selectedId = track.id
            sourceA.text = track.path
            sourceB.text = track.path
        }
        function cancelFileBrowse() {
            localFileDialog.targetDeck = -1
            localFileDialog.close()
        }
        function browseFile(deck) {
            if (!opened || deckController.busy || localFileDialog.visible || cueFileDialog.visible || (deck !== 0 && deck !== 1)) return
            localFileDialog.targetDeck = deck
            localFileDialog.open()
        }
        function cancelCueBrowse() {
            cueFileDialog.pendingSelection = false
            cueFileDialog.targetSourceId = ""
            cueFileDialog.close()
        }
        function canBrowseCues() {
            return opened && deckController.libraryReady && !deckController.busy
                   && (selectedId.length === 0 || (selectedTrack !== null && selectedTrack.available))
                   && !localFileDialog.visible && !cueFileDialog.visible
        }
        function browseCues() {
            if (!canBrowseCues()) return
            cueFileDialog.targetSourceId = selectedId
            cueFileDialog.pendingSelection = true
            cueFileDialog.open()
        }
        function canLoad(path) {
            if (deckController.busy) return false
            if (selectedId.length > 0)
                return deckController.libraryReady && selectedTrack !== null && selectedTrack.available
            return path.trim().length > 0
        }
        function canImportCues() {
            return deckController.libraryReady && !deckController.busy
                   && selectedTrack !== null && selectedTrack.available
                   && (cuePathSourceId.length === 0 || cuePathSourceId === selectedId)
                   && cuePath.text.trim().length > 0
        }
        function canJumpCue(deck) {
            const state = deckController.decks[deck]
            return deckController.libraryReady && deckController.outputReady && !deckController.busy
                   && selectedTrack !== null && selectedTrack.available
                   && state.loaded && state.sourceId === selectedId
        }
        function cueTime(seconds) {
            const milliseconds = Math.round(seconds * 1000)
            return Math.floor(milliseconds / 60000) + ":"
                   + String(Math.floor(milliseconds / 1000) % 60).padStart(2, "0") + "."
                   + String(milliseconds % 1000).padStart(3, "0")
        }
        function levelText(amplitude) {
            if (typeof amplitude !== "number" || !isFinite(amplitude) || amplitude < 0) return "unknown"
            if (amplitude === 0) return "−∞ dBFS (silence)"
            return (20 * Math.log(amplitude) / Math.LN10).toFixed(2) + " dBFS"
        }
        function load(deck) {
            const stems = deckController.stemsAvailable && stemPreparation.checked
            if (selectedId.length > 0) {
                if (stems && recoveryApproved && recoverySourceId === selectedId)
                    deckController.loadLibraryStemTrackWithRecoveryRange(deck, selectedId, recoveryInSeconds, recoveryOutSeconds)
                else if (stems) deckController.loadLibraryStemTrack(deck, selectedId)
                else deckController.loadLibraryTrack(deck, selectedId)
            } else {
                const url = window.localUrl(deck === 0 ? sourceA.text : sourceB.text)
                if (stems) deckController.loadStemDeck(deck, url)
                else deckController.loadDeck(deck, url)
            }
        }
        Dialog {
            id: recoveryRangeDialog
            objectName: "libraryRecoveryRangeDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            modal: true
            width: Math.min(580, parent.width - 48)
            title: "Approve stem recovery range"
            property string editingSourceId: ""
            readonly property real startSeconds: recoveryStart.text.trim().length ? Number(recoveryStart.text) : NaN
            readonly property real endSeconds: recoveryEnd.text.trim().length ? Number(recoveryEnd.text) : NaN
            readonly property real firstFrame: Math.round(startSeconds * 48000)
            readonly property real endFrame: Math.round(endSeconds * 48000)
            readonly property bool validRange: editingSourceId === library.selectedId && library.selectedTrack !== null
                && Number.isFinite(startSeconds) && Number.isFinite(endSeconds) && startSeconds >= 0
                && endSeconds > startSeconds && endSeconds <= Number(library.selectedTrack.frames) / 48000
                && firstFrame < endFrame && endFrame <= Number(library.selectedTrack.frames)
                && endFrame - firstFrame >= 2048 && endFrame - firstFrame <= 384000
            onAboutToShow: {
                editingSourceId = library.selectedId
                recoveryStart.text = library.recoveryApproved ? String(library.recoveryInSeconds) : ""
                recoveryEnd.text = library.recoveryApproved ? String(library.recoveryOutSeconds) : ""
                recoveryConsent.checked = false
            }
            function edited() { recoveryConsent.checked = false; library.clearRecoveryApproval() }
            contentItem: ColumnLayout {
                spacing: 12
                Label {
                    text: library.selectedTrack === null ? "No source selected" : library.selectedTrack.name
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    text: "Choose a section you approve for repetition if live stems become unavailable."
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: recoveryStart
                        objectName: "libraryRecoveryStart"
                        placeholderText: "Start seconds"
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        validator: DoubleValidator { bottom: 0; decimals: 6; locale: "C" }
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        onTextEdited: recoveryRangeDialog.edited()
                        Accessible.name: "Recovery section start in track seconds"
                    }
                    TextField {
                        id: recoveryEnd
                        objectName: "libraryRecoveryEnd"
                        placeholderText: "End seconds"
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        validator: DoubleValidator { bottom: 0; decimals: 6; locale: "C" }
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        onTextEdited: recoveryRangeDialog.edited()
                        Accessible.name: "Recovery section end in track seconds"
                    }
                }
                Label {
                    objectName: "libraryRecoveryResolvedRange"
                    text: recoveryRangeDialog.validRange
                        ? "Start " + (recoveryRangeDialog.firstFrame / 48000).toFixed(6) + " s · End "
                          + (recoveryRangeDialog.endFrame / 48000).toFixed(6) + " s · Duration "
                          + ((recoveryRangeDialog.endFrame - recoveryRangeDialog.firstFrame) / 48000).toFixed(6) + " s"
                        : "Enter start and end times within this track, up to 8 seconds apart."
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Accessible.name: text
                }
                CheckBox {
                    id: recoveryConsent
                    objectName: "libraryRecoveryConsent"
                    text: "Approve this source range for repetition"
                    checked: false
                    enabled: recoveryRangeDialog.validRange
                    Layout.fillWidth: true
                    Layout.minimumHeight: 44
                    Accessible.name: "Approve this exact track section for recovery repetition"
                }
            }
            footer: DialogButtonBox {
                Button {
                    objectName: "libraryRecoveryApprove"
                    text: "USE RANGE"
                    enabled: recoveryRangeDialog.validRange && recoveryConsent.checked
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                    Accessible.name: "Use the approved recovery range on the next stem load"
                }
                Button {
                    objectName: "libraryRecoveryRemove"
                    text: "REMOVE RANGE"
                    DialogButtonBox.buttonRole: DialogButtonBox.ResetRole
                    onClicked: { library.clearRecoveryApproval(); recoveryRangeDialog.close() }
                    Accessible.name: "Remove recovery range approval"
                }
                Button {
                    text: "CANCEL"
                    DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                }
            }
            onAccepted: {
                if (!validRange || !recoveryConsent.checked) return
                library.recoverySourceId = editingSourceId
                library.recoveryInSeconds = startSeconds
                library.recoveryOutSeconds = endSeconds
                library.recoveryApproved = true
            }
        }
        edge: Qt.BottomEdge
        width: window.width
        height: Math.min(window.height - 48, 650)
        modal: true
        padding: 24
        onAboutToHide: { cancelFileBrowse(); cancelCueBrowse() }
        Connections {
            target: deckController
            function onStateChanged() {
                if (deckController.busy) { library.cancelFileBrowse(); library.cancelCueBrowse() }
            }
        }
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
                    font.pixelSize: Brand.ConfluxTheme.textPixels(24)
                }
                Item { Layout.fillWidth: true }
                CheckBox {
                    id: stemPreparation
                    objectName: "libraryPreparedStems"
                    text: "PREPARED STEMS"
                    visible: deckController.stemsAvailable
                    enabled: deckController.stemsAvailable && !deckController.busy
                    checked: false
                    onCheckedChanged: if (!checked) library.clearRecoveryApproval()
                    Layout.minimumHeight: 44
                    Accessible.name: "Load prepared stems"
                    Accessible.description: "Prepare four local stems when loading a deck. Add to Library still saves the original track."
                }
                Button {
                    objectName: "libraryRecoveryRange"
                    text: library.recoveryApproved ? "RECOVERY RANGE SET" : "RECOVERY RANGE…"
                    visible: deckController.residentHoldAvailable
                    enabled: stemPreparation.checked && !deckController.busy
                             && library.selectedTrack !== null && library.selectedTrack.available
                    Layout.minimumHeight: 44
                    onClicked: recoveryRangeDialog.open()
                    Accessible.name: "Choose and approve a stem recovery range"
                    Accessible.description: library.recoveryApproved
                        ? "Approved track section " + library.recoveryInSeconds + " to " + library.recoveryOutSeconds
                          + " seconds. Used only when loading this selected library source as live stems."
                        : "Choose start and end times, then approve repetition. No section is selected by default."
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
                text: "BROWSE chooses a file without loading it. ADD TO LIBRARY saves the path for later; LOAD A / B puts it on that deck. Mono/stereo, 8–192 kHz · up to 11m39s per track. Replacing a track keeps the other deck and recording running."
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
                    font.pixelSize: Brand.ConfluxTheme.textPixels(18)
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
                    onTextEdited: { library.cancelFileBrowse(); library.selectedId = "" }
                    onAccepted: if (loadA.enabled) library.load(0)
                    Accessible.name: "Deck A local audio path"
                }
                Button {
                    objectName: "deck0FileBrowse"
                    text: "BROWSE…"
                    enabled: library.opened && !deckController.busy && !localFileDialog.visible && !cueFileDialog.visible
                    Layout.minimumWidth: 104
                    Layout.minimumHeight: 44
                    onClicked: library.browseFile(0)
                    Accessible.name: "Browse for Deck A audio file"
                    Accessible.description: "Choose a file for this path. This does not load or add it to the library."
                }
                Button {
                    objectName: "deck0Import"
                    text: "ADD TO LIBRARY"
                    enabled: deckController.libraryReady && !deckController.busy && sourceA.text.trim().length > 0
                    Layout.minimumWidth: 156
                    Layout.minimumHeight: 44
                    onClicked: deckController.importTrack(window.localUrl(sourceA.text))
                    Accessible.name: "Add Deck A path to local library"
                    Accessible.description: "Save this track in the library without loading a deck."
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
                    font.pixelSize: Brand.ConfluxTheme.textPixels(18)
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
                    onTextEdited: { library.cancelFileBrowse(); library.selectedId = "" }
                    onAccepted: if (loadB.enabled) library.load(1)
                    Accessible.name: "Deck B local audio path"
                }
                Button {
                    objectName: "deck1FileBrowse"
                    text: "BROWSE…"
                    enabled: library.opened && !deckController.busy && !localFileDialog.visible && !cueFileDialog.visible
                    Layout.minimumWidth: 104
                    Layout.minimumHeight: 44
                    onClicked: library.browseFile(1)
                    Accessible.name: "Browse for Deck B audio file"
                    Accessible.description: "Choose a file for this path. This does not load or add it to the library."
                }
                Button {
                    objectName: "deck1Import"
                    text: "ADD TO LIBRARY"
                    enabled: deckController.libraryReady && !deckController.busy && sourceB.text.trim().length > 0
                    Layout.minimumWidth: 156
                    Layout.minimumHeight: 44
                    onClicked: deckController.importTrack(window.localUrl(sourceB.text))
                    Accessible.name: "Add Deck B path to local library"
                    Accessible.description: "Save this track in the library without loading a deck."
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
                      : deckController.libraryTracks.length === 0 ? "Library is empty. Choose a path, then ADD TO LIBRARY to save your first track."
                      : library.selectedId.length > 0 && (library.selectedTrack === null || !library.selectedTrack.available)
                        ? "Selected source is missing. Edit a path to load another file."
                      : "Select a saved track, then LOAD A or LOAD B."
                textFormat: Text.PlainText
                color: deckController.libraryError.length > 0 ? Brand.ConfluxTheme.accent : Brand.ConfluxTheme.muted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.name: text
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16
                ListView {
                    id: savedTracks
                    objectName: "libraryTracks"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 440
                    Layout.minimumWidth: 240
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
                        Accessible.description: "Select this track for Load A, Load B or cue-sheet import. Whole-track levels appear in details. Tempo, beat, phrase and key are unknown."
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
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 660
                    Layout.minimumWidth: 560
                    spacing: 8
                    Label {
                        objectName: "libraryTrackLevels"
                        text: "Whole-track RMS: " + library.levelText(library.selectedLevels.rms)
                              + " · Sample peak: " + library.levelText(library.selectedLevels.samplePeak)
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.text
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                    Label {
                        objectName: "libraryTrackAnalysis"
                        text: "Tempo / beat / phrase / key: unknown."
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        TextField {
                            id: cuePath
                            objectName: "libraryCuePath"
                            placeholderText: "/home/you/Music/track.cue"
                            placeholderTextColor: Brand.ConfluxTheme.muted
                            selectByMouse: true
                            Layout.fillWidth: true
                            Layout.minimumHeight: 44
                            onTextEdited: { library.cancelCueBrowse(); library.cuePathSourceId = "" }
                            onAccepted: if (importCues.enabled)
                                deckController.importCueSheet(library.selectedId, window.localUrl(text))
                            Accessible.name: "Cue-sheet path for selected library track"
                        }
                        Button {
                            objectName: "libraryCueBrowse"
                            text: "BROWSE…"
                            enabled: library.canBrowseCues()
                            opacity: enabled ? 1 : .4
                            Layout.minimumWidth: 88
                            Layout.minimumHeight: 44
                            onClicked: library.browseCues()
                            Accessible.name: "Browse for selected track CUE sheet"
                            Accessible.description: "Choose a local CUE sheet without importing cues, loading or seeking a deck."
                        }
                        Button {
                            id: importCues
                            objectName: "libraryCueImport"
                            text: "IMPORT CUES"
                            enabled: library.canImportCues()
                            opacity: enabled ? 1 : .4
                            Layout.minimumWidth: 140
                            Layout.minimumHeight: 44
                            onClicked: deckController.importCueSheet(library.selectedId, window.localUrl(cuePath.text))
                            Accessible.name: "Import cue sheet for selected library track"
                        }
                    }
                    Label {
                        objectName: "libraryCueStatus"
                        text: !deckController.libraryReady ? "Wait for the library to finish opening."
                              : deckController.busy ? "Wait for preparation to finish before choosing or importing cues."
                              : cueFileDialog.visible ? "Choose a CUE sheet in the system Open window."
                              : library.selectedTrack === null ? "Choose a CUE sheet, then select its saved track from the list. Use ADD TO LIBRARY first if the track is not listed."
                              : !library.selectedTrack.available ? "The selected source is missing."
                              : "Cue target: " + library.selectedTrack.name + ". "
                                + (cuePath.text.trim().length === 0 ? "Choose its .cue sheet to import."
                                   : library.selectedCues.length === 0 ? "IMPORT CUES saves markers without loading or seeking a deck."
                                   : library.selectedCues.length + " imported cues. Jump on a deck loaded with this source.")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.muted
                        Layout.fillWidth: true
                        Accessible.name: text
                    }
                    ListView {
                        id: importedCues
                        objectName: "libraryCues"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 100
                        clip: true
                        model: library.selectedCues
                        spacing: 4
                        ScrollBar.vertical: ScrollBar { }
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            width: importedCues.width
                            height: 56
                            spacing: 8
                            Label {
                                objectName: "libraryCue" + index + "Name"
                                text: modelData.trackNumber + ". " + modelData.label
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Brand.ConfluxTheme.text
                                Layout.fillWidth: true
                                Accessible.name: text
                            }
                            Label {
                                objectName: "libraryCue" + index + "Time"
                                text: library.cueTime(modelData.seconds)
                                textFormat: Text.PlainText
                                color: Brand.ConfluxTheme.muted
                                font.family: Brand.ConfluxTheme.monoFont
                            }
                            Button {
                                objectName: "libraryCue" + index + "JumpA"
                                text: "JUMP A"
                                enabled: library.canJumpCue(0)
                                Layout.minimumWidth: 88
                                Layout.minimumHeight: 44
                                onClicked: deckController.jumpToImportedCue(0, library.selectedId, index)
                                Accessible.name: "Jump Deck A to cue " + modelData.trackNumber + ": " + modelData.label
                            }
                            Button {
                                objectName: "libraryCue" + index + "JumpB"
                                text: "JUMP B"
                                enabled: library.canJumpCue(1)
                                Layout.minimumWidth: 88
                                Layout.minimumHeight: 44
                                onClicked: deckController.jumpToImportedCue(1, library.selectedId, index)
                                Accessible.name: "Jump Deck B to cue " + modelData.trackNumber + ": " + modelData.label
                            }
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
                    id: preparationStatus
                    objectName: "preparationStatus"
                    text: library.preparationText
                    textFormat: Text.PlainText
                    color: Brand.ConfluxTheme.muted
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    Accessible.name: text
                    HoverHandler { id: preparationHover }
                    ToolTip {
                        id: preparationTooltip
                        visible: preparationHover.hovered && preparationStatus.text.length > 0
                        text: preparationStatus.text
                        contentItem: Text {
                            text: preparationTooltip.text
                            textFormat: Text.PlainText
                            font: preparationTooltip.font
                            wrapMode: Text.Wrap
                            color: preparationTooltip.palette.toolTipText
                        }
                    }
                }
                Button {
                    objectName: "preparationCancel"
                    text: "CANCEL PREPARATION"
                    visible: library.preparation.status === "Preparing"
                             || library.preparation.status === "Reading cues"
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
                    font.pixelSize: Brand.ConfluxTheme.textPixels(24)
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
                        font.pixelSize: Brand.ConfluxTheme.textPixels(18)
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
                        textFormat: Text.PlainText
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        color: Brand.ConfluxTheme.accent
                        Layout.fillWidth: true
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Brand.ConfluxTheme.divider }
                    Label {
                        text: "STEREO RECORDING"
                        font.family: Brand.ConfluxTheme.displayFont
                        font.pixelSize: Brand.ConfluxTheme.textPixels(18)
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
            Label { text: "CONTROL STATUS"; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: Brand.ConfluxTheme.textPixels(24); color: Brand.ConfluxTheme.text }
            Label {
                text: "Load, play/pause, temporary CUE, position seek and jog, channel/master levels, three-band channel EQ, crossfader, headphone cue selection and peak/RMS meters are active. Pause and press deck CUE to set a point; press again there to preview while held. CUE during playback returns to the point and pauses. Press PLAY during a preview to keep playing. The crossfader selects A at left, B at right and both channel levels unchanged at center. Mixer CUE selects the independent pre-fader headphone bus. Choose separate outputs and record the post-master mix in AUDIO / REC. Waveform overviews show actual source peaks, normalized for display. Hold and drag the platter to scratch prepared audio in either direction; release to restore its previous play or pause request and selected BPM and Master Tempo at the consumed position. Clear stored loops before scratching. Live stems do not support vinyl scratch. Arrow keys and the wheel remain positional search.\n\nPrepared tracks support manual IN/OUT and RELOOP/EXIT: set IN, wait for OUT readiness, then press OUT during playback. EXIT continues past OUT; RELOOP starts at IN again. Seek, jog, transport CUE, replacement and an audio-output rebuild clear the loop. Live stems and quantized beat-loop buttons do not support these manual loops.\n\nPrepared tracks use BPM-based tempo adjustment; move the BPM rail down to speed up. Arrows change 0.1 BPM; Shift and Page keys change 1 BPM. Click the deck LCD BPM value to override the original reference or return to automatic analysis. Automatic values are estimates and can be corrected. The speed range remains six percent around the reference. MASTER TEMPO preserves pitch. Changes follow acknowledged state and may buffer while the worker starts or reprimes. The displayed source time is nominal while processing, and the complete processed tail plays after it reaches the track end. TEMPO RESET returns to normal playback; loops and tempo processing are mutually gated for now. Live-stem tempo remains unavailable.\n\nHot cues, sync and effects await their audio features. Their panel controls are inactive. Decks C/D are reserved."
                wrapMode: Text.WordWrap
                color: Brand.ConfluxTheme.text
                Layout.fillWidth: true
            }
            Button { text: "CLOSE"; Layout.minimumHeight: 44; onClicked: controlStatus.close() }
        }
    }
}
