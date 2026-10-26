// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "../brand/integration/qtquick" as Brand

Item {
    id: root
    property var meter: ({})
    property string caption: "Stereo level"
    property bool active: true
    readonly property real peakLeft: level("peakLeft")
    readonly property real peakRight: level("peakRight")
    readonly property real rmsLeft: level("rmsLeft")
    readonly property real rmsRight: level("rmsRight")
    readonly property bool overrange: level("overrangeLeft") > 0 || level("overrangeRight") > 0
                                      || peakLeft > 1 || peakRight > 1
    implicitWidth: 22
    implicitHeight: 78

    function level(key) {
        const value = Number(meter ? meter[key] : 0)
        return isFinite(value) && value > 0 ? value : 0
    }
    function proportion(value) {
        return value > 0 ? Math.max(0, Math.min(1, (20 * Math.log(value) / Math.LN10 + 60) / 60)) : 0
    }
    function db(value) {
        return value > 0 ? (20 * Math.log(value) / Math.LN10).toFixed(1) : "−∞"
    }
    Accessible.role: Accessible.Indicator
    Accessible.name: caption
    Accessible.description: "Peak L " + db(peakLeft) + ", R " + db(peakRight)
                            + " dBFS; RMS L " + db(rmsLeft) + ", R " + db(rmsRight)
                            + " dBFS" + (overrange ? "; overrange" : "")
    ToolTip.visible: meterHover.hovered
    ToolTip.text: caption + "\nPeak L " + db(peakLeft) + ", R " + db(peakRight)
                  + " dBFS\nRMS L " + db(rmsLeft) + ", R " + db(rmsRight)
                  + " dBFS" + (overrange ? "\nOverrange samples detected" : "")
    HoverHandler { id: meterHover }

    Text {
        objectName: root.objectName.length ? root.objectName + "Overrange" : ""
        anchors.top: parent.top
        width: parent.width
        height: 10
        text: root.overrange ? "OVR" : ""
        horizontalAlignment: Text.AlignHCenter
        color: Brand.ConfluxTheme.accent
        font.family: Brand.ConfluxTheme.monoFont
        font.pixelSize: Brand.ConfluxTheme.textPixels(Math.min(9, Math.max(6, root.width / 3)))
        font.bold: true
        Accessible.name: root.caption + (root.overrange ? " overrange" : " within range")
    }
    Row {
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 11
        spacing: 3
        Repeater {
            model: 2
            Rectangle {
                id: lane
                required property int index
                readonly property real peak: index === 0 ? root.peakLeft : root.peakRight
                readonly property real rms: index === 0 ? root.rmsLeft : root.rmsRight
                width: (root.width - 3) / 2
                height: parent.height
                radius: 1
                color: Brand.ConfluxTheme.dark ? "#080E10" : "#9EA9A6"
                border.color: Brand.ConfluxTheme.controlBorder
                clip: true
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * root.proportion(lane.peak)
                    color: lane.peak >= 1 ? Brand.ConfluxTheme.accent : Brand.ConfluxTheme.success
                    opacity: root.active ? 0.3 : 0.12
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * root.proportion(lane.rms)
                    color: Brand.ConfluxTheme.success
                    opacity: root.active ? 0.9 : 0.2
                }
                Rectangle {
                    width: parent.width
                    height: 2
                    y: Math.max(0, parent.height * (1 - root.proportion(lane.peak)) - 1)
                    visible: root.active && lane.peak > 0
                    color: lane.peak >= 1 ? Brand.ConfluxTheme.accent : Brand.ConfluxTheme.text
                }
            }
        }
    }
    Text {
        anchors.bottom: parent.bottom
        width: parent.width
        text: "L R"
        horizontalAlignment: Text.AlignHCenter
        font.family: Brand.ConfluxTheme.monoFont
        font.pixelSize: Brand.ConfluxTheme.textPixels(8)
        color: Brand.ConfluxTheme.muted
    }
}
