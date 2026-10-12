import QtQuick
import Monolist

// The mark of the song playing: three bars that rise and fall while it plays
// and hold still where they are while it is paused.
//
// Each bar is a full-height plate slid up and down inside a clipped slot, so
// what moves is a position, never a size (DESIGN 2.3).
//
// Stepped on a timer, on whole pixels, not animated (DESIGN 2.7): an
// animation draws the whole window again at every refresh of the screen, 180
// times a second on a fast one, for as long as the song plays, to move three
// bars a few pixels tall. Fifteen steps a second look the same, and a step
// that lands on the pixel the bar is already on draws nothing.
Item {
    id: root

    property bool running: false
    property color color: Theme.accent
    // Something stands over it (Now Playing over the pages): held still.
    property bool covered: false
    // Moving only where it can be seen: not covered, and the window neither
    // minimised nor hidden, where nothing is drawn but the timer would
    // still tick.
    readonly property bool moving: running && visible && !covered
                                   && Window.visibility !== Window.Minimized
                                   && Window.visibility !== Window.Hidden

    // On whole pixels, the three as a group centred in the box.
    readonly property int step: Math.round(width / 3)
    readonly property int bar: Math.max(2, step - 2)
    readonly property int x0: Math.floor((width - (2 * step + bar)) / 2)

    implicitWidth: 14
    implicitHeight: 12

    // Where the bars are in their rise and fall, in milliseconds: moved on
    // by the timer, kept while they hold still. Every bar takes its place
    // from it, so all three step together and draw once a step.
    property real clock: 0
    property real lastTick: 0

    Timer {
        interval: 66
        repeat: true
        running: root.moving
        onRunningChanged: root.lastTick = Date.now()
        onTriggered: {
            const now = Date.now()
            // A late tick (the computer busy, or asleep) moves only one step.
            root.clock += Math.min(now - root.lastTick, 2 * interval)
            root.lastTick = now
        }
    }

    Repeater {
        model: 3

        Item {
            id: slot
            required property int index
            // Each bar its own pace and its own low point, so the three never
            // move as one.
            readonly property var lows: [0.55, 0.25, 0.7]
            readonly property var highs: [0.1, 0.0, 0.2]
            readonly property var paces: [420, 330, 510]

            x: root.x0 + index * root.step
            width: root.bar
            height: root.height
            clip: true

            Rectangle {
                width: parent.width
                height: parent.height
                color: root.color
                // Low, up to high and back down over two paces, eased in and
                // out as a sine (what the two eased halves of an animation
                // were), on a whole pixel.
                y: {
                    const low = root.height * slot.lows[slot.index]
                    const high = root.height * slot.highs[slot.index]
                    const turn = (1 - Math.cos(Math.PI * root.clock / slot.paces[slot.index])) / 2
                    return Math.round(low + (high - low) * turn)
                }
            }
        }
    }
}
