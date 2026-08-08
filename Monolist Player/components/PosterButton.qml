import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The inverted button that sits on the red poster: flush-left label,
// square corners, ink-on-paper flip on hover.
Button {
    id: control

    property string iconName: "play"

    implicitHeight: 46
    implicitWidth: row.implicitWidth + Theme.space4 + Theme.space6
    padding: 0
    hoverEnabled: true

    background: Rectangle {
        color: control.hovered || control.down ? Theme.text : Theme.accentForeground
    }

    contentItem: Item {
        Row {
            id: row
            anchors.left: parent.left
            anchors.leftMargin: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            Icon {
                name: control.iconName
                width: 16
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: control.hovered || control.down ? Theme.accentForeground : Theme.text
            }

            Text {
                text: control.text
                anchors.verticalCenter: parent.verticalCenter
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Theme.weightBlack
                color: control.hovered || control.down ? Theme.accentForeground : Theme.text
            }
        }
    }
}
