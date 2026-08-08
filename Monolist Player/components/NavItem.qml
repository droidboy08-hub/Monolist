import QtQuick
import Monolist

Item {
    id: root

    property string iconName: ""
    property string label: ""
    property bool active: false
    signal clicked()

    implicitHeight: 42
    width: parent ? parent.width : 296

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.surface : "transparent"
    }

    Row {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6
        spacing: Theme.space3

        Icon {
            name: root.iconName
            width: 18
            height: 18
            color: root.active ? Theme.accent700 : Theme.text
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.label
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 15
            font.weight: root.active ? Font.Bold : Theme.weightMedium
            color: root.active ? Theme.accent700 : Theme.text
        }
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
}
