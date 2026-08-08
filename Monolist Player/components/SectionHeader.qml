import QtQuick
import Monolist

Item {
    id: root

    property string number: ""
    property string title: ""
    property string action: ""
    signal actionTriggered()

    implicitHeight: 40
    width: parent ? parent.width : 0

    Row {
        id: headingRow
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space4

        // Republish the heading's baseline on the Row so the action link can
        // anchor to it. Anchoring straight at `heading` fails — it is a child
        // of this Row, so it is neither parent nor sibling of the link.
        baselineOffset: heading.y + heading.baselineOffset

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
        anchors.baseline: headingRow.baseline
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.12)
        color: linkHover.hovered ? Theme.accent700 : Theme.neutral700

        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.actionTriggered() }
    }
}
