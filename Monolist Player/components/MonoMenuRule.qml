import QtQuick
import QtQuick.Controls.Basic
import Monolist

// A hairline between groups of a MonoMenu's rows.
MenuSeparator {
    implicitHeight: visible ? Theme.space1 * 2 + 1 : 0
    topPadding: Theme.space1
    bottomPadding: Theme.space1

    contentItem: Rectangle {
        implicitHeight: 1
        color: Theme.hairline
    }
}
