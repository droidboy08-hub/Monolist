import QtQuick
import Monolist

// A card: the cover, a title, a line under it, and a footer label with the
// play mark. The cover blooms into colour under the pointer.
Rectangle {
    id: root

    property string title: ""
    property string artist: ""
    property string year: ""
    property string format: "LP"
    // Overrides the year and format in the footer when set.
    property string footer: ""
    property string artwork: ""
    // A playlist's mosaic; takes the place of `artwork` when it has any.
    property var artworks: []
    // "liked" or "new": see CollectionCover.
    property string plate: ""
    signal playRequested()

    color: hover.hovered ? Theme.surface : Theme.bg
    border.width: Theme.ruleWidth
    border.color: Theme.text
    implicitHeight: width + 100

    Behavior on color {
        enabled: !hover.hovered
        ColorAnimation { duration: Theme.quick }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space3
        spacing: Theme.space3

        CollectionCover {
            width: parent.width
            height: width
            artworks: root.artworks && root.artworks.length > 0 ? root.artworks
                    : root.artwork.length > 0 ? [root.artwork] : []
            plate: root.plate
            colour: hover.hovered
        }

        Column {
            width: parent.width
            spacing: 2

            Text {
                width: parent.width
                text: root.title
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Theme.weightBlack
                color: Theme.text
            }
            Text {
                width: parent.width
                text: root.artist
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.neutral700
            }
        }
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space3
        height: 26

        Rectangle {
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Text {
            anchors.left: parent.left
            anchors.right: playMark.left
            anchors.rightMargin: Theme.space2
            anchors.bottom: parent.bottom
            text: root.footer.length > 0 ? root.footer
                  : [root.year, root.format].filter(function(part) { return part.length > 0 }).join(" · ")
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.1)
            color: Theme.neutral600
        }

        Icon {
            id: playMark
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            name: root.plate === "new" ? "plus" : "play"
            width: 14
            height: 14
            color: Theme.accent
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.playRequested() }
}
