import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The play queue, docked at the right. What has already played is left out:
// the list opens on the song playing now, then what will play, in order, with
// the point where autoplay's radio takes over marked.
Rectangle {
    id: root

    signal closeRequested()

    color: Theme.bg

    component Caption: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.12)
        color: Theme.neutral700
    }

    // — head, level with the top bar —
    Item {
        id: head
        width: parent.width
        height: 64

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            text: "Queue"
            font.family: Theme.fontFamily
            font.pixelSize: 20
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(20, -0.01)
            color: Theme.text
        }

        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            iconName: "x"
            iconColor: Theme.text
            iconSize: 14
            onClicked: root.closeRequested()
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    ListView {
        id: list
        anchors.top: head.bottom
        anchors.bottom: foot.top
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Player.queue

        ScrollBar.vertical: MonoScrollBar { width: 8 }

        delegate: Column {
            id: entry

            required property int index
            required property string title
            required property string artist
            required property string artwork
            required property string durationText
            required property bool isCurrent
            required property bool isPast

            width: ListView.view ? ListView.view.width : 0
            visible: !isPast
            height: visible ? implicitHeight : 0

            Caption {
                visible: entry.isCurrent
                leftPadding: Theme.space6
                topPadding: Theme.space6
                bottomPadding: Theme.space2
                text: "NOW PLAYING"
            }
            Caption {
                visible: entry.index === Player.queue.currentIndex + 1
                leftPadding: Theme.space6
                topPadding: Theme.space6
                bottomPadding: Theme.space2
                text: "UP NEXT · " + Player.queue.upcomingCount
            }
            Caption {
                visible: entry.index === Player.queue.radioStartIndex
                leftPadding: Theme.space6
                topPadding: entry.index === Player.queue.currentIndex + 1 ? 0 : Theme.space6
                bottomPadding: Theme.space2
                text: "AUTOPLAY · SONGS LIKE WHAT YOU PLAYED"
                color: Theme.accent700
            }

            Item {
                width: entry.width
                height: 56

                Rectangle {
                    anchors.fill: parent
                    color: entryHover.hovered && !entry.isCurrent ? Theme.rowHover : "transparent"
                }

                Artwork {
                    id: art
                    x: Theme.space6
                    width: 40
                    height: 40
                    anchors.verticalCenter: parent.verticalCenter
                    placeholder: ""
                    source: entry.artwork
                }

                Column {
                    anchors.left: art.right
                    anchors.leftMargin: Theme.space3
                    anchors.right: tail.left
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        width: parent.width
                        text: entry.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: entry.isCurrent ? Font.Bold : Theme.weightRegular
                        color: entry.isCurrent ? Theme.accent700 : Theme.text
                    }
                    Text {
                        width: parent.width
                        text: entry.artist
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                }

                Item {
                    id: tail
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    height: 30

                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !removeButton.visible
                        text: entry.durationText
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: entry.isCurrent ? Theme.accent700 : Theme.neutral700
                    }

                    IconButton {
                        id: removeButton
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: entryHover.hovered && !entry.isCurrent
                        side: 30
                        iconName: "x"
                        iconColor: Theme.text
                        iconSize: 12
                        onClicked: Player.removeFromQueue(entry.index)
                    }
                }

                HoverHandler { id: entryHover; cursorShape: entry.isCurrent ? Qt.ArrowCursor : Qt.PointingHandCursor }
                TapHandler {
                    enabled: !entry.isCurrent
                    onTapped: Player.playIndex(entry.index)
                }
            }
        }
    }

    Text {
        anchors.centerIn: list
        visible: Player.queue.count === 0
        width: list.width - Theme.space6 * 2
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "Nothing queued. Play something from Search, Home or your Downloads."
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // — foot: autoplay and clearing —
    Item {
        id: foot
        anchors.bottom: parent.bottom
        width: parent.width
        height: 56

        Rectangle {
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            Rectangle {
                width: 18
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                color: Player.autoplay ? Theme.accent : "transparent"
                border.width: Theme.ruleWidth
                border.color: Player.autoplay ? Theme.accent : Theme.text

                Icon {
                    anchors.centerIn: parent
                    width: 12
                    height: 12
                    name: "check"
                    thickness: 3
                    visible: Player.autoplay
                    color: Theme.accentForeground
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Autoplay similar songs"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.text
            }

            TapHandler { onTapped: Player.autoplay = !Player.autoplay }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            visible: Player.queue.upcomingCount > 0
            text: "CLEAR"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: clearHover.hovered ? Theme.accent700 : Theme.neutral700

            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Player.clearUpcoming() }
        }
    }

    // The system's divider: a 2px rule down the left edge.
    Rectangle {
        width: Theme.ruleWidth
        height: parent.height
        color: Theme.divider
    }
}
