import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The ghost icon button: a glyph, with nothing behind it.
//
// It used to answer the pointer with a filled square in 10% accent. That drew
// a pale red box around every arrow, dot and heart in the app — three at once
// inside a hovered track row — and a box is a second shape competing with the
// glyph inside it. The glyph carries it instead, on a ladder that never runs
// out of meaning:
//
//   grey  `neutral700`   there if you want it
//   ink   `text`         the pointer is on it
//   red   `accent`       the thing it controls is on
//
// A glyph that is already signalling cannot go to ink without losing what it
// says, so it brightens instead (`accent600`) — which is how the play button
// has always answered a pointer.
//
// Focus still draws a ring: a pointer can see what it is over, a keyboard
// cannot.
Button {
    id: control

    property string iconName: ""
    // The glyph at rest. `accent` for the marks that are meant to be found —
    // navigation arrows, the plus — and for any control that is currently on.
    property color iconColor: Theme.accent
    // Under the pointer. Derived, so almost no call site has to say it.
    property color hoverColor: Qt.colorEqual(control.iconColor, Theme.accent)
                               || Qt.colorEqual(control.iconColor, Theme.accent700)
                               ? Theme.accent600 : Theme.text
    property int iconSize: 16
    property int side: 32

    implicitWidth: side
    implicitHeight: side
    padding: 0
    flat: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        color: "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.accent
    }

    contentItem: Icon {
        name: control.iconName
        color: control.hovered || control.down ? control.hoverColor : control.iconColor
        implicitWidth: control.iconSize
        implicitHeight: control.iconSize

        // Arrives instantly, leaves over `quick` (DESIGN 2.6).
        Behavior on color {
            enabled: !control.hovered && !control.down
            ColorAnimation { duration: Theme.quick }
        }
    }
}
