import QtQuick
import Phono

Item {
    id: root

    property string number: ""
    property string name: ""
    property int trackCount: 0
    signal activated()

    implicitHeight: 32

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.surface : "transparent"
    }

    Item {
        anchors.fill: parent
        anchors.leftMargin: Theme.space6
        anchors.rightMargin: Theme.space6

        Text {
            id: num
            text: root.number
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.neutral500
        }

        Text {
            anchors.left: num.right
            anchors.leftMargin: Theme.space3
            anchors.right: count.left
            anchors.rightMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            text: root.name
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Theme.weightMedium
            color: Theme.text
        }

        Text {
            id: count
            text: root.trackCount
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.neutral500
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.activated() }
}
