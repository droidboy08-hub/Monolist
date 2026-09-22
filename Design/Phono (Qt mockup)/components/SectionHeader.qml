import QtQuick
import Phono

Item {
    id: root

    property string number: ""
    property string title: ""
    property string action: ""
    signal actionTriggered()

    implicitHeight: 40
    width: parent ? parent.width : 0

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space4

        Text {
            text: root.number
            anchors.baseline: heading.baseline
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Theme.weightBlack
            color: Theme.accent700
        }

        Text {
            id: heading
            text: root.title
            font.family: Theme.fontFamily
            font.pixelSize: 28
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(28, -0.02)
            color: Theme.text
        }
    }

    Text {
        visible: root.action.length > 0
        text: root.action
        anchors.right: parent.right
        anchors.baseline: heading.baseline
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.12)
        color: linkHover.hovered ? Theme.accent700 : Theme.neutral700

        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.actionTriggered() }
    }
}
