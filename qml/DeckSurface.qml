// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "../brand/integration/qtquick" as Brand

Item {
    id: surface
    readonly property string transportStatus: !deck.loaded ? "EMPTY" : deck.sourceReadError ? "READ ERROR"
                                             : deck.buffering ? "BUFFERING" : deck.cuePreview ? "CUE PREVIEW"
                                             : deck.playing ? "PLAYING" : "PAUSED"
    required property int deckIndex
    required property var controller
    readonly property var deck: controller.decks[deckIndex]
    readonly property bool showingPreparation: Boolean(deck.loading)
    readonly property var presentation: showingPreparation ? (deck.pendingPresentation || ({})) : deck
    readonly property real loadProgress: showingPreparation ? Math.max(0, Math.min(1, Number(deck.loadProgress) || 0)) : 1
    readonly property string sourceId: String(deck.sourceId || "")
    readonly property bool canControl: Boolean(deck.loaded) && controller.outputReady && !deck.replacing
    readonly property var loop: deck.loop || ({})
    readonly property var tempoState: deck.tempo || ({})
    readonly property var scratchState: deck.scratch || ({})
    readonly property string scratchSourceGeneration: String(deck.sourceGeneration || "0")
    readonly property var bpmState: deck.bpm || ({})
    readonly property real referenceBpm: Number(bpmState.reference) || 0
    readonly property bool hasReferenceBpm: Boolean(deck.loaded) && Number.isFinite(referenceBpm) && referenceBpm >= 20 && referenceBpm <= 400
    readonly property real acknowledgedBpm: hasReferenceBpm ? referenceBpm * (1 + (Number(tempoState.percent) || 0) / 100) : 0
    readonly property bool canEditBpm: Boolean(deck.loaded) && !deck.replacing && !showingPreparation
    readonly property bool hasLoop: Boolean(loop.inSet) || Boolean(loop.outSet) || Boolean(loop.cached)
    readonly property bool canScratch: canControl && Boolean(scratchState.supported)
                                       && deck.overviewStem !== "source" && !deck.cueHeld && !deck.cuePreview
                                       && !deck.sourceReadError && !scratchState.errorCode
                                       && !hasLoop && !loop.active && !tempoState.active
    readonly property string scratchUnavailableReason: !deck.loaded ? "Load a prepared track first."
        : !canControl ? "Wait for audio output or track installation."
        : deck.overviewStem === "source" ? "Live stems do not support vinyl scratch."
        : deck.cueHeld || deck.cuePreview ? "Release CUE before scratching."
        : hasLoop || loop.active ? "Clear the stored manual loop first."
        : tempoState.active ? "Press TEMPO RESET before scratching, including at zero speed change."
        : deck.sourceReadError || scratchState.errorCode ? "The prepared scratch source has a read error."
        : "Vinyl scratch is unavailable for this prepared source."
    readonly property bool canTempo: canControl && Boolean(tempoState.supported) && !deck.sourceReadError && !tempoState.errorCode
                                    && !deck.cuePreview && !hasLoop
    readonly property bool canResetTempo: canControl && Boolean(tempoState.supported)
                                         && Boolean(tempoState.active) && !deck.cuePreview
    readonly property bool canLoop: canControl && Boolean(loop.supported) && !deck.sourceReadError
                                   && !deck.cuePreview && !tempoState.active
    readonly property string deckName: deckIndex === 0 ? "Deck A" : "Deck B"
    readonly property color deckColor: deckIndex === 0 ? Brand.ConfluxTheme.deckA : Brand.ConfluxTheme.deckB
    readonly property color displayAccent: deckIndex === 0 ? "#58D6C3" : "#F2C568"
    readonly property string trackTitle: showingPreparation ? (presentation.title || deck.pendingName || "Preparing track…")
                                         : deck.loaded ? (deck.title || deck.name) : "Browse a local track"
    readonly property string artworkSource: (showingPreparation || deck.loaded) && typeof presentation.artworkDataUrl === "string"
                                             && presentation.artworkDataUrl.startsWith("data:image/png;base64,")
                                             ? presentation.artworkDataUrl : ""
    readonly property bool compactDisplay: display.height < 232
    readonly property real edge: width >= 560 ? 22 : 16
    readonly property real sideWidth: width >= 560 ? 76 : 66
    readonly property real screenHeight: Math.max(174, Math.min(height * 0.28,
        (width - 2 * edge - sideWidth - 14) * 0.60))
    readonly property real hotCueTop: 40 + screenHeight + 20
    readonly property real loopTop: hotCueTop + 52
    readonly property real jogTop: loopTop + 60
    // Reserve a transport/tempo lane on either side of the circular platter.
    readonly property real jogDiameter: Math.max(220, Math.min(
        width - 2 * (edge + 56 + 8), height - jogTop - 30))
    property var cachedWaveform: []
    signal browseRequested(int deck)
    implicitWidth: 470
    implicitHeight: 670

    function timeText(seconds) {
        const value = Math.max(0, Math.floor(Number(seconds) || 0))
        return Math.floor(value / 60) + ":" + String(value % 60).padStart(2, "0")
    }
    onCanEditBpmChanged: { if (!canEditBpm && bpmEditor) bpmEditor.close() }
    onSourceIdChanged: { if (bpmEditor) bpmEditor.close(); if (jogInput) jogInput.finishScratch() }
    onScratchSourceGenerationChanged: { if (jogInput) jogInput.finishScratch() }
    onCanScratchChanged: { if (!canScratch && jogInput) jogInput.finishScratch() }
    onLoadProgressChanged: { if (waveform) waveform.requestPaint() }
    onShowingPreparationChanged: { if (waveform) waveform.requestPaint() }
    function refreshWaveform() {
        const current = controller.decks[deckIndex] || ({})
        const next = (current.loading ? current.pendingWaveform : current.waveform) || []
        let unchanged = next.length === cachedWaveform.length
        for (let index = 0; unchanged && index < next.length; ++index)
            unchanged = Number(next[index]) === Number(cachedWaveform[index])
        if (unchanged)
            return
        const copy = []
        for (let index = 0; index < next.length; ++index)
            copy.push(Number(next[index]))
        cachedWaveform = copy
        waveform.requestPaint()
    }
    function refreshTempo() {
        // Read one current controller snapshot rather than depending on the
        // reevaluation order of the deck/reference/range property bindings.
        const current = controller.decks[deckIndex] || ({})
        const bpm = current.bpm || ({})
        const state = current.tempo || ({})
        const reference = Number(bpm.reference)
        const known = Boolean(current.loaded) && Number.isFinite(reference) && reference >= 20 && reference <= 400
        const percent = Number(state.percent)
        const previousGuard = tempo.restoringAcknowledgement
        tempo.restoringAcknowledgement = true
        tempo.acknowledgedValue = known ? reference * (1 + (Number.isFinite(percent) ? percent : 0) / 100) : 0
        tempo.from = known ? 1.06 * reference : 1
        tempo.to = known ? .94 * reference : 0
        if (!tempo.pressed)
            tempo.value = tempo.acknowledgedValue
        tempo.restoringAcknowledgement = previousGuard
    }
    Component.onCompleted: { refreshWaveform(); refreshTempo(); seek.refreshAccepted() }
    Connections {
        target: surface.controller
        function onStateChanged() { surface.refreshWaveform(); surface.refreshTempo(); seek.refreshAccepted() }
    }
    Connections {
        target: Brand.ConfluxTheme
        function onDarkChanged() {
            waveform.requestPaint()
            knurl.requestPaint()
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        border.width: 1
        border.color: Brand.ConfluxTheme.dark ? "#52585F" : "#92948F"
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(Brand.ConfluxTheme.surface, 1.2) }
            GradientStop { position: 0.44; color: Brand.ConfluxTheme.surface }
            GradientStop { position: 1; color: Qt.darker(Brand.ConfluxTheme.surface, 1.18) }
        }
        Rectangle {
            x: 2
            y: parent.height - 12
            width: parent.width - 4
            height: 10
            radius: 3
            color: Brand.ConfluxTheme.dark ? "#0A0C0F" : "#B3B4AF"
        }
    }
    Repeater {
        model: 4
        Rectangle {
            required property int index
            x: index % 2 === 0 ? 9 : surface.width - 15
            y: index < 2 ? 10 : surface.height - 24
            width: 6
            height: 6
            radius: 3
            color: Brand.ConfluxTheme.dark ? "#0B0D10" : "#898E92"
            border.color: Brand.ConfluxTheme.dark ? "#555B63" : "#D3D5D2"
            Rectangle { anchors.centerIn: parent; width: 4; height: 1; color: Brand.ConfluxTheme.dark ? "#555B63" : "#454B50"; rotation: 28 }
        }
    }
    Label {
        objectName: "deck" + surface.deckIndex + "Ownership"
        x: surface.edge
        y: 13
        width: surface.width - 2 * surface.edge - 20
        height: 20
        text: surface.deckName.toUpperCase() + " / MANUAL"
        elide: Text.ElideRight
        Accessible.name: surface.deckName + " control ownership: Manual"
        font.family: Brand.ConfluxTheme.displayFont
        font.pixelSize: 14
        font.letterSpacing: 1.2
        color: Brand.ConfluxTheme.text
    }
    Rectangle {
        x: surface.width - surface.edge - 8
        y: 16
        width: 7
        height: 7
        radius: 3.5
        color: surface.deck.loaded ? surface.deckColor : Brand.ConfluxTheme.divider
    }

    Rectangle {
        id: display
        objectName: "deck" + surface.deckIndex + "Display"
        x: surface.edge
        y: 40
        width: surface.width - 2 * surface.edge - surface.sideWidth - 14
        height: surface.screenHeight
        radius: 5
        color: "#050A0B"
        border.color: Brand.ConfluxTheme.dark ? "#626B70" : "#848F92"
        border.width: 2
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            color: "transparent"
            border.width: 1
            border.color: "#19272B"
        }
        Label {
            x: 14
            y: 12
            objectName: "deck" + surface.deckIndex + "WaveformKind"
            text: surface.showingPreparation ? "PREPARING · " + (surface.deck.loadPhaseText || "Reading audio")
                  : !surface.deck.loaded ? "NO TRACK LOADED"
                  : surface.deck.overviewStem === "drums" ? "PREPARED STEMS · DRUMS WAVEFORM"
                  : surface.deck.overviewStem === "source" ? "LIVE STEMS · SOURCE WAVEFORM" : "LOCAL FILE"
            color: surface.displayAccent
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 10
            font.letterSpacing: 0.5
            width: parent.width - 28
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        Rectangle {
            id: artworkFrame
            objectName: "deck" + surface.deckIndex + "ArtworkFrame"
            x: 14
            y: surface.compactDisplay ? 28 : 30
            width: surface.compactDisplay ? 48 : 64
            height: width
            radius: 3
            color: "#102024"
            border.color: "#304B50"
            Image {
                id: artwork
                objectName: "deck" + surface.deckIndex + "Artwork"
                anchors.fill: parent
                anchors.margins: 2
                source: surface.artworkSource
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
                opacity: surface.showingPreparation ? 0.4 + 0.6 * surface.loadProgress : 1
                visible: status === Image.Ready
                Accessible.role: Accessible.Graphic
                Accessible.name: surface.deckName + " artwork for " + surface.trackTitle
            }
            Item {
                objectName: "deck" + surface.deckIndex + "ArtworkPlaceholder"
                anchors.fill: parent
                visible: artwork.status !== Image.Ready
                Accessible.role: Accessible.Graphic
                Accessible.name: surface.deckName + " artwork unavailable"
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 6
                    width: parent.width * 0.52
                    height: width
                    radius: width / 2
                    color: "#081215"
                    border.color: "#3B5D62"
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width * 0.38
                        height: width
                        radius: width / 2
                        color: surface.displayAccent
                        opacity: 0.65
                    }
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: surface.showingPreparation ? "LOADING" : surface.deck.loaded ? "NO ART" : "EMPTY"
                    textFormat: Text.PlainText
                    color: "#8EAAA7"
                    font.family: Brand.ConfluxTheme.monoFont
                    font.pixelSize: 7
                }
            }
        }
        Label {
            id: titleLabel
            objectName: "deck" + surface.deckIndex + "Title"
            x: artworkFrame.x + artworkFrame.width + 10
            y: artworkFrame.y
            width: parent.width - x - 14
            text: surface.trackTitle
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: surface.showingPreparation ? "#98AAA7" : "#EFF8F6"
            font.family: Brand.ConfluxTheme.displayFont
            font.pixelSize: surface.compactDisplay ? 18 : 26
            Accessible.name: surface.deckName + " title: " + text
        }
        Label {
            id: trackArtist
            objectName: "deck" + surface.deckIndex + "Artist"
            x: titleLabel.x
            y: titleLabel.y + titleLabel.height + 1
            width: titleLabel.width
            text: surface.showingPreparation ? (surface.presentation.artist || "Reading track information…")
                  : surface.deck.loaded ? (surface.deck.artist || "Artist unknown") : "—"
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: "#B5CDC9"
            font.family: Brand.ConfluxTheme.bodyFont
            font.pixelSize: surface.compactDisplay ? 11 : 14
            Accessible.name: surface.deckName + " artist: " + text
        }
        Label {
            objectName: "deck" + surface.deckIndex + "Album"
            x: titleLabel.x
            y: trackArtist.y + trackArtist.height + 1
            width: titleLabel.width - (surface.compactDisplay ? 96 : 0)
            text: surface.showingPreparation ? (surface.presentation.album || "")
                  : surface.deck.loaded ? (surface.deck.album || "Album unknown") : "—"
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 1
            color: "#8EAAA7"
            font.family: Brand.ConfluxTheme.bodyFont
            font.pixelSize: surface.compactDisplay ? 10 : 12
            Accessible.name: surface.deckName + " album: " + text
        }
        Label {
            objectName: "deck" + surface.deckIndex + "Elapsed"
            x: 14
            y: surface.compactDisplay ? 80 : 100
            width: parent.width * 0.55 - 14
            text: surface.showingPreparation ? "—:—" : surface.deck.loaded ? surface.timeText(seek.pressed ? seek.value : surface.deck.positionSeconds) : "—:—"
            color: "#F2F8F4"
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: surface.compactDisplay ? 26 : 36
        }
        Button {
            id: bpmReference
            objectName: "deck" + surface.deckIndex + "BpmReference"
            anchors.right: parent.right
            anchors.rightMargin: 14
            y: surface.compactDisplay ? 70 : 98
            width: 96
            height: 44
            padding: 0
            enabled: surface.canEditBpm
            text: surface.showingPreparation ? "Preparing track; BPM not yet available"
                  : surface.hasReferenceBpm ? surface.acknowledgedBpm.toFixed(1) + " BPM · " + (surface.bpmState.manual ? "MANUAL" : "ESTIMATED")
                  : "BPM UNKNOWN · SET REFERENCE"
            Accessible.name: surface.deckName + " tempo, " + text + ", edit reference BPM"
            Accessible.description: "Edit the track's original-speed BPM. This changes the reference number, not the audio or deck sync."
            onClicked: bpmEditor.openForSource()
            background: Rectangle { color: "transparent"; border.color: parent.activeFocus ? surface.displayAccent : "transparent"; radius: 2 }
            contentItem: Item {
                Label {
                    objectName: "deck" + surface.deckIndex + "BpmCurrent"
                    y: 0
                    width: parent.width
                    height: 20
                    text: surface.showingPreparation ? "… BPM" : surface.hasReferenceBpm ? surface.acknowledgedBpm.toFixed(1) + " BPM" : "— BPM"
                    textFormat: Text.PlainText
                    color: surface.displayAccent
                    font.family: Brand.ConfluxTheme.monoFont
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    y: 20
                    width: parent.width
                    height: 10
                    text: surface.showingPreparation ? "PREPARING" : surface.hasReferenceBpm ? (surface.bpmState.manual ? "MANUAL" : "ESTIMATED") : "SET REFERENCE"
                    color: "#8EAAA7"
                    font.family: Brand.ConfluxTheme.monoFont
                    font.pixelSize: 8
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    objectName: "deck" + surface.deckIndex + "Remaining"
                    y: 30
                    width: parent.width
                    height: 14
                    text: !surface.showingPreparation && surface.deck.loaded ? "−" + surface.timeText(Math.max(0, surface.deck.durationSeconds - surface.deck.positionSeconds)) : "—:—"
                    color: surface.displayAccent
                    font.family: Brand.ConfluxTheme.monoFont
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
        Label {
            x: 14
            y: 148
            visible: !surface.compactDisplay
            text: surface.showingPreparation ? (surface.deck.loaded ? "CURRENT AUDIO: " + surface.deck.name : "WAITING FOR PREPARED AUDIO")
                  : surface.deck.loaded ? surface.transportStatus + "  ·  STEREO 48 kHz" : "LOAD TO PREPARE AUDIO"
            textFormat: Text.PlainText
            width: parent.width - 28
            elide: Text.ElideRight
            color: "#8EAAA7"
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 9
        }
        Canvas {
            id: waveform
            objectName: "deck" + surface.deckIndex + "Waveform"
            x: 14
            y: parent.height - height - 24
            width: parent.width - 28
            height: surface.compactDisplay ? 31 : 47 + Math.max(0, display.height - 250)
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = "#304545"
                ctx.lineWidth = 1
                ctx.beginPath()
                ctx.moveTo(0, height / 2)
                ctx.lineTo(width, height / 2)
                ctx.stroke()
                const values = surface.cachedWaveform
                if (!values.length)
                    return
                const step = width / values.length
                for (let index = 0; index < values.length; ++index) {
                    const peak = Math.max(0, Math.min(1, Number(values[index]) || 0))
                    const bar = peak * (height - 6)
                    ctx.fillStyle = surface.showingPreparation && index / values.length >= surface.loadProgress
                                    ? "#52605E" : surface.displayAccent
                    if (bar > 0)
                        ctx.fillRect(index * step, (height - bar) / 2, Math.max(1, step * 0.72), bar)
                }
            }
        }
        Slider {
            id: seek
            objectName: "deck" + surface.deckIndex + "Seek"
            x: 10
            y: waveform.y - 4
            width: parent.width - 20
            height: waveform.height + (surface.compactDisplay ? 13 : 8)
            leftPadding: 4
            rightPadding: 4
            from: 0
            to: 0.001
            stepSize: 0.1
            enabled: surface.canControl
            visible: !surface.showingPreparation
            pressed: seekInput.gestureActive && seekInput.pressed
            Accessible.name: surface.deckName + " overview position"
            Accessible.description: "Seek within the loaded track. Overview peaks are normalized to that source."
            property bool inputReady: false
            property bool restoringAcknowledgement: false
            property real acknowledgedValue: 0
            function refreshAccepted() {
                const current = surface.controller.decks[surface.deckIndex] || ({})
                const duration = Number(current.durationSeconds)
                const position = Number(current.positionSeconds)
                const previousGuard = restoringAcknowledgement
                restoringAcknowledgement = true
                // Keep range clamps and source replacement in the same guarded
                // snapshot; neither is an input request.
                to = Number.isFinite(duration) && duration > 0 ? duration : 0.001
                acknowledgedValue = Number.isFinite(position) ? Math.max(0, Math.min(to, position)) : 0
                if (!seekInput.gestureActive)
                    value = acknowledgedValue
                restoringAcknowledgement = previousGuard
            }
            onValueChanged: {
                // Custom pointer previews have one separate release commit.
                if (!inputReady || restoringAcknowledgement || seekInput.gestureActive)
                    return
                const candidate = value
                restoringAcknowledgement = true
                if (surface.canControl && Number.isFinite(candidate)
                    && Math.abs(candidate - acknowledgedValue) > 1e-9)
                    surface.controller.seekSeconds(surface.deckIndex, candidate)
                refreshAccepted()
                restoringAcknowledgement = false
            }
            onActiveFocusChanged: { if (!activeFocus && seekInput) seekInput.gestureActive = false }
            Component.onCompleted: { inputReady = true; refreshAccepted() }
            MouseArea {
                id: seekInput
                objectName: "deck" + surface.deckIndex + "SeekInput"
                anchors.fill: parent
                z: 1
                enabled: seek.enabled
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                Accessible.ignored: true
                property bool gestureActive: false
                property string gestureSource: ""
                onGestureActiveChanged: { if (!gestureActive) seek.refreshAccepted() }
                function valueAtPointer(pointerX) {
                    const span = Math.max(1, seek.availableWidth - seek.handle.width)
                    let position = Math.max(0, Math.min(1,
                        (pointerX - seek.leftPadding - seek.handle.width / 2) / span))
                    if (seek.mirrored)
                        position = 1 - position
                    return Math.max(seek.from, Math.min(seek.to, seek.valueAt(position)))
                }
                onPressed: mouse => {
                    seek.forceActiveFocus(Qt.MouseFocusReason)
                    gestureSource = surface.sourceId
                    gestureActive = true
                    seek.value = valueAtPointer(mouse.x)
                }
                onPositionChanged: mouse => {
                    if (pressed && gestureActive)
                        seek.value = valueAtPointer(mouse.x)
                }
                onReleased: mouse => {
                    const seconds = valueAtPointer(mouse.x)
                    const commit = gestureActive && surface.canControl && gestureSource === surface.sourceId
                    gestureActive = false
                    if (commit)
                        surface.controller.seekSeconds(surface.deckIndex, seconds)
                }
                onCanceled: gestureActive = false
                onEnabledChanged: { if (!enabled) gestureActive = false }
                onVisibleChanged: { if (!visible) gestureActive = false }
                Connections {
                    target: surface
                    function onSourceIdChanged() { seekInput.gestureActive = false }
                }
            }
            background: Rectangle {
                x: seek.leftPadding
                y: 1
                width: seek.availableWidth
                height: seek.height - 2
                color: "transparent"
                radius: 2
                border.color: seek.activeFocus ? surface.deckColor : "transparent"
                border.width: 2
            }
            handle: Rectangle {
                x: seek.leftPadding + seek.visualPosition * (seek.availableWidth - width)
                y: 4
                width: 3
                height: seek.height - 8
                color: surface.canControl ? "#F4FBF6" : "#596664"
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 0; width: 9; height: 3; color: parent.color }
            }
        }

        Item {
            objectName: "deck" + surface.deckIndex + "LoadFeedback"
            x: 14
            y: parent.height - 20
            width: parent.width - 28
            height: 15
            visible: surface.showingPreparation
            Accessible.role: Accessible.ProgressBar
            Accessible.name: surface.deckName + " preparing " + surface.trackTitle
            Accessible.description: (surface.deck.loadPhaseText || "Preparing audio")
                                    + ". Completed stages: " + (surface.deck.loadCompletedStages || 0)
                                    + " of " + (surface.deck.loadTotalStages || 0)
            Rectangle {
                y: 0
                width: parent.width
                height: 3
                color: "#344240"
                Rectangle {
                    objectName: "deck" + surface.deckIndex + "LoadProgress"
                    width: parent.width * surface.loadProgress
                    height: parent.height
                    color: surface.displayAccent
                }
            }
            Label {
                objectName: "deck" + surface.deckIndex + "LoadStatus"
                y: 4
                width: parent.width - 16
                height: 11
                text: surface.deck.loadPhase === "cancel" ? "CANCELLING…"
                      : (surface.deck.loadCompletedStages || 0) + "/" + (surface.deck.loadTotalStages || 0)
                        + " STAGES · " + (surface.deck.loadPhaseText || "Preparing audio")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: "#B5CDC9"
                font.family: Brand.ConfluxTheme.monoFont
                font.pixelSize: 8
            }
            BusyIndicator {
                anchors.right: parent.right
                y: 3
                width: 12
                height: 12
                running: surface.showingPreparation && surface.visible
            }
        }

    }

    Column {
        x: surface.width - surface.edge - surface.sideWidth
        y: 43
        width: surface.sideWidth
        spacing: 7
        DeckButton {
            focusColor: surface.deckColor
            anchors.horizontalCenter: parent.horizontalCenter
            width: 54
            height: 54
            round: true
            text: "Browse"
            displayText: "≡"
            fontPixelSize: 27
            buttonObjectName: "deck" + surface.deckIndex + "Browse"
            accessibleName: "Browse " + surface.deckName
            onClicked: surface.browseRequested(surface.deckIndex)
        }
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "BROWSE"; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 11; color: Brand.ConfluxTheme.text }
        DeckButton {
            focusColor: surface.deckColor
            width: parent.width
            height: 44
            text: "LOAD"
            accessibleName: "Choose a file for " + surface.deckName
            onClicked: surface.browseRequested(surface.deckIndex)
        }
        DeckButton {
            focusColor: surface.deckColor
            width: parent.width
            height: 34
            text: "SOURCE"
            available: false
            unavailableReason: "Source switching is not implemented. Use Browse for a local file."
        }
    }

    Label {
        x: surface.edge
        y: surface.hotCueTop - 16
        text: surface.deck.stemBank ? "STEM LEVELS" : "HOT CUE"
        font.family: Brand.ConfluxTheme.displayFont
        font.pixelSize: 10
        font.letterSpacing: 1
        color: Brand.ConfluxTheme.muted
    }
    Row {
        x: surface.edge
        y: surface.hotCueTop
        visible: Boolean(surface.deck.loaded) && Boolean(surface.deck.stemBank)
        spacing: 5
        Repeater {
            model: ["DRUMS", "BASS", "OTHER", "VOCALS"]
            Slider {
                id: stemLevel
                required property int index
                required property string modelData
                objectName: "deck" + surface.deckIndex + "Stem" + index + "Gain"
                width: (surface.width - 2 * surface.edge - 15) / 4
                height: 44
                leftPadding: 7
                rightPadding: 7
                topPadding: 0
                bottomPadding: 0
                from: 0
                to: 1
                stepSize: 0.01
                enabled: surface.canControl
                Accessible.name: surface.deckName + " " + modelData.toLowerCase() + " stem gain"
                Accessible.description: "Level from zero to one. Muted stems keep the shared track position."
                property bool inputReady: false
                property bool restoringAcknowledgement: false
                property real acknowledgedValue: 1
                function refreshAccepted() {
                    const current = surface.controller.decks[surface.deckIndex] || ({})
                    const gains = current.stemGains || []
                    const gain = gains.length === 4 ? Number(gains[index]) : 1
                    const previousGuard = restoringAcknowledgement
                    restoringAcknowledgement = true
                    acknowledgedValue = Number.isFinite(gain) ? Math.max(0, Math.min(1, gain)) : 1
                    if (!pressed)
                        value = acknowledgedValue
                    restoringAcknowledgement = previousGuard
                }
                onValueChanged: {
                    if (!inputReady || restoringAcknowledgement)
                        return
                    const candidate = value
                    restoringAcknowledgement = true
                    if (surface.canControl && surface.deck.stemBank && Number.isFinite(candidate)
                        && (pressed || Math.abs(candidate - acknowledgedValue) > 1e-9))
                        surface.controller.setStemGain(surface.deckIndex, index, candidate)
                    refreshAccepted()
                    restoringAcknowledgement = false
                }
                onPressedChanged: { if (!pressed) refreshAccepted() }
                Component.onCompleted: { inputReady = true; refreshAccepted() }
                Connections {
                    target: surface.controller
                    function onStateChanged() { stemLevel.refreshAccepted() }
                }
                background: Item {
                    x: stemLevel.leftPadding
                    width: stemLevel.availableWidth
                    height: stemLevel.height
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 1
                        text: stemLevel.modelData + " " + Math.round(stemLevel.value * 100) + "%"
                        font.family: Brand.ConfluxTheme.monoFont
                        font.pixelSize: 9
                        color: stemLevel.enabled ? Brand.ConfluxTheme.text : Brand.ConfluxTheme.muted
                    }
                    Rectangle {
                        y: 30
                        width: parent.width
                        height: 4
                        color: Brand.ConfluxTheme.divider
                        Rectangle {
                            width: stemLevel.visualPosition * parent.width
                            height: parent.height
                            color: surface.deckColor
                            opacity: stemLevel.enabled ? 1 : 0.4
                        }
                    }
                }
                handle: Rectangle {
                    x: stemLevel.leftPadding + stemLevel.visualPosition * (stemLevel.availableWidth - width)
                    y: 23
                    width: 10
                    height: 18
                    radius: 2
                    color: Brand.ConfluxTheme.raised
                    border.width: 2
                    border.color: stemLevel.activeFocus ? surface.deckColor : surface.deckColor
                    opacity: stemLevel.enabled ? 1 : 0.4
                }
            }
        }
    }
    Row {
        x: surface.edge
        y: surface.hotCueTop
        visible: !Boolean(surface.deck.stemBank)
        spacing: 5
        Repeater {
            model: 8
            DeckButton {
                focusColor: surface.deckColor
                required property int index
                width: (surface.width - 2 * surface.edge - 35) / 8
                height: 36
                text: String.fromCharCode(65 + index)
                indicatorColor: surface.deckColor
                available: false
                unavailableReason: "Hot cues are not implemented in this milestone."
            }
        }
    }
    Row {
        x: surface.edge
        y: surface.loopTop
        spacing: 5
        DeckButton {
            focusColor: surface.deckColor
            width: 44; height: 36; text: "IN"
            buttonObjectName: "deck" + surface.deckIndex + "LoopIn"
            accessibleName: surface.deckName + " loop IN"
            indicatorColor: Brand.ConfluxTheme.warning
            indicatorOn: Boolean(surface.loop.inSet)
            available: surface.canLoop && surface.deck.positionSeconds < surface.deck.durationSeconds
            actionDescription: "Capture the loop start at the current audio position and prepare up to 64 seconds."
            unavailableReason: "Manual loops require ready prepared audio and a position before the track ends."
            onPressed: surface.controller.loopIn(surface.deckIndex)
        }
        DeckButton {
            focusColor: surface.deckColor
            width: 44; height: 36; text: "OUT"
            buttonObjectName: "deck" + surface.deckIndex + "LoopOut"
            accessibleName: surface.deckName + " loop OUT"
            indicatorColor: Brand.ConfluxTheme.warning
            indicatorOn: Boolean(surface.loop.outSet)
            available: surface.canLoop && surface.deck.playing && Boolean(surface.loop.ready)
                       && !surface.loop.outSet && surface.deck.positionSeconds >= surface.loop.minimumOutSeconds
                       && surface.deck.positionSeconds <= surface.loop.limitSeconds
            actionDescription: "Capture the current audio position as the loop end and repeat from IN."
            unavailableReason: "Set IN, wait for preparation, then press OUT during playback within the prepared range."
            onPressed: surface.controller.loopOut(surface.deckIndex)
        }
        DeckButton {
            focusColor: surface.deckColor
            width: 62; height: 36; text: "RELOOP"
            displayText: surface.loop.active ? "EXIT" : "RELOOP"
            buttonObjectName: "deck" + surface.deckIndex + "LoopToggle"
            accessibleName: surface.deckName + (surface.loop.active ? " loop EXIT" : " RELOOP")
            indicatorColor: Brand.ConfluxTheme.warning
            indicatorOn: Boolean(surface.loop.active)
            available: surface.canLoop && Boolean(surface.loop.ready) && Boolean(surface.loop.outSet)
            actionDescription: surface.loop.active ? "Exit the loop and continue past OUT without jumping."
                                                   : "Start the stored loop again from IN."
            unavailableReason: "Set a prepared IN and OUT loop before using RELOOP or EXIT."
            onPressed: surface.controller.loopToggle(surface.deckIndex)
        }
        DeckButton { focusColor: surface.deckColor; width: 45; height: 36; text: "CALL ◀"; fontPixelSize: 9; available: false; unavailableReason: "Memory cue navigation is not implemented." }
        DeckButton { focusColor: surface.deckColor; width: 45; height: 36; text: "CALL ▶"; fontPixelSize: 9; available: false; unavailableReason: "Memory cue navigation is not implemented." }
    }
    Label {
        objectName: "deck" + surface.deckIndex + "LoopStatus"
        x: surface.edge
        y: surface.loopTop + 39
        width: surface.width - 2 * surface.edge - 75
        height: 15
        text: surface.tempoState.active ? "LOOP / RESET TEMPO FIRST"
              : surface.loop.errorCode ? "LOOP CACHE ERROR / RELOAD OR SET IN"
              : !surface.loop.supported ? "MANUAL LOOP / PREPARED AUDIO"
              : !surface.loop.inSet ? "MANUAL LOOP / SET IN"
              : !surface.loop.ready ? "LOOP / PREPARING IN"
              : surface.loop.outSet ? (surface.loop.active ? "LOOP " : "RELOOP ")
                    + surface.timeText(surface.loop.inSeconds) + " – " + surface.timeText(surface.loop.outSeconds)
              : surface.deck.positionSeconds > surface.loop.limitSeconds ? "IN RANGE ENDED / SET IN AGAIN"
              : surface.deck.positionSeconds < surface.loop.minimumOutSeconds ? "IN SET / WAIT FOR OUT"
              : "IN " + surface.timeText(surface.loop.inSeconds) + " / OUT READY"
        color: surface.loop.active ? Brand.ConfluxTheme.warning : Brand.ConfluxTheme.muted
        font.family: Brand.ConfluxTheme.monoFont
        font.pixelSize: 9
        elide: Text.ElideRight
        Accessible.name: surface.deckName + " " + text
    }
    DeckButton {
        focusColor: surface.deckColor
        x: surface.width - surface.edge - 66
        y: surface.loopTop
        width: 66
        height: 36
        text: "MASTER\nTEMPO"
        buttonObjectName: "deck" + surface.deckIndex + "MasterTempo"
        accessibleName: surface.deckName + " Master Tempo"
        fontPixelSize: 9
        indicatorColor: Brand.ConfluxTheme.warning
        indicatorOn: Boolean(surface.tempoState.keyLock)
        available: surface.canTempo
        actionDescription: "On: changing BPM keeps the original pitch. Off: faster playback raises pitch and slower playback lowers it."
        toolTipText: "MASTER TEMPO · " + (surface.tempoState.keyLock ? "ON: pitch stays the same as BPM changes."
                                                                      : "OFF: pitch rises or falls with BPM. Press to preserve pitch.")
        unavailableReason: "Master Tempo requires ready prepared audio without a cue preview, read error or stored loop."
        onPressed: surface.controller.toggleMasterTempo(surface.deckIndex)
    }

    Item {
        id: jog
        objectName: "deck" + surface.deckIndex + "Jog"
        x: (surface.width - width) / 2
        y: surface.jogTop
        width: surface.jogDiameter
        height: width
        enabled: surface.canControl
        activeFocusOnTab: true
        readonly property var hostWindow: jog.Window.window
        readonly property real value: Number(surface.deck.positionSeconds) || 0
        readonly property real minimumValue: 0
        readonly property real maximumValue: Number(surface.deck.durationSeconds) || 0
        readonly property real stepSize: 0.01
        Accessible.role: Accessible.Dial
        Accessible.focusable: true
        Accessible.name: surface.deckName + " vinyl scratch and jog search"
        Accessible.description: (surface.canScratch
            ? "Hold to stop the prepared audio; drag clockwise or counterclockwise to scratch at two source seconds per turn. Release to restore the previous play or pause request."
            : surface.scratchUnavailableReason)
            + " Arrow keys or wheel remain track search: ten milliseconds, or one second with Shift."
        Accessible.onIncreaseAction: jog.step(480)
        Accessible.onDecreaseAction: jog.step(-480)
        function step(frames) {
            // A search action terminates a held pointer transaction first.
            jogInput.finishScratch()
            return surface.canControl && surface.controller.jogFrames(surface.deckIndex, frames)
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Escape && jogInput.scratchToken.length > 0) {
                jogInput.finishScratch()
                event.accepted = true
                return
            }
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                return
            let direction = 0
            if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
                direction = 1
            else if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
                direction = -1
            if (direction) {
                jog.step(direction * ((event.modifiers & Qt.ShiftModifier) ? 48000 : 480))
                event.accepted = true
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            border.width: 1
            border.color: Brand.ConfluxTheme.dark ? "#77828B" : "#697276"
            gradient: Gradient {
                GradientStop { position: 0; color: Brand.ConfluxTheme.dark ? "#68727C" : "#CDD0CC" }
                GradientStop { position: 0.28; color: Brand.ConfluxTheme.dark ? "#272E36" : "#7E878C" }
                GradientStop { position: 0.72; color: Brand.ConfluxTheme.dark ? "#14191F" : "#9AA1A0" }
                GradientStop { position: 1; color: Brand.ConfluxTheme.dark ? "#68737E" : "#DBDED8" }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 3
            border.color: jog.activeFocus ? surface.deckColor : "transparent"
        }
        Canvas {
            id: knurl
            anchors.fill: parent
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const radius = width / 2
                ctx.strokeStyle = Brand.ConfluxTheme.dark ? "#14191F" : "#5A6268"
                ctx.lineWidth = Math.max(2, width / 110)
                for (let index = 0; index < 72; ++index) {
                    const angle = index * Math.PI * 2 / 72
                    ctx.beginPath()
                    ctx.moveTo(radius + Math.cos(angle) * radius * 0.91, radius + Math.sin(angle) * radius * 0.91)
                    ctx.lineTo(radius + Math.cos(angle) * radius * 0.985, radius + Math.sin(angle) * radius * 0.985)
                    ctx.stroke()
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: parent.width * 0.065
            radius: width / 2
            border.color: "#76838A"
            border.width: 2
            gradient: Gradient {
                GradientStop { position: 0; color: "#252B31" }
                GradientStop { position: 0.45; color: "#0B0F14" }
                GradientStop { position: 1; color: "#151B22" }
            }
            Rectangle { anchors.fill: parent; anchors.margins: 11; radius: width / 2; color: "transparent"; border.color: "#2A353B" }
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.37
            height: width
            radius: width / 2
            color: "#071014"
            border.color: surface.displayAccent
            border.width: surface.deck.playing ? 4 : 2
            Column {
                anchors.centerIn: parent
                spacing: 4
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: surface.deckName.toUpperCase(); color: surface.displayAccent; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 12 }
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: surface.deck.loaded ? surface.timeText(surface.deck.positionSeconds) : "—:—"; color: "#EFF8F6"; font.family: Brand.ConfluxTheme.monoFont; font.pixelSize: jog.width >= 330 ? 23 : 17 }
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: surface.transportStatus; color: "#9DB1B0"; font.family: Brand.ConfluxTheme.monoFont; font.pixelSize: 8 }
            }
        }
        MouseArea {
            id: jogInput
            objectName: "deck" + surface.deckIndex + "ScratchInput"
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            property string scratchToken: ""
            property string gestureSourceId: ""
            property string gestureSourceGeneration: ""
            property bool dragging: false
            property bool angleValid: false
            property real previousAngle: 0
            property real cumulativeFrames: 0
            property real lastSentFrames: 0
            function radiusAt(x, y) { return Math.hypot(x - width / 2, y - height / 2) }
            function ownerCurrent() {
                const current = surface.controller.decks[surface.deckIndex] || ({})
                return gestureSourceId === String(current.sourceId || "")
                    && gestureSourceGeneration === String(current.sourceGeneration || "0")
            }
            function finishScratch() {
                const token = scratchToken
                // Clear before invoking a synchronous controller notification.
                scratchToken = ""
                dragging = false
                angleValid = false
                cumulativeFrames = 0
                lastSentFrames = 0
                gestureSourceId = ""
                gestureSourceGeneration = ""
                if (token.length > 0)
                    surface.controller.endScratch(surface.deckIndex, token)
                // Queue-full release retry belongs to the source/token-bound controller.
            }
            function moveAt(x, y) {
                if (!dragging || scratchToken.length === 0)
                    return
                if (!ownerCurrent() || !surface.canScratch) {
                    finishScratch()
                    return
                }
                if (radiusAt(x, y) < width * 0.15) {
                    angleValid = false
                    return
                }
                const next = Math.atan2(y - height / 2, x - width / 2)
                let angle = next - previousAngle
                previousAngle = next
                if (!angleValid) {
                    angleValid = true
                    return
                }
                if (angle > Math.PI)
                    angle -= 2 * Math.PI
                else if (angle < -Math.PI)
                    angle += 2 * Math.PI
                cumulativeFrames += angle * 96000 / (2 * Math.PI)
                const frames = Math.trunc(cumulativeFrames)
                if (!Number.isSafeInteger(frames)) {
                    finishScratch()
                    return
                }
                if (frames === lastSentFrames)
                    return
                lastSentFrames = frames
                if (!surface.controller.moveScratch(surface.deckIndex, scratchToken, frames))
                    finishScratch()
            }
            onPressed: mouse => {
                if (!surface.canScratch || radiusAt(mouse.x, mouse.y) > width / 2) {
                    mouse.accepted = false
                    return
                }
                jog.forceActiveFocus(Qt.MouseFocusReason)
                const current = surface.controller.decks[surface.deckIndex] || ({})
                const source = String(current.sourceId || "")
                const generation = String(current.sourceGeneration || "0")
                const angle = Math.atan2(mouse.y - height / 2, mouse.x - width / 2)
                const validAngle = radiusAt(mouse.x, mouse.y) >= width * 0.15
                const token = surface.controller.beginScratch(surface.deckIndex)
                if (typeof token !== "string" || token.length === 0) {
                    mouse.accepted = false
                    return
                }
                scratchToken = token
                gestureSourceId = source
                gestureSourceGeneration = generation
                dragging = true
                cumulativeFrames = 0
                lastSentFrames = 0
                previousAngle = angle
                angleValid = validAngle
                // Begin may synchronously replace/disable the source or release focus.
                if (!pressed || !surface.canScratch || !ownerCurrent() || !jog.activeFocus
                    || !jog.hostWindow || !jog.hostWindow.active)
                    finishScratch()
            }
            onPositionChanged: mouse => { if (pressed) moveAt(mouse.x, mouse.y) }
            onReleased: mouse => { moveAt(mouse.x, mouse.y); finishScratch() }
            onCanceled: finishScratch()
            onEnabledChanged: { if (!enabled) finishScratch() }
            onVisibleChanged: { if (!visible) finishScratch() }
            Component.onDestruction: finishScratch()
            onWheel: wheel => {
                const steps = wheel.pixelDelta.y ? wheel.pixelDelta.y / 40 : wheel.angleDelta.y / 120
                if (!steps) {
                    wheel.accepted = false
                    return
                }
                jog.forceActiveFocus(Qt.MouseFocusReason)
                const frames = Math.round(steps * ((wheel.modifiers & Qt.ShiftModifier) ? 48000 : 480))
                if (frames)
                    jog.step(frames)
                wheel.accepted = true
            }
        }
        onActiveFocusChanged: { if (!activeFocus) jogInput.finishScratch() }
        onHostWindowChanged: { if ((!hostWindow || !hostWindow.active) && jogInput) jogInput.finishScratch() }
        Connections {
            target: jog.hostWindow
            function onActiveChanged() { if (!jog.hostWindow || !jog.hostWindow.active) jogInput.finishScratch() }
            function onVisibleChanged() { if (!jog.hostWindow || !jog.hostWindow.visible) jogInput.finishScratch() }
        }
        Connections {
            target: surface.controller
            function onStateChanged() {
                if (jogInput.scratchToken.length > 0 && !jogInput.ownerCurrent())
                    jogInput.finishScratch()
            }
        }
        ToolTip.visible: jogInput.containsMouse && !jogInput.pressed
        ToolTip.delay: 650
        ToolTip.text: surface.canScratch
            ? "VINYL · Hold to stop · Drag either direction · Release to resume · Arrows/wheel: search"
            : "VINYL UNAVAILABLE · " + surface.scratchUnavailableReason + " Arrows/wheel: search"
    }

    Column {
        x: surface.edge
        y: surface.height - 151
        spacing: 10
        DeckButton {
            focusColor: surface.deckColor
            id: cue
            property bool acceptedCue: false
            width: 56
            height: 56
            round: true
            text: "CUE"
            buttonObjectName: "deck" + surface.deckIndex + "Cue"
            accessibleName: "Cue " + surface.deckName
            holdAction: true
            indicatorColor: Brand.ConfluxTheme.warning
            indicatorOn: surface.canControl && (Boolean(surface.deck.cueHeld) || Boolean(surface.deck.cuePreview)
                         || (!surface.deck.playing && Math.abs(Number(surface.deck.positionSeconds)
                                                               - Number(surface.deck.cueSeconds)) < 1 / 48000))
            available: surface.canControl
            unavailableReason: "Load a track and wait for audio output to use CUE."
            actionDescription: "Cue point " + surface.timeText(surface.deck.cueSeconds)
                               + ". Set while paused; return while playing. Hold to preview; release to return."
            function releaseCue() {
                if (!acceptedCue)
                    return
                acceptedCue = false
                surface.controller.cueRelease(surface.deckIndex)
            }
            onPressed: {
                if (acceptedCue || !surface.canControl)
                    return
                if (!surface.controller.cuePress(surface.deckIndex))
                    return
                acceptedCue = true
                // A synchronous controller update may have disabled/canceled
                // the physical press before cuePress returned its acceptance.
                if (!held || !surface.canControl)
                    releaseCue()
            }
            onReleased: releaseCue()
            onCanceled: releaseCue()
        }
        DeckButton {
            focusColor: surface.deckColor
            width: 56
            height: 56
            round: true
            text: surface.deck.playing ? "Pause" : "Play"
            displayText: surface.deck.playing ? "Ⅱ" : "▶"
            fontPixelSize: 22
            buttonObjectName: "deck" + surface.deckIndex + "Play"
            accessibleName: text + " " + surface.deckName
            indicatorColor: Brand.ConfluxTheme.success
            indicatorOn: Boolean(surface.deck.playing)
            available: surface.canControl
            unavailableReason: "Load a track and wait for audio output to use playback."
            onPressed: surface.controller.setPlaying(surface.deckIndex, !surface.deck.playing)
        }
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "PLAY / PAUSE"; color: Brand.ConfluxTheme.muted; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 9 }
    }

    Item {
        id: tempoRail
        x: surface.width - surface.edge - 48
        y: surface.jogTop + 5
        width: 48
        height: Math.max(180, surface.height - y - 53)
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "BPM"; color: Brand.ConfluxTheme.muted; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 10 }
        Slider {
            id: tempo
            objectName: "deck" + surface.deckIndex + "Tempo"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 25
            width: 44
            height: parent.height - 112
            orientation: Qt.Vertical
            // A descending range gives the CDJ's downward-positive native drag.
            from: 1
            to: 0
            // Qt 6.4's accessibility bridge reads these range properties.
            readonly property real minimumValue: Math.min(from, to)
            readonly property real maximumValue: Math.max(from, to)
            stepSize: .1
            live: true
            focusPolicy: Qt.StrongFocus
            enabled: surface.canTempo && surface.hasReferenceBpm
            opacity: enabled ? 1 : .48
            Accessible.name: surface.deckName + " tempo BPM"
            Accessible.description: "BPM within six percent of the reference. Down increases speed; up decreases it. Arrows change 0.1 BPM, Shift changes 1 BPM. Master Tempo keeps musical pitch; Reset restores reference BPM."
            Accessible.onIncreaseAction: requestStep(.1)
            Accessible.onDecreaseAction: requestStep(-.1)
            property bool restoringAcknowledgement: false
            property real acknowledgedValue: 0
            function sameBpm(a, b) {
                return Number.isFinite(a) && Number.isFinite(b) && Math.abs(a - b) <= 1e-9
            }
            function restoreAccepted() {
                if (pressed)
                    return
                const previousGuard = restoringAcknowledgement
                restoringAcknowledgement = true
                value = acknowledgedValue
                restoringAcknowledgement = previousGuard
            }
            function requestBpm(bpm) {
                if (surface.canTempo && surface.hasReferenceBpm && Number.isFinite(bpm))
                    surface.controller.setTempoBpm(surface.deckIndex, Math.max(minimumValue, Math.min(maximumValue, bpm)))
            }
            function requestStep(delta) {
                requestBpm(acknowledgedValue + delta)
            }
            Keys.priority: Keys.BeforeItem
            Keys.onPressed: event => {
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                    return
                const step = (event.modifiers & Qt.ShiftModifier) ? 1 : .1
                if (event.key === Qt.Key_Down || event.key === Qt.Key_Right)
                    requestStep(step)
                else if (event.key === Qt.Key_Up || event.key === Qt.Key_Left)
                    requestStep(-step)
                else if (event.key === Qt.Key_PageDown)
                    requestStep(1)
                else if (event.key === Qt.Key_PageUp)
                    requestStep(-1)
                else if (event.key === Qt.Key_Home)
                    requestBpm(minimumValue)
                else if (event.key === Qt.Key_End)
                    requestBpm(maximumValue)
                else
                    return
                event.accepted = true
            }
            onValueChanged: {
                if (restoringAcknowledgement)
                    return
                if (!Number.isFinite(value)) {
                    restoreAccepted()
                    return
                }
                if (pressed) {
                    requestBpm(value)
                } else if (!sameBpm(value, acknowledgedValue)) {
                    const candidate = value
                    restoringAcknowledgement = true
                    requestBpm(candidate)
                    value = acknowledgedValue
                    restoringAcknowledgement = false
                }
            }
            onPressedChanged: { if (!pressed) restoreAccepted() }
            background: Item {
                x: 0
                y: tempo.topPadding
                width: tempo.width
                height: tempo.availableHeight
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 5; height: parent.height; color: Brand.ConfluxTheme.dark ? "#080C10" : "#7A8284"; border.color: Brand.ConfluxTheme.divider }
                Repeater {
                    model: 11
                    Rectangle {
                        required property int index
                        x: index === 5 ? 0 : 7
                        y: index * (parent.height - 1) / 10
                        width: index === 5 ? 15 : 8
                        height: 1
                        color: Brand.ConfluxTheme.muted
                    }
                }
            }
            handle: Rectangle {
                x: 3
                y: tempo.topPadding + tempo.visualPosition * (tempo.availableHeight - height)
                width: tempo.width - 6
                height: 19
                radius: 2
                color: Brand.ConfluxTheme.raised
                border.width: tempo.activeFocus ? 2 : 1
                border.color: tempo.activeFocus ? surface.deckColor : Brand.ConfluxTheme.controlBorder
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: parent.height / 2; width: parent.width - 5; height: 1; color: Brand.ConfluxTheme.muted }
            }
        }
        Label {
            objectName: "deck" + surface.deckIndex + "TempoValue"
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height - 84
            text: surface.hasReferenceBpm ? surface.acknowledgedBpm.toFixed(1) : "—"
            textFormat: Text.PlainText
            color: Brand.ConfluxTheme.text
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 9
        }
        Label {
            objectName: "deck" + surface.deckIndex + "TempoStatus"
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height - 68
            text: surface.tempoState.errorCode ? (surface.tempoState.active ? "ERROR / RESET" : "TEMPO ERROR")
                  : !surface.tempoState.supported ? "UNAVAILABLE"
                  : surface.tempoState.pending ? "PENDING"
                  : surface.tempoState.finished ? "FINISHED"
                  : surface.tempoState.active ? "READY" : "ORIGINAL"
            textFormat: Text.PlainText
            color: surface.tempoState.errorCode ? Brand.ConfluxTheme.warning : Brand.ConfluxTheme.muted
            font.family: Brand.ConfluxTheme.displayFont
            font.pixelSize: 8
            Accessible.name: surface.deckName + " tempo " + text
        }
        DeckButton {
            focusColor: surface.deckColor
            x: 0
            y: parent.height - 44
            width: 48
            height: 44
            text: "RESET"
            fontPixelSize: 8
            buttonObjectName: "deck" + surface.deckIndex + "TempoReset"
            accessibleName: surface.deckName + " reset tempo"
            available: surface.canResetTempo
            actionDescription: "Restore original speed and leave processed tempo mode, keeping the Master Tempo selection."
            unavailableReason: "Reset becomes available when prepared tempo processing is active."
            onPressed: surface.controller.resetTempo(surface.deckIndex)
        }
        MouseArea { id: tempoHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
        ToolTip.visible: tempoHover.containsMouse
        ToolTip.delay: 450
        ToolTip.text: surface.hasReferenceBpm ? "BPM ±6% · Down: faster · Up: slower · Arrows: 0.1 BPM · Shift: 1 BPM · Reset restores reference BPM."
                                            : "Set the track's reference BPM in the display to use the BPM rail."
    }

    Popup {
        id: bpmEditor
        objectName: "deck" + surface.deckIndex + "BpmEditor"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 320
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property string openedSourceId: ""
        readonly property bool sameSource: openedSourceId === surface.sourceId
        function openForSource() {
            if (!surface.canEditBpm)
                return
            openedSourceId = surface.sourceId
            open()
        }
        onOpened: {
            bpmInput.text = surface.hasReferenceBpm ? surface.referenceBpm.toFixed(1) : ""
            bpmInput.forceActiveFocus()
            bpmInput.selectAll()
        }
        function applyReference() {
            if (!surface.canEditBpm || !sameSource || !bpmInput.acceptableInput)
                return
            const bpm = Number(bpmInput.text)
            if (!Number.isFinite(bpm) || bpm < 20 || bpm > 400)
                return
            surface.controller.setBpmOverride(surface.deckIndex, bpm)
            close()
        }
        contentItem: Column {
            spacing: 10
            Label { text: surface.deckName + " reference BPM"; font.bold: true }
            Label {
                width: parent.width
                text: "Original-speed BPM. Changing this number does not change the audio or sync decks."
                wrapMode: Text.WordWrap
            }
            Label {
                objectName: "deck" + surface.deckIndex + "BpmEstimate"
                text: Number(surface.bpmState.estimated) > 0 ? "Estimated: " + Number(surface.bpmState.estimated).toFixed(1) + " BPM" : "Estimated: unavailable"
            }
            Label { text: "Manual reference BPM (20–400)" }
            TextField {
                id: bpmInput
                objectName: "deck" + surface.deckIndex + "BpmInput"
                width: parent.width
                height: 44
                selectByMouse: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                validator: DoubleValidator { bottom: 20; top: 400; decimals: 1; notation: DoubleValidator.StandardNotation; locale: "C" }
                Accessible.name: surface.deckName + " manual reference BPM"
                onAccepted: bpmEditor.applyReference()
            }
            Row {
                spacing: 8
                Button {
                    objectName: "deck" + surface.deckIndex + "BpmApply"
                    width: (bpmInput.width - 8) / 2
                    height: 44
                    text: "Apply"
                    enabled: surface.canEditBpm && bpmEditor.sameSource && bpmInput.acceptableInput
                    onClicked: bpmEditor.applyReference()
                }
                Button {
                    objectName: "deck" + surface.deckIndex + "BpmAutomatic"
                    width: (bpmInput.width - 8) / 2
                    height: 44
                    text: "Use automatic"
                    enabled: surface.canEditBpm && bpmEditor.sameSource && Boolean(surface.bpmState.manual)
                    onClicked: {
                        if (!surface.canEditBpm || !bpmEditor.sameSource)
                            return
                        surface.controller.clearBpmOverride(surface.deckIndex)
                        bpmEditor.close()
                    }
                }
            }
            Label {
                width: parent.width
                text: surface.hasReferenceBpm ? "Using " + (surface.bpmState.manual ? "manual" : "estimated") + " reference: " + surface.referenceBpm.toFixed(1) + " BPM" : "Reference BPM is unknown."
                wrapMode: Text.WordWrap
            }
        }
    }
}
