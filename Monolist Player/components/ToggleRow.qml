import QtQuick
import Monolist

// A square check box with a label and a line of explanation under it.
Item {
    id: toggle

    property string label: ""
    property string hint: ""
    property bool checked: false
    signal toggled()

    implicitHeight: Math.max(18, toggleText.implicitHeight)

    Rectangle {
        id: box
        y: 1
        width: 18
        height: 18
        color: toggle.checked ? Theme.accent : "transparent"
        border.width: Theme.ruleWidth
        border.color: toggle.checked ? Theme.accent : Theme.text

        Icon {
            anchors.centerIn: parent
            width: 12
            height: 12
            name: "check"
            thickness: 3
            visible: toggle.checked
            color: Theme.accentForeground
        }
    }

    Column {
        id: toggleText
        anchors.left: box.right
        anchors.leftMargin: Theme.space3
        anchors.right: parent.right
        spacing: 2

        Text {
            width: parent.width
            text: toggle.label
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.text
        }
        Text {
            visible: text.length > 0
            width: parent.width
            text: toggle.hint
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }
    }

    HoverHandler { cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: toggle.toggled() }
}
