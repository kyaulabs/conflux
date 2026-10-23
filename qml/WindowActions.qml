// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "../brand/integration/qtquick" as Brand

Row {
    id: controls
    required property var targetWindow
    spacing: 2
    Repeater {
        model: ["Minimize", "Maximize", "Close"]
        Button {
            required property string modelData
            required property int index
            objectName: "window" + modelData
            width: 44
            height: 44
            text: index === 0 ? "−" : index === 1
                  ? (controls.targetWindow.visibility === Window.Maximized ? "❐" : "□") : "×"
            font.pixelSize: Brand.ConfluxTheme.textPixels(index === 2 ? 27 : 23)
            Accessible.name: index === 1 && controls.targetWindow.visibility === Window.Maximized
                             ? "Restore window" : modelData + " window"
            ToolTip.visible: hovered || activeFocus
            ToolTip.delay: 500
            ToolTip.text: Accessible.name
            onClicked: {
                if (index === 0) controls.targetWindow.showMinimized()
                else if (index === 1) {
                    if (controls.targetWindow.visibility === Window.Maximized) controls.targetWindow.showNormal()
                    else controls.targetWindow.showMaximized()
                } else controls.targetWindow.close()
            }
            background: Rectangle {
                radius: 3
                color: parent.hovered || parent.down
                       ? (index === 2 ? Brand.ConfluxTheme.accent : Brand.ConfluxTheme.raised)
                       : "transparent"
                border.width: parent.activeFocus ? 2 : 0
                border.color: Brand.ConfluxTheme.primary
            }
        }
    }
}
