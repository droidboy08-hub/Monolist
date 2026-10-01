import QtQuick
import Monolist

// A one-line confirmation low in the window ("Added to Night Drive") that
// leaves by itself. Ink on paper inverted, with the signal-red square.
//
// Some answers can be taken back — "Won't suggest “Kyoto” again" — and those
// carry the way back at the end of the line, in red: UNDO. They stay a little
// longer, long enough to reach for it.
Rectangle {
    id: root

    // The link's word, and what it does; empty for a plain answer.
    property string actionText: ""
    property var action: null

    function show(message, actionLabel, onAction) {
        label.text = message
        actionText = actionLabel ? actionLabel : ""
        action = typeof onAction === "function" ? onAction : null
        hideTimer.interval = actionText.length > 0 ? 6000 : 2600
        opacity = 1
        hideTimer.restart()
    }

    // Once: the answer has been given, and the toast goes with it.
    function act() {
        const run = action
        action = null
        actionText = ""
        hideTimer.stop()
        opacity = 0
        if (run)
            run()
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

    // It covers whatever row is beneath it, and a click meant for it (UNDO,
    // above all) must not also press that row: pointer handlers let a press
    // go on to the items underneath, an item that accepts it does not.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
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

        // A tracked link, as in a shelf's header; red because on ink it is
        // the one thing to press.
        Text {
            visible: root.actionText.length > 0
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: Theme.space2
            text: root.actionText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            // Red on the plate, which is ink in light and paper in dark:
            // the brighter red on ink, the deeper on paper.
            color: Theme.dark ? (actionArea.containsMouse ? Theme.accent600 : "#ae1800")
                              : (actionArea.containsMouse ? Theme.accent600 : Theme.accent)

            MouseArea {
                id: actionArea
                // A little beyond the word, so it is not a small target.
                anchors.fill: parent
                anchors.margins: -Theme.space2
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.act()
            }
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
