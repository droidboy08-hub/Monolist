import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One row of a MonoMenu. Hidden rows take no room, so a menu can hold entries
// that only apply in some places.
MenuItem {
    id: entry

    implicitHeight: visible ? 34 : 0
    leftPadding: Theme.space4
    rightPadding: Theme.space4

    contentItem: Text {
        text: entry.text
        rightPadding: entry.subMenu ? Theme.space4 : 0
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: entry.enabled ? Theme.text : Theme.neutral500
    }

    arrow: Icon {
        x: entry.width - width - Theme.space3
        y: (entry.height - height) / 2
        visible: entry.subMenu !== null
        name: "chevron-right"
        width: 12
        height: 12
        color: Theme.text
    }

    background: Rectangle {
        color: entry.highlighted ? Theme.rowHover : "transparent"
    }
}
