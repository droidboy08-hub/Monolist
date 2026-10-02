import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One of Home's Jump back in tiles: a place's cover and its name on a plate.
// A click opens it. Under the pointer the plate turns to ink, the cover to
// colour, and a red play square comes up at its end, which plays the place
// from the start without opening it (where it can be played: an artist's
// page cannot, and opens instead).
Item {
    id: root

    property string name: ""
    // One cover, or up to four for a playlist's mosaic (CollectionCover).
    property var artworks: []
    // CollectionCover's plate: "liked" for Liked songs.
    property string plate: ""
    property bool playable: true
    // Being fetched to play: the square stays up.
    property bool loading: false

    signal opened()
    signal playRequested()
    signal menuRequested()

    implicitHeight: 64
    readonly property bool marked: hover.hovered

    // Ink at once under the pointer, paper again over a moment (DESIGN 2.6).
    Rectangle {
        anchors.fill: parent
        color: root.marked ? Theme.text : Theme.surface

        Behavior on color {
            enabled: !root.marked
            ColorAnimation { duration: Theme.quick }
        }
    }

    CollectionCover {
        id: cover
        width: parent.height
        height: parent.height
        artworks: root.artworks
        plate: root.plate
        colour: root.marked
    }

    Text {
        anchors.left: cover.right
        anchors.leftMargin: Theme.space4
        anchors.right: play.visible ? play.left : parent.right
        anchors.rightMargin: Theme.space3
        anchors.verticalCenter: parent.verticalCenter
        text: root.name
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        textFormat: Text.PlainText
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Theme.weightBlack
        lineHeight: 1.1
        color: root.marked ? Theme.bg : Theme.text
    }

    // A button of its own, so a click on it is not also the plate's.
    Rectangle {
        id: play
        visible: root.playable && (root.marked || root.loading)
        width: 36
        height: 36
        anchors.right: parent.right
        anchors.rightMargin: Theme.space3
        anchors.verticalCenter: parent.verticalCenter
        color: playArea.containsMouse ? Theme.accent600 : Theme.accent

        Icon {
            anchors.centerIn: parent
            width: 14
            height: 14
            name: "play"
            color: Theme.accentForeground
        }

        MouseArea {
            id: playArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.playRequested()
        }

        ToolTip.visible: playArea.containsMouse
        ToolTip.delay: 600
        ToolTip.text: "Play " + root.name
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.opened() }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.menuRequested()
    }
}
