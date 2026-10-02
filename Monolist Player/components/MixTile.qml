import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One of Home's Made for you mixes: four of its songs' covers as a square
// mosaic, a band of its own colour across the foot ("MIX 01"), and under it
// what the mix is and who is on it. A click opens the mix's page; under the
// pointer the covers come to colour and the band offers play, which plays
// the whole mix.
Item {
    id: root

    property int number: 1
    property string title: ""
    property string subtitle: ""
    // Up to four covers; a missing one is a plate.
    property var covers: []
    // Printed as it is in either appearance: the band carries paper type.
    property color band: "#ec3013"

    signal opened()
    signal playRequested()

    implicitHeight: width + details.implicitHeight + Theme.space2
    readonly property bool marked: hover.hovered

    Item {
        id: art
        width: root.width
        height: width
        clip: true

        Grid {
            anchors.fill: parent
            columns: 2

            Repeater {
                model: 4

                delegate: Artwork {
                    required property int index
                    width: art.width / 2
                    height: art.height / 2
                    source: root.covers && root.covers.length > index ? root.covers[index] : ""
                    placeholder: ""
                    colour: root.marked
                }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 40
            color: root.band

            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.space3
                anchors.verticalCenter: parent.verticalCenter
                text: "MIX " + (root.number < 10 ? "0" + root.number : String(root.number))
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Theme.weightBlack
                font.letterSpacing: Theme.tracking(12, 0.14)
                color: Theme.paper
            }

            // Play, under the pointer: a button of its own, so a click on it
            // plays rather than opens.
            Item {
                visible: root.marked
                width: 40
                height: 40
                anchors.right: parent.right

                Icon {
                    anchors.centerIn: parent
                    width: 16
                    height: 16
                    name: "play"
                    color: Theme.paper
                }

                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.playRequested()
                }

                ToolTip.visible: playArea.containsMouse
                ToolTip.delay: 600
                ToolTip.text: "Play the mix"
            }
        }
    }

    Column {
        id: details
        anchors.top: art.bottom
        anchors.topMargin: Theme.space2
        width: root.width
        spacing: 2

        Text {
            width: parent.width
            text: root.title
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Theme.weightBlack
            color: Theme.text
        }
        Text {
            width: parent.width
            text: root.subtitle
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            lineHeight: 1.2
            color: Theme.neutral700
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.opened() }
}
