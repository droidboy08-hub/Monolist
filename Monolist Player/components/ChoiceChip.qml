import QtQuick
import Monolist

// One segment of a switch: flat, 2px rule, inverted when chosen. Set a Row of
// them to spacing -Theme.ruleWidth so neighbours share a single rule.
Rectangle {
    id: choice

    property string label: ""
    property bool selected: false
    signal picked()

    implicitWidth: choiceLabel.implicitWidth + Theme.space4 * 2
    implicitHeight: 32
    color: selected ? Theme.text : (choiceHover.hovered ? Theme.rowHover : "transparent")
    border.width: Theme.ruleWidth
    border.color: Theme.text

    Text {
        id: choiceLabel
        anchors.centerIn: parent
        text: choice.label
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.08)
        color: choice.selected ? Theme.bg : Theme.text
    }

    HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: choice.picked() }
}
