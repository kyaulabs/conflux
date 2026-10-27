// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import "../qml" as View

Item {
    id: root
    width: 470
    height: 670
    property int payloadReads: 0
    property int activeDeck: 0
    property bool alternate: false
    property var peaks: [[0.1, 0.5, 0.9], [0.2, 0.8], [0.4, 0.9]]
    property var first: ({loaded: true, loading: false, waveformRevision: "1", sourceId: "same-source",
                          name: "Generated", positionSeconds: 0, durationSeconds: 120, playing: false, gain: 0.5,
                          pendingWaveform: []})
    property var second: ({loaded: true, loading: false, waveformRevision: "1", sourceId: "same-source",
                           name: "Generated peer", positionSeconds: 0, durationSeconds: 120, playing: false, gain: 0.5,
                           pendingWaveform: []})
    function snapshot(input, which) {
        const value = {}
        for (const key in input)
            value[key] = input[key]
        Object.defineProperty(value, "waveform", {get: function() {
            root.payloadReads += 1
            return root.peaks[which]
        }})
        return value
    }
    QtObject {
        id: primary
        property bool outputReady: false
        property var decks: [root.snapshot(root.first, 0), root.snapshot(root.second, 1)]
        signal stateChanged()
    }
    QtObject {
        id: replacement
        property bool outputReady: false
        property var decks: [root.snapshot(root.first, 2), root.snapshot(root.second, 2)]
        signal stateChanged()
    }
    View.DeckSurface {
        id: surface
        anchors.fill: parent
        deckIndex: root.activeDeck
        controller: root.alternate ? replacement : primary
    }
    function pump() {
        primary.decks = [snapshot(first, 0), snapshot(second, 1)]
        replacement.decks = [snapshot(first, 2), snapshot(second, 2)]
        if (alternate) replacement.stateChanged()
        else primary.stateChanged()
    }
    function require(condition, reason) {
        if (!condition) throw new Error(reason)
    }
    function cached() { return JSON.stringify(surface.cachedWaveform) }
    function run() {
        pump()
        require(cached() === "[0.1,0.5,0.9]", "Initial waveform missing")
        const before = payloadReads
        for (let update = 0; update < 64; ++update) {
            first.positionSeconds = update / 10
            first.gain = update / 100
            pump()
        }
        require(payloadReads === before, "Unchanged accepted waveform was read on a state update")
        peaks[0] = [0.6, 0.4]
        first.waveformRevision = "2"
        pump()
        require(cached() === "[0.6,0.4]", "Same-source overview replacement stayed stale")
        first.waveformRevision = "3"
        pump()
        const identical = payloadReads
        pump()
        require(payloadReads === identical, "Identical replacement did not retain its revision")
        first.loading = true
        first.pendingWaveform = [0.1, 0.2]
        pump()
        require(cached() === "[0.1,0.2]", "Pending overview missing")
        first.pendingWaveform = [0.3, 0.4]
        pump()
        require(cached() === "[0.3,0.4]", "Pending overview did not advance")
        first.loading = false
        pump()
        require(cached() === "[0.6,0.4]", "Cancelled preparation did not restore accepted overview")
        first.loaded = false
        peaks[0] = []
        pump()
        require(cached() === "[]", "Unload retained the old overview")
        first.loaded = true
        peaks[0] = [0.7]
        pump()
        require(cached() === "[0.7]", "Reload retained the empty overview")
        second.waveformRevision = "3"
        activeDeck = 1
        pump()
        require(cached() === "[0.2,0.8]", "Different deck with the same revision stayed stale")
        alternate = true
        pump()
        require(cached() === "[0.4,0.9]", "Different controller with the same revision stayed stale")
        delete second.waveformRevision
        pump()
        peaks[2] = [0.9, 0.1]
        pump()
        require(cached() === "[0.9,0.1]", "Missing revision prevented a payload update")
        return true
    }
}
