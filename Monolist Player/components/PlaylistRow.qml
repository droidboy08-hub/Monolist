import QtQuick
import Monolist

// One line of the sidebar's playlist list: its number (or a glyph), its name
// and how many songs it holds. Red when it is the page open. Under the
// pointer the count gives way to three dots, the playlist's menu, which a
// right click anywhere on the line opens too.
Item {
    id: root

    property string number: ""
    // Shown in place of the number, for Liked songs.
    property string iconName: ""
    property string name: ""
    property int trackCount: 0
    property bool active: false
    // Pinned above the rest: its mark in the number's place, in the accent.
    property bool pinned: false
    signal activated()
    signal menuRequested()

    implicitHeight: 32

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.surface : "transparent"

        Behavior on color {
            enabled: !hover.hovered
            ColorAnimation { duration: Theme.quick }
        }
    }

    Item {
        anchors.fill: parent
        anchors.leftMargin: Theme.space6
        anchors.rightMargin: Theme.space6

        Text {
            id: num
            visible: root.iconName.length === 0
            text: root.number
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: root.active || root.pinned ? Theme.accent700 : Theme.neutral500
        }

        Icon {
            visible: root.iconName.length > 0
            name: root.iconName
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 13
            height: 13
            color: Theme.accent
        }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20 + Theme.space3
            anchors.right: moreButton.visible ? moreButton.left : count.left
            anchors.rightMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            text: root.name
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: root.active ? Font.Bold : Theme.weightMedium
            color: root.active ? Theme.accent700 : Theme.text
        }

        Text {
            id: count
            visible: !hover.hovered
            text: root.trackCount
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: root.active ? Theme.accent700 : Theme.neutral500
        }

        // Where the count was; a button, so its press does not also open
        // the playlist.
        IconButton {
            id: moreButton
            visible: hover.hovered
            anchors.right: parent.right
            anchors.rightMargin: -Theme.space1
            anchors.verticalCenter: parent.verticalCenter
            side: 24
            iconName: "dots"
            iconSize: 14
            iconColor: Theme.neutral700
            onClicked: root.menuRequested()
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.activated() }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: root.menuRequested()
    }
}
