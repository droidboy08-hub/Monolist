import QtQuick
import Monolist
import Monolist.Backend

Column {
    id: root

    property var model: null
    property int activeIndex: -1
    signal trackActivated(int index)

    // Column visibility follows the window: metadata drops before the title does.
    readonly property bool showAlbum: width >= 900
    readonly property bool showArtist: width >= 700
    readonly property int timeWidth: 64
    readonly property int indexWidth: 48
    readonly property int freeWidth: width - indexWidth - timeWidth
    readonly property int albumColumnWidth: showAlbum ? Math.round(freeWidth * 0.30) : 0
    readonly property int artistColumnWidth: showArtist ? Math.round(freeWidth * 0.28) : 0
    readonly property int titleColumnWidth: freeWidth - albumColumnWidth - artistColumnWidth

    spacing: 0

    // — head —
    Item {
        width: root.width
        height: 32

        Text {
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            text: "#"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
        Text {
            x: root.indexWidth
            anchors.verticalCenter: parent.verticalCenter
            text: "TITLE"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
        Text {
            visible: root.showArtist
            x: root.indexWidth + root.titleColumnWidth
            anchors.verticalCenter: parent.verticalCenter
            text: "ARTIST"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
        Text {
            visible: root.showAlbum
            x: root.indexWidth + root.titleColumnWidth + root.artistColumnWidth
            anchors.verticalCenter: parent.verticalCenter
            text: "ALBUM"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
        Icon {
            name: "clock"
            width: 14
            height: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.neutral700
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    // — rows —
    Repeater {
        model: root.model

        delegate: Item {
            id: row

            required property int index
            required property string title
            required property string artist
            required property string album
            required property string durationText

            width: root.width
            height: 40
            readonly property bool isActive: index === root.activeIndex

            Rectangle {
                anchors.fill: parent
                color: rowHover.hovered ? Theme.rowHover : "transparent"
            }

            Text {
                x: 0
                width: root.indexWidth
                anchors.verticalCenter: parent.verticalCenter
                text: row.isActive && Player.playing ? "▶" : String(row.index + 1)
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: row.isActive ? Theme.accent700 : Theme.text
            }

            Text {
                x: root.indexWidth
                width: Math.max(0, root.titleColumnWidth - Theme.space4)
                anchors.verticalCenter: parent.verticalCenter
                text: row.title
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: row.isActive ? Font.Bold : Theme.weightRegular
                color: row.isActive ? Theme.accent700 : Theme.text
            }

            Text {
                visible: root.showArtist
                x: root.indexWidth + root.titleColumnWidth
                width: Math.max(0, root.artistColumnWidth - Theme.space4)
                anchors.verticalCenter: parent.verticalCenter
                text: row.artist
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: row.isActive ? Font.Bold : Theme.weightRegular
                color: row.isActive ? Theme.accent700 : Theme.text
            }

            Text {
                visible: root.showAlbum
                x: root.indexWidth + root.titleColumnWidth + root.artistColumnWidth
                width: Math.max(0, root.albumColumnWidth - Theme.space4)
                anchors.verticalCenter: parent.verticalCenter
                text: row.album
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 14
                color: Theme.neutral700
            }

            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: row.durationText
                font.family: Theme.fontFamily
                font.pixelSize: 14
                color: row.isActive ? Theme.accent700 : Theme.text
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.hairline
            }

            HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.trackActivated(row.index) }
        }
    }
}
