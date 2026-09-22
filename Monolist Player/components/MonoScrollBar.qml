import QtQuick
import QtQuick.Controls.Basic
import Monolist

// The system's scroll bar: a flat neutral bar, there only when there is
// something to scroll. (Replacing the style's handle also drops the style's
// own hiding, hence `visible`.)
ScrollBar {
    id: bar

    width: 10
    policy: ScrollBar.AsNeeded
    visible: size < 1.0

    contentItem: Rectangle {
        implicitWidth: 6
        color: bar.pressed ? Theme.neutral500 : Theme.neutral300
    }
}
