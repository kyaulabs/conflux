// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "../brand/integration/qtquick" as Brand

Item {
    id: control
    property string text: ""
    property string displayText: text
    property string buttonObjectName: ""
    property string accessibleName: text
    property string unavailableReason: ""
    property bool available: true
    property bool round: false
    property bool indicatorOn: false
    property color indicatorColor: "transparent"
    property int fontPixelSize: round ? 14 : 11
    signal clicked()
    implicitWidth: round ? 56 : 64
    implicitHeight: round ? 56 : 44

    Button {
        id: face
        anchors.fill: parent
        objectName: control.buttonObjectName
        text: control.text
        enabled: control.available
        hoverEnabled: true
        opacity: enabled ? 1 : 0.46
        Accessible.name: control.accessibleName
        Accessible.description: control.unavailableReason
        onClicked: control.clicked()

        background: Rectangle {
            radius: control.round ? width / 2 : 4
            border.width: face.activeFocus ? 2 : 1
            border.color: face.activeFocus ? Brand.ConfluxTheme.primary
                         : (control.round && control.indicatorColor.a > 0
                            ? Qt.rgba(control.indicatorColor.r, control.indicatorColor.g,
                                      control.indicatorColor.b, control.indicatorOn ? 1 : 0.58)
                            : Brand.ConfluxTheme.controlBorder)
            gradient: Gradient {
                GradientStop { position: 0; color: face.down ? Qt.darker(Brand.ConfluxTheme.surface, 1.3) : Qt.lighter(Brand.ConfluxTheme.raised, 1.25) }
                GradientStop { position: 0.5; color: Brand.ConfluxTheme.raised }
                GradientStop { position: 1; color: face.down ? Brand.ConfluxTheme.raised : Qt.darker(Brand.ConfluxTheme.surface, 1.25) }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: control.round ? 4 : 3
                radius: control.round ? width / 2 : 2
                color: "transparent"
                border.width: 1
                border.color: Brand.ConfluxTheme.dark ? "#16191E" : "#B0B1AF"
            }
            Rectangle {
                visible: !control.round && control.indicatorColor.a > 0
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                width: Math.max(8, parent.width - 18)
                height: 2
                radius: 1
                color: control.indicatorColor
                opacity: control.indicatorOn ? 1 : 0.25
            }
        }
        contentItem: Item {
            Label {
                anchors.centerIn: parent
                width: parent.width
                text: control.displayText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: Brand.ConfluxTheme.displayFont
                font.pixelSize: control.fontPixelSize
                font.bold: true
                font.letterSpacing: control.round ? 0 : 0.6
                color: control.round && control.indicatorOn ? control.indicatorColor : Brand.ConfluxTheme.text
            }
        }
    }
    // Disabled controls remain honest, while their explanatory tooltip can
    // still be reached without enabling the actual button.
    MouseArea {
        id: tipHover
        anchors.fill: parent
        enabled: !control.available
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
    ToolTip.visible: tipHover.containsMouse && unavailableReason.length > 0
    ToolTip.delay: 450
    ToolTip.text: unavailableReason
}
