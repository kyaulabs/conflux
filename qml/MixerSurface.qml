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
                    readonly property bool available: audioDeck >= 0 && Boolean(deck.loaded) && root.controller.outputReady
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

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 25
                            radius: 3
                            color: Brand.ConfluxTheme.dark ? "#292D33" : "#CACDCB"
                            border.color: Brand.ConfluxTheme.dark ? "#535A64" : "#8B9295"
                            Accessible.name: "Channel " + strip.modelData.name + " headphone cue unavailable"
                            Accessible.description: "Separate headphone cue routing is not implemented."
                            Text {
                                anchors.centerIn: parent
                                text: "CUE"
                                color: root.etched
                                font.family: Brand.ConfluxTheme.displayFont
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            HoverHandler { id: cueHover }
                            ToolTip.visible: cueHover.hovered
                            ToolTip.text: "Separate headphone cue routing is not implemented."
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
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 66
                    color: Brand.ConfluxTheme.dark ? "#0B1114" : "#CCD1CF"
                    border.color: Brand.ConfluxTheme.controlBorder
                    radius: 2
                    Text {
                        anchors.fill: parent
                        anchors.margins: 4
                        text: "METERS\nUNAVAILABLE"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.family: Brand.ConfluxTheme.displayFont
                        font.pixelSize: 10
                        color: Brand.ConfluxTheme.muted
                    }
                    HoverHandler { id: meterHover }
                    ToolTip.visible: meterHover.hovered
                    ToolTip.text: "Audio level metering is not implemented."
                }
                Text {
                    Layout.fillWidth: true
                    text: "BEAT FX"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: root.etched
                }
                SurfaceKnob {
                    Layout.fillWidth: true
                    knobDiameter: 34
                    caption: "SELECT"
                    helpText: "Beat effects are not implemented."
                }
                SurfaceKnob {
                    Layout.fillWidth: true
                    knobDiameter: 34
                    caption: "TIME"
                    helpText: "Effect timing is not implemented."
                }
                SurfaceKnob {
                    Layout.fillWidth: true
                    knobDiameter: 34
                    caption: "LEVEL / DEPTH"
                    helpText: "Effect level and depth are not implemented."
                }
                Item { Layout.fillHeight: true }
                Text {
                    Layout.fillWidth: true
                    text: "FX / CUE\nINACTIVE"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Brand.ConfluxTheme.displayFont
                    font.pixelSize: 11
                    color: root.etched
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            color: Brand.ConfluxTheme.dark ? "#171A1F" : "#D8DAD7"
            border.color: Brand.ConfluxTheme.dark ? "#414750" : "#A7ACA9"
            radius: 3
            Accessible.name: "Crossfader unavailable"
            Accessible.description: "Crossfader routing is not implemented."
            Text {
                anchors.top: parent.top
                anchors.topMargin: 4
                anchors.horizontalCenter: parent.horizontalCenter
                text: "CROSSFADER / UNAVAILABLE"
                color: root.etched
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: 10
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 31
                width: parent.width - 42
                height: 6
                radius: 3
                color: Brand.ConfluxTheme.dark ? "#05070A" : "#767E84"
                border.color: Brand.ConfluxTheme.controlBorder
                Rectangle {
                    anchors.centerIn: parent
                    width: 22
                    height: 19
                    radius: 2
                    color: Brand.ConfluxTheme.dark ? "#5C636B" : "#A4ABA9"
                    border.color: Brand.ConfluxTheme.controlBorder
                    Rectangle { anchors.centerIn: parent; width: 2; height: 15; color: "#1B2025" }
                }
            }
            HoverHandler { id: crossHover }
            ToolTip.visible: crossHover.hovered
            ToolTip.text: "Crossfader routing is not implemented."
        }

        Text {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            text: "A / B LEVEL + MASTER ACTIVE\nEQ · FILTER · CUE · FX INACTIVE"
            horizontalAlignment: Text.AlignHCenter
            color: root.etched
            font.family: Brand.ConfluxTheme.displayFont
            font.pixelSize: 10
            lineHeight: 1.1
        }
    }
}
