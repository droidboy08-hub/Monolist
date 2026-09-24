import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One shelf of suggestions: a heading, the reason it exists, and the songs.
//
// Text rows rather than cards, because a catalogue row is a name and nothing
// else — there is no artwork to show without first resolving every one of
// them, which would cost a search each. The names are the honest thing to
// show, and pressing one is what turns it into a song.
Item {
    id: root

    property string title: ""
    property string reason: ""
    property var rows: []

    signal rowActivated(int index)

    implicitHeight: body.implicitHeight

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Text {
            width: parent.width
            text: root.title
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 19
            font.weight: Theme.weightBlack
            color: Theme.text
        }

        Text {
            width: parent.width
            visible: root.reason.length > 0
            text: root.reason
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }

        Column {
            id: list
            width: parent.width

            Repeater {
                model: root.rows

                Item {
                    id: row

                    required property var modelData
                    required property int index

                    width: list.width
                    height: 40

                    Rectangle {
                        anchors.fill: parent
                        color: hover.hovered ? Theme.rowHover : "transparent"
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        text: String(row.index + 1).padStart(2, "0")
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: hover.hovered ? Theme.accent : Theme.neutral700
                    }

                    Text {
                        x: 28
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, parent.width * 0.55 - 28)
                        text: row.modelData.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Theme.weightBlack
                        color: Theme.text
                    }

                    Text {
                        x: parent.width * 0.55
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.45
                        text: row.modelData.artist
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.neutral700
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.ruleWidth
                        anchors.bottom: parent.bottom
                        color: Theme.neutral300
                    }

                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.rowActivated(row.index) }
                }
            }
        }
    }
}
