import QtQuick
import Monolist

// A MonoMenu entry that takes a second click, for what cannot be taken back
// with one: deleting a file, taking a song out of every playlist. The first
// click only arms it, saying what the second will do, and must leave the menu
// open for that; a click that reached the item would close the menu, so both
// are taken here instead. Disarmed after three seconds, and when the menu
// closes.
MonoMenuItem {
    id: entry

    // What it says armed ("Click again to delete the file").
    property string armedText: ""
    property bool armed: false
    signal confirmed()

    function activate() {
        if (armed) {
            armed = false
            entry.confirmed()
            if (entry.menu)
                entry.menu.dismiss()
        } else {
            armed = true
            disarm.restart()
        }
    }

    textColor: armed ? Theme.accent700 : Theme.text

    // Shown armed in place of `text`, which the caller keeps for the entry at
    // rest: MenuItem draws `text`, so the swap is made on the label itself.
    contentItem: Text {
        text: entry.armed ? entry.armedText : entry.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: entry.enabled ? entry.textColor : Theme.neutral500
    }

    MouseArea {
        anchors.fill: parent
        z: 1
        onClicked: entry.activate()
    }
    // The keys that would click it. Held down, a key repeats, and the repeat
    // must not be taken for the second press.
    Keys.onPressed: function(event) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                && event.key !== Qt.Key_Enter && event.key !== Qt.Key_Select)
            return
        event.accepted = true
        if (!event.isAutoRepeat)
            entry.activate()
    }

    Timer {
        id: disarm
        interval: 3000
        onTriggered: entry.armed = false
    }

    Connections {
        target: entry.menu
        function onClosed() { entry.armed = false }
    }
}
