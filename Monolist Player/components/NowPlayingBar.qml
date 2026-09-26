import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    // As the window narrows the bar gives things up in one order: the volume
    // slider folds into a button (the output button, which has nothing to
    // choose between yet, goes with it), then the progress line moves under
    // the buttons, then the title goes. The Now Playing and queue buttons
    // never go: nothing else opens either of them.
    readonly property bool showVolume: width >= 1120
    readonly property bool showMeta: width >= 760
    // Under the buttons once one line would leave the progress line too short
    // to aim at. Without the title it stays under, so narrowing the window
    // never puts it back on the line it has just left.
    readonly property bool stacked: !showMeta || transport.width < 440
    // In Now Playing this is whether its UP NEXT pane is showing.
    property bool queueOpen: false
    property bool nowPlayingOpen: false
    signal queueToggled()
    signal nowPlayingToggled()

    // Muting is a volume of nothing. The level it had is kept here so unmuting
    // brings it back; a restart finds the volume as it was left, muted or not.
    property real volumeBeforeMute: 0.65
    readonly property bool muted: Player.volume <= 0
    readonly property string volumeIcon: muted ? "volume-x"
                                       : Player.volume < 0.5 ? "volume-1" : "volume-2"

    function toggleMute() {
        if (muted) {
            Player.setVolume(volumeBeforeMute > 0 ? volumeBeforeMute : 0.65)
        } else {
            volumeBeforeMute = Player.volume
            Player.setVolume(0)
        }
    }

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
        width: root.showMeta ? 320 : 52
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
                                                       || Player.statusError

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
                    color: Player.engineAvailable && !Player.statusError ? Theme.neutral700
                                                                        : Theme.accent
                }
            }

            LikeButton {
                visible: root.showMeta
                anchors.verticalCenter: parent.verticalCenter
                liked: Player.favourite
                side: 30
                iconSize: 15
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
        anchors.right: rightControls.left
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        height: root.stacked ? buttons.height + Theme.space1 + timeline.height : buttons.height

        Row {
            id: buttons
            x: root.stacked ? Math.round((parent.width - width) / 2) : 0
            spacing: Theme.space2

            IconButton {
                iconName: "shuffle"
                iconColor: Player.shuffle ? Theme.accent : Theme.neutral700
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.setShuffle(!Player.shuffle)
            }
            IconButton {
                iconName: "skip-back"
                iconColor: Theme.neutral700
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.previous()
            }

            Rectangle {
                width: 44
                height: 44
                color: playHover.hovered ? Theme.accent600 : Theme.accent
                anchors.verticalCenter: parent.verticalCenter

                Behavior on color {
                    enabled: !playHover.hovered
                    ColorAnimation { duration: Theme.quick }
                }

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
                iconColor: Theme.neutral700
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.next()
            }
            IconButton {
                iconName: Player.repeatMode === 2 ? "repeat-1" : "repeat"
                iconColor: Player.repeatMode === 0 ? Theme.neutral700 : Theme.accent
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.cycleRepeat()
            }
        }

        // The times and the progress line: beside the buttons, or under them
        // across the whole width when beside would leave the line too short.
        Item {
            id: timeline
            x: root.stacked ? 0 : buttons.width + Theme.space4
            y: root.stacked ? buttons.height + Theme.space1 : Math.round((parent.height - height) / 2)
            width: parent.width - x
            height: elapsed.implicitHeight

            Text {
                id: elapsed
                anchors.left: parent.left
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
    }

    // — output —
    Row {
        id: rightControls
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space2

        IconButton {
            iconName: "maximize-2"
            iconColor: root.nowPlayingOpen ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.nowPlayingToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.nowPlayingOpen ? "Close Now Playing" : "Now Playing and lyrics"
        }
        IconButton {
            iconName: "list-music"
            iconColor: root.queueOpen ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.queueToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.nowPlayingOpen ? (root.queueOpen ? "Show lyrics" : "Up next")
                                              : (root.queueOpen ? "Hide queue" : "Queue")
        }
        IconButton {
            visible: root.showVolume
            iconName: "monitor-speaker"
            iconColor: Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
        }

        IconButton {
            visible: root.showVolume
            iconName: root.volumeIcon
            iconColor: root.muted ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.toggleMute()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.muted ? "Unmute" : "Mute"
        }

        ProgressSlider {
            visible: root.showVolume
            width: 90
            anchors.verticalCenter: parent.verticalCenter
            value: Player.volume
            fillColor: Theme.text
            onMoved: function(v) { Player.setVolume(v) }
        }

        // Too narrow for the slider: one button, and the slider and mute in a
        // popup above it. Red while muted, so that still shows when folded.
        IconButton {
            id: volumeButton
            visible: !root.showVolume
            iconName: root.volumeIcon
            iconColor: root.muted ? Theme.accent : volumePopup.visible ? Theme.text : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: volumePopup.visible ? volumePopup.close() : volumePopup.open()
            onVisibleChanged: if (!visible) volumePopup.close()
            ToolTip.visible: hovered && !volumePopup.visible
            ToolTip.delay: 600
            ToolTip.text: root.muted ? "Volume (muted)" : "Volume"

            // It stands on the bar's top rule, as if pulled up out of the bar,
            // and appears without motion: it answers a click, as a menu does.
            Popup {
                id: volumePopup
                x: volumeButton.width - width
                margins: Theme.space2
                topPadding: Theme.space2
                bottomPadding: Theme.space2
                leftPadding: Theme.space2
                rightPadding: Theme.space4
                // A press on the button is its toggle, so it does not count as
                // a press outside; the button closes it itself.
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                focus: true
                onAboutToShow: y = -volumeButton.mapToItem(root, 0, 0).y - height + Theme.ruleWidth

                background: Rectangle {
                    color: Theme.bg
                    border.width: Theme.ruleWidth
                    border.color: Theme.text
                }

                contentItem: Row {
                    spacing: Theme.space2

                    IconButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: root.volumeIcon
                        iconColor: root.muted ? Theme.accent : Theme.neutral700
                        iconSize: 15
                        onClicked: root.toggleMute()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: root.muted ? "Unmute" : "Mute"
                    }

                    ProgressSlider {
                        width: 120
                        anchors.verticalCenter: parent.verticalCenter
                        value: Player.volume
                        fillColor: Theme.text
                        onMoved: function(v) { Player.setVolume(v) }
                    }
                }
            }
        }
    }
}
