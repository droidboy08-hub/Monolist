import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The system's menu: paper in a 2px ink frame, flat rows that tint under the
// pointer. Actions and submenus declared in it get MonoMenuItem's look.
Menu {
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
