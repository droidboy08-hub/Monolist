import QtQuick
import Monolist

// The red poster: a kicker, the title set huge, one action. With `artwork`,
// the cover sits at the right, printed black and white like every photograph
// in the system.
Rectangle {
    id: root

    property string kicker: "NEW ALBUM — OUT NOW"
    property string titleLine1: ""
    property string titleLine2: ""
    property string meta: ""
    property string artwork: ""
    property string buttonText: "Play album"
    signal playRequested()

    color: Theme.accent
    implicitHeight: Math.max(content.implicitHeight, cover.visible ? cover.height : 0) + Theme.space8 * 2

    Artwork {
        id: cover
        visible: root.artwork.length > 0 && root.width >= 760
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space8
        width: visible ? Math.min(280, Math.round(root.width * 0.26)) : 0
        height: width
        placeholder: ""
        source: root.artwork
    }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: cover.visible ? cover.left : parent.right
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
            text: root.titleLine2.length > 0 ? root.titleLine1 + "\n" + root.titleLine2 : root.titleLine1
            font.family: Theme.fontFamily
            font.pixelSize: root.width < 900 ? 48 : 72
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(root.width < 900 ? 48 : 72, -0.03)
            lineHeight: 0.95
            lineHeightMode: Text.ProportionalHeight
            color: Theme.accentForeground
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
        }

        Row {
            spacing: Theme.space4
            topPadding: Theme.space2

            PosterButton {
                text: root.buttonText
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
