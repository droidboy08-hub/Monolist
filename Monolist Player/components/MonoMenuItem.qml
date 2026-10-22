import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One row of a MonoMenu. Hidden rows take no room, so a menu can hold entries
// that only apply in some places.
MenuItem {
    id: entry

    // Ink, unless the entry has something to warn about.
    property color textColor: Theme.text
    // The one of a set that is in use, such as the output menu's device: a
    // check at the row's end, and the entry in the accent, as the song
    // playing is in a list. Not `checked`, which a click would toggle.
    property bool current: false
    // A note at the row's end in grey, such as the keys that do the same
    // ("Ctrl+Shift+M"). It is read, not pressed, so it is small and quiet.
    property string hint: ""

    // A submenu's row is made by the menu it is in, and follows the
    // submenu's own `shown` (MonoMenu); every other row says for itself.
    visible: {
        const submenu = entry.subMenu as MonoMenu
        return submenu === null || submenu.shown
    }
    implicitHeight: visible ? 34 : 0
    leftPadding: Theme.space4
    rightPadding: Theme.space4

    contentItem: Text {
        text: entry.text
        // Clear of whatever stands at the row's end: the check, the arrow,
        // the hint.
        rightPadding: (entry.subMenu || entry.current ? Theme.space4 : 0)
                      + (entry.hint.length > 0 ? hintText.implicitWidth + Theme.space4 : 0)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 13
        font.weight: entry.current ? Theme.weightMedium : Theme.weightRegular
        color: !entry.enabled ? Theme.neutral500
               : entry.current ? Theme.accent700 : entry.textColor
    }

    Text {
        id: hintText
        visible: entry.hint.length > 0
        x: entry.width - width - entry.rightPadding
           - (entry.subMenu || entry.current ? Theme.space4 : 0)
        anchors.verticalCenter: parent.verticalCenter
        text: entry.hint
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: entry.enabled ? Theme.neutral700 : Theme.neutral500
    }

    indicator: Icon {
        x: entry.width - width - Theme.space3
        y: (entry.height - height) / 2
        visible: entry.current
        name: "check"
        width: 12
        height: 12
        color: Theme.accent
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
