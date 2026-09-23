import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The heart, and the one icon button with no plate behind it.
//
// Every other ghost button answers the pointer with a 10%-accent square,
// because its glyph has nothing else to say. The heart does: it can fill. A
// square of pale red sitting directly behind a red heart made the accent read
// as a smudge rather than a signal, and it boxed in the one shape in the app
// people actually look at. So the glyph carries the whole state here —
// hovering part-fills it, which is a preview of what the click will leave
// behind, and the like itself completes the fill.
//
// Focus still draws a ring: a pointer can see what it is over, a keyboard
// cannot.
Button {
    id: control

    property bool liked: false
    // What an unliked heart is drawn in when nothing is pointing at it. Grey,
    // like every other icon at rest: ink is reserved for the pointer, and the
    // heart says "on" in red rather than in weight.
    property color restColor: Theme.neutral700
    property int iconSize: 16
    property int side: 32

    readonly property color live: control.hovered || control.down ? Theme.accent600 : Theme.accent

    implicitWidth: side
    implicitHeight: side
    padding: 0
    flat: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1 : 0.4

    background: Rectangle {
        color: "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.accent
    }

    contentItem: Item {
        // The outline is always drawn, so the heart never changes shape or
        // size — only what is inside it (DESIGN 2.3: no scale).
        Icon {
            anchors.centerIn: parent
            name: "heart"
            width: control.iconSize
            height: control.iconSize
            color: control.liked || control.hovered || control.down
                   ? control.live : control.restColor

            // Arrives instantly, leaves over `quick` (DESIGN 2.6).
            Behavior on color {
                enabled: !control.hovered && !control.down
                ColorAnimation { duration: Theme.quick }
            }
        }

        // The like. Always animated: `quick` is the motion table's entry for
        // exactly this ("a colour or opacity changing in place: a like"), and
        // it is also how the bar answers a heart pressed in a track list.
        Icon {
            anchors.centerIn: parent
            name: "heart-filled"
            width: control.iconSize
            height: control.iconSize
            color: control.live
            opacity: control.liked ? 1 : 0

            Behavior on opacity {
                NumberAnimation { duration: Theme.quick; easing.type: Theme.enterCurve }
            }
        }

        // The preview, on its own layer so that the pointer's feedback can be
        // instant while the like above it still animates. Pressing fills
        // further, so the button is visibly on its way somewhere.
        Icon {
            anchors.centerIn: parent
            name: "heart-filled"
            width: control.iconSize
            height: control.iconSize
            color: control.live
            opacity: control.liked ? 0
                   : control.down ? 0.5
                   : control.hovered ? 0.22 : 0

            // Off only while the pointer is arriving on an unliked heart —
            // the one transition that should not animate. Once it is liked the
            // fade is back on, so this layer hands over to the one above
            // instead of blinking out from under it.
            Behavior on opacity {
                enabled: control.liked || !(control.hovered || control.down)
                NumberAnimation { duration: Theme.quick; easing.type: Theme.exitCurve }
            }
        }
    }
}
