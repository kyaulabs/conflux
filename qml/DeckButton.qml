// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "../brand/integration/qtquick" as Brand

Item {
    id: control
    property string text: ""
    property string displayText: text
    property string buttonObjectName: ""
    property string accessibleName: text
    property string unavailableReason: ""
    property string actionDescription: unavailableReason
    property string toolTipText: ""
    property color focusColor: Brand.ConfluxTheme.primary
    property bool holdAction: false
    readonly property bool held: physicalPress
    readonly property var hostWindow: face.Window.window
    property bool physicalPress: false
    property int heldKey: 0
    property int pressGeneration: 0
    property int pendingCancelGeneration: -1
    property bool available: true
    property bool round: false
    property bool indicatorOn: false
    property color indicatorColor: "transparent"
    property int fontPixelSize: round ? 14 : 11
    signal clicked()
    signal pressed()
    signal released()
    signal canceled()

    function beginPress(key) {
        if (!available)
            return
        if (physicalPress && pendingCancelGeneration === pressGeneration)
            finishPress(true)
        if (physicalPress)
            return
        pressGeneration += 1
        heldKey = key || 0
        physicalPress = true
        pressed()
    }
    function finishPress(cancel) {
        if (!physicalPress)
            return
        physicalPress = false
        heldKey = 0
        pendingCancelGeneration = -1
        if (cancel)
            canceled()
        else
            released()
    }
    function requestCancel() {
        if (!physicalPress)
            return
        if (!holdAction || !hostWindow || !hostWindow.active) {
            finishPress(true)
            return
        }
        const generation = pressGeneration
        if (pendingCancelGeneration === generation)
            return
        pendingCancelGeneration = generation
        // A Play press in this input event must reach the controller before
        // same-window focus cleanup ends a held CUE preview.
        Qt.callLater(function() {
            if (!control)
                return
            if (control.pendingCancelGeneration === generation)
                control.pendingCancelGeneration = -1
            if (control.physicalPress && control.pressGeneration === generation)
                control.finishPress(true)
        })
    }
    onAvailableChanged: { if (!available) finishPress(true) }
    onHostWindowChanged: { if (!hostWindow || !hostWindow.active) finishPress(true) }
    Component.onDestruction: finishPress(true)
    Connections {
        target: control.hostWindow
        function onActiveChanged() {
            if (!control.hostWindow || !control.hostWindow.active)
                control.finishPress(true)
        }
    }
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
        Accessible.description: control.actionDescription
        onClicked: control.clicked()
        onPressed: control.beginPress()
        onReleased: control.finishPress(false)
        onCanceled: control.requestCancel()
        onEnabledChanged: { if (!enabled) control.finishPress(true) }
        onVisibleChanged: { if (!visible) control.finishPress(true) }
        onActiveFocusChanged: {
            if (control.holdAction && !activeFocus)
                control.requestCancel()
        }
        // Space keeps the native Button press/release path. Enter/Return get
        // matching hold semantics for CUE instead of an immediate activation.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) {
            if (control.holdAction && event.key === Qt.Key_Space && event.isAutoRepeat) {
                event.accepted = true
                return
            }
            if (!control.holdAction || (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)) {
                event.accepted = false
                return
            }
            event.accepted = true
            if (!event.isAutoRepeat && !control.physicalPress) {
                control.beginPress(event.key)
            }
        }
        Keys.onReleased: function(event) {
            if (control.holdAction && event.key === Qt.Key_Space && event.isAutoRepeat) {
                event.accepted = true
                return
            }
            if (!control.holdAction || (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)) {
                event.accepted = false
                return
            }
            event.accepted = true
            if (!event.isAutoRepeat && control.heldKey === event.key)
                control.finishPress(false)
        }

        background: Rectangle {
            radius: control.round ? width / 2 : 4
            border.width: face.activeFocus ? 2 : 1
            border.color: face.activeFocus ? control.focusColor
                         : (control.round && control.indicatorColor.a > 0
                            ? Qt.rgba(control.indicatorColor.r, control.indicatorColor.g,
                                      control.indicatorColor.b, control.indicatorOn ? 1 : 0.58)
                            : Brand.ConfluxTheme.controlBorder)
            gradient: Gradient {
                GradientStop { position: 0; color: face.down || control.physicalPress ? Qt.darker(Brand.ConfluxTheme.surface, 1.3) : Qt.lighter(Brand.ConfluxTheme.raised, 1.25) }
                GradientStop { position: 0.5; color: Brand.ConfluxTheme.raised }
                GradientStop { position: 1; color: face.down || control.physicalPress ? Brand.ConfluxTheme.raised : Qt.darker(Brand.ConfluxTheme.surface, 1.25) }
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
    ToolTip.visible: available ? face.hovered && toolTipText.length > 0
                               : tipHover.containsMouse && unavailableReason.length > 0
    ToolTip.delay: 450
    ToolTip.text: available ? toolTipText : unavailableReason
}
