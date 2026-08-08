import QtQuick
import Monolist

Rectangle {
    id: root

    property string kicker: "NEW ALBUM — OUT NOW"
    property string titleLine1: ""
    property string titleLine2: ""
    property string meta: ""
    signal playRequested()

    color: Theme.accent
    implicitHeight: content.implicitHeight + Theme.space8 * 2

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space8
        spacing: Theme.space4

        Text {
            text: root.kicker
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.18)
            color: Theme.accentForeground
        }

        Text {
            width: parent.width
            text: root.titleLine1 + "\n" + root.titleLine2
            font.family: Theme.fontFamily
            font.pixelSize: root.width < 900 ? 48 : 72
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(root.width < 900 ? 48 : 72, -0.03)
            lineHeight: 0.95
            lineHeightMode: Text.ProportionalHeight
            color: Theme.accentForeground
            wrapMode: Text.WordWrap
        }

        Row {
            spacing: Theme.space4
            topPadding: Theme.space2

            PosterButton {
                text: "Play album"
                onClicked: root.playRequested()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.meta
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Theme.weightMedium
                color: Theme.accentForeground
            }
        }
    }
}
