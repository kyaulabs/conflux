// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "../brand/integration/qtquick" as Brand

Item {
    id: root
    property string caption: ""
    property string accessibleName: caption
    property string helpText: ""
    property string dialObjectName: ""
    property bool activeControl: false
    property real knobDiameter: 30
    property color indicatorColor: Brand.ConfluxTheme.primary
    property alias value: dial.value
    property real acknowledgedValue: 0.5
    signal moved(real newValue)
    implicitWidth: Math.max(46, knobDiameter + 12)
    implicitHeight: knobDiameter + 17

    Accessible.role: Accessible.Grouping
    Accessible.name: accessibleName
    Accessible.description: helpText
    ToolTip.visible: hover.hovered || dial.activeFocus
    ToolTip.text: helpText
    ToolTip.delay: 350
    HoverHandler { id: hover }
    function refreshAcknowledgement(value) {
        dial.refreshAcknowledgement(value)
    }
    onAcknowledgedValueChanged: refreshAcknowledgement(acknowledgedValue)

    Dial {
        id: dial
        objectName: root.dialObjectName
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.knobDiameter
        height: width
        from: 0
        to: 1
        readonly property real minimumValue: Math.min(from, to)
        readonly property real maximumValue: Math.max(from, to)
        stepSize: 0.01
        enabled: root.activeControl
        focusPolicy: Qt.StrongFocus
        inputMode: Dial.Vertical
        Accessible.name: root.accessibleName
        Accessible.description: root.helpText
        property bool initialized: false
        property bool restoringAcknowledgement: false
        property real acceptedValue: 0.5
        function refreshAcknowledgement(candidate) {
            if (!initialized || !Number.isFinite(candidate))
                return
            const previousGuard = restoringAcknowledgement
            restoringAcknowledgement = true
            acceptedValue = Math.max(from, Math.min(to, candidate))
            if (!pressed)
                value = acceptedValue
            restoringAcknowledgement = previousGuard
        }
        function restoreAccepted() {
            if (!initialized || pressed)
                return
            const previousGuard = restoringAcknowledgement
            restoringAcknowledgement = true
            value = acceptedValue
            restoringAcknowledgement = previousGuard
        }
        function requestValue(candidate) {
            if (initialized && root.activeControl && Number.isFinite(candidate))
                root.moved(Math.max(from, Math.min(to, candidate)))
        }
        onValueChanged: {
            if (!initialized || restoringAcknowledgement)
                return
            if (!Number.isFinite(value)) {
                restoreAccepted()
            } else if (pressed) {
                requestValue(value)
            } else if (value !== acceptedValue) {
                const candidate = value
                const previousGuard = restoringAcknowledgement
                restoringAcknowledgement = true
                requestValue(candidate)
                value = acceptedValue
                restoringAcknowledgement = previousGuard
            }
        }
        onPressedChanged: { if (!pressed) restoreAccepted() }
        Component.onCompleted: { initialized = true; refreshAcknowledgement(root.acknowledgedValue) }

        background: Rectangle {
            radius: width / 2
            border.width: dial.activeFocus ? 2 : 1
            border.color: dial.activeFocus ? Brand.ConfluxTheme.primary : Brand.ConfluxTheme.controlBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Brand.ConfluxTheme.dark ? "#747982" : "#D5D6D3" }
                GradientStop { position: 0.45; color: Brand.ConfluxTheme.dark ? "#34383F" : "#949A9F" }
                GradientStop { position: 1; color: Brand.ConfluxTheme.dark ? "#101216" : "#676D74" }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 3
                radius: width / 2
                color: Brand.ConfluxTheme.dark ? "#171A1E" : "#E4E3DF"
                border.color: Brand.ConfluxTheme.dark ? "#080A0D" : "#666C72"
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: width / 2
                    gradient: Gradient {
                        GradientStop { position: 0; color: Brand.ConfluxTheme.dark ? "#353A42" : "#FAF9F6" }
                        GradientStop { position: 1; color: Brand.ConfluxTheme.dark ? "#20242B" : "#B7BAB9" }
                    }
                    border.color: Brand.ConfluxTheme.dark ? "#4A5058" : "#FFFFFF"
                }
            }
        }
        handle: Item {
            x: dial.width / 2 - width / 2
            y: dial.height / 2 - height / 2
            width: dial.width
            height: dial.height
            rotation: -140 + dial.position * 280
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 4
                width: 2
                height: Math.max(5, root.knobDiameter * 0.19)
                radius: 1
                color: root.activeControl ? root.indicatorColor : Brand.ConfluxTheme.muted
            }
        }
    }
    Text {
        anchors.top: dial.bottom
        anchors.topMargin: 3
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.caption
        color: Brand.ConfluxTheme.muted
        font.family: Brand.ConfluxTheme.displayFont
        font.pixelSize: 11
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
    }
}
