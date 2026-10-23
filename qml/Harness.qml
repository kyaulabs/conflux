// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../brand/integration/qtquick" as Brand

ApplicationWindow {
    id: window
    readonly property real indicatorPosition: indicator.x
    readonly property bool reducedMotion: Brand.ConfluxTheme.reducedMotion
    width: 640
    height: 400
    minimumWidth: 480
    minimumHeight: 360
    visible: true
    title: "CONFLUX · M0"
    color: Brand.ConfluxTheme.canvas
    font.family: Brand.ConfluxTheme.bodyFont
    palette.windowText: Brand.ConfluxTheme.text
    palette.window: Brand.ConfluxTheme.canvas
    palette.text: Brand.ConfluxTheme.text
    palette.buttonText: Brand.ConfluxTheme.text
    palette.button: Brand.ConfluxTheme.raised
    palette.base: Brand.ConfluxTheme.surface
    palette.highlight: Brand.ConfluxTheme.primary
    palette.highlightedText: Brand.ConfluxTheme.primaryText
    palette.mid: Brand.ConfluxTheme.controlBorder
    palette.dark: Brand.ConfluxTheme.controlBorder
    palette.midlight: Brand.ConfluxTheme.controlBorder
    palette.light: Brand.ConfluxTheme.raised
    Component.onCompleted: Brand.ConfluxTheme.dark = initialDark

    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "F6"; onActivated: themeSwitch.toggle() }

    Connections {
        target: repaintTimeline
        enabled: controlledRepaint
        function onValueChanged(value) {
            indicator.x = 150 * (1 - Math.cos(2 * Math.PI * value))
        }
    }
    Connections {
        target: Brand.ConfluxTheme
        enabled: controlledRepaint
        function onReducedMotionChanged() {
            repaintTimeline.setPaused(window.reducedMotion)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 16
        Image {
            source: Brand.ConfluxTheme.dark
                    ? "../brand/assets/logos/conflux-horizontal-dark.svg"
                    : "../brand/assets/logos/conflux-horizontal-light.svg"
            Layout.preferredWidth: 240
            Layout.preferredHeight: 64
            fillMode: Image.PreserveAspectFit
            Accessible.name: "CONFLUX"
        }
        Label {
            text: "M0 · Runtime probe"
            color: Brand.ConfluxTheme.text
            font.pixelSize: Brand.ConfluxTheme.textPixels(24)
        }
        Label {
            text: "Qt " + qtVersion + " · " + platformName
            font.family: Brand.ConfluxTheme.monoFont
            color: Brand.ConfluxTheme.muted
        }
        Label {
            text: "Rendering is active. Run conflux-audio-probe separately for audio timing."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Brand.ConfluxTheme.text
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            color: Brand.ConfluxTheme.divider
            Rectangle {
                id: indicator
                width: parent.width / 4
                height: parent.height
                color: Brand.ConfluxTheme.signal
                SequentialAnimation on x {
                    loops: Animation.Infinite
                    running: !controlledRepaint && !Brand.ConfluxTheme.reducedMotion
                    NumberAnimation { to: 300; duration: 1600; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 0; duration: 1600; easing.type: Easing.InOutSine }
                }
            }
        }
        RowLayout {
            Switch {
                id: themeSwitch
                text: "Dark theme"
                checked: initialDark
                onCheckedChanged: Brand.ConfluxTheme.dark = checked
                Accessible.name: "Dark theme (F6)"
                Layout.minimumHeight: 48
            }
            CheckBox {
                text: "Reduce motion"
                onToggled: Brand.ConfluxTheme.reducedMotion = checked
                Layout.minimumHeight: 48
            }
        }
        Item { Layout.fillHeight: true }
        Label {
            text: "Build and rendering harness · Ctrl+Q to close"
            color: Brand.ConfluxTheme.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
}
