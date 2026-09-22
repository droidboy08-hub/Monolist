import QtQuick
import QtQuick.Controls.Basic
import Monolist

// A page's own actions: the primary one filled in signal red, the rest drawn
// as 2px outlines that fill with ink under the pointer. Square, like
// everything else.
Button {
    id: control

    property string iconName: ""
    property bool primary: false
    readonly property color labelColour: primary ? Theme.accentForeground
                                                 : (hovered || down ? Theme.bg : Theme.text)

    implicitHeight: 40
    // With no text, a square around the glyph.
    implicitWidth: text.length > 0 ? label.implicitWidth + Theme.space4 * 2 : implicitHeight
    padding: 0
    hoverEnabled: true
    opacity: enabled ? 1 : 0.4

    background: Rectangle {
        color: control.primary ? (control.hovered || control.down ? Theme.accent600 : Theme.accent)
                               : (control.hovered || control.down ? Theme.text : "transparent")
        border.width: control.primary ? 0 : Theme.ruleWidth
        border.color: Theme.text
    }

    contentItem: Item {
        Row {
            id: label
            anchors.centerIn: parent
            spacing: Theme.space2

            Icon {
                visible: control.iconName.length > 0
                name: control.iconName
                width: 15
                height: 15
                anchors.verticalCenter: parent.verticalCenter
                color: control.labelColour
            }
            Text {
                visible: control.text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Theme.weightBlack
                color: control.labelColour
            }
        }
    }
}
