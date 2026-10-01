import QtQuick
import Monolist

// The mark of the song playing: three bars that rise and fall while it plays
// and hold still where they are while it is paused.
//
// Each bar is a full-height plate slid up and down inside a clipped slot, so
// what moves is a position, never a size (DESIGN 2.3).
Item {
    id: root

    property bool running: false
    property color color: Theme.accent
    // Something stands over it (Now Playing over the pages): held still.
    property bool covered: false
    // Moving only where it can be seen: not covered, and the window neither
    // minimised nor hidden, where nothing is drawn but the animation would
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
                y: root.height * slot.lows[slot.index]

                SequentialAnimation on y {
                    running: root.moving
                    loops: Animation.Infinite
                    NumberAnimation {
                        to: root.height * slot.highs[slot.index]
                        duration: slot.paces[slot.index]
                        easing.type: Easing.InOutSine
                    }
                    NumberAnimation {
                        to: root.height * slot.lows[slot.index]
                        duration: slot.paces[slot.index]
                        easing.type: Easing.InOutSine
                    }
                }
            }
        }
    }
}
