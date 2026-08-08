import QtQuick
import Phono

Rectangle {
    id: root

    property string title: ""
    property string artist: ""
    property string year: ""
    property string format: "LP"
    property string artwork: ""
    signal playRequested()

    color: hover.hovered ? Theme.surface : Theme.bg
    border.width: Theme.ruleWidth
    border.color: Theme.text
    implicitHeight: width + 100

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space3
        spacing: Theme.space3

        Artwork {
            width: parent.width
            height: width
            source: root.artwork
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
            anchors.bottom: parent.bottom
            text: root.year + " · " + root.format
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.1)
            color: Theme.neutral600
        }

        Icon {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            name: "play"
            width: 14
            height: 14
            color: Theme.accent
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.playRequested() }
}
