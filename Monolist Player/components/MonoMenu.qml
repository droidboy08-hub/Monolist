import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The system's menu: paper in a 2px ink frame, flat rows that tint under the
// pointer. Actions and submenus declared in it get MonoMenuItem's look.
Menu {
    // As a submenu, whether its row is in the menu above. The menu above
    // makes that row itself, so this is the one handle a submenu has on it:
    // the song's menu keeps the player bar's own entries this way, shown only
    // when the bar's "…" asked (TrackMenu).
    property bool shown: true

    topPadding: Theme.ruleWidth
    bottomPadding: Theme.ruleWidth

    background: Rectangle {
        implicitWidth: 230
        color: Theme.bg
        border.width: Theme.ruleWidth
        border.color: Theme.text
    }

    delegate: MonoMenuItem {}
}
