import QtQuick
import Monolist

// A one-line confirmation low in the window ("Added to Night Drive") that
// leaves by itself. Ink on paper inverted, with the signal-red square.
Rectangle {
    id: root

    function show(message) {
        label.text = message
        opacity = 1
        hideTimer.restart()
    }

    color: Theme.text
    implicitWidth: row.implicitWidth + Theme.space4 * 2
    implicitHeight: 40
    opacity: 0
    visible: opacity > 0
    // It appears where it is, nudged up as it arrives: enough to notice at
    // the edge of vision, not enough to look at.
    transform: Translate { y: (1 - root.opacity) * Theme.space2 }

    Behavior on opacity {
        NumberAnimation { duration: Theme.normal; easing.type: Theme.enterCurve }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space3

        Rectangle {
            width: 8
            height: 8
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.accent
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.Bold
            color: Theme.bg
        }
    }

    Timer {
        id: hideTimer
        interval: 2600
        onTriggered: root.opacity = 0
    }

    // Hovering keeps it up long enough to read.
    HoverHandler {
        onHoveredChanged: hovered ? hideTimer.stop() : hideTimer.restart()
    }
}
