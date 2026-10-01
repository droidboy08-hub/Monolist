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

            x: index * root.width / 3
            width: Math.max(2, Math.round(root.width / 3) - 2)
            height: root.height
            clip: true

            Rectangle {
                width: parent.width
                height: parent.height
                color: root.color
                y: root.height * slot.lows[slot.index]

                SequentialAnimation on y {
                    running: root.running && root.visible
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
