import QtQuick
import QtQuick.Controls.Basic
import Phono

// The ghost icon button: square, no radius, accent tint on hover and press.
Button {
    id: control

    property string iconName: ""
    property color iconColor: Theme.accent
    property int iconSize: 16
    property int side: 32

    implicitWidth: side
    implicitHeight: side
    padding: 0
    flat: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.ghostActive
             : control.hovered ? Theme.ghostHover
             : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.accent
    }

    contentItem: Icon {
        name: control.iconName
        color: control.iconColor
        implicitWidth: control.iconSize
        implicitHeight: control.iconSize
    }
}
