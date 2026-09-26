import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The square that sits on a picture — the cover or the video — and changes
// it: paper with an ink glyph, flipped to ink with a paper glyph under the
// pointer, like the poster's own button. On a picture a bare glyph would be
// lost in whatever the picture is doing, so this one keeps its plate.
//
// `available` false greys it where it stands rather than hiding it, so the
// feature it offers is found before it is needed; its tip still says why.
Rectangle {
    id: root

    property string iconName: ""
    property string tip: ""
    property bool available: true
    readonly property bool hovered: hover.hovered
    signal clicked()

    readonly property bool live: available && hover.hovered

    width: 40
    height: 40
    color: live ? Theme.text : Theme.bg
    opacity: available ? 1 : 0.45

    Behavior on color {
        enabled: !root.live
        ColorAnimation { duration: Theme.quick }
    }

    Icon {
        anchors.centerIn: parent
        width: 18
        height: 18
        name: root.iconName
        color: root.live ? Theme.bg : Theme.text
    }

    HoverHandler {
        id: hover
        cursorShape: root.available ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    // Takes the press for itself, so a double-click on the button is not
    // also a double-click on the picture under it.
    TapHandler {
        enabled: root.available
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.clicked()
    }

    ToolTip.visible: hover.hovered && root.tip.length > 0
    ToolTip.delay: 400
    ToolTip.text: root.tip
}
