import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    readonly property bool showVolume: width >= 1040
    readonly property bool showMeta: width >= 760
    property bool queueOpen: false
    property bool nowPlayingOpen: false
    signal queueToggled()
    signal nowPlayingToggled()

    color: Theme.bg
    implicitHeight: Theme.playerBarHeight

    Rectangle {
        width: parent.width
        height: Theme.ruleWidth
        color: Theme.text
    }

    // — now playing —
    Item {
        id: nowPlaying
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        width: root.showMeta ? 320 : 120
        height: 52

        Row {
            anchors.fill: parent
            spacing: Theme.space3

            // The cover and the title open Now Playing.
            Artwork {
                width: 52
                height: 52
                placeholder: "Art"
                source: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nowPlayingToggled() }
            }

            Column {
                visible: root.showMeta
                anchors.verticalCenter: parent.verticalCenter
                // artwork, then the heart and the download control beside the text
                width: parent.width - 52 - 30 * 2 - Theme.space3 * 3
                spacing: 1

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nowPlayingToggled() }

                Text {
                    width: parent.width
                    text: Player.currentTrack.title !== undefined ? Player.currentTrack.title : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Theme.weightBlack
                    color: Theme.text
                }
                // Doubles as the status line: while a source is resolving, or
                // when the audio engine is missing entirely, that matters more
                // than the artist and there is nowhere else it would be seen.
                Text {
                    readonly property bool showStatus: !Player.engineAvailable || Player.resolving

                    width: parent.width
                    text: showStatus
                          ? Player.statusText
                          : (Player.currentTrack.artist !== undefined
                             ? Player.currentTrack.artist
                               + (Player.currentTrack.album ? " — " + Player.currentTrack.album : "")
                             : "")
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Player.engineAvailable ? Theme.neutral700 : Theme.accent
                }
            }

            IconButton {
                visible: root.showMeta
                anchors.verticalCenter: parent.verticalCenter
                iconName: Player.favourite ? "heart-filled" : "heart"
                side: 30
                iconSize: 15
                iconColor: Player.favourite ? Theme.accent : Theme.text
                onClicked: Player.toggleFavourite()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: Player.favourite ? "Remove from Liked songs" : "Add to Liked songs"
            }

            DownloadButton {
                readonly property string sourceId: Player.currentTrack.sourceId !== undefined
                                                   ? Player.currentTrack.sourceId : ""
                visible: root.showMeta && sourceId.length > 0 && Downloads.available
                anchors.verticalCenter: parent.verticalCenter
                side: 30
                iconSize: 15
                videoId: sourceId
                title: Player.currentTrack.title !== undefined ? Player.currentTrack.title : ""
                artist: Player.currentTrack.artist !== undefined ? Player.currentTrack.artist : ""
                artwork: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""
                durationMs: Player.duration
            }
        }
    }

    // — transport —
    Item {
        id: transport
        anchors.left: nowPlaying.right
        anchors.leftMargin: Theme.space6
        anchors.right: rightControls.visible ? rightControls.left : parent.right
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        height: 44

        Row {
            id: buttons
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space2

            IconButton {
                iconName: "shuffle"
                iconColor: Player.shuffle ? Theme.accent : Theme.text
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.setShuffle(!Player.shuffle)
            }
            IconButton {
                iconName: "skip-back"
                iconColor: Theme.text
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.previous()
            }

            Rectangle {
                width: 44
                height: 44
                color: playHover.hovered ? Theme.accent600 : Theme.accent
                anchors.verticalCenter: parent.verticalCenter

                Icon {
                    anchors.centerIn: parent
                    name: Player.playing ? "pause" : "play"
                    width: 18
                    height: 18
                    color: Theme.accentForeground
                }

                HoverHandler { id: playHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Player.togglePlay() }
            }

            IconButton {
                iconName: "skip-forward"
                iconColor: Theme.text
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.next()
            }
            IconButton {
                iconName: Player.repeatMode === 2 ? "repeat-1" : "repeat"
                iconColor: Player.repeatMode === 0 ? Theme.text : Theme.accent
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.cycleRepeat()
            }
        }

        Text {
            id: elapsed
            anchors.left: buttons.right
            anchors.leftMargin: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            text: Player.positionText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }

        ProgressSlider {
            anchors.left: elapsed.right
            anchors.right: total.left
            anchors.leftMargin: Theme.space4
            anchors.rightMargin: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            value: Player.progress
            onMoved: function(v) { Player.seekFraction(v) }
        }

        Text {
            id: total
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: Player.durationText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }
    }

    // — output —
    Row {
        id: rightControls
        visible: root.showVolume
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space2

        IconButton {
            iconName: "maximize-2"
            iconColor: root.nowPlayingOpen ? Theme.accent : Theme.text
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.nowPlayingToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.nowPlayingOpen ? "Close Now Playing" : "Now Playing and lyrics"
        }
        IconButton {
            iconName: "list-music"
            iconColor: root.queueOpen ? Theme.accent : Theme.text
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.queueToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.queueOpen ? "Hide queue" : "Queue"
        }
        IconButton {
            iconName: "monitor-speaker"
            iconColor: Theme.text
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
        }

        Icon {
            name: "volume-2"
            width: 15
            height: 15
            color: Theme.neutral700
            anchors.verticalCenter: parent.verticalCenter
        }

        ProgressSlider {
            width: 90
            anchors.verticalCenter: parent.verticalCenter
            value: Player.volume
            fillColor: Theme.text
            onMoved: function(v) { Player.setVolume(v) }
        }
    }
}
