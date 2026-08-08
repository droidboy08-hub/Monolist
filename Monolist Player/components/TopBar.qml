import QtQuick
import Monolist

Rectangle {
    id: root

    property string breadcrumb: ""
    property alias searchText: searchField.text
    readonly property bool searchFocused: searchField.activeFocus
    property bool showMenuButton: false
    readonly property bool compact: width < 720

    signal menuRequested()
    signal backRequested()
    signal forwardRequested()
    signal searchActivated(string term)

    color: Theme.bg
    implicitHeight: 64

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.ruleWidth
        color: Theme.divider
    }

    Row {
        id: leftGroup
        anchors.left: parent.left
        anchors.leftMargin: Theme.space8
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space2

        IconButton {
            visible: root.showMenuButton
            iconName: "menu"
            onClicked: root.menuRequested()
        }
        IconButton { iconName: "arrow-left"; onClicked: root.backRequested() }
        IconButton { iconName: "arrow-right"; onClicked: root.forwardRequested() }
    }

    Text {
        anchors.left: leftGroup.right
        anchors.leftMargin: Theme.space4
        anchors.right: searchBox.left
        anchors.rightMargin: Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.compact
        text: root.breadcrumb
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.14)
        color: Theme.neutral600
    }

    Rectangle {
        id: searchBox
        anchors.right: parent.right
        anchors.rightMargin: Theme.space8
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(360, Math.max(200, root.width * 0.28))
        height: 36
        color: "transparent"
        border.width: Theme.ruleWidth
        border.color: searchField.activeFocus ? Theme.accent : Theme.text

        Icon {
            id: searchIcon
            name: "search"
            width: 15
            height: 15
            color: Theme.neutral600
            anchors.left: parent.left
            anchors.leftMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
        }

        TextInput {
            id: searchField
            anchors.left: searchIcon.right
            anchors.leftMargin: Theme.space2
            anchors.right: parent.right
            anchors.rightMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.onAccent
            clip: true
            onAccepted: root.searchActivated(text)

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: searchField.text.length === 0
                text: "Artists, albums, tracks…"
                font: searchField.font
                color: Theme.neutral500
            }
        }
    }
}
