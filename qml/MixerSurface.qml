// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../brand/integration/qtquick" as Brand

Rectangle {
    id: root
    required property var controller
    implicitWidth: 360
    implicitHeight: 670
    radius: 9
    border.color: Brand.ConfluxTheme.dark ? "#51555D" : "#969A9E"
    border.width: 1
    gradient: Gradient {
        GradientStop { position: 0; color: Brand.ConfluxTheme.dark ? "#34383E" : "#ECECE8" }
        GradientStop { position: 0.12; color: Brand.ConfluxTheme.dark ? "#24272C" : "#D9DAD7" }
        GradientStop { position: 0.85; color: Brand.ConfluxTheme.dark ? "#1D2025" : "#CDCFCD" }
        GradientStop { position: 1; color: Brand.ConfluxTheme.dark ? "#30343B" : "#E5E5E1" }
    }
    readonly property color etched: Brand.ConfluxTheme.dark ? "#949AA3" : "#555B63"
    readonly property real smallKnob: height < 760 ? 27 : 34

    // Fine horizontal lines suggest a brushed face without a bitmap texture.
    Repeater {
        model: Math.floor(root.height / 5)
        Rectangle {
            required property int index
            x: 2
            y: index * 5 + 2
            width: root.width - 4
            height: 1
            color: Brand.ConfluxTheme.dark ? "#FFFFFF" : "#525860"
            opacity: 0.025
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 11
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 27
            Text {
                text: "CONFLUX / MIXER"
                color: Brand.ConfluxTheme.text
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 19
                font.weight: Font.DemiBold
                font.letterSpacing: 1
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "4 CHANNEL"
                color: root.etched
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 11
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 390
            spacing: 5

            Repeater {
                model: [
                    { name: "C", channel: 1, audioDeck: -1 },
                    { name: "A", channel: 2, audioDeck: 0 },
                    { name: "B", channel: 3, audioDeck: 1 },
                    { name: "D", channel: 4, audioDeck: -1 }
                ]
                delegate: Rectangle {
                    id: strip
                    required property var modelData
                    readonly property int audioDeck: modelData.audioDeck
                    readonly property var deck: audioDeck >= 0 ? root.controller.decks[audioDeck] : null
                    readonly property bool available: audioDeck >= 0 && Boolean(deck.loaded) && root.controller.outputReady && !deck.replacing
                    readonly property color deckColor: audioDeck === 0 ? Brand.ConfluxTheme.deckA
                                                       : audioDeck === 1 ? Brand.ConfluxTheme.deckB : root.etched
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 40
                    color: Brand.ConfluxTheme.dark ? "#1A1D22" : "#DEDFDB"
                    border.color: Brand.ConfluxTheme.dark ? "#3A3F47" : "#A7ACA9"
                    radius: 3

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 3

                        Column {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "CH " + strip.modelData.channel + " / " + strip.modelData.name
                                font.family: Brand.ConfluxTheme.displayFont
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: strip.deckColor
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: strip.audioDeck < 0 ? "RESERVED" : "DECK " + strip.modelData.name
                                font.family: Brand.ConfluxTheme.displayFont
                                font.pixelSize: 9
                                color: root.etched
                            }
                        }

                        Repeater {
                            model: ["TRIM", "HI", "MID", "LOW", "COLOR"]
                            SurfaceKnob {
                                required property string modelData
                                Layout.fillWidth: true
                                knobDiameter: root.smallKnob
                                caption: modelData
                                helpText: modelData === "TRIM" ? "Input trim is not implemented. Use CHANNEL LEVEL."
                                          : modelData === "COLOR" ? "Channel filter and color effects are not implemented."
                                          : "Channel equalization is not implemented."
                            }
                        }

                        Button {
                            id: headphoneCue
                            objectName: strip.audioDeck >= 0 ? "deck" + strip.audioDeck + "CueSelect" : "reserved" + strip.modelData.name + "CueSelect"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 25
                            text: "CUE"
                            checkable: true
                            checked: strip.audioDeck >= 0 && Boolean(strip.deck.cueSelected)
                            enabled: strip.available
                            focusPolicy: Qt.StrongFocus
                            onClicked: root.controller.setCueSelected(strip.audioDeck, !Boolean(strip.deck.cueSelected))
                            Accessible.name: "Deck " + strip.modelData.name + " headphone cue"
                            Accessible.description: "Select the pre-fader deck for the separate headphone output."
                            contentItem: Text {
                                text: headphoneCue.text
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                color: headphoneCue.checked ? Brand.ConfluxTheme.primaryText : root.etched
                                font.family: Brand.ConfluxTheme.displayFont
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            background: Rectangle {
                                radius: 3
                                color: headphoneCue.checked ? Brand.ConfluxTheme.primary
                                                      : Brand.ConfluxTheme.dark ? "#292D33" : "#CACDCB"
                                border.color: headphoneCue.activeFocus ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
                            }
                            ToolTip.visible: hovered || activeFocus
                            ToolTip.text: strip.audioDeck < 0 ? "Additional decks are not implemented."
                                         : root.controller.audioRouting.cueReady ? "Pre-fader headphone cue for Deck " + strip.modelData.name
                                         : "Select separate headphone outputs in AUDIO / REC. Cue stays silent until routed."
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "LEVEL"
                            horizontalAlignment: Text.AlignHCenter
                            color: Brand.ConfluxTheme.text
                            font.family: Brand.ConfluxTheme.displayFont
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 100
                            spacing: 2
                            Slider {
                                id: levelFader
                                objectName: strip.audioDeck >= 0 ? "deck" + strip.audioDeck + "Gain" : "reserved" + strip.modelData.name + "Gain"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 100
                                orientation: Qt.Vertical
                                from: 0
                                to: 1
                                stepSize: 0.01
                                value: strip.audioDeck >= 0 ? strip.deck.gain : 0
                                enabled: strip.available
                                focusPolicy: Qt.StrongFocus
                                leftPadding: 3
                                rightPadding: 3
                                topPadding: 8
                                bottomPadding: 8
                                Accessible.name: "Deck " + strip.modelData.name + " gain"
                                Accessible.description: strip.audioDeck < 0 ? "Additional decks are not implemented."
                                                           : "Linear channel level, zero to one hundred percent."
                                onMoved: root.controller.setDeckGain(strip.audioDeck, value)
                                ToolTip.visible: hovered || activeFocus
                                ToolTip.text: strip.audioDeck < 0 ? "Additional decks are not implemented."
                                             : !strip.deck.loaded ? "Load Deck " + strip.modelData.name + " to change its level."
                                             : "Channel level " + Math.round(value * 100) + "%"

                                background: Item {
                                    x: levelFader.leftPadding
                                    y: levelFader.topPadding
                                    width: levelFader.availableWidth
                                    height: levelFader.availableHeight
                                    Repeater {
                                        model: 9
                                        Rectangle {
                                            required property int index
                                            x: index % 2 === 0 ? 1 : 5
                                            y: index * (parent.height - 1) / 8
                                            width: parent.width - x * 2
                                            height: 1
                                            color: root.etched
                                            opacity: 0.65
                                        }
                                    }
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 8
                                        height: parent.height
                                        radius: 4
                                        color: Brand.ConfluxTheme.dark ? "#080A0D" : "#777E83"
                                        border.color: Brand.ConfluxTheme.dark ? "#424954" : "#F2F2ED"
                                        Rectangle {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: 2
                                            height: parent.height - 4
                                            y: 2
                                            color: "#050608"
                                        }
                                    }
                                }
                                handle: Rectangle {
                                    x: levelFader.leftPadding + (levelFader.availableWidth - width) / 2
                                    y: levelFader.topPadding + levelFader.visualPosition * (levelFader.availableHeight - height)
                                    width: Math.min(36, levelFader.availableWidth)
                                    height: 25
                                    radius: 3
                                    opacity: strip.available ? 1 : 0.5
                                    border.width: levelFader.activeFocus ? 2 : 1
                                    border.color: levelFader.activeFocus ? Brand.ConfluxTheme.primary : "#777E86"
                                    gradient: Gradient {
                                        GradientStop { position: 0; color: "#E3E5E2" }
                                        GradientStop { position: 0.4; color: "#8D9398" }
                                        GradientStop { position: 0.48; color: "#C9CDCD" }
                                        GradientStop { position: 1; color: "#555D65" }
                                    }
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: parent.width - 5
                                        height: 2
                                        color: "#15191E"
                                    }
                                }
                            }
                            StereoMeter {
                                objectName: strip.audioDeck >= 0 ? "deck" + strip.audioDeck + "Meter" : "reserved" + strip.modelData.name + "Meter"
                                Layout.preferredWidth: 17
                                Layout.fillHeight: true
                                meter: strip.audioDeck >= 0 ? strip.deck.meter : ({})
                                active: strip.available
                                caption: "Deck " + strip.modelData.name + " pre-fader peak and RMS"
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 17
                            text: strip.audioDeck < 0 ? "UNAVAILABLE" : Math.round(strip.deck.gain * 100) + "%"
                            horizontalAlignment: Text.AlignHCenter
                            color: strip.audioDeck < 0 ? root.etched : strip.deckColor
                            font.family: Brand.ConfluxTheme.displayFont
                            font.pixelSize: strip.audioDeck < 0 ? 9 : 14
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 65
                Layout.fillHeight: true
                spacing: 9
                SurfaceKnob {
                    Layout.fillWidth: true
                    knobDiameter: 46
                    caption: "MASTER LEVEL"
                    dialObjectName: "masterGain"
                    activeControl: root.controller.outputReady
                    value: root.controller.masterGain
                    indicatorColor: Brand.ConfluxTheme.primary
                    helpText: "Linear master level, zero to one hundred percent."
                    onMoved: function(newValue) { root.controller.setMasterGain(newValue) }
                }
                Text {
                    Layout.fillWidth: true
                    text: Math.round(root.controller.masterGain * 100) + "%"
                    horizontalAlignment: Text.AlignHCenter
                    color: Brand.ConfluxTheme.text
                    font.family: Brand.ConfluxTheme.monoFont
                    font.pixelSize: 13
                }
                Text {
                    Layout.fillWidth: true
                    text: "MASTER"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 11
                    color: root.etched
                }
                StereoMeter {
                    objectName: "masterMeter"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 70
                    meter: root.controller.meters.master
                    active: root.controller.outputReady
                    caption: "Post-gain master peak and RMS"
                }
                SurfaceKnob {
                    Layout.fillWidth: true
                    knobDiameter: 34
                    caption: "CUE LEVEL"
                    dialObjectName: "cueGain"
                    activeControl: root.controller.outputReady
                    value: root.controller.cueGain
                    indicatorColor: Brand.ConfluxTheme.primary
                    helpText: "Separate headphone level " + Math.round(root.controller.cueGain * 100) + "%. No cue audio is sent to the master output."
                    onMoved: function(newValue) { root.controller.setCueGain(newValue) }
                }
                StereoMeter {
                    objectName: "cueMeter"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 70
                    meter: root.controller.meters.cue
                    active: root.controller.audioRouting.cueReady
                    caption: "Post-gain headphone cue peak and RMS"
                }
                Text {
                    Layout.fillWidth: true
                    text: root.controller.audioRouting.cueReady ? "CUE ROUTED" : "CUE UNROUTED"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 11
                    color: root.controller.audioRouting.cueReady ? Brand.ConfluxTheme.success : root.etched
                }
                Item { Layout.fillHeight: true }
                Text {
                    Layout.fillWidth: true
                    text: "BEAT FX\nINACTIVE"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 11
                    color: root.etched
                }
            }
        }

        Slider {
            id: crossfader
            objectName: "crossfader"
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            from: 0
            to: 1
            stepSize: 0.01
            live: true
            leftPadding: 21
            rightPadding: 21
            topPadding: 24
            bottomPadding: 11
            enabled: root.controller.outputReady
            focusPolicy: Qt.StrongFocus
            Accessible.name: "Crossfader A to B"
            Accessible.description: "Left plays Deck A only; right plays Deck B only. Center keeps both channel levels unchanged. Headphone cue is independent."
            onMoved: root.controller.setCrossfader(value)
            Binding {
                target: crossfader
                property: "value"
                value: root.controller.crossfader
                when: !crossfader.pressed
                restoreMode: Binding.RestoreNone
            }
            background: Rectangle {
                width: crossfader.width
                height: crossfader.height
                color: Brand.ConfluxTheme.dark ? "#171A1F" : "#D8DAD7"
                border.color: crossfader.activeFocus ? Brand.ConfluxTheme.primary
                                                     : Brand.ConfluxTheme.dark ? "#414750" : "#A7ACA9"
                border.width: crossfader.activeFocus ? 2 : 1
                radius: 3
                Text {
                    anchors.top: parent.top
                    anchors.topMargin: 4
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "A  /  CROSSFADER  /  B"
                    color: crossfader.enabled ? Brand.ConfluxTheme.text : root.etched
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 10
                }
                Rectangle {
                    x: crossfader.leftPadding
                    y: 31
                    width: crossfader.availableWidth
                    height: 6
                    radius: 3
                    color: Brand.ConfluxTheme.dark ? "#05070A" : "#767E84"
                    border.color: Brand.ConfluxTheme.controlBorder
                }
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 26
                    width: 1
                    height: 16
                    color: root.etched
                }
            }
            handle: Rectangle {
                x: crossfader.leftPadding + crossfader.visualPosition * (crossfader.availableWidth - width)
                y: crossfader.topPadding + (crossfader.availableHeight - height) / 2
                width: 22
                height: 19
                radius: 2
                opacity: crossfader.enabled ? 1 : .5
                color: Brand.ConfluxTheme.dark ? "#5C636B" : "#A4ABA9"
                border.color: crossfader.activeFocus ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
                Rectangle { anchors.centerIn: parent; width: 2; height: 15; color: "#1B2025" }
            }
            ToolTip.visible: hovered || activeFocus
            ToolTip.text: !enabled ? "Audio output must be ready to use the crossfader."
                                  : "Position " + Math.round(value * 100) + "%: A at left, both at center, B at right. Channel levels and headphone cue stay independent."
        }

        Text {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            text: "A / B LEVEL · HEADPHONE CUE · METERS\nEQ · FILTER · FX INACTIVE"
            horizontalAlignment: Text.AlignHCenter
            color: root.etched
            font.family: Brand.ConfluxTheme.displayFont
            font.pixelSize: 10
            lineHeight: 1.1
        }
    }
}
