// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "../brand/integration/qtquick" as Brand

Item {
    id: surface
    readonly property string transportStatus: !deck.loaded ? "EMPTY" : deck.sourceReadError ? "READ ERROR"
                                             : deck.buffering ? "BUFFERING" : deck.cuePreview ? "CUE PREVIEW"
                                             : deck.playing ? "PLAYING" : "PAUSED"
    required property int deckIndex
    required property var controller
    readonly property var deck: controller.decks[deckIndex]
    readonly property bool canControl: Boolean(deck.loaded) && controller.outputReady && !deck.replacing
    readonly property string deckName: deckIndex === 0 ? "Deck A" : "Deck B"
    readonly property color deckColor: deckIndex === 0 ? Brand.ConfluxTheme.deckA : Brand.ConfluxTheme.deckB
    readonly property color displayAccent: deckIndex === 0 ? "#58D6C3" : "#F2C568"
    readonly property string trackTitle: deck.loaded ? (deck.title || deck.name) : "Browse a local track"
    readonly property string artworkSource: deck.loaded && typeof deck.artworkDataUrl === "string"
                                             && deck.artworkDataUrl.startsWith("data:image/png;base64,")
                                             ? deck.artworkDataUrl : ""
    readonly property bool compactDisplay: display.height < 232
    readonly property real edge: width >= 560 ? 22 : 16
    readonly property real sideWidth: width >= 560 ? 76 : 66
    readonly property real screenHeight: Math.max(174, Math.min(250, height * 0.28))
    readonly property real hotCueTop: 40 + screenHeight + 20
    readonly property real loopTop: hotCueTop + 52
    readonly property real jogTop: loopTop + 60
    readonly property real jogDiameter: Math.max(220, Math.min(390, width - 152, height - jogTop - 30))
    property var cachedWaveform: []
    signal browseRequested(int deck)
    implicitWidth: 470
    implicitHeight: 670

    function timeText(seconds) {
        const value = Math.max(0, Math.floor(Number(seconds) || 0))
        return Math.floor(value / 60) + ":" + String(value % 60).padStart(2, "0")
    }
    function refreshWaveform() {
        const next = deck.waveform || []
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
    Component.onCompleted: refreshWaveform()
    Connections {
        target: surface.controller
        function onStateChanged() { surface.refreshWaveform() }
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
        x: surface.edge
        y: 13
        text: surface.deckName.toUpperCase() + "  /  LOCAL PLAYER"
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
            text: !surface.deck.loaded ? "NO TRACK LOADED"
                  : surface.deck.overviewStem === "drums" ? "PREPARED STEMS · DRUMS WAVEFORM"
                  : surface.deck.overviewStem === "source" ? "LIVE STEMS · SOURCE WAVEFORM" : "LOCAL FILE"
            color: surface.displayAccent
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 10
            font.letterSpacing: 0.5
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
                    text: surface.deck.loaded ? "NO ART" : "EMPTY"
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
            color: "#EFF8F6"
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
            text: surface.deck.loaded ? (surface.deck.artist || "Artist unknown") : "—"
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
            width: titleLabel.width
            text: surface.deck.loaded ? (surface.deck.album || "Album unknown") : "—"
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
            text: surface.deck.loaded ? surface.timeText(seek.pressed ? seek.value : surface.deck.positionSeconds) : "—:—"
            color: "#F2F8F4"
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: surface.compactDisplay ? 26 : 36
        }
        Column {
            anchors.right: parent.right
            anchors.rightMargin: 14
            y: surface.compactDisplay ? 80 : 101
            spacing: 1
            Label { text: "REMAIN"; color: "#8EAAA7"; font.family: Brand.ConfluxTheme.monoFont; font.pixelSize: 8 }
            Label {
                objectName: "deck" + surface.deckIndex + "Remaining"
                text: surface.deck.loaded ? "−" + surface.timeText(Math.max(0, surface.deck.durationSeconds - surface.deck.positionSeconds)) : "—:—"
                color: surface.displayAccent
                font.family: Brand.ConfluxTheme.monoFont
                font.pixelSize: surface.compactDisplay ? 16 : 21
            }
        }
        Label {
            x: 14
            y: 148
            visible: !surface.compactDisplay
            text: surface.deck.loaded ? surface.transportStatus + "  ·  STEREO 48 kHz" : "LOAD TO PREPARE AUDIO"
            color: "#8EAAA7"
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 9
        }
        Canvas {
            id: waveform
            x: 14
            y: parent.height - (surface.compactDisplay ? 59 : 71)
            width: parent.width - 28
            height: surface.compactDisplay ? 31 : 47
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
                ctx.fillStyle = surface.displayAccent
                for (let index = 0; index < values.length; ++index) {
                    const peak = Math.max(0, Math.min(1, Number(values[index]) || 0))
                    const bar = peak * (height - 6)
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
            height: surface.compactDisplay ? 44 : 55
            leftPadding: 4
            rightPadding: 4
            from: 0
            to: Math.max(0.001, Number(surface.deck.durationSeconds) || 0)
            stepSize: 0.1
            enabled: surface.canControl
            property real pendingSeek: 0
            property bool dragged: false
            Accessible.name: surface.deckName + " overview position"
            Accessible.description: "Seek within the loaded track. Overview peaks are normalized to that source."
            onMoved: {
                pendingSeek = value
                if (pressed)
                    dragged = true
                else if (surface.canControl)
                    surface.controller.seekSeconds(surface.deckIndex, value)
            }
            onPressedChanged: {
                if (pressed) {
                    dragged = false
                } else if (dragged) {
                    if (surface.canControl)
                        surface.controller.seekSeconds(surface.deckIndex, pendingSeek)
                    dragged = false
                }
            }
            Binding {
                target: seek
                property: "value"
                value: Number(surface.deck.positionSeconds) || 0
                when: !seek.pressed
                restoreMode: Binding.RestoreNone
            }
            background: Rectangle {
                x: seek.leftPadding
                y: 1
                width: seek.availableWidth
                height: seek.height - 2
                color: "transparent"
                radius: 2
                border.color: seek.activeFocus ? Brand.ConfluxTheme.primary : "transparent"
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
        Label {
            x: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            text: !surface.cachedWaveform.length ? "NO OVERVIEW"
                  : surface.deck.overviewStem === "drums" ? "OVERVIEW · DRUMS PEAK" : "OVERVIEW · SOURCE PEAK"
            color: "#8EAAA7"
            font.family: Brand.ConfluxTheme.monoFont
            font.pixelSize: 8
        }
    }

    Column {
        x: surface.width - surface.edge - surface.sideWidth
        y: 43
        width: surface.sideWidth
        spacing: 7
        DeckButton {
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
            width: parent.width
            height: 44
            text: "LOAD"
            accessibleName: "Choose a file for " + surface.deckName
            onClicked: surface.browseRequested(surface.deckIndex)
        }
        DeckButton {
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
                onMoved: if (surface.canControl && surface.deck.stemBank)
                             surface.controller.setStemGain(surface.deckIndex, index, value)
                Binding {
                    target: stemLevel
                    property: "value"
                    value: surface.deck.stemGains && surface.deck.stemGains.length === 4
                           ? Number(surface.deck.stemGains[stemLevel.index]) : 1
                    when: !stemLevel.pressed
                    restoreMode: Binding.RestoreNone
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
                    border.color: stemLevel.activeFocus ? Brand.ConfluxTheme.primary : surface.deckColor
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
        DeckButton { width: 44; height: 36; text: "IN"; indicatorColor: Brand.ConfluxTheme.warning; available: false; unavailableReason: "Loop in/out points are not implemented." }
        DeckButton { width: 44; height: 36; text: "OUT"; indicatorColor: Brand.ConfluxTheme.warning; available: false; unavailableReason: "Loop in/out points are not implemented." }
        DeckButton { width: 62; height: 36; text: "RELOOP"; available: false; unavailableReason: "Loop recall is not implemented." }
        DeckButton { width: 45; height: 36; text: "CALL ◀"; fontPixelSize: 9; available: false; unavailableReason: "Memory cue navigation is not implemented." }
        DeckButton { width: 45; height: 36; text: "CALL ▶"; fontPixelSize: 9; available: false; unavailableReason: "Memory cue navigation is not implemented." }
    }
    DeckButton {
        x: surface.width - surface.edge - 66
        y: surface.loopTop
        width: 66
        height: 36
        text: "MASTER\nTEMPO"
        fontPixelSize: 9
        available: false
        unavailableReason: "Pitch-independent tempo adjustment is not implemented."
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
        readonly property real value: Number(surface.deck.positionSeconds) || 0
        readonly property real minimumValue: 0
        readonly property real maximumValue: Number(surface.deck.durationSeconds) || 0
        readonly property real stepSize: 0.01
        Accessible.role: Accessible.Dial
        Accessible.focusable: true
        Accessible.name: surface.deckName + " jog search"
        Accessible.description: "Search the track while keeping its play or pause state. Drag clockwise to move forward, two seconds per turn. Arrow keys or wheel move 10 milliseconds; hold Shift for one second."
        Accessible.onIncreaseAction: jog.step(480)
        Accessible.onDecreaseAction: jog.step(-480)
        function step(frames) {
            return surface.canControl && surface.controller.jogFrames(surface.deckIndex, frames)
        }
        Keys.onPressed: event => {
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
            border.color: jog.activeFocus ? Brand.ConfluxTheme.primary : "transparent"
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
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            property bool dragging: false
            property bool angleValid: false
            property real previousAngle: 0
            property real fractionalFrames: 0
            function radiusAt(x, y) { return Math.hypot(x - width / 2, y - height / 2) }
            function resetDrag() {
                dragging = false
                angleValid = false
                fractionalFrames = 0
            }
            onPressed: mouse => {
                if (radiusAt(mouse.x, mouse.y) > width / 2) {
                    mouse.accepted = false
                    return
                }
                jog.forceActiveFocus(Qt.MouseFocusReason)
                dragging = true
                fractionalFrames = 0
                angleValid = radiusAt(mouse.x, mouse.y) >= width * 0.15
                previousAngle = Math.atan2(mouse.y - height / 2, mouse.x - width / 2)
            }
            onPositionChanged: mouse => {
                if (!pressed || !dragging)
                    return
                if (radiusAt(mouse.x, mouse.y) < width * 0.15) {
                    angleValid = false
                    return
                }
                const next = Math.atan2(mouse.y - height / 2, mouse.x - width / 2)
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
                fractionalFrames += angle * 96000 / (2 * Math.PI)
                const frames = Math.trunc(fractionalFrames)
                fractionalFrames -= frames
                if (frames && !jog.step(frames))
                    resetDrag()
            }
            onReleased: resetDrag()
            onCanceled: resetDrag()
            onEnabledChanged: { if (!enabled) resetDrag() }
            onVisibleChanged: { if (!visible) resetDrag() }
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
        onActiveFocusChanged: { if (!activeFocus) jogInput.resetDrag() }
        ToolTip.visible: jogInput.containsMouse && !jogInput.pressed
        ToolTip.delay: 650
        ToolTip.text: "JOG SEARCH · Drag: 2 s per turn · Arrows/wheel: 10 ms · Shift: 1 s"
    }

    Column {
        x: surface.edge
        y: surface.height - 151
        spacing: 10
        DeckButton {
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
        x: surface.width - surface.edge - 48
        y: surface.jogTop + 5
        width: 48
        height: Math.max(180, surface.height - y - 53)
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "TEMPO"; color: Brand.ConfluxTheme.muted; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 10 }
        Slider {
            id: tempo
            anchors.horizontalCenter: parent.horizontalCenter
            y: 25
            width: 44
            height: parent.height - 48
            orientation: Qt.Vertical
            from: -1
            to: 1
            value: 0
            enabled: false
            opacity: 0.48
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
                border.color: Brand.ConfluxTheme.controlBorder
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: parent.height / 2; width: parent.width - 5; height: 1; color: Brand.ConfluxTheme.muted }
            }
        }
        Label { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; text: "INACTIVE"; color: Brand.ConfluxTheme.muted; font.family: Brand.ConfluxTheme.displayFont; font.pixelSize: 8 }
        MouseArea { id: tempoHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
        ToolTip.visible: tempoHover.containsMouse
        ToolTip.delay: 450
        ToolTip.text: "Tempo adjustment is not implemented in this milestone."
    }
}
