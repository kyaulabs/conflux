pragma Singleton
import QtQuick

// Starter palette for a local QML module. Load the bundled fonts in the app.
QtObject {
    property bool dark: true
    readonly property color signal: "#FF3030"
    readonly property color canvas: dark ? "#111317" : "#FAF9F6"
    readonly property color surface: dark ? "#1A1D22" : "#FFFFFF"
    readonly property color raised: dark ? "#24282F" : "#F0EFEB"
    readonly property color text: dark ? "#F5F4F0" : "#24252B"
    readonly property color muted: dark ? "#B5B7BE" : "#5D626A"
    readonly property color accent: dark ? "#FF877D" : "#B92323"
    readonly property color divider: dark ? "#505660" : "#747A83"
    readonly property color controlBorder: dark ? "#78808A" : "#747A83"
    readonly property color primary: dark ? "#FF5B50" : "#C52524"
    readonly property color primaryText: dark ? "#111317" : "#FFFFFF"
    readonly property color deckA: dark ? "#58D6C3" : "#00786F"
    readonly property color deckB: dark ? "#F2C568" : "#9A5E00"
    readonly property color success: dark ? "#6ED9AE" : "#197B59"
    readonly property color warning: dark ? "#F5C469" : "#92620A"
    readonly property string bodyFont: "Barlow"
    readonly property string displayFont: "Barlow Condensed"
    readonly property string monoFont: "JetBrains Mono"
    readonly property int unit: 4
    readonly property int controlRadius: 6
    readonly property int panelRadius: 12
    property bool reducedMotion: false
    readonly property int feedbackDuration: reducedMotion ? 0 : 160
    readonly property int panelDuration: reducedMotion ? 0 : 240
}
