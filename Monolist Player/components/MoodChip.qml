import QtQuick
import Monolist

// One of YouTube Music's Moods & genres: its name in a ruled box, with the
// stripe of colour YouTube Music gives it down the left edge. Opens the
// mood's or genre's shelves of playlists.
Item {
    id: root

    // { title, browseId, params, color } (Catalog.moods' chips).
    property var chip: ({})
    signal activated()

    implicitWidth: Math.max(120, label.implicitWidth + stripe.width + Theme.space4 * 2)
    implicitHeight: 44

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.surface : "transparent"
        border.width: Theme.ruleWidth
        border.color: hover.hovered ? Theme.text : Theme.neutral300
    }

    Rectangle {
        id: stripe
        width: 6
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.ruleWidth
        color: root.chip && root.chip.color ? root.chip.color : Theme.accent
    }

    Text {
        id: label
        anchors.left: stripe.right
        anchors.leftMargin: Theme.space4
        anchors.right: parent.right
        anchors.rightMargin: Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        text: root.chip && root.chip.title ? root.chip.title : ""
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Font.Bold
        color: Theme.text
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.activated() }
}
