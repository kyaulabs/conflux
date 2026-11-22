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
    property int touchContactId: -1
    property int pressGeneration: 0
    property int pendingCancelGeneration: -1
    property bool available: true
    property bool round: false
    property bool accessibleToggle: false
    property bool indicatorOn: false
    property color indicatorColor: "transparent"
    property int fontPixelSize: round ? 14 : 11
    signal clicked()
    signal pressed()
    signal released()
    signal canceled()

    function activateAccessiblePress() {
        if (!available || physicalPress)
            return
        beginPress()
        finishPress(false)
    }
    function finishPendingCancel() {
        if (physicalPress && pendingCancelGeneration === pressGeneration)
            finishPress(true)
    }
    function beginPress(key) {
        if (!available)
            return
        finishPendingCancel()
        if (!available || physicalPress)
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
        touchContactId = -1
        pendingCancelGeneration = -1
        if (cancel)
            canceled()
        else
            released()
    }
    function requestCancel(afterBindings = false) {
        if (!physicalPress)
            return
        if (!afterBindings && (!holdAction || !hostWindow || !hostWindow.active)) {
            finishPress(true)
            return
        }
        const generation = pressGeneration
        if (pendingCancelGeneration === generation)
            return
        pendingCancelGeneration = generation
        // A held CUE release can publish a new deck snapshot. On disable,
        // let the availability binding finish before dispatching that release.
        // On focus changes, let a Play press in this event take precedence.
        Qt.callLater(function() {
            if (!control)
                return
            if (control.pendingCancelGeneration === generation)
                control.pendingCancelGeneration = -1
            if (control.physicalPress && control.pressGeneration === generation)
                control.finishPress(true)
        })
    }
    onAvailableChanged: { if (!available) requestCancel(holdAction) }
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

    CheckBox {
        id: face
        anchors.fill: parent
        objectName: control.buttonObjectName
        text: control.text
        enabled: control.available
        hoverEnabled: true
        opacity: enabled ? 1 : 0.46
        indicator: null
        checkable: control.accessibleToggle
        Accessible.checkable: control.accessibleToggle
        checked: control.accessibleToggle && control.indicatorOn
        // The engine acknowledges changes through indicatorOn.
        nextCheckState: function() { return checkState }
        Accessible.name: control.accessibleName
        Accessible.description: control.actionDescription
        // Route assistive actions through the control's hold ownership. Qt's
        // default press can release a CUE preview held by another input.
        Connections {
            target: control.holdAction || control.accessibleToggle ? face.Accessible : null
            function onPressAction() { control.activateAccessiblePress() }
            function onToggleAction() {
                if (control.accessibleToggle)
                    control.activateAccessiblePress()
            }
        }
        onClicked: control.clicked()
        onPressed: { if (control.touchContactId < 0) control.beginPress() }
        onReleased: { if (control.touchContactId < 0) control.finishPress(false) }
        onCanceled: {
            if (control.touchContactId < 0)
                control.requestCancel(control.holdAction && !control.available)
        }
        onEnabledChanged: {
            if (!enabled) {
                if (control.holdAction && !control.available)
                    control.requestCancel(true)
                else
                    control.finishPress(true)
            }
        }
        onVisibleChanged: { if (!visible) control.finishPress(true) }
        onActiveFocusChanged: {
            if (control.holdAction && !activeFocus && control.touchContactId < 0)
                control.requestCancel(!control.available)
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
                font.pixelSize: Brand.ConfluxTheme.textPixels(control.fontPixelSize)
                font.bold: true
                font.letterSpacing: control.round ? 0 : 0.6
                color: control.round && control.indicatorOn ? control.indicatorColor : Brand.ConfluxTheme.text
            }
        }
    }
    // A held CUE touch belongs to its contact even when another control takes focus.
    Loader {
        anchors.fill: parent
        active: control.holdAction
        sourceComponent: MultiPointTouchArea {
            enabled: control.available && face.visible
            mouseEnabled: false
            function heldContact(points) {
                for (const point of points) {
                    if (point.pointId === control.touchContactId)
                        return point
                }
                return null
            }
            onPressed: points => {
                if (!control.available || points.length === 0)
                    return
                control.finishPendingCancel()
                if (!control.available || control.physicalPress)
                    return
                const pointId = points[0].pointId
                control.touchContactId = pointId
                control.beginPress()
                if (control.touchContactId === pointId)
                    face.forceActiveFocus(Qt.MouseFocusReason)
            }
            onUpdated: points => {
                const point = heldContact(points)
                if (point && !contains(Qt.point(point.x, point.y)))
                    control.finishPress(true)
            }
            onReleased: points => { if (heldContact(points)) control.finishPress(false) }
            onCanceled: points => {
                if (heldContact(points)) {
                    if (control.available)
                        control.finishPress(true)
                    else
                        control.requestCancel(true)
                }
            }
            onGestureStarted: gesture => gesture.grab()
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
